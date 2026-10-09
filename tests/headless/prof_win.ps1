# Where does the desktop loop spend its cycles? Boots kanarchy-desktop.iso under
# Windows QEMU (WHPX by default: realistic CPU speed), turns the kernel profiler
# on (`prof on` -> [prof] lines, Mcycles per zone per second), loads a page and
# runs three phases: scroll the page, move the mouse over the desktop, idle.
# Prints the average of every [prof] / [fps] line per phase.
#
#   powershell -ExecutionPolicy Bypass -File tests\headless\prof_win.ps1 [-Url ...] [-Cpus 1] [-Iso path]
param(
    [string]$Accel = "whpx",
    [string]$Url = "https://en.wikipedia.org/wiki/Operating_system",
    [int]$Cpus = 1,
    [int]$ScrollKeys = 60,
    [string]$Qemu = "C:\Program Files\qemu\qemu-system-x86_64.exe",
    [int]$Port = 45455,
    [string]$Iso = ""
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$iso = if ($Iso) { $Iso } else { Join-Path $root "kanarchy-desktop.iso" }
$out = Join-Path $env:TEMP "okvm-win"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $out ("prof_{0}_{1}.log" -f $Accel, $Port)
Remove-Item $log -ErrorAction SilentlyContinue

$qargs = @("-accel", $Accel, "-m", "512", "-smp", "$Cpus", "-cdrom", $iso, "-boot", "d", "-vga", "std",
          "-device", "e1000,netdev=net0", "-netdev", "user,id=net0",
          "-serial", "file:$log", "-display", "none", "-no-reboot",
          "-monitor", "tcp:127.0.0.1:$Port,server,nowait")
$p = Start-Process -FilePath $Qemu -ArgumentList $qargs -PassThru -WindowStyle Hidden

function Mon([string[]]$cmds, [int]$gapMs = 40) {
    $c = New-Object System.Net.Sockets.TcpClient("127.0.0.1", $Port)
    $w = New-Object System.IO.StreamWriter($c.GetStream())
    foreach ($m in $cmds) { $w.Write("$m`n"); $w.Flush(); Start-Sleep -Milliseconds $gapMs }
    Start-Sleep -Milliseconds 100
    $c.Close()
}
function Send-Keys([string]$s) {
    $map = @{ ' '='spc'; '/'='slash'; '.'='dot'; ':'='shift-semicolon'; '-'='minus'; "`n"='ret'; '='='equal'; '_'='shift-minus'; '?'='shift-slash'; '&'='shift-7'; '%'='shift-5'; ([string][char]27)='esc' }
    $cmds = @()
    foreach ($ch in $s.ToCharArray()) {
        $k = if ($map.ContainsKey([string]$ch)) { $map[[string]$ch] }
             elseif ([char]::IsUpper($ch)) { "shift-" + [string]([char]::ToLower($ch)) }
             else { [string]$ch }
        $cmds += "sendkey $k"
    }
    Mon $cmds
}
function Read-Log {
    if (-not (Test-Path $log)) { return "" }
    $fs = [System.IO.File]::Open($log, "Open", "Read", "ReadWrite")
    try { (New-Object System.IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
}
function Wait-For([string]$pat, [int]$sec, [int]$from = 0) {
    $t = [DateTime]::Now
    while (([DateTime]::Now - $t).TotalSeconds -lt $sec) { if ((Read-Log).IndexOf($pat, $from) -ge 0) { return $true }; Start-Sleep -Milliseconds 200 }
    return $false
}
function Phase-Summary([string]$name, [int]$from) {
    $text = (Read-Log).Substring($from)
    $sum = @{}; $n = @{}
    foreach ($ln in ($text -split "`n")) {
        if ($ln -match "^\[prof\](.*)") {
            foreach ($m in [regex]::Matches($Matches[1], "(\w+)=(\d+)/(\d+)")) {
                $k = $m.Groups[1].Value
                $sum[$k] = [int]$sum[$k] + [int]$m.Groups[2].Value
                $n[$k + "#"] = [int]$n[$k + "#"] + [int]$m.Groups[3].Value
            }
            $sum["_lines"] = [int]$sum["_lines"] + 1
        }
        if ($ln -match "^\[fps\] (\d+) loops=(\d+) busy=(\d+)") {
            $sum["_fps"] = [int]$sum["_fps"] + [int]$Matches[1]; $sum["_busy"] = [int]$sum["_busy"] + [int]$Matches[3]; $sum["_fpsn"] = [int]$sum["_fpsn"] + 1
        }
    }
    $lines = [math]::Max(1, [int]$sum["_lines"])
    $o = "{0,-8} fps={1:N1} busy={2}%  " -f $name, ([double]$sum["_fps"] / [math]::Max(1, [int]$sum["_fpsn"])), [int]([double]$sum["_busy"] / [math]::Max(1, [int]$sum["_fpsn"]))
    foreach ($k in @("keys","okai","windows","taskbar","flush","halt","job","paint","scroll","blit")) {
        $o += "{0}={1:N0}M({2}) " -f $k, ([double]$sum[$k] / $lines), [int]($n[$k + "#"] / $lines)
    }
    $o
}

try {
    if (-not (Wait-For "[mem] heap" 60)) { throw "no boot" }
    Start-Sleep -Seconds 5
    Send-Keys "prof on`n"
    Start-Sleep -Seconds 1
    Send-Keys "okai`n"
    if (-not (Wait-For "home rendered" 60)) { throw "okai did not open" }
    Start-Sleep -Seconds 2
    Send-Keys ([string][char]27); Start-Sleep -Milliseconds 200
    Send-Keys "g"; Start-Sleep -Milliseconds 400
    $pos = (Read-Log).Length
    Send-Keys ($Url + "`n")
    if (-not (Wait-For "[okai] render tab" 180 $pos)) { throw "page did not render" }
    Start-Sleep -Seconds 12
    "page loaded (cpus=$Cpus accel=$Accel)"

    $from = (Read-Log).Length
    for ($i = 0; $i -lt $ScrollKeys; $i++) { Send-Keys "j"; Start-Sleep -Milliseconds 60 }
    Start-Sleep -Seconds 2
    Phase-Summary "scroll" $from

    $from = (Read-Log).Length
    $end = [DateTime]::Now.AddSeconds(8); $d = 1
    while ([DateTime]::Now -lt $end) { Mon @("mouse_move $(12*$d) $(7*$d)","mouse_move $(12*$d) $(7*$d)","mouse_move $(12*$d) $(7*$d)") 30; $d = -$d }
    Start-Sleep -Seconds 1
    Phase-Summary "mouse" $from

    $from = (Read-Log).Length
    Start-Sleep -Seconds 6
    Phase-Summary "idle" $from
} finally {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
}
