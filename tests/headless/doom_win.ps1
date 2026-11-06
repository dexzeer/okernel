# Doom's shareware episode in DOSBox on the Linux subsystem, under WHPX (TCG cannot get it past its loading text in time).
#   powershell -ExecutionPolicy Bypass -File tests\headless\doom_win.ps1 [-Iso build-host\lxtest.iso] [-Wav out.wav] [-Out dir]
# Needs an image from tools/lx/dosbox_root.sh with Doom's zip. Plays with keys and the mouse and judges the picture, then runs
# Doom's own -timedemo and prints the frame rate. With -Wav the guest's sound is recorded (never played).
param([string]$Iso = "", [string]$Wav = "", [string]$Out = "", [int]$Port = 45462, [string]$DoomArgs = "-nosound", [switch]$NoDemo)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Iso) { $Iso = Join-Path $root "build-host\lxtest.iso" }
if (-not $Out) { $Out = Join-Path $env:TEMP "okvm-win" }
New-Item -ItemType Directory -Force $Out | Out-Null
$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe"
$log = Join-Path $Out "doom.log"
Remove-Item $log -ErrorAction SilentlyContinue
$qargs = @("-accel", "whpx", "-m", "2048", "-cdrom", $Iso, "-boot", "d", "-vga", "std", "-device", "e1000,netdev=net0", "-netdev", "user,id=net0",
           "-serial", "file:$log", "-display", "none", "-no-reboot", "-monitor", "tcp:127.0.0.1:$Port,server,nowait")
