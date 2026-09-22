# AP15: measure the /ws/video MSE feed WITHOUT a browser.
#
# What it answers: does the fMP4 media timeline track wall clock?
#   drift(t) = (tfdt(t) - tfdt(0)) / 90000 - (arrival(t) - arrival(0))
# A media timeline that runs AHEAD of wall clock is a buffer that grows in the
# browser and latency that climbs - exactly the AP15 symptom. A timeline that
# tracks wall clock means the growth is not ours.
#
# It also reports the arrival pattern (inter-arrival jitter, bursts), because a
# server that batches N frames and then sleeps is a rebuffer source of its own.
#
# PowerShell on purpose: MSYS bash /dev/tcp loses responses, and
# System.Net.WebSockets.ClientWebSocket speaks the real protocol.
#
#   pwsh tools/mse-drift.ps1 -Cam 192.168.1.10 -Pw '...' -Clients 3 -Seconds 600
#
# Credentials are a parameter and are never written to the log.

param(
    [string]$Cam      = '192.168.1.10',
    [string]$Pw       = '',
    [int]$Clients     = 1,
    [int]$Seconds     = 120,
    [string]$Stream   = '0',           # as upstream preview.js spells it: 0 = MAIN, 1 = SUB
    [string]$OutDir   = ''
)

