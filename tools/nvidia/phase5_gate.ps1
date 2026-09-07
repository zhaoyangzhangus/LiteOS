[CmdletBinding()]
param(
    [string[]]$HardwareLog = @()
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRelative = 'third_party/nvidia/595.84/open-gpu-kernel-modules'
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$phase3Root = Join-Path $repoRoot 'build\nvidia\phase3'
$phase4Root = Join-Path $repoRoot 'build\nvidia\phase4'
$phase5Root = Join-Path $repoRoot 'build\nvidia\phase5'
$phase5Kernel = Join-Path $phase5Root 'kernel.elf'
$phase5Esp = Join-Path $phase5Root 'esp'
$linkList = Join-Path $phase5Root 'phase5_link_objects.list'
$firmwareManifest = Join-Path $phase5Root 'firmware.manifest'
$firmwareRoot = Join-Path $repoRoot 'build\nvidia\official-595.84\extracted-gitbash\firmware'
$bridgeSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_vendor_bridge.c'
$driverSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm\nv_rm_driver.c'
$firmwareSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\vendor_abi\nvabi_firmware.c'
$unsupportedSource = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\vendor_abi\nvabi_nv_unsupported.S'
$phase5UnsupportedObject = Join-Path $phase5Root 'objects\vendor_abi\0006.o'
$phase5MemoryObject = Join-Path $phase5Root 'objects\vendor_abi\0003.o'
$runtimeSource = Join-Path $repoRoot 'kernel\init\runtime.c'
$devicesSource = Join-Path $repoRoot 'kernel\init\devices.c'

function Fail-Gate([string]$Message) {
    throw "P5 Gate failed: $Message"
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail-Gate "missing $Description`: $Path"
    }
}

function Require-Text([string]$Text, [string]$Pattern, [string]$Description) {
    if ($Text -notmatch [regex]::Escape($Pattern)) {
        Fail-Gate "required evidence is absent: $Description"
    }
}

Require-File $phase5Kernel 'Phase 5 kernel image'
Require-File $linkList 'Phase 5 link list'
Require-File $firmwareManifest 'Phase 5 firmware manifest'
Require-File (Join-Path $phase4Root 'kernel.elf') 'Phase 4 prerequisite image'
Require-File (Join-Path $phase3Root 'vendor-rm.o') 'Phase 3 vendor object'
Require-File $bridgeSource 'Phase 5 vendor bridge source'
Require-File $driverSource 'Phase 5 driver source'
Require-File $firmwareSource 'Phase 5 firmware adapter source'
Require-File $unsupportedSource 'vendor ABI source'
Require-File $phase5UnsupportedObject 'Phase 5 ABI object without firmware fallback'
Require-File $phase5MemoryObject 'Phase 5 incremental memory ABI object'
Require-File $runtimeSource 'Phase 5 runtime source'
Require-File $devicesSource 'Phase 5 device-init source'

$vendorDiff = @(& git -C $repoRoot diff --name-only -- $vendorRelative 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'git vendor diff check failed' }
if (@($vendorDiff | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate "vendor source tree is modified: $($vendorDiff[0])"
}

# The previous Gate is represented by the recorded real-machine Phase 4
# trace.  The trace is checked for all hardware facts used by P4; no fresh
# full P3/P4 build is triggered here.
$priorLogsRoot = Join-Path $repoRoot 'build-realtest\real-hardware'
$p4Evidence = @()
if (Test-Path -LiteralPath $priorLogsRoot -PathType Container) {
    $p4Evidence = @(Get-ChildItem -LiteralPath $priorLogsRoot -Recurse -File -Filter 'realtest.log' |
        ForEach-Object {
            $text = Get-Content -LiteralPath $_.FullName -Raw
            if ($text -match 'LITEOS_NVIDIA_PCI_FOUND\s+\S+\s+ID=10de:2f04' -and
                $text -match 'LITEOS_NVIDIA_BAR0_READY' -and
                $text -match 'LITEOS_NVIDIA_BAR1_METADATA' -and
                $text -match 'LITEOS_NVIDIA_PHASE4_RM_READY' -and
                $text -match 'LITEOS_NVIDIA_PHASE4_GPU_OBJECT_READY' -and
                $text -match 'LITEOS_NVIDIA_PHASE4_HAL_READY\s+BLACKWELL\s+GB205' -and
                $text -match 'LITEOS_NVIDIA_PHASE4_GATE_READY' -and
                $text -match 'LITEOS_REALTEST_PASS') {
                $_.FullName
            }
        })
}
if ($p4Evidence.Count -eq 0) { Fail-Gate 'no recorded Phase 4 hardware Gate evidence' }
Write-Output ('P4_PREREQUISITE=PASS log={0}' -f $p4Evidence[0])

$readelf = Get-Command x86_64-elf-readelf -ErrorAction SilentlyContinue
if ($null -eq $readelf) { $readelf = Get-Command readelf -ErrorAction Stop }
$nm = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nm) { $nm = Get-Command nm -ErrorAction Stop }

