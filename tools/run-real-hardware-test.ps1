param(
    [int]$DiskNumber = 1,
    [int]$PartitionNumber = 2,
    [ValidatePattern('^[A-Za-z]:?$')]
    [string]$DriveLetter = 'F',
    [string]$Build = 'build-realtest',
    [switch]$DiagnosticBot,
    [switch]$ForceReboot,
    [switch]$NoReboot
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$letter = $DriveLetter.TrimEnd(':').ToUpperInvariant()
$runId = Get-Date -Format 'yyyyMMdd-HHmmss'
$runDirectory = Join-Path (Join-Path $projectRoot $Build) "real-hardware\$runId"
$backupDirectory = Join-Path $runDirectory 'backup'
$runOncePath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\RunOnce'
$runOnceName = "LiteOSRealTest_$($runId.Replace('-', '_'))"
$bootSequenceSet = $false
$firmwareGuid = '{4c495445-4f53-5254-4553-544355524e31}'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class LiteOsFirmwareEnvironment {
    [StructLayout(LayoutKind.Sequential)]
    public struct Luid { public UInt32 LowPart; public Int32 HighPart; }
    [StructLayout(LayoutKind.Sequential)]
    public struct TokenPrivileges { public UInt32 Count; public Luid Luid; public UInt32 Attributes; }
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, UInt32 access, out IntPtr token);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool LookupPrivilegeValue(string system, string name, out Luid luid);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disable,
        ref TokenPrivileges privileges, UInt32 length, IntPtr previous, IntPtr returnLength);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public static bool EnableFirmwarePrivilege() {
        IntPtr token;
        Luid luid;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28U, out token)) return false;
        try {
            TokenPrivileges privileges = new TokenPrivileges();
            if (!LookupPrivilegeValue(null, "SeSystemEnvironmentPrivilege", out luid)) return false;
            privileges.Count = 1U; privileges.Luid = luid; privileges.Attributes = 2U;
            return AdjustTokenPrivileges(token, false, ref privileges, 0U,
                IntPtr.Zero, IntPtr.Zero);
        }
        finally { CloseHandle(token); }
    }
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool SetFirmwareEnvironmentVariableEx(
        string name, string guid, byte[] value, UInt32 size, UInt32 attributes);
}
'@

if ($letter.Length -ne 1) { throw "DriveLetter must be one letter: $DriveLetter" }
if ($DiskNumber -lt 0 -or $PartitionNumber -lt 1) {
    throw 'DiskNumber and PartitionNumber are invalid.'
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'This operation must run from an elevated PowerShell window.'
}

New-Item -ItemType Directory -Force -Path $runDirectory,$backupDirectory | Out-Null

function Invoke-NativeChecked([string]$FilePath, [string[]]$Arguments) {
    $output = @(& $FilePath @Arguments 2>&1 | ForEach-Object { [string]$_ })
    $status = $LASTEXITCODE
    if ($status -ne 0) {
        throw "$FilePath failed ($status): $($output -join ' ')"
    }
    return ($output -join "`r`n")
}

function Save-Text([string]$Path, [string]$Text) {
    Set-Content -LiteralPath $Path -Value $Text -Encoding UTF8
}

function Set-FirmwareState([string]$state) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($state)
    if (-not [LiteOsFirmwareEnvironment]::EnableFirmwarePrivilege()) { return $false }
    return [LiteOsFirmwareEnvironment]::SetFirmwareEnvironmentVariableEx(
        'LiteOSRealTestState', $firmwareGuid, $bytes, [uint32]$bytes.Length, 7)
}

function Backup-Target([string]$relativePath, [string]$volumeRoot) {
    $source = Join-Path $volumeRoot $relativePath
    if (-not (Test-Path -LiteralPath $source)) { return }
    $destination = Join-Path $backupDirectory $relativePath
    $parent = Split-Path -Parent $destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
}

function Copy-DirectoryContents([string]$sourceDirectory, [string]$targetDirectory) {
    New-Item -ItemType Directory -Force -Path $targetDirectory | Out-Null
    foreach ($child in (Get-ChildItem -LiteralPath $sourceDirectory -Force)) {
        $destination = Join-Path $targetDirectory $child.Name
        if ($child.PSIsContainer) {
            Copy-DirectoryContents $child.FullName $destination
        }
        else {
            Copy-Item -LiteralPath $child.FullName -Destination $destination -Force
        }
    }
}

$disk = Get-Disk -Number $DiskNumber
$partition = Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber
$volume = Get-Volume -DriveLetter $letter
if ($disk.BusType.ToString() -ne 'USB') {
    throw "Refusing non-USB target disk $DiskNumber (BusType=$($disk.BusType))."
}
if ($disk.IsBoot -or $disk.IsSystem) {
    throw "Refusing boot/system disk $DiskNumber."
}
if ($partition.DriveLetter.ToString().ToUpperInvariant() -ne $letter) {
    throw "Partition $DiskNumber/$PartitionNumber is not mounted as $letter`:."
}
if ($volume.FileSystem -ne 'FAT32') {
    throw "Refusing non-FAT32 target volume $letter`: (FileSystem=$($volume.FileSystem))."
}

