# KAnarchy Wheels under Windows QEMU (WHPX): how fast does the game really run, and does a held key work?
#   powershell -ExecutionPolicy Bypass -File tests\headless\game_win.ps1 [-Level 1] [-Seconds 12] [-Accel whpx] [-Shot out.ppm]
# Prints the game's own cost lines ("[game] N updates at X us, N pictures at Y us") and the main loop's [fps] lines.
param(
    [int]$Level = 1,
    [int]$Seconds = 12,
    [string]$Accel = "whpx",
    [string]$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    [int]$Port = 45460,
    [string[]]$Extra = @(),
    [string]$PreCmd = "",
    [string]$Shot = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$iso = Join-Path $root "kanarchy-desktop.iso"
$out = Join-Path $env:TEMP "okvm-win"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out "game_$Accel.log"
Remove-Item $log -ErrorAction SilentlyContinue
$qargs = @("-accel", $Accel, "-m", "512", "-cdrom", $iso, "-boot", "d", "-vga", "std", "-serial", "file:$log", "-display", "none", "-no-reboot",
           "-monitor", "tcp:127.0.0.1:$Port,server,nowait")
if ($Extra) { $qargs += $Extra }
$p = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden

function Mon([string]$cmd) {
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
    $w = New-Object System.IO.StreamWriter($c.GetStream())
    $w.Write("$cmd`n"); $w.Flush(); Start-Sleep -Milliseconds 150
    $c.Close()
}
function Send-Keys([string]$s) {
    $map = @{ ' '='spc'; "`n"='ret' }
    foreach ($ch in $s.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } else { [string]$ch }
        Mon "sendkey $k"; Start-Sleep -Milliseconds 40
    }
}
function Read-Log {
    if (-not (Test-Path $log)) { return "" }
    $fs = [System.IO.File]::Open($log, "Open", "Read", "ReadWrite")
    try { (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
}
function Wait-For([string]$pat, [int]$sec) {
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt $sec) { if ((Read-Log).Contains($pat)) { return $true }; Start-Sleep -Milliseconds 200 }
    return $false
}
try {
    if (-not (Wait-For "[mem] heap" 90)) { throw "no boot" }
    Start-Sleep -Seconds 4
    if ($PreCmd) { Send-Keys ($PreCmd + "`n"); Start-Sleep -Seconds 1 }
    Send-Keys "game $Level`n"
    if (-not (Wait-For "[game] started" 30)) { throw "the game did not start" }
    Start-Sleep -Seconds 2
    Mon "sendkey up $($Seconds * 1000)"          # hold UP for the whole run
    Start-Sleep -Seconds $Seconds
    if ($Shot) { Mon "screendump $Shot"; Start-Sleep -Seconds 2 }
    (Read-Log) -split "`n" | Where-Object { $_ -match "^\[game\]|^\[fps\]|\[dmccl\]" } | Select-Object -Last 14
    $bad = (Read-Log) -split "`n" | Where-Object { $_ -match "PAGE FAULT|\[ISR\] Exception|kmalloc FAIL" } | Select-Object -First 3
    "faults: $bad"
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
