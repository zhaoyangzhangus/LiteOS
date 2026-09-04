[CmdletBinding()]
param(
    [string]$Make = 'make',
    [string]$HardwareLog = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$phase3Gate = Join-Path $PSScriptRoot 'phase3_gate.ps1'
$bridgeSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_vendor_bridge.c'
$driverSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_driver.c'
$kernelElf = Join-Path $repoRoot 'build\nvidia\phase4\kernel.elf'
$linkList = Join-Path $repoRoot 'build\nvidia\phase4\phase4_link_objects.list'

function Fail-Gate([string]$Message) {
    throw "P4 Gate failed: $Message"
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail-Gate "missing $Description`: $Path"
    }
}

Require-File $phase3Gate 'P3 Gate script'

# Phase ordering is enforced here as well as in the Phase 4 build script.  A
# stale Phase 3 report or stale Phase 4 image cannot satisfy this Gate.
$p3Args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $phase3Gate,
            '-Make', $Make)
$p3Output = @(& powershell.exe @p3Args 2>&1 |
    ForEach-Object { $_.ToString() })
$p3ExitCode = $LASTEXITCODE
foreach ($line in $p3Output) { Write-Output $line }
if ($p3ExitCode -ne 0 -or
    @($p3Output | Where-Object { $_ -eq 'GATE_P3=PASS' }).Count -ne 1) {
    Fail-Gate 'P3 Gate did not pass'
}

Require-File $kernelElf 'Phase 4 kernel image'
Require-File $linkList 'Phase 4 link list'
Require-File $bridgeSource 'vendor bridge source'
Require-File $driverSource 'LiteOS RM driver source'

$readelfCommand = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelfCommand) { $readelfCommand = Get-Command readelf -ErrorAction Stop }
$nmCommand = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nmCommand) { $nmCommand = Get-Command nm -ErrorAction Stop }

