[CmdletBinding()]
param(
    [string]$Make = 'make.exe',
    [string]$Linker = 'x86_64-elf-ld',
    [string]$Nm = 'x86_64-elf-nm',
    [string]$Phase12Build = 'build/nvidia/phase12',
    [string]$Phase12RealTestLog = 'build-realtest/real-hardware/20260909-121533/realtest.log',
    [switch]$RealTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'object-paths.ps1')

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$phase9Root = Join-Path $repoRoot 'build\nvidia\phase9'
$phase9LinkList = Join-Path $phase9Root 'phase9_link_objects.list'
$w3Root = Join-Path $repoRoot 'build\nvidia\phase-w3'
$w3SystemRoot = Join-Path $w3Root 'system'
$w3KernelRoot = Join-Path $w3SystemRoot 'kernel'
$w3Kernel = Join-Path $w3Root 'kernel.elf'
$w3EspKernel = Join-Path $w3Root 'esp\EFI\OS\kernel.elf'
$w3Closure = Join-Path $w3Root 'kernel-closure.o'
$w3LinkList = Join-Path $w3Root 'phase-w3_link_objects.list'
$linkerScript = Join-Path $PSScriptRoot 'phase4-kernel.ld'
$nvkmsCore = Join-Path $repoRoot 'build\nvidia\modeset-target\vkms-core.o'
$nvkmsObjects = Join-Path $repoRoot 'build\nvidia\modeset-target\objects.list'
$nvkmsBuild = Join-Path $repoRoot 'tools\nvidia\nvkms_build.ps1'

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Description`: $Path"
    }
}

function Invoke-Native([string]$Tool, [string[]]$Arguments) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $Tool @Arguments 2>&1 |
            ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $previous }
    foreach ($line in $output) { Write-Output $line }
    if ($exitCode -ne 0) { throw "native tool failed ($exitCode): $Tool" }
}

function Write-AsciiLines([string]$Path, [System.Collections.IEnumerable]$Lines) {
    $contents = if ($null -eq $Lines) { [string[]]@() } else { [string[]]@($Lines) }
    [IO.File]::WriteAllLines($Path, $contents, [Text.Encoding]::ASCII)
}

function Copy-FileIfChanged([string]$Source, [string]$Destination) {
    Require-File $Source 'generated W3 kernel'
    Require-File $Destination 'W3 ESP kernel'
    $sourceHash = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($sourceHash -eq $destinationHash) {
        Write-Output 'W3_ESP_KERNEL_SKIPPED=1'
        return
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    $destinationHash = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($sourceHash -ne $destinationHash) {
        throw 'W3 ESP kernel synchronization hash mismatch'
    }
    Write-Output 'W3_ESP_KERNEL_COPIED=1'
}

Require-File $phase9LinkList 'Phase 9 base link list'
Require-File (Join-Path $phase9Root 'kernel.elf') 'Phase 9 base kernel image'
Require-File $linkerScript 'kernel linker script'
Require-File $nvkmsBuild 'official NVKMS target builder'

# W3 is downstream of the formally passing generic-buffer and CPU-readback
# gates.  The previous gate is evaluated here so this target cannot silently
# become a phase skip.
$w2Gate = Join-Path $repoRoot 'tools\nvidia\phase-w2_gate.ps1'
$w2Output = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $w2Gate `
    -Phase12Build $Phase12Build -Phase12RealTestLog $Phase12RealTestLog 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $w2Output) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    @($w2Output | Where-Object { $_ -eq 'GATE_W2=PASS' }).Count -ne 1) {
    throw 'W2 Gate is not passing; W3 build is refused.'
}
Write-Output 'W3_PREVIOUS_GATE=PASS'

# The W3 link consumes the target-local NVKMS aggregate.  Re-run its
# signature-aware builder here so a change to the System KAPI/OS adapter is
# reflected in vkms-core.o before the final W3 link.  The official 595.84
# source tree remains an input-only tree; all generated objects stay under
# build/nvidia/modeset-target.
$nvkmsArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
               $nvkmsBuild)
$nvkmsOutput = @(& powershell.exe @nvkmsArgs 2>&1 |
    ForEach-Object { $_.ToString() })
foreach ($line in $nvkmsOutput) { Write-Output $line }
if ($LASTEXITCODE -ne 0 -or
    @($nvkmsOutput | Where-Object {
        $_ -eq 'NVKMS_TARGET_BUILD=PASS'
    }).Count -ne 1) {
    throw 'NVKMS target build is not passing; W3 link is refused.'
}
Write-Output 'W3_NVKMS_BUILD=PASS'
Require-File $nvkmsCore 'linked official NVKMS core'
Require-File $nvkmsObjects 'official NVKMS object list'

$makePath = (Get-Command $Make -ErrorAction Stop).Source
$linkerPath = (Get-Command $Linker -ErrorAction Stop).Source
$nmPath = (Get-Command $Nm -ErrorAction Stop).Source
New-Item -ItemType Directory -Force -Path $w3KernelRoot | Out-Null

