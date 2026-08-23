param(
    [switch]$NoMouse,
    [int]$Cpu = 4,
    [int]$MemoryMiB = 4096,
    [string]$QemuPath = '',
    [string]$FirmwarePath = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$espPath = Join-Path $projectRoot 'build\esp'
$nativeImage = Join-Path $projectRoot 'build\qemu-native-fat.img'
$serialLog = Join-Path $projectRoot 'build\qemu-native-serial.log'
$qemuLog = Join-Path $projectRoot 'build\qemu-native.log'

function Find-Executable([string]$explicitPath, [string[]]$candidates, [string]$name) {
    if ($explicitPath) {
        if (Test-Path -LiteralPath $explicitPath) {
            return (Resolve-Path -LiteralPath $explicitPath).Path
        }
        throw "Executable not found: $explicitPath"
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $fromPath = Get-Command $name -ErrorAction SilentlyContinue
    if ($fromPath) { return $fromPath.Source }
    throw "$name was not found. Install Windows QEMU first."
}

$qemu = Find-Executable $QemuPath @(
    'C:\Program Files\qemu\qemu-system-x86_64.exe',
    'C:\Program Files\QEMU\qemu-system-x86_64.exe'
) 'qemu-system-x86_64.exe'
$qemuDirectory = Split-Path -Parent $qemu
$qemuImg = Find-Executable '' @(
    (Join-Path $qemuDirectory 'qemu-img.exe'),
    'C:\Program Files\qemu\qemu-img.exe'
) 'qemu-img.exe'
$firmware = Find-Executable $FirmwarePath @(
    (Join-Path $qemuDirectory 'share\edk2-x86_64-code.fd'),
    'C:\Program Files\qemu\share\edk2-x86_64-code.fd'
) 'edk2-x86_64-code.fd'

foreach ($requiredPath in @(
    (Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'),
    (Join-Path $espPath 'EFI\LITEOS\kernel.elf')
)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Missing boot file: $requiredPath. Run ./build-wsl.sh esp DEBUG=2 in WSL first."
    }
}

$buildDirectory = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
Remove-Item -LiteralPath $nativeImage -Force -ErrorAction SilentlyContinue

# Build a disposable FAT image from the ESP directory. The guest can write to
# this image without modifying build\esp on the host.
& $qemuImg convert -O raw "fat:ro:$espPath" $nativeImage
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $nativeImage)) {
    throw 'qemu-img could not create the FAT image.'
}

$qemuArgs = @(
    '-machine', 'q35',
    '-vga', 'none',
    '-device', 'VGA,vgamem_mb=64',
    '-m', "${MemoryMiB}M",
    '-cpu', 'qemu64',
    '-smp', "$Cpu",
    '-drive', "if=pflash,format=raw,readonly=on,file=$firmware",
    '-boot', 'order=c',
    '-serial', "file:$serialLog",
    '-no-reboot',
    '-no-shutdown',
    '-d', 'guest_errors',
    '-D', $qemuLog,
    '-drive', "if=none,id=liteos-usb-stick,format=raw,file=$nativeImage",
    '-device', 'qemu-xhci,id=liteos-xhci,msix=on',
    '-device', 'usb-storage,id=liteos-usb-storage,drive=liteos-usb-stick,bus=liteos-xhci.0,port=1',
    '-device', 'usb-kbd,bus=liteos-xhci.0',
    '-monitor', 'none',
    '-accel', 'tcg,thread=multi',
    # GTK can grab the keyboard as soon as the pointer enters the VM window.
    # This avoids the SDL-only focus/capture ambiguity on Windows.
    '-display', 'gtk,grab-on-hover=on,zoom-to-fit=off'
)

if (-not $NoMouse) {
    $qemuArgs += @('-device', 'usb-mouse,bus=liteos-xhci.0')
}

Write-Host "Starting Windows QEMU: $qemu"
Write-Host "Serial log: $serialLog"
Write-Host 'Move the pointer into the QEMU window to capture keyboard input.'
Write-Host 'Use Ctrl+Alt+G to release or toggle QEMU input capture.'
& $qemu @qemuArgs
