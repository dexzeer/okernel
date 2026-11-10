# Boots KAnarchy OS into Doom (shareware, via DOSBox on the lx subsystem) so you can play it.
# Usage:
#   powershell -ExecutionPolicy Bypass -File .\play-doom.ps1 [-Iso path\to.iso] [-NoSound]
#
# Prereqs in the image: /bin/dosbox, /bin/xokai, /bin/dosrun, /dos/doom/DOOM.EXE (built by tools/lx/dosbox_root.sh).
# Needs >= 1 GB guest RAM (uses 2048 MB). On Windows this uses WHPX when available (falls back to TCG).

param(
    [string]$Iso = "",
    [switch]$NoSound,
    [int]$Port = 45471
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
if (-not $Iso) {
    $candidates = @(
        (Join-Path $root "build-host\okb\build-host\lxtest.iso"),
        (Join-Path $root "build-host\lxtest.iso"),
        (Join-Path $root "kanarchy-desktop.iso")
    )
    foreach ($c in $candidates) { if (Test-Path $c) { $Iso = $c; break } }
}
if (-not $Iso -or -not (Test-Path $Iso)) {
    Write-Host "No ISO found. Pass -Iso <path> to an image that contains DOSBox+xokai+Doom."
    Write-Host "Build hint: put doom.zip in build-host\dosgame\dl\, run tools/lx/dosbox_root.sh into the lx root, rebuild the lx test image. See lx/README.md."
    exit 1
}

$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
if (-not (Test-Path $Qemu)) { Write-Host "QEMU not found at $Qemu (install QEMU or edit `$Qemu in this script)."; exit 1 }

$Out = Join-Path $env:TEMP "okvm-doom"
New-Item -ItemType Directory -Force $Out | Out-Null
$log = Join-Path $Out "doom-play.log"
Remove-Item $log -ErrorAction SilentlyContinue

$doomArgs = if ($NoSound) { "-nosound -warp 1 1" } else { "-warp 1 1" }
$qargs = @(
    "-accel", "whpx", "-accel", "tcg",
    "-smp", "2", "-m", "2048",
    "-cdrom", $Iso, "-boot", "d", "-vga", "std",
    "-device", "e1000,netdev=net0", "-netdev", "user,id=net0",
    "-serial", "file:$log",
    "-monitor", "tcp:127.0.0.1:$Port,server,nowait"
)
if (-not $NoSound) {
    # SDL/AC97 like run-windows.bat (AC'97 avoids QEMU HDA timing issues on Windows). in.voices=0 = no mic.
    $qargs += @("-audiodev", "sdl,id=snd0,out.buffer-length=120000,in.voices=0,out.frequency=48000", "-device", "AC97,audiodev=snd0")
}

Write-Host "==> Booting: $Iso"
Write-Host "    RAM 2048 MB, display ON (play in the QEMU window), serial log: $log"
$p = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru

function Hmp([string[]]$cmds, [int]$gap = 40) {
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
    $w = New-Object System.IO.StreamWriter($c.GetStream())
    foreach ($m in $cmds) { $w.Write("$m`n"); $w.Flush(); Start-Sleep -Milliseconds $gap }
    Start-Sleep -Milliseconds 120; $c.Close()
}
function Send-Keys([string]$s) {
    $map = @{ ' '='spc'; '/'='slash'; '.'='dot'; ':'='shift-semicolon'; '-'='minus'; "`n"='ret'; '='='equal'; '_'='shift-minus'; '&'='shift-7'; '"'='shift-apostrophe'; '>'='shift-dot'; '`'='grave_accent' }
    Hmp @($s.ToCharArray() | ForEach-Object {
        $k = if ($map.ContainsKey([string]$_)) { $map[[string]$_] }
             elseif ([char]::IsUpper($_)) { "shift-" + [string]([char]::ToLower($_)) }
             else { [string]$_ }
        "sendkey $k"
    })
}
function Read-Log {
    if (-not (Test-Path $log)) { return "" }
    $fs = [System.IO.File]::Open($log, "Open", "Read", "ReadWrite")
    try { (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
}
function Wait-For([string]$pat, [int]$sec) {
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt $sec) {
        if ($p.HasExited) { return $false }
        if ((Read-Log).Contains($pat)) { return $true }
        Start-Sleep -Milliseconds 300
    }
    return $false
}
function Shot([string]$name) {
    $f = Join-Path $Out "$name.ppm"
    Hmp @("screendump $f"); Start-Sleep -Milliseconds 900
    if (Test-Path $f) { return ,[System.IO.File]::ReadAllBytes($f) } else { return $null }
}
function Colours($a) {
    if ($null -eq $a -or $a.Length -lt 100) { return 0 }
    $h = @{}; $hdr = 20
    for ($y = 400; $y -lt 760; $y += 6) {
        for ($x = 660; $x -lt 1260; $x += 6) {
            $i = $hdr + ($y * 1920 + $x) * 3
            if ($i + 2 -lt $a.Length) { $h[[int]$a[$i] * 65536 + [int]$a[$i + 1] * 256 + [int]$a[$i + 2]] = 1 }
        }
    }
    return $h.Count
}

try {
    Write-Host "==> Waiting for the lx subsystem..."
    if (-not (Wait-For "[lx] Linux subsystem ready" 120)) {
        Write-Host "FAIL: desktop/lx subsystem did not come up (check $log). QEMU left running for inspection."
        exit 1
    }
    Start-Sleep -Seconds 6

    Write-Host "==> Starting xokai (X server)..."
    Send-Keys "lx /bin/xokai &`n"
    if (-not (Wait-For "xokai: display :0 ready" 45)) {
        Write-Host "FAIL: xokai did not start. Is /bin/xokai in this image? (log: $log). QEMU left running."
        exit 1
    }
    Start-Sleep -Seconds 1

    Write-Host "==> Launching Doom (E1M1)..."
    $pos = (Read-Log).Length
    Send-Keys ("lx /bin/sh /bin/dosrun -c `"mount c /dos`" -c c: -c `"cd doom`" -c `"doom $doomArgs`"`n")

    # Give DOSBox+Doom time to load, then sanity-check that it drew something colourful.
    Start-Sleep -Seconds 22
    $shot = Shot "doom_check"
    $c = Colours $shot
    $faults = [regex]::Matches((Read-Log), '\[(ISR|panic)\]').Count
    $seg = (Read-Log).Substring([Math]::Min($pos, (Read-Log).Length))

    if ($c -gt 25 -and $faults -eq 0) {
        Write-Host "ok   Doom is drawing ($c sampled colours), no kernel faults."
    } elseif ($faults -gt 0) {
        Write-Host "WARN: kernel faults detected ($faults) - check $log (QEMU still running)."
    } else {
        Write-Host "WARN: couldn't confirm Doom drew ($c colours). It may still be loading, or the image lacks /dos/doom. Check the QEMU window / $log."
    }
    # Only warn on strong failure signals in the post-launch segment (avoid false positives like "[seed] //bin/panic").
    if ($seg -match "Unable to change to:|Illegal command: doom|File .* not found|DOS/4GW|exited, status [1-9]\S*.*dos(run|box)") {
        Write-Host "WARN: launch may have failed (see $log)."
    }

    Write-Host ""
    Write-Host "============================================================"
    Write-Host "  DOOM is (or should be) up in the QEMU window."
    Write-Host "  Controls (shareware, E1M1):"
    Write-Host "    Arrow keys  - move (Up = forward, Down = back, Left/Right = turn)"
    Write-Host "    Ctrl        - fire"
    Write-Host "    Space       - use / open doors / activate switches"
    Write-Host "    Shift       - run"
    Write-Host "    1-7         - select weapon"
    Write-Host "    Esc         - menu"
    Write-Host "  Mouse: click the game window once to capture it, then mouse turns/aims,"
    Write-Host "         left click fires (mouse enabled via DEFAULT.CFG)."
    Write-Host "  Sound: $([string]$(if ($NoSound) {'OFF (-NoSound)'} else {'ON (AC97/SDL)'}))."
    Write-Host "  Serial log: $log"
    Write-Host "  Close the QEMU window when you're done playing."
    Write-Host "============================================================"
    Write-Host "(This script leaves QEMU running. Ctrl+C here won't kill the game.)"
    # Keep the script alive while QEMU runs so the console shows status; user closes QEMU window to exit.
    Wait-Process -Id $p.Id
} catch {
    Write-Host "ERROR: $($_.Exception.Message)"
    Write-Host "Log: $log"
    if (-not $p.HasExited) { Write-Host "QEMU left running (PID $($p.Id)) for inspection." }
    exit 1
}
