param(
    [switch]$NoMouse,
    [switch]$Headless,
    [switch]$Debug,
    [switch]$NvmeRoot,
    [switch]$AllDisks,
    [int]$GdbPort = 1234,
    [int]$Cpu = 4,
    [int]$MemoryMiB = 4096,
    [string]$Build = '',
    [string]$QemuPath = '',
    [string]$FirmwarePath = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildName = if ($Build) { $Build } elseif ($env:LITEOS_BUILD) { $env:LITEOS_BUILD } elseif ($env:BUILD) { $env:BUILD } else { 'build' }
if ([IO.Path]::IsPathRooted($buildName) -or $buildName -match '(^|[\\/])\.\.(?:[\\/]|$)') {
    throw "Build directory must be relative to the project: $buildName"
}
$buildDirectory = Join-Path $projectRoot $buildName
$espPath = Join-Path $buildDirectory 'esp'
$nativeImage = Join-Path $buildDirectory 'qemu-native-fat.img'
$secondaryImage = Join-Path $buildDirectory 'qemu-native-nvme-data.img'
$serialLog = Join-Path $buildDirectory 'qemu-native-serial.log'
$qemuLog = Join-Path $buildDirectory 'qemu-native.log'
$debugPidFile = Join-Path $buildDirectory 'qemu-native-debug.pid'
$qemuProcess = $null

function Test-IsQemuProcess([System.Diagnostics.Process]$process) {
    if (-not $process) { return $false }

    try {
        if ($process.ProcessName -ine 'qemu-system-x86_64') { return $false }
        $processPath = $process.Path
        if (-not $processPath) { return $true }
        return ([IO.Path]::GetFileName($processPath) -ieq 'qemu-system-x86_64.exe')
    }
    catch {
        return $false
    }
}

function Stop-QemuProcess([System.Diagnostics.Process]$process) {
    if (-not $process) { return }

    try { $process.Refresh() } catch { return }
    if ($process.HasExited) { return }

    try { $process.CloseMainWindow() | Out-Null } catch { }
    try {
        if (-not $process.WaitForExit(1000)) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $null = $process.WaitForExit(1000)
        }
    }
    catch {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    }
}

function Stop-StaleDebugQemu {
    # A cancelled VS Code task can terminate this wrapper before the child
    # process. Reclaim only a QEMU process that belongs to the debug port so a
    # later F5 does not fail with an opaque "address already in use" error.
    if (Test-Path -LiteralPath $debugPidFile) {
        $rawPid = (Get-Content -LiteralPath $debugPidFile -Raw).Trim()
        $trackedPid = 0
        if ([int]::TryParse($rawPid, [ref]$trackedPid) -and $trackedPid -gt 0) {
            $trackedProcess = Get-Process -Id $trackedPid -ErrorAction SilentlyContinue
            if ($trackedProcess) {
                if (-not (Test-IsQemuProcess $trackedProcess)) {
                    throw "Debug PID file points to a non-QEMU process: $trackedPid"
                }
                Stop-QemuProcess $trackedProcess
            }
        }
        Remove-Item -LiteralPath $debugPidFile -Force -ErrorAction SilentlyContinue
    }

    $listeners = @(Get-NetTCPConnection -State Listen -LocalPort $GdbPort -ErrorAction SilentlyContinue)
    foreach ($listener in $listeners) {
        $owner = Get-Process -Id $listener.OwningProcess -ErrorAction SilentlyContinue
        if (-not $owner) {
            throw "GDB port $GdbPort is already in use by PID $($listener.OwningProcess)."
        }
        if (-not (Test-IsQemuProcess $owner)) {
            throw "GDB port $GdbPort is already in use by $($owner.ProcessName) (PID $($owner.Id))."
        }
        Stop-QemuProcess $owner
    }
}

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
        throw "Missing boot file: $requiredPath. Run .\tools\build-windows.ps1 -Build $buildName -Debug 2 -Serial first."
    }
}

if ($Debug) { Stop-StaleDebugQemu }
if ($NvmeRoot -and $AllDisks) {
    throw '-NvmeRoot and -AllDisks cannot be combined in the native launcher.'
}

