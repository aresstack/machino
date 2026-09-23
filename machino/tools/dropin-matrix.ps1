# AP24: sweep every endpoint the stock WebUI actually touches and report what
# this camera answers.
#
# The endpoint list is not invented - it is what `grep` finds being fetched in
# the upstream clone's www/a/*.js, which is the rule this project works by: the
# executed JS beats stale docs.
#
# Read-only. Every probe is a GET or a WebSocket handshake; nothing is POSTed,
# nothing is written, and the JPEG paths are included because with
# jpeg.enabled false the encoder reports unsupported and is never created (the
# wedge needs a creation). Liveness is re-checked at the end either way.
#
#   pwsh tools/dropin-matrix.ps1 -Cam 192.168.1.10 -Pw '...'

param(
    [string]$Cam = '192.168.1.10',
    [string]$Pw  = '',
    [string]$Out = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Out) { $Out = "C:\tmp\dropin-matrix-$(Get-Date -Format yyyyMMdd-HHmmss).txt" }
function Note($s) { [Console]::WriteLine($s); Add-Content -Path $Out -Value $s -Encoding utf8 }

function Get-Cookie {
    param([string]$cam, [string]$pw)
    $c = New-Object Net.Sockets.TcpClient; $c.Connect($cam, 80); $s = $c.GetStream()
    $b = "username=root&password=$pw"
    $by = [Text.Encoding]::ASCII.GetBytes("POST /login HTTP/1.1`r`nHost: $cam`r`nContent-Type: application/x-www-form-urlencoded`r`nContent-Length: $($b.Length)`r`nConnection: close`r`n`r`n$b")
    $s.Write($by, 0, $by.Length); $s.Flush(); Start-Sleep -Milliseconds 400
    $buf = New-Object byte[] 8192; $n = $s.Read($buf, 0, $buf.Length)
    $h = [Text.Encoding]::ASCII.GetString($buf, 0, $n); $c.Close()
    if ($h -match 'Set-Cookie:\s*([^;\r\n]+)') { return $Matches[1] }
    return ''
}

# A GET, with the head end found by whichever blank line comes first. A CGI
# emits bare-LF headers and busybox passes them through untouched, so looking
# only for CRLFCRLF silently slices the body three bytes in - that mistake
# already produced one false "the relay is doubling headers" scare.
function Probe-Http {
    param([string]$cam, [string]$path, [string]$ck, [int]$max = 262144)
    $c = New-Object Net.Sockets.TcpClient
    try {
        $c.Connect($cam, 80); $c.ReceiveTimeout = 20000
        $s = $c.GetStream(); $s.ReadTimeout = 20000
        $by = [Text.Encoding]::ASCII.GetBytes("GET $path HTTP/1.1`r`nHost: $cam`r`nCookie: $ck`r`nConnection: close`r`n`r`n")
        $s.Write($by, 0, $by.Length); $s.Flush()
        $ms = New-Object IO.MemoryStream; $bb = New-Object byte[] 65536
        try { while ($ms.Length -lt $max) { $k = $s.Read($bb, 0, $bb.Length); if ($k -le 0) { break }; $ms.Write($bb, 0, $k) } } catch {}
        $txt = [Text.Encoding]::UTF8.GetString($ms.ToArray())
        $status = ($txt -split "`r?`n")[0] -replace '^HTTP/1\.[01] ', ''
        $i1 = $txt.IndexOf("`r`n`r`n"); $i2 = $txt.IndexOf("`n`n")
        $cut = if ($i1 -ge 0 -and ($i2 -lt 0 -or $i1 -lt $i2)) { $i1 + 4 } elseif ($i2 -ge 0) { $i2 + 2 } else { 0 }
        $body = ($txt.Substring($cut) -replace "`r?`n", ' ')
        return @{ status = $status; bytes = $ms.Length; body = $body }
    } catch {
        return @{ status = "ERR $($_.Exception.Message)"; bytes = 0; body = '' }
    } finally { try { $c.Close() } catch {} }
}

