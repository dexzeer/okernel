# Hover-artifact repro under Windows QEMU/WHPX (real-mouse timing; TCG in
# okvm rarely hits the race). Opens okai, sweeps the cursor across the
# taskbar buttons and the caption buttons with small moves, and screendumps
# mid-hover and after leaving to %TEMP%\okvm-win\hov_<Tag>_{tbmid,tb,capmid,cap}.ppm.
# A stale (half-drawn) hover highlight in the *_cap / *_tb dumps = regression.
#   powershell -ExecutionPolicy Bypass -File tests\headless\hover_win.ps1 [-Iso path] [-Tag name]
param([string]$Iso = "", [string]$Tag = "new", [int]$Port = 45462)
$ErrorActionPreference = "Stop"
$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Iso) { $Iso = Join-Path $root "kanarchy-desktop.iso" }
$out = Join-Path $env:TEMP "okvm-win"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out "hov_$Tag.log"
Remove-Item $log -ErrorAction SilentlyContinue
$qargs = @("-accel", "whpx", "-m", "512", "-cdrom", $Iso, "-boot", "d", "-vga", "std",
          "-device", "e1000,netdev=net0", "-netdev", "user,id=net0",
          "-serial", "file:$log", "-display", "none", "-no-reboot",
          "-monitor", "tcp:127.0.0.1:$Port,server,nowait")
$p = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden
$script:c = $null; $script:w = $null
function Open { $script:c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port); $script:w = New-Object System.IO.StreamWriter($script:c.GetStream()) }
function M([string]$m, [int]$ms = 8) { $script:w.Write("$m`n"); $script:w.Flush(); Start-Sleep -Milliseconds $ms }
function Read-Log {
    if (-not (Test-Path $log)) { return "" }
    $fs = [System.IO.File]::Open($log, "Open", "Read", "ReadWrite")
    try { (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
}
try {
    $t = [DateTime]::Now
    while (-not (Read-Log).Contains("[mem] heap") -and ([DateTime]::Now - $t).TotalSeconds -lt 60) { Start-Sleep -Milliseconds 300 }
    Start-Sleep -Seconds 5
    Open
    foreach ($ch in "okai".ToCharArray()) { M "sendkey $ch" 60 }
    M "sendkey ret" 60
    $t = [DateTime]::Now
    while (-not (Read-Log).Contains("home rendered") -and ([DateTime]::Now - $t).TotalSeconds -lt 30) { Start-Sleep -Milliseconds 300 }
    Start-Sleep -Seconds 2
    # bottom-left corner (clamped), then up into the taskbar row
    for ($i = 0; $i -lt 40; $i++) { M "mouse_move -60 60" 10 }
    for ($i = 0; $i -lt 6; $i++) { M "mouse_move 0 -1" 15 }
    # sweep right across the window buttons and back, with small vertical wobble
    for ($r = 0; $r -lt 3; $r++) {
        for ($i = 0; $i -lt 80; $i++) { M ("mouse_move 2 " + ((($i % 4) - 1) * 1)) 10 }
        for ($i = 0; $i -lt 80; $i++) { M ("mouse_move -2 " + ((($i % 4) - 1) * -1)) 10 }
        if ($r -eq 0) { for ($i = 0; $i -lt 30; $i++) { M ("mouse_move 2 " + ((($i % 4) - 1) * 1)) 10 }; M "screendump $out\hov_${Tag}_tbmid.ppm" 1500; for ($i = 0; $i -lt 30; $i++) { M "mouse_move -2 0" 10 } }
    }
    for ($i = 0; $i -lt 20; $i++) { M "mouse_move 0 -6" 10 }   # leave upward
    Start-Sleep -Seconds 2
    M "screendump $out\hov_${Tag}_tb.ppm" 1500
    # top-right corner, then onto the caption buttons and sweep left/right
    for ($i = 0; $i -lt 40; $i++) { M "mouse_move 60 -60" 10 }
    for ($i = 0; $i -lt 5; $i++) { M "mouse_move -1 1" 15 }
    for ($r = 0; $r -lt 3; $r++) {
        for ($i = 0; $i -lt 50; $i++) { M ("mouse_move -1 " + ((($i % 4) - 1) * 1)) 10 }
        for ($i = 0; $i -lt 50; $i++) { M ("mouse_move 1 " + ((($i % 4) - 1) * -1)) 10 }
        if ($r -eq 0) { for ($i = 0; $i -lt 8; $i++) { M "mouse_move -1 0" 10 }; M "screendump $out\hov_${Tag}_capmid.ppm" 1500; for ($i = 0; $i -lt 8; $i++) { M "mouse_move 1 0" 10 } }
    }
    for ($i = 0; $i -lt 20; $i++) { M "mouse_move -3 6" 10 }   # leave down-left onto the page
    Start-Sleep -Seconds 2
    M "screendump $out\hov_${Tag}_cap.ppm" 1500
    $script:c.Close()
} finally {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
}
"done $Tag"
