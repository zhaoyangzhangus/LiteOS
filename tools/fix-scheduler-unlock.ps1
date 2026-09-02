$ErrorActionPreference = "Stop"

$File = Join-Path $PSScriptRoot "..\kernel\sched\core.c"
$File = [System.IO.Path]::GetFullPath($File)
$Backup = "$File.scheduler-fix.bak"

if (-not (Test-Path $File)) {
    throw "File not found: $File"
}

$Text = [System.IO.File]::ReadAllText($File)

function Get-FunctionRange {
    param(
        [string]$Source,
        [string]$Signature
    )

    $Start = $Source.IndexOf($Signature)
    if ($Start -lt 0) {
        throw "Cannot find function: $Signature"
    }

    $Brace = $Source.IndexOf("{", $Start)
    if ($Brace -lt 0) {
        throw "Cannot find opening brace: $Signature"
    }

    $Depth = 0
    for ($i = $Brace; $i -lt $Source.Length; $i++) {
        if ($Source[$i] -eq '{') {
            $Depth++
        }
        elseif ($Source[$i] -eq '}') {
            $Depth--
            if ($Depth -eq 0) {
                return @{
                    Start = $Start
                    End   = $i + 1
                }
            }
        }
    }

    throw "Cannot find closing brace: $Signature"
}

function Replace-InFunction {
    param(
        [string]$Source,
        [string]$Signature,
        [string]$Old,
        [string]$New
    )

    $Range = Get-FunctionRange -Source $Source -Signature $Signature
    $Prefix = $Source.Substring(0, $Range.Start)
    $Function = $Source.Substring($Range.Start, $Range.End - $Range.Start)
    $Suffix = $Source.Substring($Range.End)

    if ($Function.Contains($New)) {
        Write-Host "$Signature already fixed."
        return $Source
    }

    $Index = $Function.IndexOf($Old)
    if ($Index -lt 0) {
        throw "Expected statement not found in $Signature`n$Old"
    }

    if ($Function.IndexOf($Old, $Index + $Old.Length) -ge 0) {
        throw "Multiple matching statements found in $Signature"
    }

    $Function =
        $Function.Substring(0, $Index) +
        $New +
        $Function.Substring($Index + $Old.Length)

    Write-Host "Fixed: $Signature"
    return $Prefix + $Function + $Suffix
}

$Text = Replace-InFunction `
    -Source $Text `
    -Signature "void sched_wake(" `
    -Old "scheduler_unlock_irq_disabled(&cpu->queue.lock);" `
    -New "scheduler_unlock(&cpu->queue.lock, queue_flags);"

$Text = Replace-InFunction `
    -Source $Text `
    -Signature "void schedule(" `
    -Old "scheduler_unlock(&cpu->queue.lock, queue_flags);" `
    -New "scheduler_unlock_irq_disabled(&cpu->queue.lock);"

Copy-Item $File $Backup -Force

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($File, $Text, $Utf8NoBom)

git diff --check -- kernel/sched/core.c
if ($LASTEXITCODE -ne 0) {
    throw "git diff --check failed"
}

Write-Host ""
Write-Host "Scheduler fix applied successfully."
Write-Host "Backup: $Backup"
Write-Host ""
git diff -- kernel/sched/core.c
