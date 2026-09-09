param(
  [string]$Configuration = 'Release',
  [string]$BuildDirectory = 'build\windows-x64-release'
)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$executable = Join-Path $projectRoot "$BuildDirectory\bin\$Configuration\Vanylith.exe"
if (-not (Test-Path -LiteralPath $executable)) {
  throw "Release executable not found: $executable"
}

$distRoot = Join-Path $projectRoot 'dist'
$releaseName = 'Vanylith-v0.1.0-win-x64'
$releaseDirectory = Join-Path $distRoot $releaseName
$zipPath = Join-Path $distRoot ($releaseName + '.zip')

if (Test-Path -LiteralPath $releaseDirectory) {
  $resolvedRelease = (Resolve-Path -LiteralPath $releaseDirectory).Path
  if (-not $resolvedRelease.StartsWith($distRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to clean a release directory outside dist'
  }
  Remove-Item -LiteralPath $resolvedRelease -Recurse -Force
}
if (Test-Path -LiteralPath $zipPath) {
  $resolvedZip = (Resolve-Path -LiteralPath $zipPath).Path
  if (-not $resolvedZip.StartsWith($distRoot, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to replace a ZIP outside dist'
  }
  Remove-Item -LiteralPath $resolvedZip -Force
}

New-Item -ItemType Directory -Path $releaseDirectory | Out-Null
& (Join-Path $PSScriptRoot 'audit-dependencies.ps1') -Executable $executable
Copy-Item -LiteralPath $executable -Destination (Join-Path $releaseDirectory 'Vanylith.exe')
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.txt') -Destination $releaseDirectory
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination (Join-Path $releaseDirectory 'LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $projectRoot 'NOTICE') -Destination (Join-Path $releaseDirectory 'NOTICE.txt')
Copy-Item -LiteralPath (Join-Path $projectRoot 'SECURITY.md') -Destination $releaseDirectory
Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.md') -Destination $releaseDirectory

$hashFiles = @(
  'Vanylith.exe',
  'README.txt',
  'LICENSE.txt',
  'NOTICE.txt',
  'SECURITY.md',
  'THIRD_PARTY_NOTICES.md'
)
$sumLines = foreach ($name in $hashFiles) {
  $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $releaseDirectory $name)).Hash.ToLowerInvariant()
  "$hash *$name"
}
Set-Content -LiteralPath (Join-Path $releaseDirectory 'SHA256SUMS.txt') -Value $sumLines -Encoding ascii
Compress-Archive -Path (Join-Path $releaseDirectory '*') -DestinationPath $zipPath -CompressionLevel Optimal

Get-Item -LiteralPath $zipPath
