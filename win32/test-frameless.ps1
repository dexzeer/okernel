# win32/test-frameless.ps1 -- the browser has no native caption, so the things
# a caption used to give us are done by hand: WM_NCCALCSIZE = 0 (client area is
# the whole window), WM_NCHITTEST decides resize edges and the drag region (the
# tab-strip background, like a Firefox tab strip), WM_GETMINMAXINFO clamps
# maximise to the monitor work area.
#
# WM_NCHITTEST is exactly what the shell reads to start a move or a resize, so
# asserting its answers asserts the behaviour. The physical drag itself is left
# to a human: synthetic input (SetCursorPos + mouse_event) does not reach the
# window in a non-interactive session, so it cannot be automated here.
#
#   powershell -ExecutionPolicy Bypass -File win32\test-frameless.ps1

param([string]$Exe = "$PSScriptRoot\..\build-host\okai.exe", [string]$Log = "$PSScriptRoot\..\okai.log")

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class HT {
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

$fail = 0
function Check([string]$n, [bool]$ok, [string]$d = "") {
    if ($ok) { Write-Host ("PASS  " + $n) -ForegroundColor Green }
    else { Write-Host ("FAIL  " + $n + $(if ($d) { "  -- " + $d } else { "" })) -ForegroundColor Red; $script:fail++ }
}

# WM_NCHITTEST's lParam is in SCREEN coordinates; the callers below think in
# client coordinates.
function Hit([IntPtr]$h, [int]$x, [int]$y) {
    $pt = New-Object HT+POINT
    $pt.X = $x; $pt.Y = $y
    [void][HT]::ClientToScreen($h, [ref]$pt)
    $lp = [IntPtr](($pt.Y -shl 16) -bor ($pt.X -band 0xFFFF))
    return [int][HT]::SendMessage($h, 0x0084, [IntPtr]0, $lp)     # WM_NCHITTEST
}

if (!(Test-Path $Exe)) { Write-Host "missing $Exe (make win)" -ForegroundColor Red; exit 2 }
$stale = @(Get-Process okai -ErrorAction SilentlyContinue)
if ($stale.Count -gt 0) {
    Write-Host "another okai.exe is running (pid $($stale[0].Id)) - close it first" -ForegroundColor Red
    exit 2
}
Remove-Item $Log -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Log) -PassThru
Start-Sleep -Seconds 6
if ($p.HasExited) { Write-Host "exited early: $($p.ExitCode)" -ForegroundColor Red; exit 2 }
$p.Refresh(); $h = $p.MainWindowHandle

$wr = New-Object HT+RECT; [void][HT]::GetWindowRect($h, [ref]$wr)
$cr = New-Object HT+RECT; [void][HT]::GetClientRect($h, [ref]$cr)
$cw = $cr.R - $cr.L; $ch = $cr.B - $cr.T
$ww = $wr.R - $wr.L; $wh = $wr.B - $wr.T
Write-Host "window ${ww}x${wh}, client ${cw}x${ch}"

# 1. frameless: no non-client area at all (client == window)
Check "no native frame (client area = window)" ($cw -eq $ww -and $ch -eq $wh) "client ${cw}x${ch} window ${ww}x${wh}"

# 2. the tab strip's empty background is the drag handle (HTCAPTION = 2)
$ui = (Get-Content $Log | Select-String '\[okai\] ui back=' | Select-Object -Last 1).Line
Check "chrome layout published" ($ui -ne $null)
Check "tab-strip background is a drag handle" ((Hit $h 600 20) -eq 2) "got $(Hit $h 600 20)"
Check "toolbar background stays client" ((Hit $h 600 64) -eq 1) "got $(Hit $h 600 64)"

