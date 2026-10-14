[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase-w3',
    [string]$ModeSetBuild = 'build/nvidia/modeset-target',
    [string]$Phase12Build = 'build/nvidia/phase12',
    [string]$Phase12RealTestLog = 'build-realtest/real-hardware/20260909-121533/realtest.log',
    [string]$RealTestLog = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Fail-Gate([string]$Message) { throw "W3 Gate failed: $Message" }

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

function Get-NmNames([string]$Tool, [string[]]$Arguments) {
    $lines = @(& $Tool @Arguments 2>&1 |
        ForEach-Object { $_.ToString() })
    if ($LASTEXITCODE -ne 0) { Fail-Gate "nm failed: $($Arguments -join ' ')" }
    return @($lines | ForEach-Object {
        $parts = $_ -split '\s+'
        if ($parts.Count -gt 0 -and $parts[0]) { $parts[0] }
    })
}

$w2Gate = Join-Path $repoRoot 'tools\nvidia\phase-w2_gate.ps1'
$w2Output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $w2Gate `
    -Phase12Build $Phase12Build -Phase12RealTestLog $Phase12RealTestLog 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $w2Output) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    @($w2Output | Where-Object { $_ -eq 'GATE_W2=PASS' }).Count -ne 1) {
    Fail-Gate 'previous W2 Gate is not passing'
}

$buildRoot = Resolve-RepoPath $Build
$modeRoot = Resolve-RepoPath $ModeSetBuild
$coreObject = Join-Path $modeRoot 'vkms-core.o'
$modeObjects = Join-Path $modeRoot 'objects.list'
$sourceList = Join-Path $repoRoot `
    'third_party\nvidia\595.84\open-gpu-kernel-modules\src\nvidia-modeset\srcs.mk'
$closureUndefined = Join-Path $buildRoot 'kernel-closure-undefined.list'
$closureObject = Join-Path $buildRoot 'kernel-closure.o'
$kernel = Join-Path $buildRoot 'kernel.elf'
$w3LinkList = Join-Path $buildRoot 'phase-w3_link_objects.list'
$backendSource = Join-Path $repoRoot 'kernel\drivers\display\nvidia\nv_nvkms_backend.c'
$osSource = Join-Path $repoRoot 'kernel\drivers\display\nvidia\nv_nvkms_os.c'
$runtimeSource = Join-Path $repoRoot 'kernel\init\runtime.c'
$bridgeSource = Join-Path $repoRoot `
    'kernel\drivers\gpu\nvidia\rm\nv_rm_vendor_bridge.c'
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84\open-gpu-kernel-modules'

foreach ($required in @(
        @($coreObject, 'linked official NVKMS core'),
        @($modeObjects, 'official NVKMS object list'),
        @($closureObject, 'complete W3 object closure'),
        @($closureUndefined, 'W3 closure undefined-symbol report'),
        @($kernel, 'W3 kernel image'),
        @($w3LinkList, 'W3 final link list'),
        @($sourceList, 'official NVKMS source list'),
        @($backendSource, 'System NVKMS display backend'),
        @($osSource, 'System NVKMS OS glue'),
        @($runtimeSource, 'W3 runtime hook'),
        @($bridgeSource, 'RM-to-NVKMS bridge'))) {
    Require-File $required[0] $required[1]
}

$sourceRows = @(Get-Content -LiteralPath $sourceList |
    Where-Object { $_ -match '^\s*SRCS(?:_CXX)?\s*\+=' })
if ($sourceRows.Count -ne 230) {
    Fail-Gate "official NVKMS source count is $($sourceRows.Count), expected 230"
}
$modeObjectRows = @(Get-Content -LiteralPath $modeObjects)
$officialObjectRows = @($modeObjectRows |
    Where-Object { $_ -match '/objects/\d{4}\.o$' })
if ($officialObjectRows.Count -ne 230) {
    Fail-Gate "target official object count is $($officialObjectRows.Count), expected 230"
}
foreach ($requiredRow in @(
        '/turing_shaders.xz.o', '/ampere_shaders.xz.o',
        '/hopper_shaders.xz.o', '/blackwell_shaders.xz.o',
        '/gb20x_shaders.xz.o', '/nvkms-id.o',
        '/system/nv_nvkms_os.o', '/system/nv_nvkms_backend.o',
        '/phase8/objects/nv_rm_vendor_bridge.o')) {
    if (@($modeObjectRows | Where-Object { $_ -like "*$requiredRow" }).Count -ne 1) {
        Fail-Gate "NVKMS object list is missing $requiredRow"
    }
}

$nm = (Get-Command x86_64-elf-nm -ErrorAction Stop).Source
$coreExports = Get-NmNames $nm @('-g', '--defined-only', '-P', '--', $coreObject)
foreach ($requiredExport in @(
        'nv_nvkms_init', 'nv_nvkms_display_init', 'nv_nvkms_ready',
        'nvkms_call_rm', 'nvkms_open_gpu', 'nvkms_enumerate_gpus',
        'nv_rm_vendor_kms_call_rm', 'nv_rm_vendor_kms_open_gpu',
        'nv_rm_vendor_kms_enumerate_gpus')) {
    if ($requiredExport -notin $coreExports) {
        Fail-Gate "NVKMS core export is missing: $requiredExport"
    }
}

$coreUndefined = Get-NmNames $nm @('-u', '-P', '--', $coreObject)
if (@($coreUndefined | Where-Object {
        $_ -match '^nvkms_' -or $_ -match '^nv_rm_vendor_kms_'
    }).Count -ne 0) {
    Fail-Gate 'NVKMS core still has unresolved port/bridge symbols'
}
if ((Get-Content -LiteralPath $closureUndefined -Raw).Trim() -ne '# none') {
    Fail-Gate 'complete W3 object closure has unresolved symbols'
}
$finalUndefined = @(& $nm '-u' '-P' '--' $kernel 2>&1 |
    ForEach-Object { $_.ToString() } | Where-Object { $_.Trim() })
if ($LASTEXITCODE -ne 0 -or $finalUndefined.Count -ne 0) {
    Fail-Gate 'W3 final kernel has unresolved symbols'
}

$backendText = Get-Content -LiteralPath $backendSource -Raw
foreach ($requiredText in @(
        'allocateMemory', 'mapMemory', 'applyModeSetConfig',
        'display_core_register_scanout_backend',
        'OS_NVKMS_NATIVE_MODESET_FAIL',
        'gpu_allocation_cpu_map', 'nvkms_copy_request_to_surface')) {
    Require-Text $backendText $requiredText 'real KAPI display backend path'
}
$osText = Get-Content -LiteralPath $osSource -Raw
foreach ($requiredText in @(
        'nvKmsModuleLoad', 'nvKmsKapiGetFunctionsTableInternal',
        'nvkms_call_rm', 'nv_port_mutex', 'nv_port_thread',
        'nv_nvkms_init', 'nv_nvkms_display_init')) {
    Require-Text $osText $requiredText 'System NVKMS OS interface implementation'
}
$runtimeText = Get-Content -LiteralPath $runtimeSource -Raw
foreach ($requiredText in @(
        'OS_NVKMS_W3', 'nv_nvkms_bootstrap_init',
        'OS_NVKMS_NATIVE_SCANOUT_FALLBACK')) {
    Require-Text $runtimeText $requiredText 'guarded W3 runtime entry'
}
$bridgeText = Get-Content -LiteralPath $bridgeSource -Raw
foreach ($requiredText in @(
        'nv_rm_vendor_kms_call_rm', 'rm_kernel_rmapi_op',
        'nv_rm_vendor_kms_enumerate_gpus')) {
    Require-Text $bridgeText $requiredText 'real RM/KAPI bridge boundary'
}

$allW3Rows = @((Get-Content -LiteralPath $modeObjects) +
               (Get-Content -LiteralPath $w3LinkList))
if (@($allW3Rows | Where-Object { $_ -match '(?i)nvidia-drm|nvidia_drm' }).Count -ne 0) {
    Fail-Gate 'Linux nvidia-drm wrapper was included in the W3 target'
}

$gitArguments = @('-C', $repoRoot, 'diff', '--name-only', '--',
                  'third_party/nvidia/595.84/open-gpu-kernel-modules')
$vendorDiff = @(& git @gitArguments 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0 -or $vendorDiff.Count -ne 0) {
    Fail-Gate '595.84 vendor source tree is modified'
}
$writableVendorFiles = @(Get-ChildItem -LiteralPath $vendorRoot -Recurse -File |
    Where-Object { -not $_.IsReadOnly })
if ($writableVendorFiles.Count -ne 0) {
    Fail-Gate "vendor source is writable: $($writableVendorFiles[0].FullName)"
}

Write-Output 'W3_PREVIOUS_GATE=PASS'
Write-Output 'W3_OFFICIAL_SOURCE_COUNT=230'
Write-Output 'W3_KAPI_RM_BRIDGE=PASS'
Write-Output 'W3_OS_DISPLAY_BACKEND=PASS'
Write-Output 'W3_FULL_CLOSURE=PASS'
Write-Output 'W3_NO_NVIDIA_DRM=PASS'
Write-Output 'W3_VENDOR_IMMUTABILITY=PASS'
if ([string]::IsNullOrWhiteSpace($RealTestLog)) {
    Write-Output 'W3_NATIVE_RUNTIME_GATE=NOT_RUN'
} else {
    $realLog = Resolve-RepoPath $RealTestLog
    Require-File $realLog 'W3 real-hardware log'
    $realText = Get-Content -LiteralPath $realLog -Raw
    foreach ($requiredText in @(
            'OS_NVKMS_READY version=595.84',
            'OS_NVKMS_NATIVE_MODESET_OK',
            'OS_NVKMS_NATIVE_SCANOUT_READY',
            'OS_NVKMS_NATIVE_COMPOSITOR_ACCEPTED',
            'OS_REALTEST_PASS',
            'OS_REALTEST_REBOOTING')) {
        Require-Text $realText $requiredText 'W3 native real-hardware result'
    }
    $compositorMarker = 'OS_NVKMS_NATIVE_COMPOSITOR_ACCEPTED'
    $compositorIndex = $realText.IndexOf($compositorMarker,
                                         [StringComparison]::Ordinal)
    if ($compositorIndex -lt 0) {
        Fail-Gate 'compositor submission marker is missing'
    }
    $postCompositorText = $realText.Substring(
        $compositorIndex + $compositorMarker.Length)
    foreach ($requiredText in @(
            'OS_NVKMS_NATIVE_APPLY_OK',
            'OS_NVKMS_NATIVE_EVENT_FLIP')) {
        Require-Text $postCompositorText $requiredText `
            'W3 compositor native flip result'
    }
    if ($postCompositorText -match
        'OS_NVKMS_NATIVE_(?:COPY_MAP_FAIL|APPLY_FAIL|ATOMIC_FAIL)') {
        Fail-Gate 'W3 compositor native path contains a failed commit'
    }
    if ($realText -match 'OS_NVKMS_NATIVE_SCANOUT_FALLBACK|OS_NVKMS_NATIVE_MODESET_FAIL') {
        Fail-Gate 'W3 real-hardware log contains native modeset fallback/failure'
    }
    $resultPath = Join-Path (Split-Path -Parent $realLog) 'realtest.result'
    Require-File $resultPath 'W3 real-hardware result metadata'
    Require-Text (Get-Content -LiteralPath $resultPath -Raw) 'status=pass' `
        'W3 host returned to Windows'
    Write-Output 'W3_NATIVE_RUNTIME_GATE=PASS'
    Write-Output 'W3_NATIVE_COMPOSITOR_GATE=PASS'
}
Write-Output 'GATE_W3=PASS'
