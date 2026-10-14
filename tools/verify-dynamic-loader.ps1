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

$python = Get-Command python3 -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command python -ErrorAction Stop }
& $python.Source (Join-Path $PSScriptRoot 'verify-elf-runtime.py') $Loader $Library $Test 'libgraphics.so.1'
if ($LASTEXITCODE -ne 0) { throw 'dynamic loader: invalid ELF runtime identity' }
$librarySymbols = Read-Objdump @('-T', $Library)
$testRelocations = Read-Objdump @('-R', $Test)

foreach ($symbol in @(
        'gfx_clear', 'gfx_fill_rect',
        'gfx_gradient_rect', 'gfx_frame')) {
    if (-not ($librarySymbols -split '\r?\n' | Where-Object {
        $_ -notmatch '\*UND\*' -and $_ -match ('\s' + [regex]::Escape($symbol) + '\s*$')
    })) {
        throw "dynamic loader: missing exported graphics symbol: $symbol"
    }
}
if ($testRelocations -notmatch 'JUMP_SLOT') {
    throw 'dynamic loader: test image has no dynamic call relocations'
}

Write-Output 'dynamic loader sanity passed: PT_INTERP, DT_NEEDED, exports, and JUMP_SLOT'