# Reuse the Phase 9 source inventory for the System side, but compile it into
# this isolated root with the W3 switch.  This keeps the current W2 generic
# allocation implementation in the closure without touching scheduler/MM or
# the normal boot target.
$normalTargets = New-Object 'System.Collections.Generic.List[string]'
$normalLeaves = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($row in @(Get-Content -LiteralPath $phase9LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) { continue }
    if ($row -match '(?i)/build/nvidia/phase9/system/kernel/([^/]+\.o)$') {
        $leaf = $Matches[1]
        if ($normalLeaves.Add($leaf)) {
            [void]$normalTargets.Add(
                ('build/nvidia/phase-w3/system/kernel/{0}' -f $leaf))
        }
    }
}

$w3ExtraFlags = '-DOS_NVIDIA_PHASE5=1 -DOS_NVIDIA_PHASE6=1 ' +
                '-DOS_NVKMS_W3=1 -mcmodel=kernel'
$realTestValue = if ($RealTest) { '1' } else { '0' }
$makeArgs = @(
    '-f', 'GNUmakefile',
    'BUILD=build/nvidia/phase-w3/system',
    'TOOLPREFIX=x86_64-elf-',
    'ELFTOOLPREFIX=x86_64-elf-',
    'HOSTCC=gcc',
    'DEBUG=2',
    'OS_DEBUG_SERIAL=1',
    ('OS_REALTEST={0}' -f $realTestValue),
    'OS_PHASE7=0',
    'OS_PHASE8=0',
    'OS_PHASE9=0',
    ('KERNEL_EXTRA_CFLAGS={0}' -f $w3ExtraFlags)
) + @($normalTargets)
Invoke-Native $makePath $makeArgs

$linkRows = New-Object 'System.Collections.Generic.List[string]'
foreach ($row in @(Get-Content -LiteralPath $phase9LinkList)) {
    $row = ConvertTo-ObjectListPath $row
    if (-not (Test-ObjectListData $row)) {
        [void]$linkRows.Add($row)
        continue
    }
    if ($row -match '(?i)/build/nvidia/phase9/system/kernel/([^/]+\.o)$') {
        [void]$linkRows.Add(
            (Join-Path $w3KernelRoot $Matches[1]).Replace('\', '/'))
        continue
    }
    # vkms-core.o already contains the verified bridge object.  Keep one
    # definition in the W3 link rather than relying on duplicate-definition
    # selection between two independently linked copies.
    if ($row -match '(?i)[\\/]build/nvidia/phase8/objects/nv_rm_vendor_bridge\.o$') {
        continue
    }
    [void]$linkRows.Add($row)
}
[void]$linkRows.Add($nvkmsCore.Replace('\', '/'))

foreach ($row in @($linkRows)) {
    if ($row -and $row[0] -ne '#') {
        Require-File $row 'W3 link input'
    }
}
Write-AsciiLines $w3LinkList @($linkRows)

# First link every normal object and the complete official NVKMS object into
# one relocatable closure.  This scan is deliberately performed before the
# final --gc-sections image link, so an unreferenced vendor object cannot hide
# a missing OS/RM boundary symbol.
$closureArguments = @(
    '-r', '--allow-multiple-definition', '-z', 'noexecstack', '-o',
    $w3Closure.Replace('\', '/'),
    ('@' + $w3LinkList.Replace('\', '/')))
Invoke-Native $linkerPath $closureArguments

$undefined = @(& $nmPath '-u' '-P' '--' $w3Closure 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'W3 closure undefined-symbol scan failed' }
$undefinedNames = @($undefined | ForEach-Object {
    $parts = $_ -split '\s+'
    if ($parts.Count -gt 0 -and $parts[0]) { $parts[0] }
} | Sort-Object -Unique)
$undefinedReport = Join-Path $w3Root 'kernel-closure-undefined.list'
if ($undefinedNames.Count -eq 0) {
    Write-AsciiLines $undefinedReport @('# none')
} else {
    Write-AsciiLines $undefinedReport $undefinedNames
    Write-Output ('W3_CLOSURE_UNDEFINED={0}' -f $undefinedNames.Count)
    $undefinedNames | Select-Object -First 40 | ForEach-Object {
        Write-Output ('W3_UNDEFINED={0}' -f $_)
    }
    throw 'W3 complete object closure has unresolved symbols'
}
Write-Output 'W3_CLOSURE_UNDEFINED=0'

$kernelArguments = @(
    '--gc-sections', '--allow-multiple-definition', '-T',
    $linkerScript.Replace('\', '/'), '-o', $w3Kernel.Replace('\', '/'),
    ('@' + $w3LinkList.Replace('\', '/')))
Invoke-Native $linkerPath $kernelArguments

$finalUndefined = @(& $nmPath '-u' '-P' '--' $w3Kernel 2>&1 |
    ForEach-Object { $_.ToString() })
if ($LASTEXITCODE -ne 0) { throw 'W3 kernel undefined-symbol scan failed' }
$finalUnexpected = @($finalUndefined | Where-Object { $_.Trim() })
if ($finalUnexpected.Count -ne 0) {
    throw ('W3 kernel has unresolved symbols: {0}' -f $finalUnexpected[0])
}

# The real-hardware runner stages this ESP directory directly.  Keep its
# generated kernel synchronized with the just-linked image so an incremental
# deployment cannot accidentally boot a previous W3 kernel.
Copy-FileIfChanged $w3Kernel $w3EspKernel

Write-Output ('W3_LINK_OBJECTS={0}' -f $linkRows.Count)
Write-Output ('W3_KERNEL={0}' -f $w3Kernel)
Write-Output 'W3_STATIC_LINK=PASS'
Write-Output 'W3_BUILD=PASS'
