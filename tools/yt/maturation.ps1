# Visitor-maturation experiment: keep one okai session on YouTube for N hours,
# reloading the watch page through the address bar (mouse click + typing) every
# ~10 minutes, so the visitor identity accumulates server-side history.
#   powershell -File tools\yt\maturation.ps1 -Hours 3 -Port 45474
param(
    [double]$Hours = 3,
    [int]$Port = 45474,
    [string]$VideoId = "aqz-KE-bpKQ"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$iso = Join-Path $root "kanarchy-desktop.iso"
$out = Join-Path $env:TEMP "okvm-win"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out ("mat_{0}.log" -f $Port)
Remove-Item $log -ErrorAction SilentlyContinue

$qargs = @("-accel", "whpx", "-m", "1536", "-smp", "4", "-cdrom", $iso, "-boot", "d", "-vga", "std",
          "-device", "e1000,netdev=net0", "-netdev", "user,id=net0",
          "-serial", "file:$log", "-display", "none", "-no-reboot",
          "-monitor", "tcp:127.0.0.1:$Port,server,nowait")
$p = Start-Process -FilePath "C:\Program Files\qemu\qemu-system-x86_64.exe" -ArgumentList $qargs -PassThru -WindowStyle Hidden

$ready = $false
for ($i = 0; $i -lt 90 -and -not $ready; $i++) {
    Start-Sleep -Seconds 1
    if (Test-Path $log) { $head = Get-Content $log -Raw -ErrorAction SilentlyContinue; if ($head -match "\[mem\] heap") { $ready = $true } }
    if ($p.HasExited) { break }
}
if (-not $ready) { Write-Output "guest never reached the heap line"; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }

function Hmp([string[]]$cmds) {
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
    $w = New-Object System.IO.StreamWriter($c.GetStream())
    foreach ($m in $cmds) { $w.Write("$m`n"); $w.Flush(); Start-Sleep -Milliseconds 25 }
    Start-Sleep -Milliseconds 150; $c.Close()
}
function Send-Keys([string]$s) {
    $map = @{ ' '='spc'; '/'='slash'; '.'='dot'; ':'='shift-semicolon'; '-'='minus'; "`n"='ret';
              '='='equal'; '?'='shift-slash'; '&'='shift-7' }
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
    $w = New-Object System.IO.StreamWriter($c.GetStream())
    foreach ($ch in $s.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] }
             elseif ([char]::IsUpper($ch)) { "shift-" + [string]([char]::ToLower($ch)) }
             else { [string]$ch }
        $w.Write("sendkey $k`n"); $w.Flush(); Start-Sleep -Milliseconds 35
    }
    Start-Sleep -Milliseconds 120
    $c.Close()
}
function Click-Screen([int]$x, [int]$y) {
    $cmds = @()
    for ($i = 0; $i -lt 30; $i++) { $cmds += "mouse_move -100 -100" }
    $rx = $x; $ry = $y
    while ($rx -gt 0 -or $ry -gt 0) { $dx = [Math]::Min(100, $rx); $dy = [Math]::Min(100, $ry); $cmds += "mouse_move $dx $dy"; $rx -= $dx; $ry -= $dy }
    $cmds += "mouse_button 1"; Hmp $cmds; Start-Sleep -Milliseconds 150; Hmp @("mouse_button 0")
}

Start-Sleep -Seconds 10
# open the browser on the watch page from the shell (the terminal has the focus)
Send-Keys "okai https://www.youtube.com/watch?v=$VideoId`n"
Start-Sleep -Seconds 25

$deadline = (Get-Date).AddHours($Hours)
$round = 0
while ((Get-Date) -lt $deadline -and -not $p.HasExited) {
    $round++
    Start-Sleep -Seconds 480                                     # let the page sit and beacon
    if ((Get-Date) -ge $deadline) { break }
    Click-Screen 400 30                                          # okai's address bar
    Start-Sleep -Milliseconds 400
    Send-Keys "https://www.youtube.com/watch?v=$VideoId`n"
    Start-Sleep -Seconds 25
}
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Write-Output "maturation finished after $round rounds; log: $log"
