# Reads the kernel's log area (see src/klog.h) back from the stick into a folder. Read-only.
# Run elevated:  sudo powershell -File tools\stick\readlog.ps1 -Disk 3 -Out C:\some\folder
param(
    [int]$Disk = -1,
    [string]$Image = '',          # a raw image file instead of a disk (tests)
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = 'Stop'
$RegionSectors = 4096
New-Item -ItemType Directory -Force -Path $Out | Out-Null
if ($Image) {
    $fs = New-Object IO.FileStream($Image, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $size = $fs.Length
} else {
    if ($Disk -lt 0) { throw 'give -Disk N (or -Image file)' }
    $d = Get-Disk -Number $Disk
    if ($d.BusType -ne 'USB' -or $d.IsSystem -or $d.IsBoot) { throw "disk $Disk is not a USB stick" }
    $fs = New-Object IO.FileStream("\\.\PhysicalDrive$Disk", [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $size = $d.Size
}
$fs.Seek($size - $RegionSectors * 512, 'Begin') | Out-Null
$r = New-Object byte[] ($RegionSectors * 512)
$got = 0
while ($got -lt $r.Length) { $n = $fs.Read($r, $got, $r.Length - $got); if ($n -le 0) { break }; $got += $n }
$fs.Close()
$magic = [Text.Encoding]::ASCII.GetString($r, 0, 15)
if ($magic -ne 'KANARCHY-LOG-V1') { throw "no log marker on disk $Disk (found '$magic'): this stick was not flashed with tools\stick\flash.ps1" }
$boot = [BitConverter]::ToUInt32($r, 16)
$ntab = [BitConverter]::ToUInt32($r, 20)
$scan = [BitConverter]::ToUInt32($r, 24 + 16 * 12)
function Text-At($sect, $count) {
    $s = $sect * 512
    $e = $s + $count * 512
    $i = $s
    while ($i -lt $e -and $r[$i] -ne 0) { $i++ }
    return [Text.Encoding]::ASCII.GetString($r, $s, $i - $s)
}
$even = Text-At 1 256
$odd = Text-At 257 256
[IO.File]::WriteAllText((Join-Path $Out 'log_even_boots.txt'), $even)
[IO.File]::WriteAllText((Join-Path $Out 'log_odd_boots.txt'), $odd)
[IO.File]::WriteAllText((Join-Path $Out 'status.txt'), (Text-At 513 64))
$tabs = Join-Path $Out 'tables'
New-Item -ItemType Directory -Force -Path $tabs | Out-Null
for ($i = 0; $i -lt $ntab -and $i -lt 16; $i++) {
    $o = 24 + 12 * $i
    $sig = [Text.Encoding]::ASCII.GetString($r, $o, 4)
    $sect = [BitConverter]::ToUInt32($r, $o + 4)
    $len = [BitConverter]::ToUInt32($r, $o + 8)
    if ($sect * 512 + $len -le $r.Length) {
        $b = New-Object byte[] $len
        [Array]::Copy($r, $sect * 512, $b, 0, $len)
        [IO.File]::WriteAllBytes((Join-Path $tabs ("{0:D2}_{1}.aml" -f $i, $sig)), $b)
    }
}
$latest = if (($boot % 2) -eq 0) { 'log_even_boots.txt' } else { 'log_odd_boots.txt' }
Write-Host "boot counter $boot (the newest log is $latest), $ntab ACPI table(s) saved, scan window in progress at last write: $('{0:x}' -f $scan)"
