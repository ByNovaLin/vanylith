param(
  [Parameter(Mandatory = $true)]
  [string]$Zip,
  [ValidateRange(5, 120)]
  [int]$TimeoutSeconds = 30
)

$ErrorActionPreference = "Stop"
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$resolvedZip = (Resolve-Path -LiteralPath $Zip).Path
$stamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
$smokeRoot = Join-Path $projectRoot ("build\portable-smoke-" + $stamp)
$runtimeRoot = Join-Path $smokeRoot "runtime"
New-Item -ItemType Directory -Path $smokeRoot -Force | Out-Null
Expand-Archive -LiteralPath $resolvedZip -DestinationPath $smokeRoot
$executable = Get-ChildItem -LiteralPath $smokeRoot -Filter Vanylith.exe -Recurse | `
  Select-Object -First 1 -ExpandProperty FullName
if (-not $executable) { throw "Vanylith.exe is missing from the ZIP" }

$savedPath = $env:PATH
$cudaVariables = @(Get-ChildItem Env: | Where-Object Name -Like "CUDA*" | `
  ForEach-Object { [pscustomobject]@{ Name = $_.Name; Value = $_.Value } })
$process = $null
try {
  $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\Wbem"
  foreach ($variable in $cudaVariables) { Remove-Item -LiteralPath ("Env:" + $variable.Name) }

  $process = Start-Process -FilePath $executable `
    -ArgumentList "--no-browser", "--data-dir", ('"' + $runtimeRoot + '"') `
    -WorkingDirectory ([System.IO.Path]::GetDirectoryName($executable)) `
    -WindowStyle Hidden -PassThru
  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  $snapshot = $null
  do {
    if ($process.HasExited) { throw "Portable executable exited with code $($process.ExitCode)" }
    try { $snapshot = Invoke-RestMethod "http://127.0.0.1:8787/api/status" } catch {}
    if (-not $snapshot) { Start-Sleep -Milliseconds 250 }
  } while (-not $snapshot -and (Get-Date) -lt $deadline)
  if (-not $snapshot) { throw "Portable executable API did not become ready" }

  $enabled = @($snapshot.devices | Where-Object enabled)
  $enabledCuda = @($enabled | Where-Object backend -EQ "CUDA")
  $enabledCpu = @($enabled | Where-Object backend -EQ "CPU")
  if ($snapshot.status -ne "READY") { throw "Expected READY, received $($snapshot.status)" }
  if ($enabledCuda.Count -ne 1 -or $enabledCpu.Count -ne 0) {
    throw "Default device policy is not one CUDA GPU with CPU disabled"
  }

  [pscustomobject]@{
    zip = $resolvedZip
    extractedExecutable = $executable
    status = $snapshot.status
    selectedDevice = $enabledCuda[0].name
    cudaToolkitPathPresent = $env:PATH -match "CUDA"
    cudaEnvironmentVariablesPresent = @(Get-ChildItem Env: | Where-Object Name -Like "CUDA*").Count -ne 0
    result = "PASS"
  } | Format-List
} finally {
  if ($process -and -not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
    Wait-Process -Id $process.Id -ErrorAction SilentlyContinue
  }
  $env:PATH = $savedPath
  foreach ($variable in $cudaVariables) {
    Set-Item -LiteralPath ("Env:" + $variable.Name) -Value $variable.Value
  }
}
