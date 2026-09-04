[CmdletBinding()]
param(
    [string]$Make = 'make'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$vendorRoot = Join-Path $repoRoot 'third_party\nvidia\595.84'
$vendorSourceRoot = Join-Path $vendorRoot 'open-gpu-kernel-modules'
$abiRoot = Join-Path $repoRoot 'kernel\drivers\gpu\nvidia\vendor_abi'
$phase2Gate = Join-Path $PSScriptRoot 'phase2_gate.ps1'
$phase3Build = Join-Path $PSScriptRoot 'phase3_vendor_build.ps1'
$outputRoot = Join-Path $repoRoot 'build\nvidia\phase3'
$rmObject = Join-Path $outputRoot 'vendor-rm.o'
$liteosLinkObject = Join-Path $outputRoot 'vendor-rm-liteos.o'
$rmReport = Join-Path $outputRoot 'vendor_rm_objects.txt'
$abiReport = Join-Path $outputRoot 'vendor_abi_objects.txt'
$abiLinkList = Join-Path $outputRoot 'vendor_abi_link_objects.list'
$liteosReport = Join-Path $outputRoot 'liteos_probe_objects.txt'
$linkLog = Join-Path $outputRoot 'vendor_rm_link.log'
$liteosLinkLog = Join-Path $outputRoot 'vendor_rm_liteos_link.log'
$unresolvedReport = Join-Path $outputRoot 'vendor_rm_unresolved.txt'
$contractsSource = Join-Path $abiRoot 'nvabi_contracts.c'
$miscSource = Join-Path $abiRoot 'nvabi_misc.c'

function Fail-Gate([string]$Message) {
    throw "P3 Gate failed: $Message"
}

function Read-DataLines([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Fail-Gate "missing report: $Path"
    }
    return @(Get-Content -LiteralPath $Path | Where-Object {
        $_ -and $_[0] -ne '#'
    })
}

function Assert-CompiledObjectReport([string]$Path, [string]$ExpectedComponent,
                                      [int]$ExpectedCount) {
    $rows = @(Read-DataLines $Path)
    if ($rows.Count -ne $ExpectedCount) {
        Fail-Gate "object report count mismatch: $Path ($($rows.Count) != $ExpectedCount)"
    }
    $seenIds = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
    $seenObjects = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($row in $rows) {
        $fields = $row -split "`t", -1
        if ($fields.Count -ne 8) { Fail-Gate "malformed object row in $Path" }
        $componentMatches = $fields[1] -eq $ExpectedComponent
        if ($ExpectedComponent -eq 'rm' -and $fields[1] -eq 'rm-generated') {
            $componentMatches = $true
        }
        if ($fields[0] -ne 'compiled' -or -not $componentMatches) {
            Fail-Gate "non-compiled object row in $($Path): $($fields[3])"
        }
        if ($fields[6] -ne '0') { Fail-Gate "object did not compile cleanly: $($fields[3])" }
        if (-not $seenIds.Add($fields[3])) { Fail-Gate "duplicate object id: $($fields[3])" }
        if (-not $seenObjects.Add($fields[5])) { Fail-Gate "duplicate object path: $($fields[5])" }
        if (-not (Test-Path -LiteralPath $fields[5] -PathType Leaf)) {
            Fail-Gate "compiled object missing: $($fields[3])"
        }
    }
    return $rows
}

if (-not (Test-Path -LiteralPath $vendorSourceRoot -PathType Container)) {
    Fail-Gate 'immutable NVIDIA vendor source is missing'
}
if (-not (Test-Path -LiteralPath $abiRoot -PathType Container)) {
    Fail-Gate 'vendor ABI source root is missing'
}

# Phase 3 is downstream of every earlier phase.  This invokes the previous
# Gate, including the production kernel and port test, before touching the
# Phase 3 build graph.
$p2Args = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $phase2Gate, '-Make', $Make)
$p2Output = @(& powershell.exe @p2Args 2>&1 | ForEach-Object { $_.ToString() })
$p2ExitCode = $LASTEXITCODE
foreach ($line in $p2Output) { Write-Output $line }
if ($p2ExitCode -ne 0 -or @($p2Output | Where-Object { $_ -eq 'GATE_P2=PASS' }).Count -ne 1) {
    Fail-Gate 'P2 Gate did not pass'
}

$buildArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $phase3Build)
$buildOutput = @(& powershell.exe @buildArgs 2>&1 | ForEach-Object { $_.ToString() })
$buildExitCode = $LASTEXITCODE
foreach ($line in $buildOutput) { Write-Output $line }
if ($buildExitCode -ne 0) { Fail-Gate 'Phase 3 vendor build failed' }