New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
Remove-Item -LiteralPath $nativeImage -Force -ErrorAction SilentlyContinue
if ($AllDisks) {
    Remove-Item -LiteralPath $secondaryImage -Force -ErrorAction SilentlyContinue
}
Set-Content -LiteralPath $serialLog -Value '' -NoNewline

# Build a disposable FAT image from the ESP directory. The guest can write to
# this image without modifying build\esp on the host.
& $qemuImg convert -O raw "fat:ro:$espPath" $nativeImage
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $nativeImage)) {
    throw 'qemu-img could not create the FAT image.'
}
if ($AllDisks) {
    # Keep the second namespace separate from the boot image.  It is a
    # disposable FAT volume used to verify that non-root disks are discovered
    # and mounted without changing the host ESP tree.
    & $qemuImg convert -O raw "fat:ro:$espPath" $secondaryImage
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $secondaryImage)) {
        throw 'qemu-img could not create the secondary FAT image.'
    }
}

$qemuArgs = @(
    '-machine', 'q35',
    '-vga', 'none',
    '-device', 'VGA,vgamem_mb=64',
    '-m', "${MemoryMiB}M",
    # The freestanding crypto tests require a strong seed.  QEMU's qemu64
    # model hides RDRAND by default, so expose the virtual instruction without
    # changing the rest of the compatibility-oriented CPU model.
    '-cpu', 'qemu64,rdrand=on',
    '-smp', "$Cpu",
    '-drive', "if=pflash,format=raw,readonly=on,file=$firmware",
    '-boot', 'order=c',
    '-serial', "file:$serialLog",
    '-no-reboot',
    '-no-shutdown',
    '-d', 'guest_errors',
    '-D', $qemuLog
)

if ($NvmeRoot) {
    # Match run-qemu.sh: use the disposable FAT image as an NVMe namespace so
    # native Windows runs can exercise the NVMe path and IOPS metric.
    $qemuArgs += @(
        '-drive', "if=none,id=liteos-nvme0,format=raw,file=$nativeImage",
        '-device', 'nvme,drive=liteos-nvme0,serial=liteos-system,bootindex=1',
        '-device', 'qemu-xhci,id=liteos-xhci,msix=on'
    )
}
else {
    $qemuArgs += @(
        '-drive', "if=none,id=liteos-usb-stick,format=raw,file=$nativeImage",
        '-device', 'qemu-xhci,id=liteos-xhci,msix=on',
        '-device', $(if ($AllDisks) {
            'usb-storage,id=liteos-usb-storage,drive=liteos-usb-stick,bus=liteos-xhci.0,port=1,bootindex=1'
        } else {
            'usb-storage,id=liteos-usb-storage,drive=liteos-usb-stick,bus=liteos-xhci.0,port=1'
        })
    )
    if ($AllDisks) {
        $qemuArgs += @(
            '-drive', "if=none,id=liteos-nvme-data,format=raw,file=$secondaryImage",
            '-device', 'nvme,drive=liteos-nvme-data,serial=liteos-data,bootindex=2'
        )
    }
}

$qemuArgs += @(
    '-device', 'usb-kbd,bus=liteos-xhci.0',
    '-monitor', 'none',
    '-accel', 'tcg,thread=multi'
)

if ($Headless) {
    $qemuArgs += @('-display', 'none')
}
else {
    # GTK can grab the keyboard as soon as the pointer enters the VM window.
    # This avoids the SDL-only focus/capture ambiguity on Windows.
    $qemuArgs += @('-display', 'gtk,grab-on-hover=on,zoom-to-fit=off')
}

if (-not $NoMouse -and -not $Headless) {
    $qemuArgs += @('-device', 'usb-mouse,bus=liteos-xhci.0')
}

if ($Debug) {
    if ($GdbPort -lt 1 -or $GdbPort -gt 65535) {
        throw "Invalid GDB port: $GdbPort"
    }
    $qemuArgs += @('-gdb', "tcp:127.0.0.1:$GdbPort", '-S')
}

Write-Verbose "Starting Windows QEMU: $qemu"
Write-Verbose "Serial log: $serialLog"
Write-Verbose 'Move the pointer into the QEMU window to capture keyboard input.'
Write-Verbose 'Use Ctrl+Alt+G to release or toggle QEMU input capture.'

