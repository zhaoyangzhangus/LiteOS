[CmdletBinding()]
param(
    [string]$Make = 'make'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$abiSourceRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\vendor_abi'
$rmSourceRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\rm'
$phase3Root = Join-Path $repoRoot 'build\nvidia\phase3'
$outputRoot = Join-Path $repoRoot 'build\nvidia\phase4'
$objectRoot = Join-Path $outputRoot 'objects'
$liteosObjectRoot = Join-Path $objectRoot 'liteos'
$abiObjectRoot = Join-Path $objectRoot 'vendor_abi'
$strongBridgeObject = Join-Path $objectRoot 'nv_rm_vendor_bridge.o'
$linkList = Join-Path $outputRoot 'phase4_link_objects.list'
$abiLinkList = Join-Path $outputRoot 'phase4_abi_link_objects.list'
$kernelElf = Join-Path $outputRoot 'kernel.elf'
$phase4Esp = Join-Path $outputRoot 'esp'
$linkerScript = Join-Path $PSScriptRoot 'phase4-kernel.ld'
$phase3Gate = Join-Path $PSScriptRoot 'phase3_gate.ps1'
$tab = [char]9

function Write-AsciiLines([string]$Path, [System.Collections.IEnumerable]$Lines) {
    [System.IO.File]::WriteAllLines($Path, [string[]]$Lines,
                                    [System.Text.Encoding]::ASCII)
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "missing $Description`: $Path"
    }
}

if (-not (Test-Path -LiteralPath $phase3Gate -PathType Leaf)) {
    throw "P3 Gate is missing: $phase3Gate"
}

# A Phase 4 build is downstream of the completed Phase 3 Gate.  The Gate
# performs the P2 Gate and revalidates the immutable vendor graph before this
# script is allowed to link any RM object into a kernel image.
$gateArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $phase3Gate,
              '-Make', $Make)
$gateOutput = @(& powershell.exe @gateArgs 2>&1 |
    ForEach-Object { $_.ToString() })
$gateExitCode = $LASTEXITCODE
foreach ($line in $gateOutput) { Write-Output $line }
if ($gateExitCode -ne 0 -or
    @($gateOutput | Where-Object { $_ -eq 'GATE_P3=PASS' }).Count -ne 1) {
    throw 'P3 Gate did not pass; Phase 4 build is not permitted'
}

Require-File (Join-Path $phase3Root 'vendor-rm.o') 'Phase 3 RM object'
Require-File (Join-Path $phase3Root 'vendor_abi_link_objects.list') 'Phase 3 ABI link list'
Require-File (Join-Path $phase3Root 'liteos_probe_objects.list') 'Phase 3 LiteOS object list'
Require-File $linkerScript 'Phase 4 linker script'

$makeOutput = @(& $Make 'loader' 'kernel' 2>&1 |
    ForEach-Object { $_.ToString() })
$makeExitCode = $LASTEXITCODE
foreach ($line in $makeOutput) { Write-Output $line }
if ($makeExitCode -ne 0) { throw 'production loader/kernel build failed' }

$gccCommand = Get-Command x86_64-elf-gcc -ErrorAction Stop
$linkerCommand = Get-Command x86_64-elf-ld -ErrorAction Stop
$peObjcopyCommand = Get-Command x86_64-w64-mingw32-objcopy -ErrorAction Stop
$gcc = $gccCommand.Source
$linker = $linkerCommand.Source
$peObjcopy = $peObjcopyCommand.Source

New-Item -ItemType Directory -Path $liteosObjectRoot -Force | Out-Null
New-Item -ItemType Directory -Path $abiObjectRoot -Force | Out-Null
New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$liteosInputRoot = Join-Path $repoRoot 'build\kernel'
$liteosFiles = @(Get-ChildItem -LiteralPath $liteosInputRoot -File -Filter '*.o' |
    Sort-Object Name)
if ($liteosFiles.Count -eq 0) { throw 'production kernel object directory is empty' }

