param(
    [Parameter(Mandatory = $true)][string]$Loader,
    [Parameter(Mandatory = $true)][string]$Library,
    [Parameter(Mandatory = $true)][string]$Test,
    [string]$Objdump = 'x86_64-elf-objdump'
)

$ErrorActionPreference = 'Stop'

foreach ($image in @($Loader, $Library, $Test)) {
    if (-not (Test-Path -LiteralPath $image -PathType Leaf)) {
        throw "dynamic loader: missing ELF: $image"
    }
}

if (-not (Get-Command $Objdump -ErrorAction SilentlyContinue)) {
    throw "dynamic loader: missing inspection tool: $Objdump"
}

function Read-Objdump([string[]]$Arguments) {
    return (& $Objdump @Arguments | Out-String)
}

$loaderHeaders = Read-Objdump @('-p', $Loader)
$libraryHeaders = Read-Objdump @('-p', $Library)
$testHeaders = Read-Objdump @('-p', $Test)
$testInterpreter = Read-Objdump @('-s', '-j', '.interp', $Test)
$librarySymbols = Read-Objdump @('-T', $Library)
$testRelocations = Read-Objdump @('-R', $Test)

if ($loaderHeaders -match 'NEEDED' -or $libraryHeaders -match 'NEEDED') {
    throw 'dynamic loader: loader or graphics library has an external dependency'
}
if ($testHeaders -notmatch 'INTERP' -or
    $testInterpreter -notmatch '2f6c6962' -or
    $testInterpreter -notmatch '6c697465' -or
    $testInterpreter -notmatch '6f732e73' -or
    $testInterpreter -notmatch '6f2e3100') {
    throw 'dynamic loader: test image has no LiteOS interpreter'
}
if ($testHeaders -notmatch 'NEEDED' -or $testHeaders -notmatch 'libliteosgfx\.so\.1') {
    throw 'dynamic loader: test image has no graphics-library dependency'
}
foreach ($symbol in @(
        'liteos_gfx_clear', 'liteos_gfx_fill_rect',
        'liteos_gfx_gradient_rect', 'liteos_gfx_frame')) {
    if ($librarySymbols -notmatch [regex]::Escape($symbol)) {
        throw "dynamic loader: missing exported graphics symbol: $symbol"
    }
}
if ($testRelocations -notmatch 'JUMP_SLOT') {
    throw 'dynamic loader: test image has no dynamic call relocations'
}

Write-Output 'dynamic loader sanity passed: PT_INTERP, DT_NEEDED, exports, and JUMP_SLOT'
