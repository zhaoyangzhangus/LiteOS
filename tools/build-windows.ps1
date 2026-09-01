param(
    [string]$Build = '',
    [int]$Debug = 2,
    [switch]$Serial,
    [switch]$RealTest,
    [switch]$FailureTest,
    [switch]$DiagnosticBot,
    [switch]$Force,
    [string]$ToolPrefix = '',
    [string]$ElfToolPrefix = '',
    [string]$MakePath = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Assert-RelativeBuild([string]$name) {
    if (-not $name -or [IO.Path]::IsPathRooted($name) -or
        $name -match '(^|[\\/])\.\.(?:[\\/]|$)') {
        throw "Build directory must be relative to the project: $name"
    }
}

$buildName = if ($Build) {
    $Build
} elseif ($env:LITEOS_BUILD) {
    $env:LITEOS_BUILD
} elseif ($env:BUILD) {
    $env:BUILD
} else {
    'build-refactor'
}
Assert-RelativeBuild $buildName

if ($Debug -lt 0 -or $Debug -gt 2) {
    throw "Debug must be 0, 1, or 2: $Debug"
}

function Resolve-Executable([string]$explicitPath, [string[]]$names) {
    if ($explicitPath) {
        if (-not (Test-Path -LiteralPath $explicitPath)) {
            throw "Executable not found: $explicitPath"
        }
        return (Resolve-Path -LiteralPath $explicitPath).Path
    }

    foreach ($name in $names) {
        $command = Get-Command $name -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    return $null
}

# Put the native GNU tools first.  Windows ships a different find.exe in
# system32; GNU make uses find when it expands source manifests.
$nativeBinCandidates = @(
    'C:\Program Files\w64devkit\bin',
    'C:\w64devkit\bin',
    (Join-Path $projectRoot 'w64devkit\bin')
)
$nativeBin = $nativeBinCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if ($nativeBin) {
    $env:Path = "$nativeBin;$env:Path"
}

$elfBinCandidates = @(
    'C:\Program Files\x86_64-elf-tools-windows\bin',
    'C:\x86_64-elf-tools-windows\bin',
    (Join-Path $projectRoot 'x86_64-elf-tools\bin')
)
$elfBin = $elfBinCandidates |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if ($elfBin) {
    $env:Path = "$elfBin;$env:Path"
}

$make = Resolve-Executable $MakePath @('make.exe', 'mingw32-make.exe')
if (-not $make) {
    throw 'GNU make was not found. Add w64devkit\bin to PATH or pass -MakePath.'
}

$prefix = if ($ToolPrefix) {
    $ToolPrefix
} elseif ($env:LITEOS_TOOLPREFIX) {
    $env:LITEOS_TOOLPREFIX
} elseif ($env:TOOLPREFIX) {
    $env:TOOLPREFIX
} else {
    'x86_64-w64-mingw32-'
}

$elfPrefix = if ($ElfToolPrefix) {
    $ElfToolPrefix
} elseif ($env:LITEOS_ELFTOOLPREFIX) {
    $env:LITEOS_ELFTOOLPREFIX
} elseif ($env:ELFTOOLPREFIX) {
    $env:ELFTOOLPREFIX
} else {
    'x86_64-elf-'
}

$toolNames = @('gcc.exe', 'ar.exe', 'objcopy.exe', 'objdump.exe')
foreach ($toolName in $toolNames) {
    $tool = Resolve-Executable '' @($prefix + $toolName)
    if (-not $tool) {
        throw "Missing native target tool: $prefix$toolName. The LiteOS UEFI build requires the MinGW PE/COFF toolchain."
    }
}

foreach ($toolName in @('gcc.exe', 'ld.exe', 'objcopy.exe', 'objdump.exe')) {
    $tool = Resolve-Executable '' @($elfPrefix + $toolName)
    if (-not $tool) {
        throw "Missing ELF runtime tool: $elfPrefix$toolName. Install the x86_64-elf toolchain or pass -ElfToolPrefix."
    }
}

$hostCompiler = Resolve-Executable '' @('gcc.exe')
if (-not $hostCompiler) {
    throw 'Native host gcc was not found. Install w64devkit and add its bin directory to PATH.'
}

$buildDirectory = Join-Path $projectRoot $buildName
$buildLog = Join-Path $buildDirectory 'windows-build.log'
New-Item -ItemType Directory -Force -Path $buildDirectory | Out-Null
Set-Content -LiteralPath $buildLog -Value '' -NoNewline

function Test-WindowsExecutable([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return $false }
    try {
        $bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $path).Path)
        return $bytes.Length -ge 2 -and $bytes[0] -eq 0x4D -and $bytes[1] -eq 0x5A
    }
    catch {
        return $false
    }
}

# A build directory produced by WSL contains Linux host helpers.  Remove only
# those generated helpers so make rebuilds them with w64devkit; target image
# objects are still reusable because they use the same MinGW object format.
$hostHelpers = @(
    (Join-Path $buildDirectory 'build-id.exe'),
    (Join-Path $buildDirectory 'make-init-image.exe'),
    (Join-Path $buildDirectory 'make-test-elf.exe')
)
foreach ($helper in $hostHelpers) {
    if ((Test-Path -LiteralPath $helper) -and -not (Test-WindowsExecutable $helper)) {
        Remove-Item -LiteralPath $helper -Force
    }
}

$serialValue = if ($Serial) { '1' } else { '0' }
$realTestValue = if ($RealTest) { '1' } else { '0' }
$failureTestValue = if ($FailureTest) { '1' } else { '0' }
$diagnosticBotValue = if ($DiagnosticBot) { '1' } else { '0' }
$makeArguments = @(
    '-f', 'GNUmakefile'
)
if ($Force) { $makeArguments += '-B' }
$makeArguments += @(
    "BUILD=$buildName",
    "TOOLPREFIX=$prefix",
    "ELFTOOLPREFIX=$elfPrefix",
    'HOSTCC=gcc',
    "DEBUG=$Debug",
    "LITEOS_DEBUG_SERIAL=$serialValue",
    "LITEOS_REALTEST=$realTestValue",
    "LITEOS_REALTEST_FAILURE_TEST=$failureTestValue",
    "LITEOS_XHCI_DIAGNOSTIC_BOT=$diagnosticBotValue"
)
$makeArguments += 'esp'

Push-Location $projectRoot
try {
    & $make @makeArguments 2>&1 |
        ForEach-Object {
            $line = [string]$_
            Add-Content -LiteralPath $buildLog -Value $line
            if ($line -match '(?i)(error:|undefined reference|fatal|failed|cannot|invalid|no such file)') {
                Write-Host $line
            }
        }
    $status = $LASTEXITCODE
}
finally {
    Pop-Location
}

if ($status -ne 0) {
    throw "Native LiteOS build failed ($status). See $buildLog."
}

$kernel = Join-Path $buildDirectory 'esp\EFI\LITEOS\kernel.elf'
$bootloader = Join-Path $buildDirectory 'esp\EFI\BOOT\BOOTX64.EFI'
if (-not (Test-Path -LiteralPath $kernel) -or -not (Test-Path -LiteralPath $bootloader)) {
    throw "Native build completed without the ESP boot files. See $buildLog."
}

Write-Host "LiteOS native build ready: $buildName"
