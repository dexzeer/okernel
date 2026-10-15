# Open a URL in okai under WHPX and wait until the serial log contains a pattern (full-length playback checks).
#   powershell -ExecutionPolicy Bypass -File tests\headless\wait_log_win.ps1 -Url http://10.0.2.2:8005/bbb.html -Pattern "BBB ENDED" -Timeout 900
param([string]$Url, [string]$Pattern, [int]$Timeout = 600, [int]$Port = 45740, [int]$Mem = 1536, [string]$Shot = "")
$ErrorActionPreference = "Stop"
$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$out = Join-Path $env:TEMP "okvm-win"; New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out "wait_$Port.log"; Remove-Item $log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $Qemu -PassThru -WindowStyle Hidden -ArgumentList @("-accel", "whpx", "-m", "$Mem", "-cdrom", (Join-Path $root "kanarchy-desktop.iso"), "-boot", "d", "-vga", "std",
    "-device", "e1000,netdev=net0", "-netdev", "user,id=net0", "-serial", "file:$log", "-display", "none", "-no-reboot", "-monitor", "tcp:127.0.0.1:$Port,server,nowait")
function Read-Log { if (-not (Test-Path $log)) { return "" }; $fs = [System.IO.File]::Open($log, "Open", "Read", "ReadWrite"); try { (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() } }
function M([string]$m, [int]$ms = 40) { $script:w.Write("$m`n"); $script:w.Flush(); Start-Sleep -Milliseconds $ms }
try {
    $t = [DateTime]::Now; while (-not (Read-Log).Contains("[mem] heap") -and ([DateTime]::Now - $t).TotalSeconds -lt 60) { Start-Sleep -Milliseconds 300 }
    Start-Sleep -Seconds 4
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port); $script:w = New-Object System.IO.StreamWriter($c.GetStream())
    $map = @{ ' '='spc'; '/'='slash'; '.'='dot'; ':'='shift-semicolon'; '-'='minus'; '='='equal'; '_'='shift-minus'; '?'='shift-slash'; '&'='shift-7'; '%'='shift-5' }
    foreach ($ch in ("okai $Url").ToCharArray()) { $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } elseif ([char]::IsUpper($ch)) { "shift-" + [string]([char]::ToLower($ch)) } else { [string]$ch }; M "sendkey $k" 60 }
    M "sendkey ret" 60
    $t = [DateTime]::Now; $hit = $false
    while (([DateTime]::Now - $t).TotalSeconds -lt $Timeout) { if ((Read-Log).Contains($Pattern)) { $hit = $true; break }; Start-Sleep -Seconds 2 }
    "pattern seen: $hit after {0:N0}s" -f ([DateTime]::Now - $t).TotalSeconds
    if ($Shot) { M "screendump $Shot" 1500 }
    $l = Read-Log
    "faults: " + ([regex]::Matches($l, "PAGE FAULT|kmalloc FAIL|EXCEPTION")).Count
    $l -split "`n" | Where-Object { $_ -match "BBB" } | Select-Object -Last 3
} finally { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
