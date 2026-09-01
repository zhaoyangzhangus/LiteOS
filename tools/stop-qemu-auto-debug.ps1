param(
    [string]$Build = ''
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
# Always run the native cleanup first. It also scans the debug port, which
# covers a wrapper that was cancelled after its PID file was removed.
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File `
    (Join-Path $PSScriptRoot 'stop-qemu-native-debug.ps1') -Build $Build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

function Get-WslDistro {
    $preferred = @($env:LITEOS_WSL_DISTRO, 'Debian', 'Ubuntu')
    $available = @(
        (& wsl.exe -l -q 2>$null) |
            ForEach-Object { ("$_" -replace "`0", '').Trim() } |
            Where-Object { $_ }
    )
    foreach ($candidate in $preferred) {
        if ($candidate -and $available -contains $candidate) { return $candidate }
    }
    if ($available.Count -gt 0) { return $available[0] }
    return $null
}

$wslCommand = Get-Command wsl.exe -ErrorAction SilentlyContinue
if (-not $wslCommand) { exit 0 }

$distro = Get-WslDistro
if (-not $distro) { exit 0 }

$drive = $projectRoot.Substring(0, 1).ToLowerInvariant()
$rest = $projectRoot.Substring(2).Replace('\', '/')
$wslRoot = "/mnt/$drive$rest"
$escapedRoot = $wslRoot.Replace("'", "'\''")
$buildName = if ($Build) { $Build } elseif ($env:LITEOS_BUILD) { $env:LITEOS_BUILD } elseif ($env:BUILD) { $env:BUILD } else { 'build' }
$escapedBuildName = $buildName.Replace("'", "'\''")
& wsl.exe -d $distro -- bash -lc "cd '$escapedRoot' && BUILD='$escapedBuildName' ./tools/stop-qemu-debug.sh"
exit $LASTEXITCODE
