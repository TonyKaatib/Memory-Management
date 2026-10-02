#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$Database,
    [ValidateRange(2,10000)][int]$Keep = 30
)

$ErrorActionPreference = 'Stop'
& $Executable scan $Root --database $Database --quiet
$scanResult = $LASTEXITCODE
if ($scanResult -ne 0 -and $scanResult -ne 2) { exit $scanResult }
& $Executable retention --path $Root --keep $Keep --database $Database --apply
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
# Preserve code 2 so Task Scheduler shows when the stored scan has coverage gaps.
exit $scanResult
