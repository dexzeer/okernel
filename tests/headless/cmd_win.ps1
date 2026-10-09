# Boot an ISO under Windows QEMU (WHPX by default), type shell commands, wait for
# a serial pattern and print what the kernel logged since. For kernel self tests
# that must run on real hardware virtualization (timing, SSE, x2APIC ...).
#
#   powershell -ExecutionPolicy Bypass -File tests\headless\cmd_win.ps1 -Cmd "simd" -Until "simd selftest: (ALL OK|FAILED)" [-Cpus 4] [-Iso path] [-Cpu max]
param(
    [string[]]$Cmd = @("smp test"),
    [string]$Until = "selftest: (ALL OK|FAILED)",
    [string]$Accel = "whpx",
    [string]$Vga = "std",
    [int]$Cpus = 1,
    [string]$Cpu = "max",
    [int]$Timeout = 120,
    [string]$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    [int]$Port = 45456,
    [string]$Iso = "",
    [string]$Grep = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$iso = if ($Iso) { $Iso } else { Join-Path $root "kanarchy-desktop.iso" }
$out = Join-Path $env:TEMP "okvm-win"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out ("cmd_{0}_{1}.log" -f $Accel, $Port)
Remove-Item $log -ErrorAction SilentlyContinue

$qargs = @("-accel", $Accel, "-m", "512", "-smp", "$Cpus", "-cpu", $Cpu, "-cdrom", $iso, "-boot", "d", "-vga", $Vga,
          "-device", "e1000,netdev=net0", "-netdev", "user,id=net0",
          "-serial", "file:$log", "-display", "none", "-no-reboot",
          "-monitor", "tcp:127.0.0.1:$Port,server,nowait")
$p = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden

function Send-Keys([string]$s) {
    $map = @{ ' '='spc'; '/'='slash'; '.'='dot'; ':'='shift-semicolon'; '-'='minus'; "`n"='ret'; '='='equal'; '_'='shift-minus'; '?'='shift-slash'; '&'='shift-7'; '%'='shift-5' }
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
    $w = New-Object System.IO.StreamWriter($c.GetStream())
    foreach ($ch in $s.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] } elseif ([char]::IsUpper($ch)) { "shift-" + [string]([char]::ToLower($ch)) } else { [string]$ch }
        $w.Write("sendkey $k`n"); $w.Flush(); Start-Sleep -Milliseconds 40
    }
    Start-Sleep -Milliseconds 100
    $c.Close()
}
function Read-Log {
    if (-not (Test-Path $log)) { return "" }
    $fs = [System.IO.File]::Open($log, "Open", "Read", "ReadWrite")
    try { (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
}
$rc = 1
try {
    $t = [DateTime]::Now
    while (-not (Read-Log).Contains("[mem] heap")) { if (([DateTime]::Now - $t).TotalSeconds -gt 60) { throw "no boot" }; Start-Sleep -Milliseconds 300 }
    Start-Sleep -Seconds 6
    $from = (Read-Log).Length
    foreach ($c in $Cmd) { Send-Keys ($c + "`n"); Start-Sleep -Milliseconds 800 }
    $t = [DateTime]::Now
    $found = $false
    while (([DateTime]::Now - $t).TotalSeconds -lt $Timeout) {
        if ((Read-Log).Substring($from) -match $Until) { $found = $true; break }
        Start-Sleep -Milliseconds 300
    }
    Start-Sleep -Milliseconds 500
    $text = (Read-Log).Substring($from)
    $lines = $text -split "`n" | Where-Object { $_ -match "^\[(smp|simd|prof|ISR|isr|gfx|virtio|i915|diag)\]|selftest|fault|PAGE FAULT" -or ($Grep -and $_ -match $Grep) }
    $lines | ForEach-Object { $_.TrimEnd() }
    $bad = $text -split "`n" | Where-Object { $_ -match "\[ISR\]|PAGE FAULT|kmalloc FAIL|cpu \d+ .*fault" }
    if ($bad) { "KERNEL ERRORS:"; $bad | ForEach-Object { "  " + $_.TrimEnd() } }
    if ($found -and -not $bad) { "CMD_WIN PASS"; $rc = 0 } else { "CMD_WIN FAIL (pattern found: $found)" }
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
exit $rc
