#Requires -Version 7.0
param([ValidatePattern('^[A-Za-z0-9._-]+$')][string]$Name = 'NoiseKiller-cable-engineering-0.2.2')
$ErrorActionPreference = 'Stop'
$taskWorkspace = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release
$taskDestination = Join-Path (Join-Path $taskWorkspace 'dist') $Name
$taskArchive = $taskDestination + '.zip'
if ((Test-Path -LiteralPath $taskDestination) -or (Test-Path -LiteralPath $taskArchive)) {
    throw 'Package already exists. Choose a new -Name; no existing package is overwritten or deleted.'
}
New-Item -ItemType Directory -Path $taskDestination -Force | Out-Null
foreach ($taskExecutable in @('NoiseKiller.exe','NoiseKiller.Cli.exe')) {
    Copy-Item -LiteralPath (Join-Path (Join-Path $taskWorkspace 'build\Release') $taskExecutable) -Destination $taskDestination
}
foreach ($taskDocument in @('README.md','components.lock.json')) {
    Copy-Item -LiteralPath (Join-Path $taskWorkspace $taskDocument) -Destination $taskDestination
}
# Private design documents may exist locally but are absent from the public repo.
foreach ($taskDocument in @('NoiseKiller_需求规格说明书.md','NoiseKiller_架构设计文档.md','MECAG_%E5%A4%9A%E8%AF%81%E6%8D%AE%E7%AB%9E%E4%BA%89%E5%BC%8F%E5%8A%A8%E6%80%81%E5%A3%B0%E6%A0%85_%E6%96%B9%E6%A1%88.md')) {
    $taskSourceDocument = Join-Path $taskWorkspace $taskDocument
    if (Test-Path -LiteralPath $taskSourceDocument -PathType Leaf) {
        Copy-Item -LiteralPath $taskSourceDocument -Destination $taskDestination
    }
}
$taskDocsDestination = Join-Path $taskDestination 'docs'
foreach ($taskDocument in @('虚拟线路接入指南.md','MVP_构建与验收进度.md','MECAG_重叠修复与回归记录.md','MECAG_合成校准与独立留出.md')) {
    $taskSourceDocument = Join-Path (Join-Path $taskWorkspace 'docs') $taskDocument
    if (Test-Path -LiteralPath $taskSourceDocument -PathType Leaf) {
        if (-not (Test-Path -LiteralPath $taskDocsDestination)) {
            New-Item -ItemType Directory -Path $taskDocsDestination | Out-Null
        }
        Copy-Item -LiteralPath $taskSourceDocument -Destination $taskDocsDestination
    }
}
Compress-Archive -Path (Join-Path $taskDestination '*') -DestinationPath $taskArchive -CompressionLevel Optimal
Get-FileHash -LiteralPath $taskArchive -Algorithm SHA256 | Format-List
Write-Host 'Third-party cable engineering ZIP. Install the cable separately; no bundled driver or Broker. Real cable/quality acceptance remains pending.'