$volumeRoot = "$letter`:\"
$preflight = [ordered]@{
    run_id = $runId
    disk_number = $disk.Number
    partition_number = $partition.PartitionNumber
    drive_letter = $letter
    disk_model = $disk.FriendlyName
    disk_serial = $disk.SerialNumber
    disk_unique_id = $disk.UniqueId
    bus_type = $disk.BusType.ToString()
    volume_label = $volume.FileSystemLabel
    volume_unique_id = $volume.UniqueId
    volume_size = $volume.Size
    build = $Build
}
$preflight | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory 'preflight.json')
Save-Text (Join-Path $runDirectory 'disk.txt') (
    (Get-Disk -Number $DiskNumber | Format-List * | Out-String) +
    (Get-Partition -DiskNumber $DiskNumber -PartitionNumber $PartitionNumber |
        Format-List * | Out-String) +
    (Get-Volume -DriveLetter $letter | Format-List * | Out-String)
)

$firmwareBefore = Invoke-NativeChecked 'bcdedit.exe' @('/enum', 'firmware', '/v')
Save-Text (Join-Path $runDirectory 'bcd-firmware-before.txt') $firmwareBefore

$firmwareId = $null
$firmwareBlocks = $firmwareBefore -split '(?:\r?\n){2,}'
foreach ($block in $firmwareBlocks) {
    if ($block -notmatch "(?im)^\s*path\s+.*\\EFI\\BOOT\\BOOTX64\.EFI\s*$" -or
        $block -notmatch "(?im)^\s*device\s+.*$letter`:\s*$") {
        continue
    }
    $match = [regex]::Match($block, '(?im)^\s*identifier\s+(\{[^}]+\})\s*$')
    if ($match.Success) {
        if ($firmwareId) { throw "Multiple firmware entries point to $letter`:." }
        $firmwareId = $match.Groups[1].Value
    }
}
if (-not $firmwareId) {
    throw "No UEFI firmware entry for $letter`: \\EFI\\BOOT\\BOOTX64.EFI was found."
}

$buildScript = Join-Path $projectRoot 'tools\build-windows.ps1'
$buildArguments = @('-Build', $Build, '-Debug', '2', '-RealTest')
if ($DiagnosticBot) { $buildArguments += '-DiagnosticBot' }
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript @buildArguments
if ($LASTEXITCODE -ne 0) { throw "Physical real-test build failed ($LASTEXITCODE)." }

$espPath = Join-Path (Join-Path $projectRoot $Build) 'esp'
foreach ($required in @(
    'EFI\BOOT\BOOTX64.EFI',
    'EFI\LITEOS\kernel.elf',
    'EFI\LITEOS\loader.conf'
)) {
    if (-not (Test-Path -LiteralPath (Join-Path $espPath $required))) {
        throw "Build is missing ESP file: $required"
    }
}

foreach ($relative in @(
    'EFI\BOOT\BOOTX64.EFI',
    'EFI\LITEOS',
    'etc',
    'lib',
    'sbin',
    'init',
    'init-runtime'
)) {
    Backup-Target $relative $volumeRoot
}

Remove-Item -LiteralPath (Join-Path $volumeRoot 'EFI\LITEOS\realtest.log'),
    (Join-Path $volumeRoot 'realtest.log') -Force -ErrorAction SilentlyContinue
foreach ($entry in (Get-ChildItem -LiteralPath $espPath -Force)) {
    $destination = Join-Path $volumeRoot $entry.Name
    if ($entry.PSIsContainer) {
        Copy-DirectoryContents $entry.FullName $destination
    }
    else {
        Copy-Item -LiteralPath $entry.FullName -Destination $destination -Force
    }
}

