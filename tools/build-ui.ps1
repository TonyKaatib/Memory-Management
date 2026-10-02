#requires -Version 7.0
[CmdletBinding()]
param([switch]$SkipTests)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$windowExe = Join-Path $projectRoot 'ui\SpaceLedger.App\bin\x64\Release\net10.0-windows10.0.26100.0\win-x64\SpaceLedger.App.exe'
if (Get-Process -Name 'SpaceLedger.App' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $windowExe }) {
    throw 'Close the SpaceLedger window before rebuilding it.'
}
& (Join-Path $PSScriptRoot 'build.ps1') -SkipTests:$SkipTests
dotnet build (Join-Path $projectRoot 'ui\SpaceLedger.App\SpaceLedger.App.csproj') -c Release -p:Platform=x64
if ($LASTEXITCODE -ne 0) { throw 'SpaceLedger window build failed.' }
Write-Output $windowExe
