# Runs the Wi-Fi driver test against the real Realtek USB adapter. Needs an elevated shell (usbipd bind/attach).
#
# WHAT THIS DOES TO THE PC: the adapter is taken away from Windows (so Windows loses that Wi-Fi) while QEMU
# in WSL talks to it, then it is given back. If the adapter is the PC's only network, the network is gone for the duration
# (a few minutes). tools\wifi\restore_usb.ps1 undoes it by hand if this script is ever interrupted.
#
# usage: sudo powershell -File tools\wifi\run_usb_test.ps1 [-Vid 0bda] [-Pid_ c811] [-Log out.txt] [-Script test_wifi_usb.py] [-Args "..."]
param([string]$Vid = "0bda", [string]$Pid_ = "c811", [string]$Log = "$PSScriptRoot\..\..\wifi_usb_test.log",
      [string]$Script = "test_wifi_usb.py", [string]$ScriptArgs = "")
$ErrorActionPreference = "Continue"
$usbipd = "C:\Program Files\usbipd-win\usbipd.exe"
"" | Out-File -Encoding utf8 $Log
function Say($m) { $m | Tee-Object -FilePath $Log -Append }
$dev = (& $usbipd list) | Select-String -Pattern "$Vid`:$Pid_" | Select-Object -First 1
if (-not $dev) { Say "adapter $Vid`:$Pid_ not found"; exit 1 }
$busid = ($dev.ToString().Trim() -split '\s+')[0]
Say "adapter at bus id $busid"
try {
    & $usbipd bind --busid $busid --force 2>&1 | ForEach-Object { Say $_ }
    & wsl -u root -e bash -lc "modprobe vhci-hcd 2>&1; true" 2>&1 | ForEach-Object { Say $_ }
    & $usbipd attach --wsl --busid $busid 2>&1 | ForEach-Object { Say $_ }
    Start-Sleep -Seconds 5
    $cmd = "cd /mnt/d/projects/okernel && (lsusb 2>&1 | grep -i $Vid; true) && OKVM_ACCEL=kvm OKVM_EXTRA='-device qemu-xhci,id=xhci -device usb-host,bus=xhci.0,vendorid=0x$Vid,productid=0x$Pid_' python3 tests/headless/$Script $ScriptArgs"
    & wsl -u root -e bash -lc $cmd 2>&1 | ForEach-Object { Say $_ }
} finally {
    & $usbipd detach --busid $busid 2>&1 | ForEach-Object { Say $_ }
    & $usbipd unbind --busid $busid 2>&1 | ForEach-Object { Say $_ }
    Say "adapter given back to Windows"
}
