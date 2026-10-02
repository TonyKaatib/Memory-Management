#requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Plan','Add','Show','Remove')][string]$Mode = 'Plan',
    [Parameter(Mandatory)][string]$Root,
    [string]$Database,
    [ValidateRange(2,10000)][int]$Keep = 30,
    [TimeSpan]$At = '03:00:00',
    [switch]$RunWhenLoggedOff
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $projectRoot 'build\Release\spaceledger.exe'
$runner = Join-Path $PSScriptRoot 'run-scheduled.ps1'
if ($Mode -in @('Plan','Add') -and !(Test-Path -LiteralPath $executable -PathType Leaf)) { throw 'Build SpaceLedger first with .\tools\build.ps1.' }
$rootPath = if ($Mode -in @('Plan','Add')) { (Resolve-Path -LiteralPath $Root -ErrorAction Stop).ProviderPath } else { [IO.Path]::GetFullPath($Root) }
if ($Mode -in @('Plan','Add') -and !(Test-Path -LiteralPath $rootPath -PathType Container)) { throw 'Root must be a directory.' }
if (!$Database) { $Database = Join-Path $projectRoot '.spaceledger\history.db' }
$databasePath = [IO.Path]::GetFullPath($Database)
$rootPath = [IO.Path]::GetFullPath($rootPath).TrimEnd('\')
if ($rootPath.Length -eq 2 -and $rootPath[1] -eq ':') { $rootPath += '\' }
if ($At -lt [TimeSpan]::Zero -or $At -ge [TimeSpan]::FromDays(1)) { throw '-At must be a time between 00:00 and 23:59.' }

$identity = ($rootPath.ToUpperInvariant() + '|' + $databasePath.ToUpperInvariant())
$digestBytes = [Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($identity))
$digest = [Convert]::ToHexString($digestBytes).Substring(0, 12)
$taskName = "SpaceLedger-$digest"
$description = "SpaceLedger root=$rootPath database=$databasePath"

function Quote-Argument([string]$Value) {
    if ($Value.Contains('"')) { throw 'Path contains an unsupported quote character.' }
    return '"' + $Value + '"'
}

$arguments = @(
    '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', (Quote-Argument $runner),
    '-Executable', (Quote-Argument $executable), '-Root', (Quote-Argument $rootPath),
    '-Database', (Quote-Argument $databasePath), '-Keep', $Keep
) -join ' '
$shell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (!(Test-Path -LiteralPath $shell -PathType Leaf)) { throw 'Windows PowerShell was not found.' }

if ($Mode -eq 'Plan') {
    Write-Output "Task: $taskName"
    Write-Output "Root: $rootPath"
    Write-Output "Database: $databasePath"
    Write-Output "Daily at: $At (local time)"
    Write-Output "Keep: $Keep snapshots, plus latest baseline without recorded coverage issues"
    Write-Output "Logon: $(if ($RunWhenLoggedOff) { 'S4U: may run while logged off' } else { 'Interactive: runs while signed in' })"
    Write-Output "Action: $shell $arguments"
    Write-Output 'This is a preview. Use -Mode Add to register the task.'
    return
}

Import-Module ScheduledTasks -ErrorAction Stop
$existing = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($Mode -eq 'Add') {
    if ($existing) { throw "Task $taskName already exists. Remove it explicitly before changing its settings." }
    $action = New-ScheduledTaskAction -Execute $shell -Argument $arguments -WorkingDirectory $projectRoot
    $trigger = New-ScheduledTaskTrigger -Daily -At ([DateTime]::Today + $At)
    $settings = New-ScheduledTaskSettingsSet -StartWhenAvailable -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Hours 4)
    $logon = if ($RunWhenLoggedOff) { 'S4U' } else { 'Interactive' }
    $principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType $logon -RunLevel Limited
    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Description $description -ErrorAction Stop | Out-Null
    Write-Output "Registered $taskName for daily scans of $rootPath at $At."
    return
}
if (!$existing) { throw "Task $taskName was not found." }
if ($existing.Description -ne $description -or $existing.Actions.Count -ne 1 -or !$existing.Actions[0].Arguments.Contains((Quote-Argument $runner))) {
    throw "Task $taskName does not match this SpaceLedger installation; no changes made."
}
if ($Mode -eq 'Show') {
    $info = Get-ScheduledTaskInfo -TaskName $taskName -ErrorAction Stop
    Write-Output "Task: $taskName"
    Write-Output "State: $($existing.State)"
    Write-Output "Last run: $($info.LastRunTime)"
    Write-Output "Last result: $($info.LastTaskResult) (0=complete, 2=stored with coverage issues)"
    Write-Output "Next run: $($info.NextRunTime)"
    return
}
Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction Stop
Write-Output "Removed scheduled task $taskName. Snapshot history was not changed."
