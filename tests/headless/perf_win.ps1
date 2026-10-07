# Page-load profiler for Windows QEMU (WHPX or TCG) — the PowerShell twin of
# perf_load.py, for measuring the way the kernel runs on a Windows desktop.
#
#   powershell -ExecutionPolicy Bypass -File tests\headless\perf_win.ps1 [-Accel whpx|tcg] [-Urls a,b,...]
#
# Boots kanarchy-desktop.iso with qemu-system-x86_64, opens okai, loads each
# URL through the address bar and prints when the document parsed, the
# first render, and when the page settled (no fetch/render/script line for
# -Settle seconds). Serial lines are stamped with host time as they arrive.
param(
    [string]$Accel = "whpx",
    [string[]]$Urls = @("https://en.wikipedia.org/wiki/Operating_system", "https://www.python.org/", "https://example.com/"),
    [double]$Settle = 6,
    [string]$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    [int]$Port = 45454,
    [string]$Iso = ""
)
$ErrorActionPreference = "Stop"
$Urls = @($Urls | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$iso = if ($Iso) { $Iso } else { Join-Path $root "kanarchy-desktop.iso" }
$out = Join-Path $env:TEMP "okvm-win"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out ("perf_{0}_{1}.log" -f $Accel, $Port)
Remove-Item $log -ErrorAction SilentlyContinue

$qargs = @("-accel", $Accel, "-m", "512", "-cdrom", $iso, "-boot", "d", "-vga", "std",
          "-device", "e1000,netdev=net0", "-netdev", "user,id=net0",
          "-serial", "file:$log", "-display", "none", "-no-reboot",
          "-monitor", "tcp:127.0.0.1:$Port,server,nowait")
$p = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden

function Send-Keys([string]$s) {
    $map = @{ ' '='spc'; '/'='slash'; '.'='dot'; ':'='shift-semicolon'; '-'='minus'; "`n"='ret';
              '='='equal'; '_'='shift-minus'; '?'='shift-slash'; '&'='shift-7'; '%'='shift-5'; "`e"='esc' }
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
    $w = New-Object System.IO.StreamWriter($c.GetStream())
    foreach ($ch in $s.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] }
             elseif ([char]::IsUpper($ch)) { "shift-" + [string]([char]::ToLower($ch)) }
             else { [string]$ch }
        $w.Write("sendkey $k`n"); $w.Flush(); Start-Sleep -Milliseconds 40
    }
    Start-Sleep -Milliseconds 100
    $c.Close()
}
function Read-Log {
    if (-not (Test-Path $log)) { return "" }
    # QEMU holds the file open for writing: read with shared access
    $fs = [System.IO.File]::Open($log, "Open", "Read", "ReadWrite")
    try { (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
}
function Wait-For([string]$pat, [int]$sec) {
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt $sec) { if ((Read-Log).Contains($pat)) { return $true }; Start-Sleep -Milliseconds 200 }
    return $false
}

try {
    $t0 = [DateTime]::Now
    if (-not (Wait-For "[mem] heap" 60)) { throw "no boot" }
    "boot to heap: {0:N1}s" -f ([DateTime]::Now - $t0).TotalSeconds
    Start-Sleep -Seconds 4
    Send-Keys "okai`n"
    if (-not (Wait-For "home rendered" 60)) { throw "okai did not open" }
    Start-Sleep -Seconds 2
    # "settled" = no network activity (live pages keep re-rendering forever)
    $act = [regex]'\[okai\] (fetch|sub-res)|\[tls-net\]|\[http\]|parse: count='
    foreach ($u in $Urls) {
        $pos = (Read-Log).Length
        Send-Keys "`e"; Start-Sleep -Milliseconds 200   # Esc: leave any focused page field
        Send-Keys "g"; Start-Sleep -Milliseconds 400
        Send-Keys ($u + "`n")
        $nav = $null; $doc = $null; $rend = $null; $last = [DateTime]::Now; $buf = ""; $fetches = 0
        $start = [DateTime]::Now
        while (([DateTime]::Now - $start).TotalSeconds -lt 240) {
            $s = Read-Log
            if ($s.Length -gt $pos) { $buf += $s.Substring($pos); $pos = $s.Length }
            $now = [DateTime]::Now
            $lines = $buf -split "`n"
            $buf = $lines[-1]
            foreach ($ln in $lines[0..($lines.Count - 2)]) {
                if (-not $nav -and $ln.Contains("[okai] navigate")) { $nav = $now }
                if (-not $nav) { continue }
                if (-not $doc -and $ln.Contains("parse: count=")) { $doc = $now }
                if (-not $rend -and $ln.Contains("[okai] render tab")) { $rend = $now }
                if ($ln.Contains("[tls-net] queued") -or $ln.Contains("[http] sending")) { $fetches++ }
                if ($act.IsMatch($ln)) { $last = $now }
            }
            if ($nav -and ($now - $last).TotalSeconds -gt $Settle) { break }
            Start-Sleep -Milliseconds 50
        }
        $f = { param($x) if ($x -and $nav) { "{0:N1}s" -f ($x - $nav).TotalSeconds } else { "-" } }
        "SUMMARY {0,-48} doc={1} first_render={2} settled={3} fetches={4}" -f $u, (& $f $doc), (& $f $rend), (& $f $last), $fetches
    }
    $bad = (Read-Log) -split "`n" | Where-Object { $_ -match "PAGE FAULT|\[ISR\] Exception|kmalloc FAIL" } | Select-Object -First 3
    "faults: $bad"
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