$summaryRows = @($buildOutput | Where-Object { $_ -like 'SUMMARY *' })
if ($summaryRows.Count -ne 1) { Fail-Gate 'Phase 3 build summary is missing' }
$summaryMatch = [regex]::Match($summaryRows[0], '^SUMMARY selected=(\d+);compiled=(\d+);compile_failed=(\d+);abi_selected=(\d+);abi_compiled=(\d+);abi_linkable=(\d+);abi_compile_failed=(\d+);liteos_objects=(\d+);rm_link_exit=(\d+);liteos_link_exit=(\d+);linked_unresolved=(\d+)$')
if (-not $summaryMatch.Success) { Fail-Gate 'malformed Phase 3 build summary' }
if ([int]$summaryMatch.Groups[1].Value -ne 1026 -or
    [int]$summaryMatch.Groups[2].Value -ne 1026 -or
    [int]$summaryMatch.Groups[3].Value -ne 0 -or
    [int]$summaryMatch.Groups[4].Value -ne 12 -or
    [int]$summaryMatch.Groups[5].Value -ne 12 -or
    [int]$summaryMatch.Groups[6].Value -ne 11 -or
    [int]$summaryMatch.Groups[7].Value -ne 0 -or
    [int]$summaryMatch.Groups[8].Value -le 0 -or
    [int]$summaryMatch.Groups[9].Value -ne 0 -or
    [int]$summaryMatch.Groups[10].Value -ne 0 -or
    [int]$summaryMatch.Groups[11].Value -ne 0) {
    Fail-Gate "Phase 3 build summary does not describe a closed graph: $($summaryRows[0])"
}

$rmRows = @(Assert-CompiledObjectReport $rmReport 'rm' 1026)
$abiRows = @(Assert-CompiledObjectReport $abiReport 'vendor_abi' 12)
$liteosRows = @(Read-DataLines $liteosReport)
if ($liteosRows.Count -le 0) { Fail-Gate 'LiteOS support object inventory is empty' }
foreach ($row in $liteosRows) {
    $fields = $row -split "`t", -1
    if ($fields.Count -ne 6 -or $fields[0] -ne 'converted' -or $fields[4] -ne '0') {
        Fail-Gate "LiteOS support object conversion failed: $row"
    }
    if (-not (Test-Path -LiteralPath $fields[3] -PathType Leaf)) {
        Fail-Gate "converted LiteOS object missing: $($fields[1])"
    }
}
if ($liteosRows.Count -ne [int]$summaryMatch.Groups[8].Value) {
    Fail-Gate 'LiteOS object report and build summary disagree'
}