if ($Wav) { $qargs += @("-audiodev", "wav,id=a0,path=$Wav", "-device", "AC97,audiodev=a0") }
$p = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden
$script:fails = @()
function Check($ok, [string]$what) { if ($ok) { "ok   $what" } else { "FAIL $what"; $script:fails += $what } }
function Hmp([string[]]$cmds, [int]$gap = 40) {
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port); $w = New-Object System.IO.StreamWriter($c.GetStream())
    foreach ($m in $cmds) { $w.Write("$m`n"); $w.Flush(); Start-Sleep -Milliseconds $gap }
    Start-Sleep -Milliseconds 100; $c.Close()
}
function Send-Keys([string]$s) {
    $map = @{ ' '='spc'; '/'='slash'; '.'='dot'; ':'='shift-semicolon'; '-'='minus'; "`n"='ret'; '='='equal'; '_'='shift-minus'; '&'='shift-7'; '"'='shift-apostrophe'; '>'='shift-dot' }
    Hmp @($s.ToCharArray() | ForEach-Object { $k = if ($map.ContainsKey([string]$_)) { $map[[string]$_] } elseif ([char]::IsUpper($_)) { "shift-" + [string]([char]::ToLower($_)) } else { [string]$_ }; "sendkey $k" })
}
function Read-Log { if (-not (Test-Path $log)) { return "" }; $fs = [System.IO.File]::Open($log, "Open", "Read", "ReadWrite"); try { (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() } }
function Wait-For([string]$pat, [int]$sec) { $t = [DateTime]::Now; while (([DateTime]::Now - $t).TotalSeconds -lt $sec) { if ((Read-Log).Contains($pat)) { return $true }; Start-Sleep -Milliseconds 200 }; return $false }
function Exits { ([regex]::Matches((Read-Log), "exited, status")).Count }
function Shot([string]$name) { $f = Join-Path $Out "$name.ppm"; Hmp @("screendump $f"); Start-Sleep -Milliseconds 900; ,[System.IO.File]::ReadAllBytes($f) }
function Differ($a, $b) {   # pixels that changed inside Doom's window, sampled
    $hdr = 20; $n = 0; $all = 0
    for ($y = 400; $y -lt 760; $y += 6) { for ($x = 660; $x -lt 1260; $x += 6) { $i = $hdr + ($y * 1920 + $x) * 3; $all++; if ($a[$i] -ne $b[$i] -or $a[$i + 1] -ne $b[$i + 1] -or $a[$i + 2] -ne $b[$i + 2]) { $n++ } } }
    return [int](100 * $n / $all)
}
function Colours($a) { $h = @{}; for ($y = 400; $y -lt 760; $y += 6) { for ($x = 660; $x -lt 1260; $x += 6) { $i = 20 + ($y * 1920 + $x) * 3; $h[[int]$a[$i] * 65536 + [int]$a[$i + 1] * 256 + [int]$a[$i + 2]] = 1 } }; return $h.Count }
try {
    Check (Wait-For "[lx] Linux subsystem ready" 60) "desktop up"; Start-Sleep -Seconds 6
    Send-Keys "lx /bin/xokai &`n"; Check (Wait-For "xokai: display :0 ready" 30) "the server is up"; Start-Sleep -Seconds 1
    $pos = (Read-Log).Length; $n0 = Exits
    Send-Keys ("lx /bin/sh /bin/dosrun -c `"mount c /dos`" -c c: -c `"cd doom`" -c `"doom $DoomArgs -warp 1 1`"`n")
    Start-Sleep -Seconds 22
    $a = Shot "doom_a"
    $c = Colours $a; Check ($c -gt 25) "Doom draws ($c colours)"
    Hmp @("sendkey up 2500"); Start-Sleep -Seconds 3
    $b = Shot "doom_b"; $d = Differ $a $b; Check ($d -gt 25) "the up arrow walks forward ($d% of the picture changed)"
    Hmp @("mouse_button 1"); Start-Sleep -Milliseconds 200; Hmp @("mouse_button 0"); Start-Sleep -Milliseconds 800
    $c1 = Shot "doom_c"
    Hmp (1..12 | ForEach-Object { "mouse_move 40 0" }) 60; Start-Sleep -Milliseconds 800
    $e = Shot "doom_d"; $d = Differ $c1 $e; Check ($d -gt 25) "the mouse turns ($d% of the picture changed)"
    Hmp @("mouse_button 1"); Start-Sleep -Milliseconds 300; Hmp @("mouse_button 0"); Start-Sleep -Milliseconds 400
    $f = Shot "doom_e"
    Check ((Exits) -eq $n0) "Doom is still running"
    $seg = (Read-Log).Substring($pos)
    $pres = @([regex]::Matches($seg, 'presents=(\d+)') | ForEach-Object { [int]$_.Groups[1].Value })
    "     image updates a second, last 10: " + (($pres | Select-Object -Last 10) -join " ")
    $faults = [regex]::Matches((Read-Log), '\[(ISR|panic)\]').Count; Check ($faults -eq 0) "no kernel faults"
    if ($Wav) { Start-Sleep -Seconds 10 }
    if (-not $NoDemo) {
        Stop-Process -Id $p.Id -Force; Start-Sleep -Seconds 2; Remove-Item $log -ErrorAction SilentlyContinue
        $p = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden
        Wait-For "[lx] Linux subsystem ready" 60 | Out-Null; Start-Sleep -Seconds 6
        Send-Keys "lx /bin/xokai &`n"; Wait-For "xokai: display :0 ready" 30 | Out-Null; Start-Sleep -Seconds 1
        $n0 = Exits
        Send-Keys ("lx /bin/sh /bin/dosrun -c `"mount c /dos`" -c c: -c `"cd doom`" -c `"doom -nosound -timedemo demo1 > demo.txt`" -c exit`n")
        $t = [DateTime]::Now; while (([DateTime]::Now - $t).TotalSeconds -lt 240 -and (Exits) -le $n0) { Start-Sleep -Seconds 1 }
        "     demo took {0:N0} s" -f ([DateTime]::Now - $t).TotalSeconds
        $pos = (Read-Log).Length; Send-Keys "lx /bin/sh /bin/doomdemo`n"; Start-Sleep -Seconds 4
        $out = (Read-Log).Substring($pos)
        $m = [regex]::Match($out, 'timed (\d+) gametics in (\d+) realtics')
        if ($m.Success) { $g = [double]$m.Groups[1].Value; $r = [double]$m.Groups[2].Value; "     $($m.Value): {0:N1} fps" -f (35 * $g / $r); Check ($g -gt 1000) "the demo played to the end" }
        else { Check $false ("no timedemo result: " + $out.Substring(0, [Math]::Min(200, $out.Length))) }
    }
} finally { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
if ($script:fails.Count) { "RESULT FAIL: " + ($script:fails -join "; ") ; exit 1 } else { "RESULT ALLPASS" }
