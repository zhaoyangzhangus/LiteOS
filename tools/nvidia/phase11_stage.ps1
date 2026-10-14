[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase11',
    [string]$TestBinary = 'build/nvidia/phase11/phase11-userspace/p11-triangle'
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
    Require-Directory $SourceRoot 'Phase 10 source ESP'
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
        Copy-FileIfChanged $source.FullName (Join-Path $TargetRoot $relative)
    }
}

$buildRoot = Resolve-RepoPath $Build
$phase10Esp = Join-Path $repoRoot 'build\nvidia\phase10\esp'
$phase11Kernel = Join-Path $buildRoot 'kernel.elf'
$testBinaryPath = Resolve-RepoPath $TestBinary
$espRoot = Join-Path $buildRoot 'esp'
$phase10Glvnd = Join-Path $phase10Esp 'usr\share\glvnd\egl_vendor.d'
$absoluteGlvnd = Join-Path $espRoot 'usr\share\glvnd\egl_vendor.d'
$phase11NvidiaManifest = Join-Path $repoRoot 'tools\nvidia\phase10_glvnd_10_nvidia.json'

Require-Directory $phase10Esp 'Phase 10 source ESP'
Require-File $phase11Kernel 'Phase 11 kernel image'
Require-File $testBinaryPath 'Phase 11 triangle probe'

$script:stageCopied = 0
$script:stageSkipped = 0

# Preserve the verified P10 userspace/firmware closure.  Only the P11 kernel and
# probe are replaced in this stage.
Copy-DirectoryContents $phase10Esp $espRoot
Copy-FileIfChanged $testBinaryPath (Join-Path $espRoot 'tests\p11-triangle')
Copy-FileIfChanged $phase11Kernel (Join-Path $espRoot 'EFI\OS\kernel.elf')

# Keep the absolute GLVND manifests required by the official libEGL loader.
Require-Directory $phase10Glvnd '595.84 GLVND vendor manifest directory'
foreach ($manifest in @(Get-ChildItem -LiteralPath $phase10Glvnd -File)) {
    Copy-FileIfChanged $manifest.FullName (Join-Path $absoluteGlvnd $manifest.Name)
}
Require-File $phase11NvidiaManifest 'System GLVND NVIDIA manifest'
Copy-FileIfChanged $phase11NvidiaManifest (Join-Path $absoluteGlvnd '10_nvidia.json')

Write-Output ('P11_STAGE_COPIED={0}' -f $script:stageCopied)
Write-Output ('P11_STAGE_SKIPPED={0}' -f $script:stageSkipped)
Write-Output ('P11_ESP={0}' -f $espRoot)
Write-Output 'P11_STAGE=PASS'
