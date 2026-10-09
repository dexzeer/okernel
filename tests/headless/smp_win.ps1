# SMP under real hardware virtualization (Windows Hypervisor Platform). TCG cannot
# emulate x2APIC, so this boots the test ISO (kanarchy-x2apic.iso: boot CPU forced
# into x2APIC, SMP self test run at boot) and the normal ISO (xAPIC), with N CPUs,
# and checks the serial log.
#
#   powershell -ExecutionPolicy Bypass -File tests\headless\smp_win.ps1 [-Cpus 4]
param(
    [int]$Cpus = 4,
    [int]$Seconds = 40,
    [string]$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
)
$root = Split-Path (Split-Path $PSScriptRoot)
$ok = $true
foreach ($case in @(@{iso="kanarchy-x2apic.iso"; mode="x2APIC"}, @{iso="kanarchy-desktop.iso"; mode="xAPIC"})) {
    $log = Join-Path $env:TEMP ("smp_whpx_" + $case.mode + ".log")
    Remove-Item $log -ErrorAction SilentlyContinue
    $args = @("-accel","whpx","-m","512","-smp","$Cpus","-cpu","max","-cdrom",(Join-Path $root $case.iso),
              "-boot","d","-vga","std","-serial","file:$log","-display","none","-no-reboot")
    $p = Start-Process -FilePath $Qemu -ArgumentList $args -PassThru -WindowStyle Hidden
    Start-Sleep $Seconds
    Stop-Process $p -Force
    $text = Get-Content $log -Raw
    $mode = if ($text -match "\(x2APIC\)") { "x2APIC" } else { "xAPIC" }
    $run = if ($text -match "\[smp\] (\d+) CPUs running") { [int]$Matches[1] } else { 1 }
    $fault = $text -match "\[smp\] cpu \d+ .*fault"
    $exc = $text -match "\[ISR\]"
    $self = if ($case.iso -like "*x2apic*") { $text -match "smp selftest: ALL OK" } else { $true }
    $pass = ($mode -eq $case.mode) -and ($run -eq $Cpus) -and (-not $fault) -and (-not $exc) -and $self
    Write-Host ("{0}: mode={1} cpus={2} fault={3} exception={4} selftest={5} -> {6}" -f $case.iso,$mode,$run,$fault,$exc,$self,$(if ($pass) {"PASS"} else {"FAIL"}))
    Get-Content $log | Select-String "\[smp\] [A-F] |speedup|selftest" | ForEach-Object { "    " + $_.Line }
    if (-not $pass) { $ok = $false }
}
if ($ok) { Write-Host "SMP_WHPX PASS"; exit 0 } else { Write-Host "SMP_WHPX FAIL"; exit 1 }