$contractRows = @($abiRows | Where-Object {
    ($_ -split "`t", -1)[4] -like '*vendor_abi/nvabi_contracts.c'
})
if ($contractRows.Count -ne 1) { Fail-Gate 'exact ABI contract translation unit is missing' }
$contractFields = $contractRows[0] -split "`t", -1
$contractObject = $contractFields[5].Replace('\', '/')
$abiLinkObjects = @(Get-Content -LiteralPath $abiLinkList | Where-Object { $_ -and $_[0] -ne '#' })
if ($abiLinkObjects.Count -ne 11) { Fail-Gate 'ABI link object count is not 11' }
if (@($abiLinkObjects | Where-Object { $_.Replace('\', '/') -eq $contractObject }).Count -ne 0) {
    Fail-Gate 'prototype contract object must not provide runtime definitions'
}
$contractCalls = @(Get-Content -LiteralPath $contractsSource | Where-Object {
    $_ -match '^\s*NVABI_CONTRACT\([A-Za-z_][A-Za-z0-9_]*\);\s*$'
})
if ($contractCalls.Count -ne 492) { Fail-Gate "ABI contract count is $($contractCalls.Count), expected 492" }

foreach ($logPath in @($linkLog, $liteosLinkLog)) {
    $logLines = @(Get-Content -LiteralPath $logPath)
    if (@($logLines | Where-Object { $_ -match '^status=linked;exit=0;' }).Count -ne 1) {
        Fail-Gate "successful link record is missing: $logPath"
    }
}
if (-not (Test-Path -LiteralPath $rmObject -PathType Leaf) -or
    -not (Test-Path -LiteralPath $liteosLinkObject -PathType Leaf)) {
    Fail-Gate 'RM link output is missing'
}

$unresolvedLines = @(Get-Content -LiteralPath $unresolvedReport)
$unresolvedHeader = @($unresolvedLines | Where-Object { $_ -like '# rm_compiled=*' })
if ($unresolvedHeader.Count -ne 1) { Fail-Gate 'unresolved report header is missing' }
$unresolvedMatch = [regex]::Match($unresolvedHeader[0], '^# rm_compiled=(\d+);abi_compiled=(\d+);abi_linkable=(\d+);liteos_objects=(\d+);rm_link_exit=(\d+);liteos_link_exit=(\d+);linked_nm_exit=(\d+);linked_unresolved=(\d+)$')
if (-not $unresolvedMatch.Success -or
    [int]$unresolvedMatch.Groups[1].Value -ne 1026 -or
    [int]$unresolvedMatch.Groups[2].Value -ne 12 -or
    [int]$unresolvedMatch.Groups[3].Value -ne 11 -or
    [int]$unresolvedMatch.Groups[4].Value -ne $liteosRows.Count -or
    [int]$unresolvedMatch.Groups[5].Value -ne 0 -or
    [int]$unresolvedMatch.Groups[6].Value -ne 0 -or
    [int]$unresolvedMatch.Groups[7].Value -ne 0 -or
    [int]$unresolvedMatch.Groups[8].Value -ne 0) {
    Fail-Gate 'unresolved report does not prove zero required symbols'
}
$unresolvedStart = [Array]::IndexOf([string[]]$unresolvedLines, '[unresolved_external_symbols]')
if ($unresolvedStart -lt 0) { Fail-Gate 'unresolved symbol section is missing' }
$unresolvedSymbols = @($unresolvedLines | Select-Object -Skip ($unresolvedStart + 1) |
    Where-Object { $_ -and $_[0] -ne '#' })
if ($unresolvedSymbols.Count -ne 0) { Fail-Gate "required unresolved symbols remain: $($unresolvedSymbols[0])" }

$nmCommand = Get-Command x86_64-elf-nm -ErrorAction SilentlyContinue
if ($null -eq $nmCommand) { $nmCommand = Get-Command nm -ErrorAction Stop }
$finalNmLines = @(& $nmCommand.Source -u -P -- $liteosLinkObject 2>&1 |
    ForEach-Object { $_.ToString() })
$finalNmExitCode = $LASTEXITCODE
if ($finalNmExitCode -ne 0) { Fail-Gate 'nm failed on final RM-plus-LiteOS object' }
$finalUndefined = @($finalNmLines | Where-Object { $_.Trim() })
if ($finalUndefined.Count -ne 0) { Fail-Gate "nm found unresolved symbols: $($finalUndefined[0])" }
Write-Output 'ABI_COMPILE=PASS'
Write-Output 'NVIDIA_RM_LITEOS_LINK=PASS'
Write-Output 'UNRESOLVED_REQUIRED=0'

# nvabi_misc.c is reserved for the small, explicitly classified leftovers;
# the large categories live in their own adapters or fail-closed units.
$miscLines = @(Get-Content -LiteralPath $miscSource | Where-Object {
    $trimmed = $_.Trim()
    $trimmed -and -not $trimmed.StartsWith('//') -and
        -not $trimmed.StartsWith('/*') -and -not $trimmed.StartsWith('*') -and
        -not $trimmed.StartsWith('*/')
})
$miscText = Get-Content -LiteralPath $miscSource -Raw
if ($miscLines.Count -gt 100 -or $miscText -notmatch '(?i)capability' -or
    $miscText -notmatch '(?i)nv_printf|out_string') {
    Fail-Gate 'nvabi_misc.c is not small and explicitly classified'
}
Write-Output 'NVABI_MISC=PASS'

# Every unsupported assembly entry must log its symbol/stage and then use a
# typed failure representation.  Literal zero return values are allowed only
# for bool/pointer declarations; a literal success status is rejected.
$unsupportedFiles = @(Get-ChildItem -LiteralPath $abiRoot -File -Filter '*_unsupported.S' | Sort-Object Name)
if ($unsupportedFiles.Count -ne 5) { Fail-Gate 'unexpected unsupported ABI file set' }
foreach ($file in $unsupportedFiles) {
    $lines = @(Get-Content -LiteralPath $file.FullName)
    $globalRows = @($lines | Where-Object { $_ -match '^\s*\.globl\s+([A-Za-z_][A-Za-z0-9_]*)\s*$' })
    $logRows = @($lines | Where-Object { $_ -match '^\s*call\s+nvabi_log_unsupported\s*$' })
    if ($globalRows.Count -eq 0 -or $globalRows.Count -ne $logRows.Count) {
        Fail-Gate "unsupported ABI logging coverage mismatch: $($file.Name)"
    }
    if (@($lines | Where-Object { $_ -match '^\s*mov[lq]\s+\$(?:0|0x0+),\s*%r?ax\s*$' }).Count -ne 0) {
        Fail-Gate "unsupported ABI contains literal success return: $($file.Name)"
    }
    $globalIndexes = @()
    for ($lineIndex = 0; $lineIndex -lt $lines.Count; $lineIndex++) {
        if ($lines[$lineIndex] -match '^\s*\.globl\s+[A-Za-z_][A-Za-z0-9_]*\s*$') {
            $globalIndexes += $lineIndex
        }
    }
    for ($globalIndex = 0; $globalIndex -lt $globalIndexes.Count; $globalIndex++) {
        $start = $globalIndexes[$globalIndex]
        $end = if ($globalIndex + 1 -lt $globalIndexes.Count) {
            $globalIndexes[$globalIndex + 1]
        } else { $lines.Count }
        $block = ($lines[$start..($end - 1)] -join "`n")
        if ($block -notmatch '(?m)^\s*call\s+nvabi_log_unsupported\s*$' -or
            $block -notmatch '(?m)^\s*(?:#\s*void return|xorl\s+%eax|mov[lq]\s+\$(?:0x56|0x80010002|-13|-38|-1),)') {
            Fail-Gate "unsupported ABI entry is not fail-closed: $($file.Name) line $($start + 1)"
        }
    }
}
Write-Output 'NO_FAKE_SUCCESS=PASS'
Write-Output 'GATE_P3=PASS'