$serialFailurePattern = '(^|[^A-Z])(FAIL|ERROR|FATAL|PANIC|FAULT|TIMEOUT|ASSERT)([^A-Z]|$)'
$serialRemainder = ''

function Write-FailureSerialLines([string]$text, [bool]$flush) {
    $combined = $script:serialRemainder + $text
    if (-not $combined) { return }

    $lines = $combined.Split("`n")
    if ($flush) {
        $script:serialRemainder = ''
        $completeLines = $lines.Count
    }
    else {
        $script:serialRemainder = $lines[$lines.Count - 1]
        $completeLines = $lines.Count - 1
    }

    for ($index = 0; $index -lt $completeLines; ++$index) {
        $line = $lines[$index].TrimEnd("`r")
        if ($line -match $serialFailurePattern) {
            Write-Host $line
        }
    }
}

function Write-DebugMarker([string]$marker) {
    if ($env:LITEOS_DEBUG_PROTOCOL -eq '1') {
        [Console]::Out.WriteLine($marker)
        [Console]::Out.Flush()
    }
}

function Read-NewSerialText([string]$path, [ref]$position) {
    if (-not (Test-Path -LiteralPath $path)) { return }

    $stream = [System.IO.File]::Open(
        $path,
        [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read,
        [System.IO.FileShare]::ReadWrite)
    try {
        if ($stream.Length -lt $position.Value) { $position.Value = 0 }
        if ($stream.Length -le $position.Value) { return }
        $stream.Position = $position.Value
        $reader = New-Object -TypeName System.IO.StreamReader -ArgumentList @(
            $stream,
            [System.Text.Encoding]::ASCII,
            $false,
            4096,
            $true)
        try {
            $text = $reader.ReadToEnd()
            $position.Value = $stream.Position
        }
        finally {
            $reader.Dispose()
        }
        if ($text) { Write-FailureSerialLines $text $false }
    }
    finally {
        $stream.Dispose()
    }
}

# Start-Process needs explicit quoting for arguments containing spaces (for
# example the QEMU firmware path under "Program Files").
$processArguments = @(
    $qemuArgs | ForEach-Object {
        $argument = [string]$_
        if ($argument -match '\s') { '"' + $argument + '"' } else { $argument }
    }
)

function Wait-QemuWithSerialMirror(
    [System.Diagnostics.Process]$process,
    [bool]$waitForGdb) {
    [long]$position = 0
    $debugReady = $false

    while (-not $process.HasExited) {
        Read-NewSerialText $serialLog ([ref]$position)
        if ($waitForGdb -and -not $debugReady) {
            $listener = Get-NetTCPConnection -State Listen -LocalAddress '127.0.0.1' -LocalPort $GdbPort -ErrorAction SilentlyContinue
            if ($listener) {
                Write-DebugMarker 'QEMU_DEBUG_READY'
                $debugReady = $true
            }
        }
        Start-Sleep -Milliseconds 50
    }
    Read-NewSerialText $serialLog ([ref]$position)
    Write-FailureSerialLines '' $true
    if ($waitForGdb -and -not $debugReady) {
        throw "QEMU exited before GDB port $GdbPort became ready."
    }
    return $process.ExitCode
}

if ($Debug) { Write-DebugMarker 'QEMU_DEBUG_START' }
$qemuProcess = Start-Process -FilePath $qemu -ArgumentList $processArguments -WorkingDirectory $projectRoot -PassThru
if ($Debug) {
    Set-Content -LiteralPath $debugPidFile -Value $qemuProcess.Id -NoNewline
}

try {
    $exitCode = Wait-QemuWithSerialMirror $qemuProcess $Debug
    exit $exitCode
}
finally {
    # Ensure Ctrl+C, VS Code Stop, and debugger startup failures cannot leave
    # a detached QEMU holding the GDB port after the PID file is removed.
    if ($Debug) { Stop-QemuProcess $qemuProcess }
    if ($Debug) {
        Remove-Item -LiteralPath $debugPidFile -Force -ErrorAction SilentlyContinue
    }
}
