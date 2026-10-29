# win32/test-gui.ps1 -- drives the Windows port the way a user does: real
# WM_LBUTTONDOWN / WM_MOUSEWHEEL / WM_CHAR messages to the window, with every
# expectation checked against okai.log (the port's serial log).
#
#   powershell -ExecutionPolicy Bypass -File win32\test-gui.ps1 [-Exe path] [-KeepOpen]
#
# Exit code 0 = all checks passed. Geometry comes from okai's own hit-test log
# lines ("[okai] ui back=...", "[okai] link[..]"), never from hard-coded pixels.

param(
    [string]$Exe = "$PSScriptRoot\..\build-host\okai.exe",
    [string]$Log = "$PSScriptRoot\..\okai.log",
    [switch]$KeepOpen
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class OKW {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

$script:fail = 0
function Check([string]$name, [bool]$ok, [string]$detail = "") {
    if ($ok) { Write-Host ("PASS  " + $name) -ForegroundColor Green }
    else { Write-Host ("FAIL  " + $name + $(if ($detail) { "  -- $detail" } else { "" })) -ForegroundColor Red; $script:fail++ }
}

function Log-Lines {
    if (!(Test-Path $Log)) { return @() }
    return @(Get-Content $Log -Encoding UTF8 -ErrorAction SilentlyContinue)
}

# First line at or after $from that matches $pattern, or $null on timeout.
function Wait-Line([string]$pattern, [int]$from, [int]$timeoutSec = 30) {
    $deadline = (Get-Date).AddSeconds($timeoutSec)
    while ($true) {
        $all = Log-Lines
        for ($i = $from; $i -lt $all.Count; $i++) {
            if ($all[$i] -match $pattern) { return [pscustomobject]@{ Index = $i; Line = $all[$i] } }
        }
        if ((Get-Date) -ge $deadline) { return $null }
        Start-Sleep -Milliseconds 200
    }
}

function Post-Char([IntPtr]$h, [string]$text) {
    foreach ($ch in $text.ToCharArray()) {
        [void][OKW]::PostMessage($h, 0x0102, [IntPtr][int][char]$ch, [IntPtr]0)
        Start-Sleep -Milliseconds 15
    }
}

function Click([IntPtr]$h, [int]$x, [int]$y) {
    $lp = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
    [void][OKW]::PostMessage($h, 0x0200, [IntPtr]0, $lp)     # WM_MOUSEMOVE
    Start-Sleep -Milliseconds 20
    [void][OKW]::PostMessage($h, 0x0201, [IntPtr]1, $lp)     # WM_LBUTTONDOWN
    Start-Sleep -Milliseconds 40
    [void][OKW]::PostMessage($h, 0x0202, [IntPtr]0, $lp)     # WM_LBUTTONUP
    Start-Sleep -Milliseconds 150
}

# $notches > 0 = wheel away from the user = scroll up (Windows delta sign).
function WheelAt([IntPtr]$h, [int]$x, [int]$y, [int]$notches) {
    $pt = New-Object OKW+POINT
    $pt.X = $x; $pt.Y = $y
    [void][OKW]::ClientToScreen($h, [ref]$pt)
    $lp = [IntPtr](($pt.Y -shl 16) -bor ($pt.X -band 0xFFFF))
    $d = 120 * $notches
    $wp = [IntPtr]((($d -shl 16) -band 0xFFFFFFFF) -as [int])
    [void][OKW]::PostMessage($h, 0x020A, $wp, $lp)           # WM_MOUSEWHEEL
    Start-Sleep -Milliseconds 200
}

# Center of "<field>=x,y,w,h" in one of okai's ui layout lines.
function Rect-Center([string]$line, [string]$field) {
    if ($line -match "$field=(\d+),(\d+),(\d+),(\d+)") {
        $x = [int]$Matches[1]; $y = [int]$Matches[2]; $w = [int]$Matches[3]; $hh = [int]$Matches[4]
        return @([int]($x + $w / 2), [int]($y + $hh / 2))
    }
    return $null
}

# The newest chrome layout line: tab widths animate, so the '+' box moves.
function Last-Ui {
    $all = Log-Lines
    for ($i = $all.Count - 1; $i -ge 0; $i--) {
        if ($all[$i] -match '\[okai\] ui back=') { return $all[$i] }
    }
    return ""
}

# ---------------------------------------------------------------- run
if (!(Test-Path $Exe)) { Write-Host "missing $Exe (run: make win)" -ForegroundColor Red; exit 2 }

# The port is single-instance on purpose (two browsers on top of each other are
# indistinguishable). A leftover instance would make this test measure the
# refusal message box instead of the browser.
$stale = @(Get-Process okai -ErrorAction SilentlyContinue)
if ($stale.Count -gt 0) {
    Write-Host "another okai.exe is running (pid $($stale[0].Id)) - close it first" -ForegroundColor Red
    exit 2
}
Remove-Item $Log -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path $Log) -PassThru
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 60 -and $h -eq [IntPtr]::Zero; $i++) {
    Start-Sleep -Milliseconds 250
    if ($p.HasExited) { break }
    $p.Refresh()
    $h = $p.MainWindowHandle
}
if ($h -eq [IntPtr]::Zero) { Write-Host "no window (exited=$($p.HasExited) code=$($p.ExitCode))" -ForegroundColor Red; exit 2 }