$liteosObjects = New-Object 'System.Collections.Generic.List[string]'
for ($index = 0; $index -lt $liteosFiles.Count; $index++) {
    $source = $liteosFiles[$index].FullName
    $target = Join-Path $liteosObjectRoot ('{0:D4}.elf.o' -f ($index + 1))
    $diagnostics = @(& $peObjcopy '-O' 'elf64-x86-64' '--' $source $target 2>&1 |
        ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    foreach ($line in $diagnostics) { Write-Output $line }
    if ($exitCode -ne 0 -or -not (Test-Path -LiteralPath $target -PathType Leaf)) {
        throw "LiteOS object conversion failed: $source"
    }
    [void]$liteosObjects.Add($target.Replace('\', '/'))
}

# The vendor source include graph intentionally selects src/nvidia's Unix
# nv.h.  The kernel-open OS interface header is reserved for the ABI objects;
# mixing its generated rs_access.h with the RM NVOC headers changes the type
# graph and is rejected by the compiler.
$cpuOpsysHeader = Join-Path $vendorSourceRoot 'src\common\sdk\nvidia\inc\cpuopsys.h'
$rmIncludesRelative = @(
    'src\nvidia\kernel\inc',
    'src\nvidia\interface',
    'src\common\sdk\nvidia\inc',
    'src\common\sdk\nvidia\inc\hw',
    'src\nvidia\arch\nvalloc\common\inc',
    'src\nvidia\arch\nvalloc\common\inc\gsp',
    'src\nvidia\arch\nvalloc\common\inc\deprecated',
    'src\nvidia\arch\nvalloc\unix\include',
    'src\nvidia\inc',
    'src\nvidia\inc\os',
    'src\common\shared\inc',
    'src\common\inc',
    'src\common\uproc\os\libos-v2.0.0\include',
    'src\common\uproc\os\common\include',
    'src\common\inc\swref',
    'src\common\inc\swref\published',
    'src\nvidia\generated',
    'src\common\nvswitch\kernel\inc',
    'src\common\nvswitch\interface',
    'src\common\nvswitch\common\inc',
    'src\common\inc\displayport',
    'src\common\nvlink\interface',
    'src\common\nvlink\inband\interface',
    'src\nvidia\src\mm\uvm\interface',
    'src\nvidia\inc\libraries',
    'src\nvidia\src\libraries',
    'src\nvidia\inc\kernel',
    'src\nvidia\src\libraries\libspdm\3.5.0\include',
    'src\nvidia\src\libraries\libspdm\3.5.0\include\hal',
    'src\nvidia\src\libraries\libspdm\3.5.0\os_stub\include',
    'src\nvidia\src\libraries\libspdm\3.5.0\os_stub',
    'src\nvidia\src\libraries\libspdm\3.5.0\os_stub\cryptlib_null',
    'src\nvidia\src\libraries\libspdm\nvidia',
    'src\common\mbedtls\3.6.2\include',
    'src\common\mbedtls\3.6.2\nvidia'
)
$bridgeIncludes = @(
    ('-I' + (Join-Path $repoRoot 'include')),
    ('-I' + (Join-Path $rmSourceRoot 'include'))
)
$bridgeIncludes += @($rmIncludesRelative | ForEach-Object {
    '-I' + (Join-Path $vendorSourceRoot $_)
})
$bridgeDefines = @(
    '-DNVRM', '-DNV_LINUX', '-DNV_UNIX', '-DNV_X86_64', '-DNV_ARCH_BITS=64',
    '-D_LANGUAGE_C', '-D__NO_CTYPE', '-DLOCK_VAL_ENABLED=0',
    '-DPORT_ATOMIC_64_BIT_SUPPORTED=1', '-DPORT_IS_KERNEL_BUILD=1',
    '-DPORT_IS_CHECKED_BUILD=0', '-DPORT_MODULE_atomic=1', '-DPORT_MODULE_core=1',
    '-DPORT_MODULE_cpu=1', '-DPORT_MODULE_crypto=1', '-DPORT_MODULE_debug=1',
    '-DPORT_MODULE_memory=1', '-DPORT_MODULE_safe=1', '-DPORT_MODULE_string=1',
    '-DPORT_MODULE_sync=1', '-DPORT_MODULE_thread=1', '-DPORT_MODULE_util=1',
    '-DPORT_MODULE_example=0', '-DPORT_MODULE_mmio=0', '-DPORT_MODULE_time=0',
    '-DRS_STANDALONE=0', '-DRS_STANDALONE_TEST=0', '-DRS_COMPATABILITY_MODE=1',
    '-DRS_PROVIDES_API_STATE=0', '-DNV_CONTAINERS_NO_TEMPLATES',
    '-DINCLUDE_NVLINK_LIB', '-DINCLUDE_NVSWITCH_LIB',
    '-DNV_PRINTF_STRINGS_ALLOWED=0', '-DNV_ASSERT_FAILED_USES_STRINGS=0',
    '-DPORT_ASSERT_FAILED_USES_STRINGS=0', '-DLITEOS_KERNEL_BUILD'
)
$bridgeFlags = @(
    '-include', $cpuOpsysHeader
) + $bridgeIncludes + $bridgeDefines + @(
    '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-Wformat=2',
    '-Wno-unused-parameter', '-O2', '-ffreestanding', '-fno-stack-protector',
    '-fno-common', '-fno-pic', '-ffunction-sections', '-fdata-sections',
    '-msoft-float', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
    '-mno-3dnow', '-Werror-implicit-function-declaration', '-Wwrite-strings',
    '-Wundef', '--std=gnu11', '-mcmodel=kernel'
)

$bridgeArgs = @($bridgeFlags) + @(
    '-c', (Join-Path $rmSourceRoot 'nv_rm_vendor_bridge.c'),
    '-o', $strongBridgeObject
)
$bridgeDiagnostics = @(& $gcc @bridgeArgs 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $bridgeDiagnostics) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    -not (Test-Path -LiteralPath $strongBridgeObject -PathType Leaf)) {
    throw 'strong Phase 4 vendor bridge compilation failed'
}

# Recompile the 11 linkable ABI adapters with the kernel code model.  Phase 3
# intentionally compiles them as a relocatable ABI probe; their absolute
# diagnostic-string relocations are not valid once the final Phase 4 image is
# placed at LiteOS' high-half address.
$abiIncludeArgs = @(
    ('-I' + (Join-Path $repoRoot 'include')),
    ('-I' + (Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\port\include')),
    ('-I' + (Join-Path $abiSourceRoot 'include')),
    ('-I' + (Join-Path $vendorSourceRoot 'kernel-open')),
    ('-I' + (Join-Path $vendorSourceRoot 'kernel-open\common\inc'))
)
$abiIncludeArgs += @($rmIncludesRelative | ForEach-Object {
    '-I' + (Join-Path $vendorSourceRoot $_)
})
$abiDefines = @(
    '-DNVRM', '-DNV_LINUX', '-DNV_UNIX', '-DNV_X86_64', '-DNV_ARCH_BITS=64',
    '-D_LANGUAGE_C', '-D__NO_CTYPE', '-DLOCK_VAL_ENABLED=0',
    '-DPORT_ATOMIC_64_BIT_SUPPORTED=1', '-DPORT_IS_KERNEL_BUILD=1',
    '-DPORT_IS_CHECKED_BUILD=0', '-DPORT_MODULE_atomic=1', '-DPORT_MODULE_core=1',
    '-DPORT_MODULE_cpu=1', '-DPORT_MODULE_crypto=1', '-DPORT_MODULE_debug=1',
    '-DPORT_MODULE_memory=1', '-DPORT_MODULE_safe=1', '-DPORT_MODULE_string=1',
    '-DPORT_MODULE_sync=1', '-DPORT_MODULE_thread=1', '-DPORT_MODULE_util=1',
    '-DPORT_MODULE_example=0', '-DPORT_MODULE_mmio=0', '-DPORT_MODULE_time=0',
    '-DRS_STANDALONE=0', '-DRS_STANDALONE_TEST=0', '-DRS_COMPATABILITY_MODE=1',
    '-DRS_PROVIDES_API_STATE=0', '-DNV_CONTAINERS_NO_TEMPLATES',
    '-DINCLUDE_NVLINK_LIB', '-DINCLUDE_NVSWITCH_LIB',
    '-DNV_PRINTF_STRINGS_ALLOWED=0', '-DNV_ASSERT_FAILED_USES_STRINGS=0',
    '-DPORT_ASSERT_FAILED_USES_STRINGS=0', '-DLITEOS_KERNEL_BUILD'
)
$abiCommonFlags = @(
    '-fno-strict-aliasing', '-fno-omit-frame-pointer', '-Wformat=2',
    '-Wno-unused-parameter', '-O2', '-ffreestanding', '-fno-stack-protector',
    '-fno-common', '-fno-pic', '-ffunction-sections', '-fdata-sections',
    '-msoft-float', '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2',
    '-mno-3dnow', '-Werror-implicit-function-declaration', '-Wwrite-strings',
    '-Wundef'
)
$abiCFlags = @('-include', $cpuOpsysHeader) + $abiIncludeArgs +
             $abiDefines + $abiCommonFlags + @('--std=gnu11', '-mcmodel=kernel')
$abiAsmFlags = @(
    '-ffreestanding', '-fno-stack-protector', '-fno-common', '-fno-pic',
    '-mno-red-zone', '-mno-mmx', '-mno-sse', '-mno-sse2', '-mno-3dnow',
    '-mcmodel=kernel'
)
$abiObjects = New-Object 'System.Collections.Generic.List[string]'
$abiObjectRows = @(Get-Content -LiteralPath (Join-Path $phase3Root 'vendor_abi_objects.txt') |
    Where-Object { $_ -and $_[0] -ne '#' })
foreach ($row in $abiObjectRows) {
    $fields = $row -split "`t", -1
    if ($fields.Count -ne 8 -or $fields[0] -ne 'compiled') {
        throw "Phase 3 ABI object report is malformed: $row"
    }
    if ($fields[4] -like '*vendor_abi/nvabi_contracts.c') { continue }

    $abiSource = Join-Path $repoRoot ($fields[4].Replace('/', '\'))
    $abiObject = Join-Path $abiObjectRoot ((Split-Path -Leaf $fields[5]))
    $abiArgs = if ($fields[2] -eq 'asm') { @($abiAsmFlags) } else { @($abiCFlags) }
    $abiArgs += @('-c', $abiSource, '-o', $abiObject)
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $abiDiagnostics = @(& $gcc @abiArgs 2>&1 |
            ForEach-Object { $_.ToString() })
        $abiExitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    foreach ($line in $abiDiagnostics) { Write-Output $line }
    if ($abiExitCode -ne 0 -or
        -not (Test-Path -LiteralPath $abiObject -PathType Leaf)) {
        throw "Phase 4 ABI object compilation failed: $abiSource"
    }
    [void]$abiObjects.Add($abiObject.Replace('\', '/'))
}
if ($abiObjects.Count -ne 11) {
    throw "Phase 4 ABI link object count is $($abiObjects.Count), expected 11"
}
Write-AsciiLines $abiLinkList $abiObjects

$linkObjects = New-Object 'System.Collections.Generic.List[string]'
foreach ($object in $liteosObjects) { [void]$linkObjects.Add($object) }
[void]$linkObjects.Add((Join-Path $phase3Root 'vendor-rm.o').Replace('\', '/'))
foreach ($object in $abiObjects) { [void]$linkObjects.Add($object) }
[void]$linkObjects.Add($strongBridgeObject.Replace('\', '/'))
Write-AsciiLines $linkList $linkObjects

$linkArgs = @(
    '--gc-sections', '--allow-multiple-definition', '-T', $linkerScript,
    '-o', $kernelElf, ('@' + $linkList)
)
$linkDiagnostics = @(& $linker @linkArgs 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $linkDiagnostics) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $kernelElf -PathType Leaf)) {
    throw 'Phase 4 RM/LiteOS final link failed'
}

New-Item -ItemType Directory -Path $phase4Esp -Force | Out-Null
Copy-Item -Path (Join-Path $repoRoot 'build\esp\*') -Destination $phase4Esp -Recurse -Force
New-Item -ItemType Directory -Path (Join-Path $phase4Esp 'EFI\LITEOS') -Force |
    Out-Null
Copy-Item -LiteralPath $kernelElf -Destination (Join-Path $phase4Esp 'EFI\LITEOS\kernel.elf') -Force

Write-Output ('PHASE4_KERNEL={0}' -f $kernelElf)
Write-Output ('PHASE4_ESP={0}' -f $phase4Esp)
Write-Output ('PHASE4_LITEOS_OBJECTS={0}' -f $liteosObjects.Count)
Write-Output ('PHASE4_LINK_OBJECTS={0}' -f $linkObjects.Count)
Write-Output 'PHASE4_BUILD=PASS'
