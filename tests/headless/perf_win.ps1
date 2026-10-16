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
    [string]$Iso = "",
    [int]$Mem = 512,
    [string]$Shot = "",    # screendump (PPM) of the last page once it settled
    [string]$AudioWav = "", # record what the guest plays (HD Audio) into this WAV
    [string[]]$Extra = @(),  # more QEMU arguments, e.g. -Extra "-icount","shift=0,sleep=off" (a guest of ~1 GIPS: a weak laptop)
    [int]$Slow = 1           # wait-time multiplier for slow guests (-icount)
    ,[int]$Smp = 1             # guest CPUs (-smp N); the video decode worker needs at least 2
    ,[string]$PreCmd = ""      # typed into the terminal before okai starts, e.g. "cpu slow 6"
    ,[string]$ShotAt = ""    # extra screendumps this many seconds after the navigation, e.g. "8 14 22" (needs -Shot: files <Shot>.<N>s.ppm)
    ,[string]$ClickAt = ""   # a mouse click on screen: "X,Y@SEC" = click (X,Y) SEC seconds after the navigation (the pointer is first pushed into the top-left corner)
    ,[string]$Display = "none"   # "sdl"/"gtk": a real QEMU window like run-windows.bat (its display refresh and audio mixing share QEMU's main thread: the stutter the headless runs do not show)
    ,[switch]$AudioSdl         # sound through QEMU's SDL backend (as run-windows.bat), not a WAV file
    ,[switch]$Prof            # turn the kernel profiler on (`prof on`): [prof] zone lines once a second
)
$ErrorActionPreference = "Stop"
$ShotTimes = @($ShotAt -split '[ ,;]+' | Where-Object { $_ } | ForEach-Object { [double]$_ })
$ClickX = 0; $ClickY = 0; $ClickT = -1
if ($ClickAt -match '^(\d+),(\d+)@([\d.]+)$') { $ClickX = [int]$Matches[1]; $ClickY = [int]$Matches[2]; $ClickT = [double]$Matches[3] }
$Urls = @($Urls | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$iso = if ($Iso) { $Iso } else { Join-Path $root "kanarchy-desktop.iso" }
$out = Join-Path $env:TEMP "okvm-win"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out ("perf_{0}_{1}.log" -f $Accel, $Port)
Remove-Item $log -ErrorAction SilentlyContinue

$qargs = @("-accel", $Accel, "-m", "$Mem", "-cdrom", $iso, "-boot", "d", "-vga", "std",
          "-device", "e1000,netdev=net0", "-netdev", "user,id=net0",
          "-serial", "file:$log", "-display", $Display, "-no-reboot",
          "-monitor", "tcp:127.0.0.1:$Port,server,nowait")
if ($Smp -gt 1) { $qargs += @("-smp", "$Smp") }
if ($Extra) { $qargs += $Extra }
if ($AudioSdl) { $qargs += @("-audiodev", "sdl,id=a0", "-device", "intel-hda", "-device", "hda-output,audiodev=a0") }
if ($AudioWav) { $qargs += @("-audiodev", "wav,id=a0,path=$AudioWav", "-device", "intel-hda", "-device", "hda-output,audiodev=a0") }
$p = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru -WindowStyle $(if ($Display -eq 'none') { 'Hidden' } else { 'Normal' })

function Send-Keys([string]$s) {
    $map = @{ ' '='spc'; '/'='slash'; '.'='dot'; ':'='shift-semicolon'; '-'='minus'; "`n"='ret';
              '='='equal'; '_'='shift-minus'; '?'='shift-slash'; '&'='shift-7'; '%'='shift-5'; ([string][char]27)='esc' }   # ESC: "`e" is only an escape in PowerShell 7 (5.1 reads a plain e)
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
function Take-Shot([string]$path) {
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
    $w = New-Object System.IO.StreamWriter($c.GetStream())
    $w.Write("screendump $path`n"); $w.Flush(); Start-Sleep -Seconds 2; $c.Close()
}
function Hmp([string[]]$cmds) {
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
    $w = New-Object System.IO.StreamWriter($c.GetStream())
    foreach ($m in $cmds) { $w.Write("$m`n"); $w.Flush(); Start-Sleep -Milliseconds 25 }
    Start-Sleep -Milliseconds 200; $c.Close()
}
function Click-Screen([int]$x, [int]$y) {
    $cmds = @()
    for ($i = 0; $i -lt 30; $i++) { $cmds += "mouse_move -100 -100" }     # corner (0,0)
    $rx = $x; $ry = $y
    while ($rx -gt 0 -or $ry -gt 0) { $dx = [Math]::Min(100, $rx); $dy = [Math]::Min(100, $ry); $cmds += "mouse_move $dx $dy"; $rx -= $dx; $ry -= $dy }
    $cmds += "mouse_button 1"; Hmp $cmds; Start-Sleep -Milliseconds 150; Hmp @("mouse_button 0")
}
function Wait-For([string]$pat, [int]$sec) {
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt $sec) { if ((Read-Log).Contains($pat)) { return $true }; Start-Sleep -Milliseconds 200 }
    return $false
}

try {
    $t0 = [DateTime]::Now
    if (-not (Wait-For "[mem] heap" (60 * $Slow))) { throw "no boot" }
    "boot to heap: {0:N1}s" -f ([DateTime]::Now - $t0).TotalSeconds
    Start-Sleep -Seconds 4
    if ($Prof) { Send-Keys "prof on`n"; Start-Sleep -Seconds 1 }
    if ($PreCmd) { Send-Keys ($PreCmd + "`n"); Start-Sleep -Seconds 1 }
    Send-Keys "okai`n"
    if (-not (Wait-For "home rendered" (60 * $Slow))) { throw "okai did not open" }
    Start-Sleep -Seconds 2
    # "settled" = no network activity (live pages keep re-rendering forever)
    $act = [regex]'\[okai\] (fetch|sub-res)|\[tls-net\]|\[http\]|parse: count='
    foreach ($u in $Urls) {
        $pos = (Read-Log).Length
        Send-Keys ([string][char]27); Start-Sleep -Milliseconds 200   # Esc: leave any focused page field
        Send-Keys "g"; Start-Sleep -Milliseconds 400
        Send-Keys ($u + "`n")
        $nav = $null; $doc = $null; $rend = $null; $last = [DateTime]::Now; $buf = ""; $fetches = 0
        $start = [DateTime]::Now
        while (([DateTime]::Now - $start).TotalSeconds -lt (240 * $Slow)) {
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
            if ($nav -and $Shot -and $ShotTimes.Count -gt 0 -and ($now - $nav).TotalSeconds -ge $ShotTimes[0]) {
                Take-Shot ("{0}.{1}s.ppm" -f [IO.Path]::ChangeExtension($Shot, $null).TrimEnd('.'), $ShotTimes[0])
                $ShotTimes = @($ShotTimes | Select-Object -Skip 1)
            }
            if ($nav -and $ClickT -ge 0 -and ($now - $nav).TotalSeconds -ge $ClickT) { Click-Screen $ClickX $ClickY; $ClickT = -1; $last = [DateTime]::Now }
            if ($nav -and ($now - $last).TotalSeconds -gt $Settle) { break }
            Start-Sleep -Milliseconds 50
        }
        $f = { param($x) if ($x -and $nav) { "{0:N1}s" -f ($x - $nav).TotalSeconds } else { "-" } }
        "SUMMARY {0,-48} doc={1} first_render={2} settled={3} fetches={4}" -f $u, (& $f $doc), (& $f $rend), (& $f $last), $fetches
    }
    if ($Shot) {
        $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
        $w = New-Object System.IO.StreamWriter($c.GetStream())
        $w.Write("screendump $Shot`n"); $w.Flush(); Start-Sleep -Seconds 2; $c.Close()
    }
    $bad = (Read-Log) -split "`n" | Where-Object { $_ -match "PAGE FAULT|\[ISR\] Exception|kmalloc FAIL" } | Select-Object -First 3
    "faults: $bad"
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
