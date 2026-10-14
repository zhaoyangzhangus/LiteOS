[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase12',
    [string]$TestBinary = 'build/nvidia/phase12/phase12-userspace/p12-window'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

function Resolve-RepoPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) { return [IO.Path]::GetFullPath($Path) }
    return [IO.Path]::GetFullPath((Join-Path $repoRoot $Path))
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Description`: $Path"
    }
}

function Require-Directory([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Missing $Description`: $Path"
    }
}

function Copy-FileIfChanged([string]$Source, [string]$Target) {
    $same = $false
    if (Test-Path -LiteralPath $Target -PathType Leaf) {
        $same = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash -eq
                (Get-FileHash -LiteralPath $Target -Algorithm SHA256).Hash
    }
    if ($same) {
        $script:stageSkipped++
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Target) |
        Out-Null
    Copy-Item -LiteralPath $Source -Destination $Target -Force
    $script:stageCopied++
}

function Copy-DirectoryContents([string]$SourceRoot, [string]$TargetRoot) {
    Require-Directory $SourceRoot 'Phase 11 source ESP'
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
        Copy-FileIfChanged $source.FullName (Join-Path $TargetRoot $relative)
    }
}

$buildRoot = Resolve-RepoPath $Build
$phase11Esp = Join-Path $repoRoot 'build\nvidia\phase11\esp'
$phase12Kernel = Join-Path $buildRoot 'kernel.elf'
$testBinaryPath = Resolve-RepoPath $TestBinary
$espRoot = Join-Path $buildRoot 'esp'
$phase11Glvnd = Join-Path $phase11Esp 'usr\share\glvnd\egl_vendor.d'
$absoluteGlvnd = Join-Path $espRoot 'usr\share\glvnd\egl_vendor.d'
$phase12NvidiaManifest = Join-Path $repoRoot 'tools\nvidia\phase10_glvnd_10_nvidia.json'

Require-Directory $phase11Esp 'Phase 11 source ESP'
Require-File $phase12Kernel 'Phase 12 kernel image'
Require-File $testBinaryPath 'Phase 12 window bridge probe'

$script:stageCopied = 0
$script:stageSkipped = 0
Copy-DirectoryContents $phase11Esp $espRoot
Copy-FileIfChanged $testBinaryPath (Join-Path $espRoot 'tests\p12-window')
Copy-FileIfChanged $phase12Kernel (Join-Path $espRoot 'EFI\OS\kernel.elf')

Require-Directory $phase11Glvnd '595.84 GLVND vendor manifest directory'
foreach ($manifest in @(Get-ChildItem -LiteralPath $phase11Glvnd -File)) {
    Copy-FileIfChanged $manifest.FullName (Join-Path $absoluteGlvnd $manifest.Name)
}
Require-File $phase12NvidiaManifest 'System GLVND NVIDIA manifest'
Copy-FileIfChanged $phase12NvidiaManifest (Join-Path $absoluteGlvnd '10_nvidia.json')

Write-Output ('P12_STAGE_COPIED={0}' -f $script:stageCopied)
Write-Output ('P12_STAGE_SKIPPED={0}' -f $script:stageSkipped)
Write-Output ('P12_ESP={0}' -f $espRoot)
Write-Output 'P12_STAGE=PASS'