$sourceBoot = Join-Path $espPath 'EFI\BOOT\BOOTX64.EFI'
$targetBoot = Join-Path $volumeRoot 'EFI\BOOT\BOOTX64.EFI'
$sourceKernel = Join-Path $espPath 'EFI\LITEOS\kernel.elf'
$targetKernel = Join-Path $volumeRoot 'EFI\LITEOS\kernel.elf'
foreach ($pair in @(
    @($sourceBoot, $targetBoot),
    @($sourceKernel, $targetKernel)
)) {
    $sourceHash = (Get-FileHash -LiteralPath $pair[0] -Algorithm SHA256).Hash
    $targetHash = (Get-FileHash -LiteralPath $pair[1] -Algorithm SHA256).Hash
    if ($sourceHash -ne $targetHash) {
        throw "F: verification failed for $($pair[1])."
    }
}
Save-Text (Join-Path $runDirectory 'copied-files.txt') (
    (Get-ChildItem -LiteralPath $espPath -Recurse -File | ForEach-Object {
        "{0} {1}" -f $_.FullName, (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }) -join "`r`n"
)

$bcdBackup = Join-Path $runDirectory 'windows-bcd-backup'
Invoke-NativeChecked 'bcdedit.exe' @('/export', $bcdBackup) | Out-Null

if (-not (Set-FirmwareState "HOST_PREPARED run=$runId")) {
    Save-Text (Join-Path $runDirectory 'firmware-state-set.txt') (
        "failed win32_error=$([Runtime.InteropServices.Marshal]::GetLastWin32Error())"
    )
}
else {
    Save-Text (Join-Path $runDirectory 'firmware-state-set.txt') "HOST_PREPARED run=$runId"
}

$collector = (Resolve-Path (Join-Path $projectRoot 'tools\collect-realtest-log.ps1')).Path
$collectorLiteral = $collector.Replace("'", "''")
$volumeIdLiteral = ([string]$volume.UniqueId).Replace("'", "''")
$buildLiteral = $Build.Replace("'", "''")
$userSidLiteral = $identity.User.Value.Replace("'", "''")
$collectorScript = "& '$collectorLiteral' -RunId '$runId' -DriveLetter '$letter' " +
    "-VolumeUniqueId '$volumeIdLiteral' -Build '$buildLiteral' " +
    "-UserSid '$userSidLiteral'"
$collectorEncoded = [Convert]::ToBase64String(
    [Text.Encoding]::Unicode.GetBytes($collectorScript))
$collectorArguments = '-NoProfile -ExecutionPolicy Bypass -EncodedCommand ' +
    $collectorEncoded
$runOnceCommand = 'powershell.exe ' + $collectorArguments
New-ItemProperty -Path $runOncePath -Name $runOnceName -PropertyType String `
    -Value $runOnceCommand -Force | Out-Null
Save-Text (Join-Path $runDirectory 'runonce-command.txt') $runOnceCommand
Save-Text (Join-Path $runDirectory 'collector-script.txt') $collectorScript

try {
    $taskAction = New-ScheduledTaskAction -Execute 'powershell.exe' `
        -Argument $collectorArguments -WorkingDirectory $projectRoot
    $taskTriggers = @(
        (New-ScheduledTaskTrigger -AtStartup),
        (New-ScheduledTaskTrigger -AtLogOn -User $identity.Name)
    )
    $taskPrincipal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' `
        -LogonType ServiceAccount -RunLevel Highest
    Register-ScheduledTask -TaskName $runOnceName -Action $taskAction `
        -Trigger $taskTriggers -Principal $taskPrincipal -Force | Out-Null
    Save-Text (Join-Path $runDirectory 'scheduled-task.txt') (
        "$runOnceName`r`nprincipal=SYSTEM`r`ntriggers=AtStartup,AtLogOn"
    )
}
catch {
    Save-Text (Join-Path $runDirectory 'scheduled-task.txt') `
        "registration-failed: $($_.Exception.Message)"
}

try {
    Invoke-NativeChecked 'bcdedit.exe' @('/set', '{fwbootmgr}', 'bootsequence', $firmwareId) |
        Out-Null
    $bootSequenceSet = $true
    $firmwareAfter = Invoke-NativeChecked 'bcdedit.exe' @('/enum', '{fwbootmgr}', '/v')
    Save-Text (Join-Path $runDirectory 'bcd-fwbootmgr-after.txt') $firmwareAfter
    if ($firmwareAfter -notmatch [regex]::Escape($firmwareId)) {
        throw "UEFI one-time boot sequence was not set to $firmwareId."
    }
    Save-Text (Join-Path $runDirectory 'reboot-requested.txt') (
        "BootNext firmware entry: $firmwareId`r`nTarget: $letter`:\EFI\BOOT\BOOTX64.EFI`r`n"
    )
    if ($NoReboot) {
        Write-Host "Prepared one-time LiteOS boot for $letter`:; reboot was not requested."
        exit 0
    }
    Write-Host "Starting one-time LiteOS boot from $letter`:. Windows will resume after LiteOS resets."
    $shutdownArguments = @('/r')
    if ($ForceReboot) { $shutdownArguments += '/f' }
    $shutdownArguments += @('/t', '0', '/d', 'p:4:1', '/c', 'LiteOS real hardware test')
    & shutdown.exe @shutdownArguments
    if ($LASTEXITCODE -ne 0) { throw "shutdown.exe failed ($LASTEXITCODE)." }
}
catch {
    Remove-ItemProperty -Path $runOncePath -Name $runOnceName -ErrorAction SilentlyContinue
    Unregister-ScheduledTask -TaskName $runOnceName -Confirm:$false `
        -ErrorAction SilentlyContinue
    if ($bootSequenceSet) {
        & bcdedit.exe /deletevalue '{fwbootmgr}' bootsequence 2>$null | Out-Null
    }
    throw
}
