param(
    [int]$Cpu = 4
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Get-WslDistro {
    $preferred = @($env:LITEOS_WSL_DISTRO, 'Debian', 'Ubuntu')
    $available = @(
        (& wsl.exe -l -q 2>$null) |
            ForEach-Object { ("$_" -replace "`0", '').Trim() } |
            Where-Object { $_ }
    )

    foreach ($candidate in $preferred) {
        if ($candidate -and $available -contains $candidate) {
            return $candidate
        }
    }
    if ($available.Count -gt 0) {
        return $available[0]
    }
    return $null
}

function Find-OptionalPath([string[]]$candidates, [string]$commandName) {
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    $command = Get-Command $commandName -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }
    return $null
}

function Get-WslProjectRoot([string]$distro) {
    if ($projectRoot -notmatch '^[A-Za-z]:\\') {
        throw "Project path is not on a Windows drive: $projectRoot"
    }
    $drive = $projectRoot.Substring(0, 1).ToLowerInvariant()
    $rest = $projectRoot.Substring(2).Replace('\', '/')
    return "/mnt/$drive$rest"
}

$distro = Get-WslDistro
$qemu = Find-OptionalPath @(
    'C:\Program Files\qemu\qemu-system-x86_64.exe',
    'C:\Program Files\QEMU\qemu-system-x86_64.exe'
) 'qemu-system-x86_64.exe'

$qemuImg = $null
$firmware = $null
if ($qemu) {
    $qemuDirectory = Split-Path -Parent $qemu
    $qemuImg = Find-OptionalPath @(
        (Join-Path $qemuDirectory 'qemu-img.exe'),
        'C:\Program Files\qemu\qemu-img.exe'
    ) 'qemu-img.exe'
    $firmware = Find-OptionalPath @(
        (Join-Path $qemuDirectory 'share\edk2-x86_64-code.fd'),
        'C:\Program Files\qemu\share\edk2-x86_64-code.fd'
    ) 'edk2-x86_64-code.fd'
}

$useNativeQemu = $qemu -and $qemuImg -and $firmware
$kernel = Join-Path $projectRoot 'build\esp\EFI\LITEOS\kernel.elf'
$bootloader = Join-Path $projectRoot 'build\esp\EFI\BOOT\BOOTX64.EFI'

if ($useNativeQemu) {
    if (-not $distro) {
        if (-not (Test-Path -LiteralPath $kernel) -or
            -not (Test-Path -LiteralPath $bootloader)) {
            throw 'WSL is required to build LiteOS, but no WSL distribution was found.'
        }
        Write-Host 'Environment: Windows native QEMU (existing build)'
    } else {
        $wslRoot = Get-WslProjectRoot $distro
        $escapedRoot = $wslRoot.Replace("'", "'\''")
        $buildCommand = "cd '$escapedRoot' && ./build-wsl.sh esp DEBUG=2 LITEOS_DEBUG_SERIAL=1"
        Write-Host "Environment: Windows native QEMU + WSL build ($distro)"
        & wsl.exe -d $distro -- bash -lc $buildCommand
        if ($LASTEXITCODE -ne 0) {
            throw "LiteOS build failed in WSL ($LASTEXITCODE)."
        }
    }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
        (Join-Path $projectRoot 'run-qemu-windows.ps1') -Cpu $Cpu
    exit $LASTEXITCODE
}

if (-not $distro) {
    throw 'No usable QEMU environment found. Install Windows QEMU or WSL with QEMU/OVMF.'
}

$wslRoot = Get-WslProjectRoot $distro
$escapedRoot = $wslRoot.Replace("'", "'\''")
$runCommand = "cd '$escapedRoot' && ./run-qemu.sh --keep-open --cpu $Cpu"
Write-Host "Environment: WSL QEMU ($distro)"
& wsl.exe -d $distro -- bash -lc $runCommand
exit $LASTEXITCODE