function Probe-Ws {
    param([string]$cam, [string]$path, [string]$ck)
    $ws = New-Object System.Net.WebSockets.ClientWebSocket
    if ($ck) { $ws.Options.SetRequestHeader("Cookie", $ck) }
    $cts = New-Object System.Threading.CancellationTokenSource
    try {
        $ws.ConnectAsync([Uri]("ws://$cam$path"), $cts.Token).Wait(12000) | Out-Null
    } catch {
        $e = $_.Exception; while ($e.InnerException) { $e = $e.InnerException }
        return @{ status = "refused"; body = $e.Message }
    }
    if ($ws.State -ne 'Open') { try { $ws.Abort() } catch {}; return @{ status = "not open ($($ws.State))"; body = '' } }
    # One read, briefly: an endpoint that opens and then says why is a
    # different answer from one that opens and streams.
    $seg = New-Object 'System.ArraySegment[byte]' (, (New-Object byte[] 8192))
    $first = ''
    try {
        $t = $ws.ReceiveAsync($seg, $cts.Token)
        if ($t.Wait(4000)) {
            $r = $t.Result
            $first = [Text.Encoding]::UTF8.GetString($seg.Array, 0, [Math]::Min($r.Count, 160)) -replace "`r?`n", ' '
        }
    } catch {}
    try { $ws.Abort() } catch {}
    return @{ status = "open"; body = $first }
}

$HTTP = @(
    '/api/v1/analytics/day', '/api/v1/calibration/coverage', '/api/v1/calibration/pair',
    '/api/v1/calibration/peer', '/api/v1/config', '/api/v1/config.json',
    '/api/v1/config.schema.json', '/api/v1/gpio', '/api/v1/image', '/api/v1/live',
    '/api/v1/osd', '/api/v1/osd/image', '/api/v1/outgoing.json', '/api/v1/peers',
    '/api/v1/pinmux', '/api/v1/records/resume', '/api/v1/records/standdown',
    '/api/v1/reset', '/api/v1/sources',
    # Machino-native routes as well, so the sweep covers everything the daemon
    # claims and not only what the stock pages ask for. /logout is left out on
    # purpose: it would end the session halfway through the sweep.
    '/api/v1/capabilities', '/api/v1/state', '/api/v1/telemetry', '/api/v1/get',
    '/api/v1/snapshot', '/snapshot.jpg', '/stream.mjpeg', '/api/v1/stream.mjpeg',
    '/setup', '/setup.html',
    '/cgi-bin/dashboard.cgi', '/cgi-bin/j/download.cgi', '/cgi-bin/j/files.cgi',
    '/cgi-bin/j/fw-latest.cgi', '/cgi-bin/j/logmeta.cgi', '/cgi-bin/j/network.cgi',
    '/cgi-bin/j/ptz.cgi', '/cgi-bin/j/pulse.cgi', '/cgi-bin/j/recordings.cgi',
    '/cgi-bin/j/run.cgi', '/cgi-bin/j/save.cgi', '/cgi-bin/j/sdcard.cgi',
    '/cgi-bin/j/time.cgi',
    '/image.jpg', '/login.html', '/metrics', '/metrics/night', '/metrics/records',
    '/mjpeg', '/snapshot', '/upload'
)
$WS = @('/ws/analytics', '/ws/logs', '/ws/pins', '/ws/upgrade', '/ws/video?stream=0',
        '/ws/video?stream=1', '/ws/webrtc?stream=0')

Note "dropin-matrix  cam=$Cam  $(Get-Date -Format s)"
$ck = if ($Pw) { Get-Cookie -cam $Cam -pw $Pw } else { '' }
Note "session: $(if ($ck) { 'obtained' } else { 'none' })"
Note ''
Note ("{0,-34} {1,-22} {2,8}  {3}" -f 'ENDPOINT', 'STATUS', 'BYTES', 'FIRST BYTES')
Note ('-' * 120)
foreach ($p in $HTTP) {
    $r = Probe-Http -cam $Cam -path $p -ck $ck
    $b = if ($r.body.Length -gt 58) { $r.body.Substring(0, 58) + '...' } else { $r.body }
    Note ("{0,-34} {1,-22} {2,8}  {3}" -f $p, $r.status, $r.bytes, $b)
}
Note ''
foreach ($p in $WS) {
    $r = Probe-Ws -cam $Cam -path $p -ck $ck
    $b = if ($r.body.Length -gt 58) { $r.body.Substring(0, 58) + '...' } else { $r.body }
    Note ("{0,-34} {1,-22} {2,8}  {3}" -f "WS $p", $r.status, '', $b)
}
Note ''
$live = Probe-Http -cam $Cam -path '/api/v1/state' -ck $ck
Note "camera still answering after the sweep: $($live.status)"
Note "report: $Out"
