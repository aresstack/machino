# Relay regression gate (AP1.10). Run this against a camera after any change
# that touches the HTTP front door, before a browser ever sees it.
#
# It exists because the keep-alive change shipped a 502 on every CGI - busybox
# hands a CGI's BARE-LF headers through untouched while static files use CRLF,
# and a parser that only looked for "\r\n\r\n" never found the end of a CGI
# head. That was caught in under a minute by probing a static path AND a CGI
# path; a browser would have shown a blank page and looked like a lockup.
#
# PowerShell rather than sh on purpose: MSYS bash's /dev/tcp silently loses
# responses on this host, which has already produced one false "the camera did
# not answer". Raw sockets here are trustworthy.
#
#   pwsh -File tools/relay-gate.ps1 -Camera 192.168.1.10 -Password <pw>
#
# Exit code 0 = gate passed, 1 = a check failed. Nothing is written to the
# camera; every request is a read.
param(
    [string]$Camera  = '192.168.1.10',
    [string]$Password = '',
    [string]$User     = 'root'
)
$ErrorActionPreference = 'Continue'
$script:fail = 0
function Ok($what)   { [Console]::WriteLine("  PASS  $what") }
function Bad($what)  { [Console]::WriteLine("  FAIL  $what"); $script:fail++ }
function Info($what) { [Console]::WriteLine("        $what") }

function Get-Session {
    $c = New-Object Net.Sockets.TcpClient; $c.Connect($Camera, 80); $s = $c.GetStream()
    $b = "username=$User&password=$Password"
    $by = [Text.Encoding]::ASCII.GetBytes("POST /login HTTP/1.1`r`nHost: $Camera`r`nContent-Type: application/x-www-form-urlencoded`r`nContent-Length: $($b.Length)`r`nConnection: close`r`n`r`n$b")
    $s.Write($by, 0, $by.Length); $s.Flush(); Start-Sleep -Milliseconds 500
    $buf = New-Object byte[] 8192; $n = $s.Read($buf, 0, $buf.Length)
    $h = [Text.Encoding]::ASCII.GetString($buf, 0, $n); $c.Close()
    @{ status = ($h -split "`r`n")[0]
       cookie = $(if ($h -match 'Set-Cookie:\s*([^;\r\n]+)') { $Matches[1] } else { '' }) }
}
function Fetch($path, $ck) {
    $c = New-Object Net.Sockets.TcpClient
    try { $c.Connect($Camera, 80) } catch { return @{ status = 'CONNECT-FAILED'; bytes = 0 } }
    $c.NoDelay = $true; $s = $c.GetStream(); $s.ReadTimeout = 25000
    $by = [Text.Encoding]::ASCII.GetBytes("GET $path HTTP/1.1`r`nHost: $Camera`r`nCookie: $ck`r`nConnection: close`r`n`r`n")
    $s.Write($by, 0, $by.Length); $s.Flush()
    $buf = New-Object byte[] 32768; $head = ''; $t = 0
    try { while ($true) { $n = $s.Read($buf, 0, $buf.Length); if ($n -le 0) { break }
            if ($t -eq 0) { $head = [Text.Encoding]::ASCII.GetString($buf, 0, [Math]::Min($n, 200)) }; $t += $n } } catch {}
    $c.Close()
    @{ status = ($head -split "`r`n")[0]; bytes = $t }
}
# Two requests down ONE socket. This is the check that cannot be a host unit
# test: http_server.cpp needs POSIX sockets and is not in the host build.
function Probe-KeepAlive($path, $ck) {
    $res = @()
    $c = New-Object Net.Sockets.TcpClient
    try { $c.Connect($Camera, 80) } catch { return @(@{ status = 'CONNECT-FAILED'; bytes = 0; conn = '' }) }
    $c.NoDelay = $true; $s = $c.GetStream(); $s.ReadTimeout = 8000
    for ($i = 1; $i -le 2; $i++) {
        try {
            $by = [Text.Encoding]::ASCII.GetBytes("GET $path HTTP/1.1`r`nHost: $Camera`r`nCookie: $ck`r`nConnection: keep-alive`r`n`r`n")
            $s.Write($by, 0, $by.Length); $s.Flush()
        } catch { $res += @{ status = 'SOCKET-CLOSED-BY-PEER'; bytes = 0; conn = '' }; break }
        $buf = New-Object byte[] 32768; $t = 0; $head = ''; $idle = 0
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt 8000) {
            if ($s.DataAvailable) {
                $n = $s.Read($buf, 0, $buf.Length); if ($n -le 0) { break }
                if ($t -eq 0) { $head = [Text.Encoding]::ASCII.GetString($buf, 0, [Math]::Min($n, 200)) }
                $t += $n; $idle = 0
            } else { Start-Sleep -Milliseconds 30; $idle += 30; if ($t -gt 0 -and $idle -gt 500) { break } }
        }
        $res += @{ status = ($head -split "`r`n")[0]; bytes = $t
                   conn = $(if ($head -match '(?im)^Connection:\s*(\S+)') { $Matches[1] } else { '(none)' }) }
        if ($t -eq 0) { break }
    }
    $c.Close()
    return $res
}

