#requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Executable, [Parameter(Mandatory)][string]$FixtureRoot)

$ErrorActionPreference = 'Stop'
$name = "Local\SpaceLedger-test-$([guid]::NewGuid().ToString('N'))"
$database = Join-Path $FixtureRoot "cancel-event-$([guid]::NewGuid().ToString('N')).db"
$signal = [System.Threading.EventWaitHandle]::new($true, [System.Threading.EventResetMode]::ManualReset, $name)
try {
    & $Executable scan $FixtureRoot --json-stream --cancel-event $name --database $database
    if ($LASTEXITCODE -ne 130) { throw "Expected cancellation exit 130, got $LASTEXITCODE" }
    if (Test-Path -LiteralPath $database) { throw 'Cancelled scan saved a database' }
} finally {
    $signal.Dispose()
}
