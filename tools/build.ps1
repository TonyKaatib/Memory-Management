#requires -Version 7.0
[CmdletBinding()]
param([switch]$SkipTests, [switch]$Fresh)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer was not found. Install the Desktop development with C++ workload.' }
$installation = (& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json | ConvertFrom-Json)[0]
if (!$installation) { throw 'No Visual Studio C++ toolchain was found.' }
$cmakeBin = Join-Path $installation.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$cmake = Join-Path $cmakeBin 'cmake.exe'
$ctest = Join-Path $cmakeBin 'ctest.exe'
if (!(Test-Path -LiteralPath $cmake)) { throw 'Install the Visual Studio C++ CMake tools component.' }
$major = ([version]$installation.installationVersion).Major
$generator = switch ($major) { 18 { 'Visual Studio 18 2026' }; 17 { 'Visual Studio 17 2022' }; default { throw 'Visual Studio 2022 or 2026 is required.' } }

function Invoke-BuildTool([string]$Executable, [string[]]$ToolArguments) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.WorkingDirectory = $projectRoot
    $start.UseShellExecute = $false
    foreach ($argument in $ToolArguments) { $start.ArgumentList.Add($argument) }
    # Some host processes supply both PATH and Path; .NET Framework MSBuild rejects
    # duplicate environment keys. Normalize only this child process's environment.
    $normalized = [System.Collections.Generic.Dictionary[string,string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($item in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $normalized[$item.Key] = $item.Value }
    $start.Environment.Clear()
    foreach ($item in $normalized.GetEnumerator()) { $start.Environment[$item.Key] = $item.Value }
    $process = [System.Diagnostics.Process]::Start($start)
    try {
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "$Executable exited with code $($process.ExitCode)" }
    } finally { $process.Dispose() }
}

$configureArguments = @('-S', '.', '-B', 'build', '-G', $generator, '-A', 'x64', '-DBUILD_TESTING=ON')
if ($Fresh) { $configureArguments += '--fresh' }
Invoke-BuildTool $cmake $configureArguments
Invoke-BuildTool $cmake @('--build', 'build', '--config', 'Release', '--parallel')
if (!$SkipTests) { Invoke-BuildTool $ctest @('--test-dir', 'build', '-C', 'Release', '--output-on-failure') }
