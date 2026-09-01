param(
    [Parameter(Mandatory = $true)]
    [string]$Image,
    [Parameter(Mandatory = $true)]
    [string]$Path,
    [Parameter(Mandatory = $true)]
    [string]$Output
)

$ErrorActionPreference = 'Stop'
$imagePath = (Resolve-Path -LiteralPath $Image).Path
$imageStream = [IO.File]::Open($imagePath, [IO.FileMode]::Open,
    [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
try {
    if ($imageStream.Length -gt [int]::MaxValue) { throw 'FAT image is too large.' }
    $imageBytes = New-Object byte[] ([int]$imageStream.Length)
    $imageStream.Read($imageBytes, 0, $imageBytes.Length) | Out-Null
}
finally {
    $imageStream.Dispose()
}
$volumeOffset = 0L

if ($imageBytes.Length -lt 512 -or
    ($imageBytes[0] -ne 0xEB -and $imageBytes[0] -ne 0xE9)) {
    for ($partition = 0; $partition -lt 4; ++$partition) {
        $entry = 446 + $partition * 16
        $type = $imageBytes[$entry + 4]
        if ($type -in @(0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E)) {
            $startLba = [BitConverter]::ToUInt32($imageBytes, $entry + 8)
            $volumeOffset = [long]$startLba * 512L
            break
        }
    }
}
if ($volumeOffset -lt 0 -or $volumeOffset + 64 -gt $imageBytes.Length) {
    throw 'FAT volume was not found in the image.'
}

function Read-U16([int]$offset) {
    return [BitConverter]::ToUInt16($imageBytes, [int]($volumeOffset + $offset))
}

function Read-U32([int]$offset) {
    return [BitConverter]::ToUInt32($imageBytes, [int]($volumeOffset + $offset))
}

function Read-DirectoryBytes([long]$offset, [int]$size) {
    if ($offset -lt 0 -or $offset + $size -gt $imageBytes.Length) {
        throw 'FAT directory is outside the image.'
    }
    $result = New-Object byte[] $size
    [Array]::Copy($imageBytes, [int]$offset, $result, 0, $size)
    return ,$result
}

$bytesPerSector = [int](Read-U16 11)
$sectorsPerCluster = [int]$imageBytes[[int]($volumeOffset + 13)]
$reservedSectors = [int](Read-U16 14)
$fatCount = [int]$imageBytes[[int]($volumeOffset + 16)]
$rootEntryCount = [int](Read-U16 17)
$totalSectors16 = [int](Read-U16 19)
$fatSize16 = [int](Read-U16 22)
$totalSectors32 = [int](Read-U32 32)
$fatSize32 = [long](Read-U32 36)

if ($bytesPerSector -lt 512 -or $sectorsPerCluster -eq 0 -or
    $fatCount -eq 0 -or $reservedSectors -eq 0) {
    throw 'Invalid FAT boot sector.'
}

$fatSize = if ($fatSize16 -ne 0) { $fatSize16 } else { $fatSize32 }
$totalSectors = if ($totalSectors16 -ne 0) { $totalSectors16 } else { [long]$totalSectors32 }
$rootDirectorySectors = [int][Math]::Ceiling(($rootEntryCount * 32.0) / $bytesPerSector)
$fatStart = $volumeOffset + [long]$reservedSectors * $bytesPerSector
$dataStartSector = $reservedSectors + $fatCount * $fatSize + $rootDirectorySectors
$dataStart = $volumeOffset + [long]$dataStartSector * $bytesPerSector
$clusterSize = $bytesPerSector * $sectorsPerCluster
$fat32 = $rootEntryCount -eq 0

$rootCluster = if ($fat32) { [int](Read-U32 44) } else { 0 }
$dataSectors = $totalSectors - $dataStartSector
$clusterCount = [int][Math]::Floor($dataSectors / $sectorsPerCluster)
if (($fat32 -and $rootCluster -lt 2) -or $clusterCount -lt 1) {
    throw 'Invalid FAT data area.'
}

function Read-FatEntry([int]$cluster) {
    $entrySize = if ($fat32) { 4L } else { 2L }
    $offset = $fatStart + $cluster * $entrySize
    if ($offset -lt 0 -or $offset + $entrySize -gt $imageBytes.Length) {
        throw "FAT entry is outside the image: $cluster"
    }
    if ($fat32) {
        return [int]([BitConverter]::ToUInt32($imageBytes, [int]$offset) -band 0x0FFFFFFF)
    }
    return [int][BitConverter]::ToUInt16($imageBytes, [int]$offset)
}

function Get-ClusterChain([int]$firstCluster) {
    $chain = New-Object 'System.Collections.Generic.List[int]'
    $cluster = $firstCluster
    for ($guard = 0; $guard -le $clusterCount; ++$guard) {
        if ($cluster -lt 2 -or $cluster -ge $clusterCount + 2) {
            throw "Invalid FAT32 cluster: $cluster"
        }
        $chain.Add($cluster)
        $next = Read-FatEntry $cluster
        $endMarker = if ($fat32) { 0x0FFFFFF8 } else { 0xFFF8 }
        $badMarker = if ($fat32) { 0x0FFFFFF7 } else { 0xFFF7 }
        if ($next -ge $endMarker) { return $chain.ToArray() }
        if ($next -eq $badMarker) { throw 'FAT bad cluster encountered.' }
        $cluster = $next
    }
    throw 'FAT32 cluster chain is cyclic or too long.'
}

function Get-DirectoryEntries([int]$firstCluster) {
    $entries = New-Object 'System.Collections.Generic.List[object]'
    $lfnParts = @{}
    $directories = New-Object 'System.Collections.Generic.List[byte[]]'
    if ($firstCluster -eq 0 -and -not $fat32) {
        $rootOffset = $volumeOffset + [long]($reservedSectors +
            $fatCount * $fatSize) * $bytesPerSector
        $rootSize = $rootDirectorySectors * $bytesPerSector
        $directories.Add((Read-DirectoryBytes $rootOffset $rootSize))
    }
    else {
        foreach ($cluster in (Get-ClusterChain $firstCluster)) {
            $offset = $dataStart + ($cluster - 2L) * $clusterSize
            $directories.Add((Read-DirectoryBytes $offset $clusterSize))
        }
    }
    foreach ($directory in $directories) {
        for ($entryOffset = 0; $entryOffset + 32 -le $directory.Length;
             $entryOffset += 32) {
            $firstByte = $directory[$entryOffset]
            if ($firstByte -eq 0) { return $entries.ToArray() }
            if ($firstByte -eq 0xE5) { $lfnParts = @{}; continue }
            $attributes = $directory[$entryOffset + 11]
            if ($attributes -eq 0x0F) {
                $order = $directory[$entryOffset] -band 0x1F
                $part = ''
                foreach ($charOffset in @(1, 3, 5, 7, 9, 14, 16, 18, 20,
                                           22, 24, 28, 30)) {
                    $value = [BitConverter]::ToUInt16($directory,
                                                       $entryOffset + $charOffset)
                    if ($value -eq 0 -or $value -eq 0xFFFF) { continue }
                    $part += [char]$value
                }
                if ($order -ne 0) { $lfnParts[$order] = $part }
                continue
            }

            $shortBytes = New-Object byte[] 11
            [Array]::Copy($directory, $entryOffset, $shortBytes, 0, 11)
            if ($shortBytes[0] -eq 0x05) { $shortBytes[0] = 0xE5 }
            $base = ([Text.Encoding]::ASCII.GetString($shortBytes, 0, 8)).Trim()
            $extension = ([Text.Encoding]::ASCII.GetString($shortBytes, 8, 3)).Trim()
            $shortName = if ($extension) { "$base.$extension" } else { $base }
            $longName = (($lfnParts.Keys | Sort-Object | ForEach-Object {
                $lfnParts[$_]
            }) -join '')
            if (-not $longName) { $longName = $shortName }
            $cluster = ([int]([BitConverter]::ToUInt16($directory,
                                                        $entryOffset + 20)) -shl 16) -bor
                       [int][BitConverter]::ToUInt16($directory, $entryOffset + 26)
            $entries.Add([PSCustomObject]@{
                Name = $longName
                ShortName = $shortName
                Attributes = $attributes
                Cluster = $cluster
                Size = [int][BitConverter]::ToUInt32($directory, $entryOffset + 28)
                Directory = (($attributes -band 0x10) -ne 0)
            })
            $lfnParts = @{}
        }
    }
    return $entries.ToArray()
}

$components = @($Path -replace '\\', '/' -split '/' |
    Where-Object { $_ -and $_ -ne '.' })
$currentCluster = $rootCluster
$target = $null
for ($index = 0; $index -lt $components.Count; ++$index) {
    $component = $components[$index]
    $target = @(Get-DirectoryEntries $currentCluster) |
        Where-Object {
            $_.Name.Equals($component, [StringComparison]::OrdinalIgnoreCase) -or
            $_.ShortName.Equals($component, [StringComparison]::OrdinalIgnoreCase)
        } |
        Select-Object -First 1
    if ($null -eq $target) { throw "FAT file not found: $Path" }
    if ($index + 1 -lt $components.Count) {
        if (-not $target.Directory) { throw "FAT path component is not a directory: $component" }
        $currentCluster = $target.Cluster
    }
}

if ($null -eq $target -or $target.Directory) { throw "FAT path is not a file: $Path" }
$fileBytes = New-Object byte[] $target.Size
$written = 0
foreach ($cluster in (Get-ClusterChain $target.Cluster)) {
    if ($written -ge $target.Size) { break }
    $offset = $dataStart + ($cluster - 2L) * $clusterSize
    $count = [Math]::Min($clusterSize, $target.Size - $written)
    $chunk = Read-DirectoryBytes $offset $count
    [Array]::Copy($chunk, 0, $fileBytes, $written, $count)
    $written += $count
}
if ($written -ne $target.Size) { throw 'FAT file cluster chain ended early.' }

$outputPath = [IO.Path]::GetFullPath($Output)
$outputDirectory = Split-Path -Parent $outputPath
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
[IO.File]::WriteAllBytes($outputPath, $fileBytes)
Write-Host "Extracted $Path ($($target.Size) bytes) to $outputPath"
