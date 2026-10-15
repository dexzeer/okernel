# YouTube search end to end under WHPX: open the home page, click the search box with the (relative) mouse, type a query,
# press Enter, and screendump the results. The pointer is homed into the top-left corner first (moves clamp there), then moved
# by exact pixel counts (1:1), so the click lands where the page's search box is.
#   powershell -ExecutionPolicy Bypass -File tests\headless\yt_search_win.ps1 [-Query linux] [-Port 45720] [-Shot out.ppm]
param([string]$Query = "linux", [int]$Port = 45720, [string]$Shot = "", [int]$BoxX = 900, [int]$BoxY = 126, [int]$Mem = 1536)
$ErrorActionPreference = "Stop"
$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$iso = Join-Path $root "kanarchy-desktop.iso"
$out = Join-Path $env:TEMP "okvm-win"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out "yts_$Port.log"
Remove-Item $log -ErrorAction SilentlyContinue
$qargs = @("-accel", "whpx", "-m", "$Mem", "-cdrom", $iso, "-boot", "d", "-vga", "std",
          "-device", "e1000,netdev=net0", "-netdev", "user,id=net0",
          "-serial", "file:$log", "-display", "none", "-no-reboot",
          "-monitor", "tcp:127.0.0.1:$Port,server,nowait")
$p = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden
$script:w = $null
function Open { $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port); $script:w = New-Object System.IO.StreamWriter($c.GetStream()) }
function M([string]$m, [int]$ms = 20) { $script:w.Write("$m`n"); $script:w.Flush(); Start-Sleep -Milliseconds $ms }
function Read-Log {
    if (-not (Test-Path $log)) { return "" }
    $fs = [System.IO.File]::Open($log, "Open", "Read", "ReadWrite")
    try { (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
}
function Wait-For([string]$pat, [int]$sec) {
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt $sec) { if ((Read-Log).Contains($pat)) { return $true }; Start-Sleep -Milliseconds 300 }
    return $false
}
function Send-Text([string]$s) {
    foreach ($ch in $s.ToCharArray()) {
        $k = if ($ch -eq ' ') { 'spc' } elseif ($ch -eq '.') { 'dot' } elseif ($ch -eq '/') { 'slash' } elseif ($ch -eq ':') { 'shift-semicolon' } elseif ($ch -eq '-') { 'minus' } else { [string]$ch }
        M "sendkey $k" 60
    }
}
try {
    if (-not (Wait-For "[mem] heap" 60)) { throw "no boot" }
    Start-Sleep -Seconds 4
    Open
    Send-Text "okai https://www.youtube.com/"; M "sendkey ret" 60
    if (-not (Wait-For "parse: count=" 90)) { throw "no page" }
    # let the app come up: the masthead icons are the sign that its scripts ran
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt 60) { if ((Read-Log) -match "received \d+ bytes \(fonts\.gstatic\.com/s/i/") { break }; Start-Sleep -Milliseconds 500 }
    Start-Sleep -Seconds 8
    M "screendump $out\yts_${Port}_home.ppm" 1500
    # home the pointer, then go to the search box
    for ($i = 0; $i -lt 30; $i++) { M "mouse_move -100 -100" 8 }
    for ($x = 0; $x -lt $BoxX; $x += 50) { M "mouse_move 50 0" 8 }
    for ($y = 0; $y -lt $BoxY; $y += 50) { M ("mouse_move 0 " + [Math]::Min(50, $BoxY - $y)) 8 }
    Start-Sleep -Milliseconds 500
    M "mouse_button 1" 120; M "mouse_button 0" 300
    Send-Text $Query
    Start-Sleep -Milliseconds 800
    M "screendump $out\yts_${Port}_typed.ppm" 1500
    M "sendkey ret" 100
    $n0 = ([regex]::Matches((Read-Log), "youtubei/v1/search")).Count
    $ok = $false
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt 60) { if ((Read-Log).Contains("results?search_query") -or ([regex]::Matches((Read-Log), "youtubei/v1/search")).Count -gt $n0) { $ok = $true; break }; Start-Sleep -Milliseconds 500 }
    "search request seen: $ok"
    Start-Sleep -Seconds 25
    $dst = if ($Shot) { $Shot } else { "$out\yts_${Port}_results.ppm" }
    M "screendump $dst" 1500
    "screenshots: $out\yts_${Port}_{home,typed,results}.ppm"
} finally {
    if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
$log2 = Read-Log
$final = [regex]::Matches($log2, "\[okai\] navigate [^\r\n]*results\?search_query=" + [regex]::Escape($Query)).Count
if ($ok -and ($final -gt 0 -or $log2.Contains("results?search_query=$Query"))) { "RESULT PASS" } else { "RESULT FAIL (no search request/navigation seen)" }
