# AP17: passive instrumentation for the rare T40NN hardlock.
#
# The failure is a COMPLETE stop: no FIN, no RST, no ARP, no UART, and no
# reboot (kernel.panic = 20 would have rebooted a panic, so it is a hang, not
# a panic). It has never been reproduced on demand. So this does not try to
# provoke it - it waits, and makes sure that when it happens the last minutes
# are still on disk.
#
# Three rules it follows, all of them learned the hard way:
#
#   1. The capture ring is BOUNDED. Two earlier captures reached 1.1 GB and
#      2.77 GB because dumpcap was started without a file count. -b files:N
#      is not optional here.
#   2. The liveness probe is SLOW and cheap: one TCP connect every -ProbeSec
#      seconds, nothing on the camera itself. No per-second sampler, no
#      fork storm on the box - an earlier sampler was suspected of being part
#      of the trigger, and an instrument that might cause the fault is not an
#      instrument.
#   3. On loss of contact the ring is FROZEN immediately. dumpcap keeps
#      overwriting; the whole point is to stop it while the death is still in
#      the window.
#
#   pwsh tools/hardlock-watch.ps1 -Cam 192.168.1.10 -Iface 5
#
# -Iface is the number from `dumpcap -D`. Ctrl-C stops it cleanly.

param(
    [string]$Cam        = '192.168.1.10',
    [int]$Iface         = 5,          # dumpcap -D index of the LAN adapter facing the camera
    [int]$ProbeSec      = 10,         # one TCP connect per this many seconds
    [int]$DeadProbes    = 3,          # consecutive failures before the ring is frozen
    [int]$RingFiles     = 12,         # bounded: RingFiles * RingKB is the hard ceiling
    [int]$RingKB        = 20480,      # 12 x 20 MB = 240 MB, and it never grows past that
    [int]$Port          = 80,
    [string]$OutDir     = ''
)

$ErrorActionPreference = 'Stop'
$dumpcap = 'C:\Program Files\Wireshark\dumpcap.exe'
if (-not (Test-Path $dumpcap)) { throw "dumpcap not found at $dumpcap" }
if (-not $OutDir) { $OutDir = "C:\tmp\hardlock-$(Get-Date -Format yyyyMMdd-HHmmss)" }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $OutDir 'watch.log'
function Note($s) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  $s"
    [Console]::WriteLine($line)
    Add-Content -Path $log -Value $line -Encoding utf8
}

Note "hardlock-watch  cam=$Cam iface=$Iface probe=${ProbeSec}s ring=${RingFiles}x${RingKB}KB (cap $([Math]::Round($RingFiles*$RingKB/1024)) MB)"
Note "out=$OutDir"

# The ring. Filtered to the camera so the files hold the conversation and not
# the rest of the LAN; the cap is enforced by dumpcap itself.
$ringBase = Join-Path $OutDir 'ring.pcapng'
# The filter is QUOTED: Start-Process joins -ArgumentList with spaces and does
# not quote anything, so a bare `host 1.2.3.4` reaches dumpcap as two separate
# arguments and it exits before writing a byte - with its window hidden, in
# silence. Its stderr goes to a file for the same reason - and -q stops the
# continuous "Packets: N" line, which would otherwise grow that file without
# bound for as long as the watch runs (the point of this script is to run for
# days).
$dcErr = Join-Path $OutDir "dumpcap.err"
$dcArgs = @("-q", "-i", "$Iface", "-f", "`"host $Cam`"", "-b", "files:$RingFiles", "-b", "filesize:$RingKB", "-w", "`"$ringBase`"")
$cap = Start-Process -FilePath $dumpcap -ArgumentList $dcArgs -PassThru -WindowStyle Hidden -RedirectStandardError $dcErr
Start-Sleep -Seconds 2
if ($cap.HasExited) { Note "dumpcap exited immediately: $(Get-Content $dcErr -Raw)"; throw "capture did not start" }
Note "dumpcap pid $($cap.Id) started"

function Probe {
    param([string]$h, [int]$p)
    $c = New-Object Net.Sockets.TcpClient
    try {
        $iar = $c.BeginConnect($h, $p, $null, $null)
        if (-not $iar.AsyncWaitHandle.WaitOne(3000)) { return $false }
        $c.EndConnect($iar)
        return $true
    } catch { return $false } finally { try { $c.Close() } catch {} }
}

$dead = 0
$lastOk = Get-Date
$probes = 0
try {
    while ($true) {
        Start-Sleep -Seconds $ProbeSec
        $probes++
        if (Probe -h $Cam -p $Port) {
            if ($dead -gt 0) { Note "recovered after $dead failed probe(s)" }
            $dead = 0
            $lastOk = Get-Date
            # A quiet heartbeat, rare enough to read: once every ~10 minutes.
            if (($probes * $ProbeSec) % 600 -lt $ProbeSec) { Note "alive (probe $probes)" }
        } else {
            $dead++
            Note "PROBE FAILED ($dead/$DeadProbes) - last contact $lastOk"
            if ($dead -ge $DeadProbes) {
                Note "CAMERA UNREACHABLE - freezing the ring"
                try { Stop-Process -Id $cap.Id -Force } catch {}
                Start-Sleep -Seconds 2
                $files = Get-ChildItem (Join-Path $OutDir 'ring*.pcapng') | Sort-Object LastWriteTime
                foreach ($f in $files) { Note ("  {0}  {1:n1} MB  {2}" -f $f.Name, ($f.Length/1MB), $f.LastWriteTime) }
                Note "last successful contact: $lastOk"
                Note "NEXT: the last file above holds the final packets. Look for the last"
                Note "      frame FROM the camera, and for anything the PC sent after it that"
                Note "      was never acknowledged. Do NOT power-cycle before the UART line"
                Note "      has been read - console logging is on since AP17 (printk 3 3 1 3)."
                break
            }
        }
    }
} finally {
    if (-not $cap.HasExited) { try { Stop-Process -Id $cap.Id -Force } catch {} }
    Note "stopped. report: $log"
}
