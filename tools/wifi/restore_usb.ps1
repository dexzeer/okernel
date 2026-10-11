# Gives the Realtek Wi-Fi adapter back to Windows if run_usb_test.ps1 was interrupted. Run from an elevated shell:
#   sudo powershell -File tools\wifi\restore_usb.ps1
$usbipd = "C:\Program Files\usbipd-win\usbipd.exe"
& $usbipd list
foreach ($line in (& $usbipd list)) {
    if ($line -match '^(\d+-\d+)\s+0bda:c811\s') {
        & $usbipd detach --busid $Matches[1]
        & $usbipd unbind --busid $Matches[1]
        "released $($Matches[1])"
    }
}
