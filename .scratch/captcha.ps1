param([string]$Url = "http://127.0.0.1:8013/captcha.html", [int]$Wait = 30)
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class P2 {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
Add-Type -AssemblyName System.Drawing
Remove-Item "D:\projects\okernel\okai.log" -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "D:\projects\okernel\build-host\okai.exe" -WorkingDirectory "D:\projects\okernel" -PassThru -ArgumentList $Url
Start-Sleep -Seconds $Wait
$p.Refresh()
"alive=$(-not $p.HasExited) title='$($p.MainWindowTitle)'"
if (!$p.HasExited) {
    $h = $p.MainWindowHandle
    $r = New-Object P2+RECT; [void][P2]::GetWindowRect($h, [ref]$r)
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [void][P2]::PrintWindow($h, $hdc, 0)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save("D:\projects\okernel\build-host\www\captcha.png", [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    Stop-Process -Id $p.Id
}
"--- console/errors:"
Get-Content "D:\projects\okernel\okai.log" | Select-String -Pattern '\[js\]|parse:|sub-res (JS|CSS)|FAILED' | Select-Object -First 40 | ForEach-Object { $_.Line }
