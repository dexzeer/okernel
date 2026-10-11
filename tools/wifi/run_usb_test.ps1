# Runs the Wi-Fi driver test against the real Realtek USB adapter. Needs an elevated shell (usbipd bind/attach).
#
# WHAT THIS DOES TO THE PC: the adapter is taken away from Windows (so Windows loses that Wi-Fi) while QEMU
# in WSL talks to it, then it is given back. If the adapter is the PC's only network, the network is gone for the duration
# (about two minutes); run it with another connection (Ethernet) or when you do not need the network.
#
# usage: sudo powershell -File tools\wifi\run_usb_test.ps1 [-Vid 0bda -Pid c811] [-Log out.txt]
param([string]$Vid = "0bda", [string]$Pid_ = "c811", [string]$Log = "$PSScriptRoot\..\..\wifi_usb_test.log")
$ErrorActionPreference = "Stop"
$dev = (& usbipd list) | Select-String -Pattern "$Vid`:$Pid_" | Select-Object -First 1
if (-not $dev) { "adapter $Vid`:$Pid_ not found" | Tee-Object -FilePath $Log; exit 1 }
$busid = ($dev.ToString().Trim() -split '\s+')[0]
"adapter at bus id $busid" | Tee-Object -FilePath $Log
try {
    & usbipd bind --busid $busid --force 2>&1 | Tee-Object -FilePath $Log -Append
    & usbipd attach --wsl --busid $busid 2>&1 | Tee-Object -FilePath $Log -Append
    Start-Sleep -Seconds 4
    $cmd = "cd /mnt/d/projects/okernel && lsusb | grep -i $Vid ; OKVM_EXTRA='-device qemu-xhci,id=xhci -device usb-host,bus=xhci.0,vendorid=0x$Vid,productid=0x$Pid_' python3 tests/headless/test_wifi_usb.py"
    & wsl -u root -e bash -lc $cmd 2>&1 | Tee-Object -FilePath $Log -Append
} finally {
    & usbipd detach --busid $busid 2>&1 | Tee-Object -FilePath $Log -Append
    & usbipd unbind --busid $busid 2>&1 | Tee-Object -FilePath $Log -Append
    "adapter given back to Windows" | Tee-Object -FilePath $Log -Append
}