$ErrorActionPreference = 'Stop'
if (-not $OutDir) { $OutDir = Join-Path $env:TEMP ("mse-drift-" + (Get-Date -Format yyyyMMdd-HHmmss)) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $OutDir 'report.txt'
function Note($s) { [Console]::WriteLine($s); Add-Content -Path $log -Value $s -Encoding utf8 }

# ---------------------------------------------------------------- login
function Get-Cookie {
    param([string]$cam, [string]$pw)
    $c = New-Object Net.Sockets.TcpClient
    $c.Connect($cam, 80)
    $s = $c.GetStream()
    $b = "username=root&password=$pw"
    $req = "POST /login HTTP/1.1`r`nHost: $cam`r`nContent-Type: application/x-www-form-urlencoded`r`nContent-Length: $($b.Length)`r`nConnection: close`r`n`r`n$b"
    $by = [Text.Encoding]::ASCII.GetBytes($req)
    $s.Write($by, 0, $by.Length); $s.Flush()
    Start-Sleep -Milliseconds 400
    $buf = New-Object byte[] 8192
    $n = $s.Read($buf, 0, $buf.Length)
    $h = [Text.Encoding]::ASCII.GetString($buf, 0, $n)
    $c.Close()
    if ($h -match 'Set-Cookie:\s*([^=]+)=([^;\r\n]+)') { return @{ name = $Matches[1]; value = $Matches[2] } }
    return $null
}

# ------------------------------------------------- one client, one socket
# Runs in a background job: a live feed per client, all of them at once.
$clientBody = {
    param($cam, $stream, $ckName, $ckValue, $seconds, $csv)

    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    # A raw header, not a CookieContainer: the camera is addressed by IP and a
    # CookieContainer will not hand an IP-domain cookie back out.
    if ($ckName) { $ws.Options.SetRequestHeader("Cookie", "$ckName=$ckValue") }
    $cts = New-Object System.Threading.CancellationTokenSource
    $uri = [Uri]("ws://$cam/ws/video?stream=$stream")
    try { $ws.ConnectAsync($uri, $cts.Token).Wait(15000) | Out-Null }
    catch { $e = $_.Exception; while ($e.InnerException) { $e = $e.InnerException }; return "connect failed: $($e.Message)" }
    if ($ws.State -ne "Open") { return "connect failed: state $($ws.State)" }

    function B32($arr, $o) { return ([uint32]$arr[$o] -shl 24) -bor ([uint32]$arr[$o+1] -shl 16) -bor ([uint32]$arr[$o+2] -shl 8) -bor [uint32]$arr[$o+3] }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $seg = New-Object 'System.ArraySegment[byte]' (, (New-Object byte[] 262144))
    $rows = New-Object Collections.Generic.List[string]
    $rows.Add('wall_ms,kind,tfdt,seq,dur,bytes,key')
    # A MemoryStream, not a List[byte]: appending 15 MB one byte at a time in
    # PowerShell makes the measuring client the slow consumer and the camera
    # then drops frames for it - the instrument would be measuring itself.
    $msg = New-Object System.IO.MemoryStream
    $deadline = $seconds * 1000

    while ($sw.ElapsedMilliseconds -lt $deadline -and $ws.State -eq 'Open') {
        $t = $ws.ReceiveAsync($seg, $cts.Token)
        if (-not $t.Wait(20000)) { $rows.Add("$($sw.ElapsedMilliseconds),timeout,0,0,0,0,0"); break }
        $r = $t.Result
        if ($r.MessageType -eq 'Close') { $rows.Add("$($sw.ElapsedMilliseconds),close,0,0,0,0,0"); break }
        $msg.Write($seg.Array, 0, $r.Count)
        if (-not $r.EndOfMessage) { continue }

        $ms = $sw.ElapsedMilliseconds
        $a  = $msg.ToArray()
        $msg.SetLength(0)
        if ($r.MessageType -eq 'Text') {
            $rows.Add("$ms,text,0,0,0,$($a.Length),0")
            continue
        }
        # binary: init segment (ftyp) or a fragment (moof)
        if ($a.Length -ge 8 -and $a[4] -eq 0x66 -and $a[5] -eq 0x74 -and $a[6] -eq 0x79 -and $a[7] -eq 0x70) {
            $rows.Add("$ms,init,0,0,0,$($a.Length),0")
            continue
        }
        if ($a.Length -lt 100 -or -not ($a[4] -eq 0x6d -and $a[5] -eq 0x6f -and $a[6] -eq 0x6f -and $a[7] -eq 0x66)) {
            $rows.Add("$ms,other,0,0,0,$($a.Length),0")
            continue
        }
        # Fixed layout written by fmp4::fragment(): mfhd(16) tfhd(16) tfdt(20,
        # version 1) then trun. Verified by tag, not assumed.
        $seqNo = B32 $a 20
        $okTfdt = ($a[52] -eq 0x74 -and $a[53] -eq 0x66 -and $a[54] -eq 0x64 -and $a[55] -eq 0x74)
        $okTrun = ($a[72] -eq 0x74 -and $a[73] -eq 0x72 -and $a[74] -eq 0x75 -and $a[75] -eq 0x6e)
        if (-not ($okTfdt -and $okTrun)) {
            $rows.Add("$ms,layout,0,$seqNo,0,$($a.Length),0")
            continue
        }
        $hi   = B32 $a 60
        $lo   = B32 $a 64
        $tfdt = [uint64]$hi * 4294967296 + [uint64]$lo
        $dur  = B32 $a 88
        $flg  = B32 $a 96
        $key  = if ($flg -eq 0x02000000) { 1 } else { 0 }
        $rows.Add("$ms,frag,$tfdt,$seqNo,$dur,$($a.Length),$key")
    }
    try { $ws.Abort() } catch {}
    Set-Content -Path $csv -Value $rows -Encoding ascii
    return "ok rows=$($rows.Count)"
}

# ---------------------------------------------------------------- run
Note "mse-drift  cam=$Cam stream=$Stream clients=$Clients seconds=$Seconds"
Note "out=$OutDir"

$ck = $null
if ($Pw) { $ck = Get-Cookie -cam $Cam -pw $Pw }
if ($ck) { Note "login: session cookie '$($ck.name)' obtained" } else { Note "login: no cookie (unauthenticated attempt)" }

$jobs = @()
for ($i = 1; $i -le $Clients; $i++) {
    $csv = Join-Path $OutDir "client$i.csv"
    $jobs += Start-Job -ScriptBlock $clientBody -ArgumentList $Cam, $Stream, $(if ($ck) { $ck.name } else { '' }), $(if ($ck) { $ck.value } else { '' }), $Seconds, $csv
    Start-Sleep -Milliseconds 250
}
Note "$($jobs.Count) client(s) started"
$jobs | Wait-Job -Timeout ($Seconds + 90) | Out-Null
foreach ($j in $jobs) { Note ("client " + $j.Id + ": " + ((Receive-Job $j) -join ' ')) }
$jobs | Remove-Job -Force

# ---------------------------------------------------------------- analyse
Note ''
Note 'client  frags  key  span_s  fps   media_s  drift_ms  drift_ppm  gap>250ms  maxgap_ms  MB'
for ($i = 1; $i -le $Clients; $i++) {
    $csv = Join-Path $OutDir "client$i.csv"
    if (-not (Test-Path $csv)) { Note ("{0,-6}  (no data)" -f $i); continue }
    $rows = @(Import-Csv $csv | Where-Object { $_.kind -eq 'frag' })
    if ($rows.Count -lt 10) { Note ("{0,-6}  only $($rows.Count) fragments" -f $i); continue }

    $t0 = [double]$rows[0].wall_ms
    $d0 = [double]$rows[0].tfdt
    $tN = [double]$rows[-1].wall_ms
    $dN = [double]$rows[-1].tfdt
    $spanS  = ($tN - $t0) / 1000.0
    $mediaS = ($dN - $d0) / 90000.0
    $driftMs = ($mediaS - $spanS) * 1000.0
    $ppm = if ($spanS -gt 0) { $driftMs / $spanS * 1000.0 } else { 0 }

    $gaps = 0; $maxGap = 0.0; $prev = $t0
    foreach ($r in $rows) {
        $g = [double]$r.wall_ms - $prev
        if ($g -gt 250) { $gaps++ }
        if ($g -gt $maxGap) { $maxGap = $g }
        $prev = [double]$r.wall_ms
    }
    $keys = @($rows | Where-Object { $_.key -eq '1' }).Count
    $mb = (($rows | Measure-Object -Property bytes -Sum).Sum) / 1MB

    # Invariant culture: a German-locale "-4.150" for -4150 ms is a misread
    # waiting to happen in a report that gets pasted into a document.
    Note ([string]::Format([Globalization.CultureInfo]::InvariantCulture,
        "{0,-6}  {1,-5}  {2,-3}  {3,-6:F1}  {4,-5:F2}  {5,-7:F1}  {6,-8:F0}  {7,-9:F0}  {8,-9}  {9,-9:F0}  {10:F1}",
        $i, $rows.Count, $keys, $spanS, ($rows.Count / [Math]::Max($spanS, 0.001)), $mediaS, $driftMs, $ppm, $gaps, $maxGap, $mb))
}
Note ''
Note 'drift_ms > 0 : media timeline runs AHEAD of wall clock -> the browser buffer grows'
Note 'drift_ms < 0 : timeline lags -> the player catches up on its own'
Note "report: $log"
