#Requires -Version 7.0
param([switch]$BuildNative)
$ErrorActionPreference = 'Stop'
$taskWorkspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskFrontend = Join-Path $taskWorkspace 'electron'
$taskHost = Join-Path $taskWorkspace 'build\Release\NoiseKiller.Host.exe'
if ($BuildNative -or -not (Test-Path -LiteralPath $taskHost -PathType Leaf)) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release -SkipTests
}
if (-not (Test-Path -LiteralPath (Join-Path $taskFrontend 'node_modules\electron\dist\electron.exe'))) {
    throw 'Electron runtime missing. Run npm ci then npx install-electron inside electron/. No system component is installed by this launcher.'
}
Push-Location -LiteralPath $taskFrontend
try {
    & npm.cmd start
    if ($LASTEXITCODE -ne 0) { throw "Electron exited with code $LASTEXITCODE" }
} finally { Pop-Location }
