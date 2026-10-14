[CmdletBinding()]
param(
    [string]$Build = 'build/nvidia/phase10',
    [string]$TestBinary = 'build/nvidia/phase10/phase10-userspace/p10-egl-context'
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
    Require-Directory $SourceRoot 'Phase 9 source ESP'
    foreach ($source in @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File)) {
        $relative = $source.FullName.Substring($SourceRoot.Length).TrimStart('\', '/')
        Copy-FileIfChanged $source.FullName (Join-Path $TargetRoot $relative)
    }
}

$buildRoot = Resolve-RepoPath $Build
$phase9Esp = Join-Path $repoRoot 'build\nvidia\phase9\esp'
$phase10Kernel = Join-Path $buildRoot 'kernel.elf'
$testBinaryPath = Resolve-RepoPath $TestBinary
$espRoot = Join-Path $buildRoot 'esp'
$phase9Glvnd = Join-Path $phase9Esp 'compat\linux\usr\share\glvnd\egl_vendor.d'
$absoluteGlvnd = Join-Path $espRoot 'usr\share\glvnd\egl_vendor.d'
$phase10NvidiaManifest = Join-Path $repoRoot 'tools\nvidia\phase10_glvnd_10_nvidia.json'

Require-Directory $phase9Esp 'Phase 9 source ESP'
Require-File $phase10Kernel 'Phase 10 kernel image'
Require-File $testBinaryPath 'Phase 10 EGL/context probe'

$script:stageCopied = 0
$script:stageSkipped = 0

# Copy the verified P9 closure first.  The P10 kernel and test are copied after
# it so an inherited P9 tree can never overwrite the new phase image.
Copy-DirectoryContents $phase9Esp $espRoot
Copy-FileIfChanged $testBinaryPath (Join-Path $espRoot 'tests\p10-egl-context')
Copy-FileIfChanged $phase10Kernel (Join-Path $espRoot 'EFI\OS\kernel.elf')

# GLVND's 595.84 libEGL loader looks for vendor manifests using the guest's
# absolute /etc/glvnd and /usr/share/glvnd paths.  Keep the verified closure
# under /compat/linux for the dynamic-linker RPATH, and materialize the same
# vendor manifests at the absolute path expected by the real loader.
Require-Directory $phase9Glvnd '595.84 GLVND vendor manifest directory'
foreach ($manifest in @(Get-ChildItem -LiteralPath $phase9Glvnd -File)) {
    Copy-FileIfChanged $manifest.FullName (Join-Path $absoluteGlvnd $manifest.Name)
}
Require-File $phase10NvidiaManifest 'System GLVND NVIDIA manifest'
Copy-FileIfChanged $phase10NvidiaManifest (Join-Path $absoluteGlvnd '10_nvidia.json')

Write-Output ('P10_STAGE_COPIED={0}' -f $script:stageCopied)
Write-Output ('P10_STAGE_SKIPPED={0}' -f $script:stageSkipped)
Write-Output ('P10_ESP={0}' -f $espRoot)
Write-Output 'P10_STAGE=PASS'
