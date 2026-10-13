#Requires -Version 7.0
param([ValidateSet('Release','Debug')][string]$Configuration = 'Release', [switch]$SkipTests)
$ErrorActionPreference = 'Stop'
$taskWorkspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskVswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $taskVswhere)) { throw 'Visual Studio Installer/vswhere was not found.' }
$taskVsPath = (& $taskVswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
if (-not $taskVsPath) { throw 'Install Visual Studio C++ desktop tools and Windows SDK. No system installation is performed by this script.' }
$taskCmake = Join-Path $taskVsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$taskCtest = Join-Path (Split-Path $taskCmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $taskCmake)) { throw 'Install the Visual Studio C++ CMake tools component.' }
$taskBuild = Join-Path $taskWorkspace 'build'
# A host can supply both Path and PATH. MSBuild's .NET Framework child-process
# launcher throws on those duplicate keys. Normalize only the child's environment;
# never edit machine/user environment variables or expose their values in logs.
function Invoke-TaskNative([string]$Executable, [string[]]$NativeArguments) {
    $taskStart = [System.Diagnostics.ProcessStartInfo]::new()
    $taskStart.FileName = $Executable
    $taskStart.WorkingDirectory = $taskWorkspace
    $taskStart.UseShellExecute = $false
    $taskStart.CreateNoWindow = $true
    $taskStart.RedirectStandardOutput = $true
    $taskStart.RedirectStandardError = $true
    foreach ($taskArgument in $NativeArguments) { $taskStart.ArgumentList.Add($taskArgument) }
    $taskStart.Environment.Clear()
    $taskNames = [System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $taskEnvironment = [Environment]::GetEnvironmentVariables()
    foreach ($taskName in $taskEnvironment.Keys) {
        if ($taskNames.Add([string]$taskName)) { $taskStart.Environment[[string]$taskName] = [string]$taskEnvironment[$taskName] }
    }
    $taskProcess = [System.Diagnostics.Process]::Start($taskStart)
    $taskOutput = $taskProcess.StandardOutput.ReadToEndAsync()
    $taskErrorOutput = $taskProcess.StandardError.ReadToEndAsync()
    $taskProcess.WaitForExit()
    $taskText = $taskOutput.GetAwaiter().GetResult()
    $taskErrorText = $taskErrorOutput.GetAwaiter().GetResult()
    if ($taskText) { Write-Host $taskText }
    if ($taskErrorText) { Write-Host $taskErrorText }
    $taskExit = $taskProcess.ExitCode
    $taskProcess.Dispose()
    if ($taskExit -ne 0) { throw "Native build/test command failed (exit $taskExit)." }
}
Invoke-TaskNative $taskCmake @('--fresh','-S',$taskWorkspace,'-B',$taskBuild,'-G','Visual Studio 17 2022','-A','x64')
Invoke-TaskNative $taskCmake @('--build',$taskBuild,'--config',$Configuration,'--parallel','4')
if (-not $SkipTests) {
    Invoke-TaskNative $taskCtest @('--test-dir',$taskBuild,'-C',$Configuration,'--output-on-failure')
}
Write-Host "User-mode engineering build: $taskBuild\$Configuration\NoiseKiller.exe"
Write-Host 'Current route: MECAG -> WASAPI render -> separately installed third-party cable. No kernel driver/Broker is built or installed; G1/G2/G3 acceptance is not certified.'