$header = @(& $readelfCommand.Source '-h' '--' $kernelElf 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'readelf -h failed' }
if (@($header | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($header | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($header | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'Phase 4 image is not an x86-64 ELF executable'
}

$programHeaders = @(& $readelfCommand.Source '-l' '--' $kernelElf 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'readelf -l failed' }
if (@($programHeaders | Where-Object { $_ -match '^\s*LOAD\s+' }).Count -lt 2 -or
    @($programHeaders | Where-Object { $_ -match '\sR E\s+' }).Count -eq 0 -or
    @($programHeaders | Where-Object { $_ -match '\sRW\s+' }).Count -eq 0) {
    Fail-Gate 'Phase 4 image does not have executable and writable load segments'
}

$symbols = @(& $nmCommand.Source '-g' '--defined-only' '--' $kernelElf 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'nm defined-symbol scan failed' }
foreach ($requiredSymbol in @(
        'kernel_entry', 'nv_rm_driver_probe', 'nv_rm_vendor_start',
        'nv_rm_vendor_stop', 'rm_init_rm', 'rm_init_private_state',
        'rm_init_adapter', 'rm_shutdown_adapter')) {
    if (@($symbols | Where-Object {
            $_ -match ('\s{0}\s*$' -f [regex]::Escape($requiredSymbol))
        }).Count -eq 0) {
        Fail-Gate "required symbol is absent: $requiredSymbol"
    }
}

$undefined = @(& $nmCommand.Source '-u' '-P' '--' $kernelElf 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'nm undefined-symbol scan failed' }
if (@($undefined | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate "final Phase 4 image has unresolved symbols: $($undefined[0])"
}

$linkRows = @(Get-Content -LiteralPath $linkList |
    Where-Object { $_ -and $_[0] -ne '#' })
if (@($linkRows | Where-Object { $_ -like '*phase3/vendor-rm.o' }).Count -ne 1 -or
    @($linkRows | Where-Object {
        $_ -like '*phase4/objects/nv_rm_vendor_bridge.o'
    }).Count -ne 1 -or
    @($linkRows | Where-Object {
        $_ -like '*phase4/objects/vendor_abi/*.o'
    }).Count -ne 11) {
    Fail-Gate 'Phase 4 link list does not contain the expected RM/ABI closure'
}

$bridgeText = Get-Content -LiteralPath $bridgeSource -Raw
foreach ($requiredText in @(
        'rm_init_rm(', 'rm_is_supported_device(', 'rm_init_private_state(',
        'rm_init_adapter(', 'rm_shutdown_adapter(', 'rm_free_private_state(',
        'rm_shutdown_rm(', 'os_pci_init_handle(',
        'gpu->chipInfo.platformId', 'gpu->chipInfo.implementationId',
        'GPU_ARCHITECTURE_BLACKWELL_GB2XX', 'GPU_IMPLEMENTATION_GB205',
        'private_state->flags & NV_INIT_FLAG_HAL',
        'private_state->flags & NV_INIT_FLAG_HAL_COMPONENTS')) {
    if ($bridgeText -notmatch [regex]::Escape($requiredText)) {
        Fail-Gate "vendor bridge evidence is absent: $requiredText"
    }
}
if ($bridgeText -match '(?im)(chipInfo\.(platformId|implementationId)|pGpuArch)\s*=') {
    Fail-Gate 'vendor bridge writes an architecture/chip override'
}
if ($bridgeText -match '(?im)\b(gsp|gmmu|pma|gpu\s*va)\b\s*[:=]') {
    Fail-Gate 'Phase 4 bridge contains a LiteOS-side GPU-owned implementation'
}

$driverText = Get-Content -LiteralPath $driverSource -Raw
if ($driverText -notmatch 'LITEOS_NVIDIA_RM_NOT_LINKED' -or
    $driverText -notmatch 'return\s+K_ENOSYS') {
    Fail-Gate 'normal-image vendor boundary is not fail-closed'
}

# The vendor tree is checked by P3 Gate.  This additional diff check makes
# the Phase 4 evidence explicit and prevents a local vendor edit from being
# hidden behind a rebuilt object.
$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate "vendor source tree is modified: $($vendorDiff[0])"
}

Write-Output 'P4_STATIC=PASS'

if ([string]::IsNullOrWhiteSpace($HardwareLog)) {
    Write-Output 'HARDWARE_GATE=UNAVAILABLE'
    Write-Output 'GATE_P4=UNAVAILABLE'
    exit 2
}

$hardwareLogPath = [System.IO.Path]::GetFullPath($HardwareLog)
Require-File $hardwareLogPath 'LiteOS hardware serial log'
$hardwareText = Get-Content -LiteralPath $hardwareLogPath -Raw

if ($hardwareText -match '(?i)qemu|std\s+vga|bochs|virtual\s+machine') {
    Fail-Gate 'QEMU or virtual-machine log cannot satisfy the real-hardware Gate'
}
foreach ($requiredHardwareText in @(
        'LITEOS_NVIDIA_PCI_FOUND\s+\S+\s+ID=10de:2f04',
        'LITEOS_NVIDIA_BAR0_READY', 'LITEOS_NVIDIA_BAR1_METADATA',
        'LITEOS_NVIDIA_RM_READY',
        'LITEOS_NVIDIA_ARCH=[0-9a-fA-F]{8}\s+IMPL=[0-9a-fA-F]{8}\s+HAL=[0-9a-fA-F]{8}',
        'LITEOS_NVIDIA_HAL_READY\s+BLACKWELL\s+GB205')) {
    if ($hardwareText -notmatch $requiredHardwareText) {
        Fail-Gate "hardware log evidence is absent: $requiredHardwareText"
    }
}
if ($hardwareText -match '(?i)fake|override|hard.?code|not_linked|probe_fail') {
    Fail-Gate 'hardware log contains a fake/override/failure marker'
}

Write-Output 'HARDWARE_GATE=PASS'
Write-Output 'GATE_P4=PASS'
