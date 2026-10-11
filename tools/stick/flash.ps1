# Writes kanarchy-laptop.iso to a USB stick AND marks the stick's last 2 MB as the kernel's log area (see src/klog.h).
# The kernel only ever writes to a stick that carries this marker, and only inside that area.
# Run elevated:  sudo powershell -File tools\stick\flash.ps1 -Disk 3
param(
    [Parameter(Mandatory = $true)][int]$Disk,
    [string]$Iso = (Join-Path $PSScriptRoot '..\..\kanarchy-laptop.iso')
)
$ErrorActionPreference = 'Stop'
$RegionSectors = 4096
$Magic = 'KANARCHY-LOG-V1'
$d = Get-Disk -Number $Disk
if ($d.BusType -ne 'USB' -or $d.IsSystem -or $d.IsBoot -or $d.Size -gt 200GB -or $d.Size -lt 1GB) {
    throw "disk $Disk is not an acceptable USB stick: $($d.FriendlyName) $($d.BusType) $([math]::Round($d.Size/1GB,1)) GB"
}
$iso = (Resolve-Path $Iso).Path
$bytes = [IO.File]::ReadAllBytes($iso)
$regionOffset = $d.Size - $RegionSectors * 512
if ($bytes.Length -ge $regionOffset) { throw 'the ISO does not fit in front of the log area' }
Write-Host "Flashing $iso ($($bytes.Length) bytes) to disk ${Disk}: $($d.FriendlyName), $([math]::Round($d.Size/1GB,1)) GB"
Get-Partition -DiskNumber $Disk -ErrorAction SilentlyContinue | Remove-Partition -Confirm:$false -ErrorAction SilentlyContinue
Clear-Disk -Number $Disk -RemoveData -RemoveOEM -Confirm:$false
Set-Disk -Number $Disk -IsOffline $false -ErrorAction SilentlyContinue
$fs = New-Object IO.FileStream("\\.\PhysicalDrive$Disk", [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::ReadWrite, 4096, [IO.FileOptions]::WriteThrough)
$pad = (512 - ($bytes.Length % 512)) % 512
$buf = New-Object byte[] ($bytes.Length + $pad)
[Array]::Copy($bytes, $buf, $bytes.Length)
$fs.Write($buf, 0, $buf.Length)
# the log area: zeros, then the header sector with the marker
$region = New-Object byte[] ($RegionSectors * 512)
$m = [Text.Encoding]::ASCII.GetBytes($Magic)
[Array]::Copy($m, $region, $m.Length)
$fs.Seek($regionOffset, 'Begin') | Out-Null
$fs.Write($region, 0, $region.Length)
$fs.Flush()
# verify both parts
$fs.Seek(0, 'Begin') | Out-Null
$rb = New-Object byte[] $buf.Length
$got = 0
while ($got -lt $rb.Length) { $n = $fs.Read($rb, $got, $rb.Length - $got); if ($n -le 0) { break }; $got += $n }
$fs.Seek($regionOffset, 'Begin') | Out-Null
$rr = New-Object byte[] 512
$got2 = 0
while ($got2 -lt 512) { $n = $fs.Read($rr, $got2, 512 - $got2); if ($n -le 0) { break }; $got2 += $n }
$fs.Close()
$h1 = (Get-FileHash -InputStream ([IO.MemoryStream]::new($buf)) -Algorithm SHA256).Hash
$h2 = (Get-FileHash -InputStream ([IO.MemoryStream]::new($rb)) -Algorithm SHA256).Hash
$markOk = ([Text.Encoding]::ASCII.GetString($rr, 0, $Magic.Length) -eq $Magic)
Write-Host "image sha256 $h1 readback $h2; marker $markOk"
if ($h1 -eq $h2 -and $markOk) { Write-Host 'VERIFIED' } else { Write-Host 'MISMATCH'; exit 1 }