$header = @(& $readelf.Source '-h' '--' $phase5Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($header | Where-Object { $_ -match 'Class:\s+ELF64' }).Count -ne 1 -or
    @($header | Where-Object { $_ -match 'Type:\s+EXEC' }).Count -ne 1 -or
    @($header | Where-Object { $_ -match 'Machine:\s+.*X86-64' }).Count -ne 1) {
    Fail-Gate 'Phase 5 image is not an x86-64 ELF executable'
}

$programHeaders = @(& $readelf.Source '-l' '--' $phase5Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or
    @($programHeaders | Where-Object { $_ -match '^\s*LOAD\s+' }).Count -lt 2 -or
    @($programHeaders | Where-Object { $_ -match '\sR E\s+' }).Count -eq 0 -or
    @($programHeaders | Where-Object { $_ -match '\sRW\s+' }).Count -eq 0) {
    Fail-Gate 'Phase 5 image load segments are incomplete'
}

$symbols = @(& $nm.Source '-g' '--defined-only' '--' $phase5Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'Phase 5 symbol scan failed' }
foreach ($requiredSymbol in @(
        'kernel_entry', 'nv_rm_driver_probe', 'nv_rm_vendor_start',
        'rm_init_adapter', 'nv_get_firmware', 'nv_put_firmware',
        'sha256_compute')) {
    if (@($symbols | Where-Object {
            $_ -match ('\s{0}\s*$' -f [regex]::Escape($requiredSymbol))
        }).Count -eq 0) {
        Fail-Gate "required symbol is absent: $requiredSymbol"
    }
}
$undefined = @(& $nm.Source '-u' '-P' '--' $phase5Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { Fail-Gate 'Phase 5 undefined-symbol scan failed' }
if (@($undefined | Where-Object { $_.Trim() }).Count -ne 0) {
    Fail-Gate "Phase 5 image has unresolved symbols: $($undefined[0])"
}

$linkRows = @(Get-Content -LiteralPath $linkList |
    Where-Object { $_ -and $_[0] -ne '#' })
if (@($linkRows | Where-Object { $_ -like '*phase3/vendor-rm.o' }).Count -ne 1 -or
    @($linkRows | Where-Object { $_ -like '*phase4/objects/nv_rm_workqueue.o' }).Count -ne 1 -or
    @($linkRows | Where-Object { $_ -like '*phase5/objects/nv_rm_vendor_bridge.o' }).Count -ne 1 -or
    @($linkRows | Where-Object { $_ -like '*phase5/objects/vendor_abi/nvabi_firmware.o' }).Count -ne 1 -or
    @($linkRows | Where-Object { $_ -like '*phase5/objects/sha256.o' }).Count -ne 1 -or
    @($linkRows | Where-Object { $_ -like '*phase4/objects/vendor_abi/*.o' }).Count -ne 9 -or
    @($linkRows | Where-Object { $_ -like '*phase5/objects/vendor_abi/0003.o' }).Count -ne 1 -or
    @($linkRows | Where-Object { $_ -like '*phase5/objects/vendor_abi/0006.o' }).Count -ne 1) {
    Fail-Gate 'Phase 5 link list does not contain the incremental RM/firmware closure'
}

$bridgeText = Get-Content -LiteralPath $bridgeSource -Raw
foreach ($requiredBridgeText in @(
        'state->request_firmware = NV_TRUE;', 'rm_init_adapter(',
        'nv_rm_vendor_start_irq(', 'LITEOS_NVIDIA_GSP_BOOT_START',
        'LITEOS_NVIDIA_GSP_BOOT_FINISH status=ready',
        'allow_fallback_to_monolithic_rm = NV_FALSE;',
        'enable_firmware_logs = NV_FALSE;')) {
    Require-Text $bridgeText $requiredBridgeText 'vendor bridge GSP lifecycle'
}
if ($bridgeText -match '(?im)\b(gmmu|pma|gpu\s*va)\b\s*[:=]') {
    Fail-Gate 'Phase 5 bridge contains a LiteOS-side GPU-owned implementation'
}
if ($bridgeText -notmatch '(?s)if\s*\(!rm_init_adapter\([^)]*\)\)') {
    Fail-Gate 'Phase 5 bridge does not fail closed on rm_init_adapter'
}

$firmwareText = Get-Content -LiteralPath $firmwareSource -Raw
foreach ($requiredFirmwareText in @(
        'vfs_open_kernel(', 'vfs_read_kernel(', 'sha256_compute(',
        'nvabi_firmware_hash_matches(', 'kfree(data)',
        'LITEOS_NVIDIA_FIRMWARE_REQUEST',
        'LITEOS_NVIDIA_FIRMWARE_RESOLVED',
        'expectation=595.84')) {
    Require-Text $firmwareText $requiredFirmwareText 'immutable firmware loading/lifetime metadata'
}
if ($firmwareText -match '(?im)\b(gmmu|pma|gpu\s*va|rpc)\b\s*[:=]') {
    Fail-Gate 'firmware adapter contains a forbidden GPU-owned implementation'
}
if ($unsupportedSource) {
    $phase5UnsupportedSymbols = @(& $nm.Source '-g' '--defined-only' '--' $phase5UnsupportedObject 2>&1 |
        ForEach-Object { $_.ToString() })
    if ($LASTEXITCODE -ne 0 -or @($phase5UnsupportedSymbols | Where-Object {
            $_ -match '\s+nv_(get|put)_firmware\s*$'
        }).Count -ne 0) {
        Fail-Gate 'Phase 5 ABI object still defines a fail-closed firmware entry point'
    }
}

$driverText = Get-Content -LiteralPath $driverSource -Raw
$runtimeText = Get-Content -LiteralPath $runtimeSource -Raw
$devicesText = Get-Content -LiteralPath $devicesSource -Raw
Require-Text $driverText 'NV_GPU_GSP_READY' 'driver GSP state'
Require-Text $runtimeText 'nv_rm_driver_probe()' 'post-filesystem Phase 5 probe'
Require-Text $devicesText 'LITEOS_NVIDIA_PROBE_DEFERRED' 'deferred early probe'
if ($driverText -match '(?im)state\s*=\s*NV_GPU_GSP_READY\s*;(?s).*?nv_rm_vendor_start') {
    Fail-Gate 'driver marks GSP ready before the vendor start boundary'
}

$expectedFirmware = @{
    'gsp_ga10x.bin' = @{ Size = 72865776; Hash = '1C107D18CD63A08FD4BA78706809825C756DB2B4A556C453FB5C003976839946' }
    'gsp_tu10x.bin' = @{ Size = 30065536; Hash = '76E4D216B8F80C995F97E114E7AA1B87475F6D694CB78D78CE2F52070FB30739' }
}
foreach ($firmwareName in $expectedFirmware.Keys) {
    $source = Join-Path $firmwareRoot $firmwareName
    $staged = Join-Path $phase5Esp ('EFI\LITEOS\firmware\{0}' -f $firmwareName)
    Require-File $source "595.84 firmware $firmwareName"
    Require-File $staged "staged firmware $firmwareName"
    foreach ($path in @($source, $staged)) {
        $info = Get-Item -LiteralPath $path
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($info.Length -ne $expectedFirmware[$firmwareName].Size -or
            $hash -ne $expectedFirmware[$firmwareName].Hash) {
            Fail-Gate "firmware provenance mismatch: $path"
        }
    }
}
Write-Output 'P5_STATIC=PASS'

$usableLogs = @()
foreach ($path in $HardwareLog) {
    if ([string]::IsNullOrWhiteSpace($path)) { continue }
    $fullPath = [IO.Path]::GetFullPath($path)
    Require-File $fullPath 'Phase 5 hardware serial log'
    $usableLogs += $fullPath
}
$usableLogs = @($usableLogs | Sort-Object -Unique)
if ($usableLogs.Count -lt 100) {
    Write-Output ('HARDWARE_GATE=UNAVAILABLE logs={0}/100' -f $usableLogs.Count)
    Write-Output 'GATE_P5=UNAVAILABLE'
    exit 2
}

$seenRuns = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($path in $usableLogs) {
    $text = Get-Content -LiteralPath $path -Raw
    if ($text -match '(?i)qemu|std\s+vga|bochs|virtual\s+machine') {
        Fail-Gate "QEMU or virtual-machine log cannot satisfy the real-hardware Gate: $path"
    }
    foreach ($requiredHardwareText in @(
            'LITEOS_NVIDIA_GSP_BOOT_START',
            'LITEOS_NVIDIA_FIRMWARE_RESOLVED path=\S+ size=\d+ sha256=\S+ expectation=595\.84 hash_ok=1',
            'LITEOS_NVIDIA_GSP_BOOT_FINISH status=ready',
            'LITEOS_NVIDIA_GSP_READY',
            'LITEOS_NVIDIA_PHASE5_GATE_READY',
            'LITEOS_REALTEST_PASS',
            'LITEOS_REALTEST_REBOOTING')) {
        if ($text -notmatch $requiredHardwareText) {
            Fail-Gate "hardware log evidence is absent: $requiredHardwareText ($path)"
        }
    }
    if ($text -match '(?i)fake|override|hard.?code|not_linked|probe_fail|GSP_BOOT_FAIL') {
        Fail-Gate "hardware log contains a fake/override/failure marker: $path"
    }
    $runMatch = [regex]::Match($text, '(?m)^run_id=(\S+)$')
    if (-not $runMatch.Success -or
        -not $seenRuns.Add($runMatch.Groups[1].Value)) {
        Fail-Gate "hardware log does not identify a unique cold boot: $path"
    }
}

Write-Output ('HARDWARE_GATE=PASS cold_boots={0}' -f $usableLogs.Count)
Write-Output 'GATE_P5=PASS'
