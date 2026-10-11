# Marks an ALREADY flashed stick (Rufus, dd, anything) as the kernel's log area, without touching the ISO on it.
# It only writes the last 2 MB of the stick, and refuses when that area is not empty (zeros), unless -Force.
# Run elevated:  sudo powershell -File tools\stick\mark.ps1 -Disk 3
param(
    [Parameter(Mandatory = $true)][int]$Disk,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$RegionSectors = 4096
$Magic = 'KANARCHY-LOG-V1'
$d = Get-Disk -Number $Disk
if ($d.BusType -ne 'USB' -or $d.IsSystem -or $d.IsBoot -or $d.Size -gt 200GB -or $d.Size -lt 1GB) {
    throw "disk $Disk is not an acceptable USB stick: $($d.FriendlyName) $($d.BusType) $([math]::Round($d.Size/1GB,1)) GB"
}
$regionOffset = $d.Size - $RegionSectors * 512
$fs = New-Object IO.FileStream("\\.\PhysicalDrive$Disk", [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite, 4096, [IO.FileOptions]::WriteThrough)
$fs.Seek($regionOffset, 'Begin') | Out-Null
$cur = New-Object byte[] ($RegionSectors * 512)
$got = 0
while ($got -lt $cur.Length) { $n = $fs.Read($cur, $got, $cur.Length - $got); if ($n -le 0) { break }; $got += $n }
$already = ([Text.Encoding]::ASCII.GetString($cur, 0, $Magic.Length) -eq $Magic)
$used = $false
foreach ($b in $cur) { if ($b -ne 0) { $used = $true; break } }
if ($used -and -not $already -and -not $Force) { $fs.Close(); throw 'the last 2 MB of this stick are not empty (a filesystem may use them): not touching it. Use tools\stick\flash.ps1 to write the ISO and the marker together.' }
$region = New-Object byte[] ($RegionSectors * 512)
$m = [Text.Encoding]::ASCII.GetBytes($Magic)
[Array]::Copy($m, $region, $m.Length)
$fs.Seek($regionOffset, 'Begin') | Out-Null
$fs.Write($region, 0, $region.Length)
$fs.Flush()
$fs.Seek($regionOffset, 'Begin') | Out-Null
$rr = New-Object byte[] 512
$g = 0
while ($g -lt 512) { $n = $fs.Read($rr, $g, 512 - $g); if ($n -le 0) { break }; $g += $n }
$fs.Close()
if ([Text.Encoding]::ASCII.GetString($rr, 0, $Magic.Length) -eq $Magic) { Write-Host "MARKED disk ${Disk}: $($d.FriendlyName). Boot it on the laptop, then run readlog.ps1." } else { Write-Host 'FAILED to verify the marker'; exit 1 }