[Console]::WriteLine("=== relay regression gate against $Camera ===")

# 1 - the login page, unauthenticated
$r = Fetch '/login.html' ''
if ($r.status -match '200' -and $r.bytes -gt 1000) { Ok "GET /login.html -> $($r.status) ($($r.bytes) B)" }
else { Bad "GET /login.html -> $($r.status) ($($r.bytes) B)" }

# 2 - login
$L = Get-Session
if ($L.status -match '200' -and $L.cookie) { Ok "POST /login -> $($L.status), session issued" }
else { Bad "POST /login -> $($L.status), cookie=$($L.cookie)" }
$ck = $L.cookie

# 3 + 6 - the CGI. No Content-Length, bare-LF headers: it must answer in full
#         and is ALLOWED to close afterwards.
$r = Fetch '/cgi-bin/live.cgi' $ck
if ($r.status -match '502') { Bad "GET /cgi-bin/live.cgi -> 502 - the bare-LF CGI regression is back" }
elseif ($r.status -match '200' -and $r.bytes -gt 15000) { Ok "GET /cgi-bin/live.cgi -> 200 ($($r.bytes) B, full body)" }
else { Bad "GET /cgi-bin/live.cgi -> $($r.status) ($($r.bytes) B)" }

# 4 + 5 - a relayed static asset, twice down one socket
$k = Probe-KeepAlive '/a/main.js' $ck
if ($k.Count -ge 1 -and $k[0].status -match '200') { Ok "relayed asset req1 -> 200 ($($k[0].bytes) B, Connection: $($k[0].conn))" }
else { Bad "relayed asset req1 -> $($k[0].status)" }
if ($k.Count -ge 2 -and $k[1].status -match '200' -and $k[1].bytes -gt 0) {
    Ok "relayed asset req2 on the SAME socket -> 200 ($($k[1].bytes) B)"
} else {
    Bad "relayed asset req2 on the same socket -> $(if ($k.Count -ge 2) { $k[1].status } else { 'no second reply' })"
    Info "this is the AP1 behaviour: before the fix the second request got nothing"
}

# native path must be unaffected
$k = Probe-KeepAlive '/api/v1/state' $ck
if ($k.Count -ge 2 -and $k[1].status -match '200') { Ok "native /api/v1/state req2 on the same socket -> 200" }
else { Bad "native /api/v1/state req2 -> $(if ($k.Count -ge 2) { $k[1].status } else { 'no second reply' })" }

[Console]::WriteLine("")
if ($script:fail -eq 0) {
    [Console]::WriteLine("=== gate PASSED ===")
    [Console]::WriteLine("Not covered here, and still needs a browser: the Live page loading in full,")
    [Console]::WriteLine("no stalled client, and no rise in unanswered requests under real load.")
    exit 0
} else {
    [Console]::WriteLine("=== gate FAILED: $($script:fail) check(s) ===")
    exit 1
}
