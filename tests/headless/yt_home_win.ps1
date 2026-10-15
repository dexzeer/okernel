# YouTube home page under WHPX (the Windows twin of test_okai_yt_home.py): loads https://www.youtube.com/ with perf_win.ps1
# and checks the serial log. Needs network access.
#   powershell -ExecutionPolicy Bypass -File tests\headless\yt_home_win.ps1 [-Port 45700] [-Shot out.ppm]
param([int]$Port = 45700, [string]$Shot = "")
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$args2 = @("-ExecutionPolicy", "Bypass", "-File", (Join-Path $here "perf_win.ps1"), "-Accel", "whpx", "-Mem", "1536", "-Port", "$Port",
           "-Settle", "20", "-Urls", "https://www.youtube.com/")
if ($Shot) { $args2 += @("-Shot", $Shot) }
powershell @args2
$log = Get-Content -Raw (Join-Path $env:TEMP "okvm-win\perf_whpx_$Port.log")
$reloads = ([regex]::Matches($log, "\[okai\] frame \d+: src ->")).Count
$asked = ([regex]::Matches($log, "queued HTTPS fonts\.gstatic\.com/s/i/")).Count
$got = ([regex]::Matches($log, "received \d+ bytes \(fonts\.gstatic\.com/s/i/")).Count
$fails = @()
if ($reloads -gt 2) { $fails += "sign-in iframe reloaded $reloads times" }
if ($asked -eq 0 -or $got -lt $asked) { $fails += "icons: $asked asked, $got answered" }
if ($log -match "PAGE FAULT|kmalloc FAIL") { $fails += "fault in log" }
"iframe reloads: $reloads  icon requests: $asked  icon answers: $got"
if ($fails.Count) { "RESULT FAIL " + ($fails -join "; "); exit 1 } else { "RESULT PASS" }