$rect = New-Object OKW+RECT
[void][OKW]::GetWindowRect($h, [ref]$rect)
$clientW = $rect.R - $rect.L
Write-Host "window ${clientW}x$($rect.B - $rect.T) hwnd=$h"

# 1. okai:home rendered, chrome hit-test geometry published
$r = Wait-Line '\[okai\] ui back=' 0 45
Check "start page rendered, chrome drawn" ($r -ne $null)
if (!$r) { if (!$KeepOpen) { Stop-Process -Id $p.Id }; exit 1 }
$ui = $r.Line

# 2. click a link tile on okai:home (page coords + the 98px chrome band)
$r = Wait-Line '\[okai\] link\[\d+\].*href=https?://example\.com' 0 15
Check "link rectangles logged" ($r -ne $null)
$linkOk = $false
if ($r -and $r.Line -match 'x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)') {
    $lx = [int]$Matches[1] + [int]$Matches[3] / 2
    $ly = [int]$Matches[2] + [int]$Matches[4] / 2 + 98
    $mark = (Log-Lines).Count
    Click $h $lx $ly
    $hit = Wait-Line '\[okai\] LINK HIT ->' $mark 15
    Check "link click is routed" ($hit -ne $null)
    $nav = Wait-Line '\[okai\] navigate http' $mark 25
    Check "link navigates to example.com" ($nav -ne $null -and $nav.Line -match 'example\.com')
    $linkOk = ($hit -ne $null -and $nav -ne $null)
} else { Check "link rectangles parsed" $false }

# 3. Back button (geometry from the ui line)
if ($linkOk) {
    $mark = (Log-Lines).Count
    $c = Rect-Center $ui "back"
    if ($c) {
        Click $h $c[0] $c[1]
        $nav = Wait-Line '\[okai\] navigate okai:home' $mark 25
        Check "back button returns to the start page" ($nav -ne $null)
    } else { Check "back button geometry" $false }
} else { Check "back button returns to the start page" $false "link step failed" }

# 4. New tab '+' in the tab strip (fresh geometry: tab widths animate)
Start-Sleep -Milliseconds 400
$mark = (Log-Lines).Count
$c = Rect-Center (Last-Ui) "newtab"
if ($c) {
    Click $h $c[0] $c[1]
    $hit = Wait-Line '\[okai\] nav action=5' $mark 15
    $nav = Wait-Line '\[okai\] navigate okai:home' $mark 20
    Check "new tab (+ opens okai:home)" ($hit -ne $null -and $nav -ne $null) $(if (!$hit) { "clicked $($c[0]),$($c[1])" } else { "" })
} else { Check "new tab geometry" $false }

# 5. Wheel: away from the user = up = dy<0, toward = down = dy>0
$mark = (Log-Lines).Count
WheelAt $h ([int]($clientW / 2)) 400 1
Check "wheel up reports dy=-1" ((Wait-Line '\[okai\] wheel dy=-1' $mark 8) -ne $null)
$mark = (Log-Lines).Count
WheelAt $h ([int]($clientW / 2)) 400 -1
Check "wheel down reports dy=1" ((Wait-Line '\[okai\] wheel dy=1' $mark 8) -ne $null)

# 6. Address bar: click it, type with a trailing typo, backspace it, Enter
$c = Rect-Center (Last-Ui) "url"
if ($c) {
    $mark = (Log-Lines).Count
    Click $h $c[0] $c[1]
    Check "address bar takes focus on click" ((Wait-Line '\[okai\] address bar focused' $mark 10) -ne $null)
    Post-Char $h "okai:homes"                                   # 's' typed by mistake
    Post-Char $h ([string][char]8)                              # backspace
    Post-Char $h ([string][char]13)                             # Enter
    $nav = Wait-Line '\[okai\] navigate okai:home' $mark 25
    Check "typed URL (after a backspace) navigates" ($nav -ne $null)
} else { Check "address bar geometry" $false }

# 7. still alive, responding, no crash record
Start-Sleep -Seconds 1
$p.Refresh()
Check "process alive after input" (-not $p.HasExited)
Check "no crash in the log" (((Log-Lines | Select-String -Pattern 'CRASH' -SimpleMatch).Count) -eq 0)

if (!$KeepOpen) { Stop-Process -Id $p.Id -ErrorAction SilentlyContinue }
Write-Host ""
if ($script:fail -eq 0) { Write-Host "ALL PASS" -ForegroundColor Green; exit 0 }
Write-Host "$($script:fail) check(s) failed" -ForegroundColor Red
exit 1