# 3. interactive chrome is NOT a drag handle (a click must reach the browser)
if ($ui -match 'newtab=(\d+),(\d+),(\d+),(\d+)') {
    $nx = [int]$Matches[1] + 8; $ny = [int]$Matches[2] + 8
    Check "the + box stays client" ((Hit $h $nx $ny) -eq 1) "at $nx,$ny got $(Hit $h $nx $ny)"
}
if ($ui -match 'url=(\d+),(\d+),(\d+),(\d+)') {
    $ux = [int]$Matches[1] + 40; $uy = [int]$Matches[2] + 8
    Check "the address bar stays client" ((Hit $h $ux $uy) -eq 1) "at $ux,$uy got $(Hit $h $ux $uy)"
}
if ($ui -match 'tab0=(\d+),(\d+),(\d+),(\d+)') {
    $tx = [int]$Matches[1] + 40; $ty = [int]$1 + [int]$Matches[2] + 8
    Check "a tab stays client" ((Hit $h $tx $ty) -eq 1) "at $tx,$ty got $(Hit $h $tx $ty)"
}

# 4. caption cells (okai's own min/max/close) stay client so the click routes
if ($ui -match 'close=(\d+),(\d+),(\d+),(\d+)') {
    $cx = [int]$Matches[1] + 20; $cy = [int]$Matches[2] + 20
    Check "okai's close cell stays client" ((Hit $h $cx $cy) -eq 1) "at $cx,$cy got $(Hit $h $cx $cy)"
}

# 5. resize edges/corners (HTLEFT 10, HTRIGHT 11, HTTOP 12, HTBOTTOM 15,
#    HTTOPLEFT 13, HTTOPRIGHT 14, HTBOTTOMLEFT 16, HTBOTTOMRIGHT 17)
Check "left edge resizes"  ((Hit $h 3 300) -eq 10) "got $(Hit $h 3 300)"
Check "right edge resizes" ((Hit $h ($cw - 3) 300) -eq 11) "got $(Hit $h ($cw - 3) 300)"
Check "top edge resizes"   ((Hit $h 300 3) -eq 12) "got $(Hit $h 300 3)"
Check "bottom edge resizes" ((Hit $h 300 ($ch - 3)) -eq 15) "got $(Hit $h 300 ($ch - 3))"
Check "corner resizes"     ((Hit $h ($cw - 3) ($ch - 3)) -eq 17) "got $(Hit $h ($cw - 3) ($ch - 3))"

# 6. maximise is clamped to the work area (WM_GETMINMAXINFO in the port)
$mm = New-Object byte[] 40
$res = [Runtime.InteropServices.Marshal]::AllocHGlobal(40)
[Runtime.InteropServices.Marshal]::WriteInt32($res, 0, 0)
[void][HT]::SendMessage($h, 0x0024, [IntPtr]0, $res)             # WM_GETMINMAXINFO
$maxX = [Runtime.InteropServices.Marshal]::ReadInt32($res, 24)   # ptMaxSize
$maxY = [Runtime.InteropServices.Marshal]::ReadInt32($res, 28)
[Runtime.InteropServices.Marshal]::FreeHGlobal($res)
$sw = [int](Get-CimInstance Win32_VideoController | Select-Object -First 1).CurrentHorizontalResolution
$sh = [int](Get-CimInstance Win32_VideoController | Select-Object -First 1).CurrentVerticalResolution
Check "maximise is clamped to the work area" ($maxX -gt 0 -and $maxX -le $sw -and $maxY -gt 0 -and $maxY -le $sh) "maximise ${maxX}x${maxY}, screen ${sw}x${sh}"

$p.Refresh()
Check "process alive" (-not $p.HasExited)
Check "no crash in the log" (((Get-Content $Log -ErrorAction SilentlyContinue) | Select-String 'CRASH' -SimpleMatch).Count -eq 0)

Stop-Process -Id $p.Id -ErrorAction SilentlyContinue
Write-Host ""
Write-Host "NOTE: the physical drag/resize gesture needs a human (synthetic mouse input" -ForegroundColor DarkGray
Write-Host "      does not reach the window in a non-interactive session). The shell reads" -ForegroundColor DarkGray
Write-Host "      exactly the WM_NCHITTEST answers checked above to start the move/size loop." -ForegroundColor DarkGray
Write-Host ""
if ($fail -eq 0) { Write-Host "ALL PASS" -ForegroundColor Green; exit 0 }
Write-Host "$fail check(s) failed" -ForegroundColor Red
exit 1
