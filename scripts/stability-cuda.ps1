param(
  [Parameter(Mandatory = $true)]
  [string]$Executable,
  [ValidateSet("GpuOnly", "CpuGpu")]
  [string]$Mode = "GpuOnly",
  [ValidateRange(60, 43200)]
  [int]$DurationSeconds = 600,
  [ValidateRange(1, 60)]
  [int]$SampleIntervalSeconds = 5,
  [ValidateRange(0, 43200)]
  [int]$PauseAfterSeconds = 0,
  [ValidateRange(1, 300)]
  [int]$PauseDurationSeconds = 10,
  [string]$DataDirectory = "",
  [string]$OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
$executableDirectory = [System.IO.Path]::GetDirectoryName($resolvedExecutable)
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
if (-not $DataDirectory) {
  $DataDirectory = Join-Path $executableDirectory ("stability-runtime-" + $stamp)
}
if (-not $OutputDirectory) {
  $OutputDirectory = Join-Path $executableDirectory "stability-logs"
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$csvPath = Join-Path $OutputDirectory ("stability-$Mode-$stamp.csv")
$summaryPath = Join-Path $OutputDirectory ("stability-$Mode-$stamp.json")
$requestHeaders = @{ Origin = "http://127.0.0.1:8787" }
$apiFailures = 0

function Invoke-VanylithPost([string]$Path, $Body) {
  try {
    return Invoke-RestMethod -Uri ("http://127.0.0.1:8787" + $Path) -Method Post `
      -Headers $requestHeaders -ContentType "application/json" `
      -Body ($Body | ConvertTo-Json -Compress)
  } catch {
    $script:apiFailures++
    throw
  }
}

function Get-VanylithStatus {
  try {
    return Invoke-RestMethod "http://127.0.0.1:8787/api/status"
  } catch {
    $script:apiFailures++
    throw
  }
}

$listener = Get-NetTCPConnection -LocalPort 8787 -State Listen -ErrorAction SilentlyContinue
if ($listener) {
  throw "Port 8787 is already in use by PID $($listener.OwningProcess)"
}

$process = Start-Process -FilePath $resolvedExecutable `
  -ArgumentList "--no-browser", "--data-dir", $DataDirectory `
  -WindowStyle Hidden -PassThru
$samples = @()
$pausePassed = $PauseAfterSeconds -eq 0
$monotonic = $true
$lastChecked = [uint64]0
$runError = ""
$stopPassed = $false
try {
  $ready = $false
  for ($attempt = 0; $attempt -lt 80; ++$attempt) {
    try {
      Get-VanylithStatus | Out-Null
      $ready = $true
      break
    } catch {
      Start-Sleep -Milliseconds 250
    }
  }
  if (-not $ready) { throw "Vanylith API did not become ready" }
  $apiFailures = 0

  Invoke-VanylithPost "/api/devices/config" @{ id = "cpu-0"; enabled = ($Mode -eq "CpuGpu") } | Out-Null
  Invoke-VanylithPost "/api/devices/config" @{ id = "cuda-0"; enabled = $true } | Out-Null
  Invoke-VanylithPost "/api/task/start" @{
    matchMode = "specific"
    matchPosition = "suffix"
    specificPrefix = ""
    specificSuffix = "11111111111111111111"
    repeatPrefixLen = 10
    repeatSuffixLen = 10
    targetCount = 1000
  } | Out-Null

  $watch = [System.Diagnostics.Stopwatch]::StartNew()
  while ($watch.Elapsed.TotalSeconds -lt $DurationSeconds) {
    Start-Sleep -Seconds $SampleIntervalSeconds
    $snapshot = Get-VanylithStatus
    if ($snapshot.status -ne "RUNNING") {
      throw "Search left RUNNING state: $($snapshot.status) $($snapshot.error)"
    }
    if ([uint64]$snapshot.checked -lt $lastChecked) { $monotonic = $false }
    $lastChecked = [uint64]$snapshot.checked
    $gpu = $snapshot.devices | Where-Object id -eq "cuda-0"
    $cpu = $snapshot.devices | Where-Object id -eq "cpu-0"
    $telemetryLine = (& nvidia-smi `
      --query-gpu=utilization.gpu,temperature.gpu,power.draw,memory.used,memory.total `
      --format=csv,noheader,nounits | Select-Object -First 1)
    $telemetry = $telemetryLine -split "," | ForEach-Object { [double]$_.Trim() }
    $liveProcess = Get-Process -Id $process.Id
    $samples += [pscustomobject]@{
      timestampUtc = (Get-Date).ToUniversalTime().ToString("o")
      wallSeconds = [math]::Round($watch.Elapsed.TotalSeconds, 1)
      status = $snapshot.status
      checked = [uint64]$snapshot.checked
      totalKeysPerSecond = [double]$snapshot.totalHashrate
      gpuKeysPerSecond = [double]$gpu.hashrate
      cpuKeysPerSecond = if ($cpu) { [double]$cpu.hashrate } else { 0.0 }
      gpuUtilizationPct = $telemetry[0]
      gpuTemperatureC = $telemetry[1]
      gpuPowerW = $telemetry[2]
      gpuMemoryUsedMiB = $telemetry[3]
      gpuMemoryTotalMiB = $telemetry[4]
      processPrivateMemoryMiB = [math]::Round($liveProcess.PrivateMemorySize64 / 1MB, 1)
      error = [string]$snapshot.error
    }

    if (-not $pausePassed -and $watch.Elapsed.TotalSeconds -ge $PauseAfterSeconds) {
      Invoke-VanylithPost "/api/task/pause" @{} | Out-Null
      $pausedFirst = Get-VanylithStatus
      Start-Sleep -Seconds $PauseDurationSeconds
      $pausedSecond = Get-VanylithStatus
      if ($pausedFirst.status -ne "PAUSED" -or $pausedSecond.status -ne "PAUSED" -or
          [uint64]$pausedFirst.checked -ne [uint64]$pausedSecond.checked) {
        throw "Pause did not hold the checked count constant"
      }
      Invoke-VanylithPost "/api/task/resume" @{} | Out-Null
      $pausePassed = $true
    }
  }

  Invoke-VanylithPost "/api/task/stop" @{} | Out-Null
  $stopped = Get-VanylithStatus
  $stopPassed = $stopped.status -eq "STOPPED"
  if (-not $stopPassed) { throw "Stop did not produce STOPPED state" }
} catch {
  $runError = $_.Exception.Message
} finally {
  if ($samples.Count -ne 0) { $samples | Export-Csv -LiteralPath $csvPath -NoTypeInformation }
  if (-not $process.HasExited) {
    try {
      $current = Get-VanylithStatus
      if ($current.status -eq "RUNNING" -or $current.status -eq "PAUSED") {
        Invoke-VanylithPost "/api/task/stop" @{} | Out-Null
      }
    } catch {}
    Stop-Process -Id $process.Id -Force
  }
}

$summary = [ordered]@{
  mode = $Mode
  requestedDurationSeconds = $DurationSeconds
  samples = $samples.Count
  firstChecked = if ($samples.Count) { $samples[0].checked } else { 0 }
  lastChecked = if ($samples.Count) { $samples[-1].checked } else { 0 }
  averageTotalKeysPerSecond = if ($samples.Count) { [math]::Round(($samples.totalKeysPerSecond | Measure-Object -Average).Average, 1) } else { 0 }
  averageGpuKeysPerSecond = if ($samples.Count) { [math]::Round(($samples.gpuKeysPerSecond | Measure-Object -Average).Average, 1) } else { 0 }
  averageCpuKeysPerSecond = if ($samples.Count) { [math]::Round(($samples.cpuKeysPerSecond | Measure-Object -Average).Average, 1) } else { 0 }
  averageGpuUtilizationPct = if ($samples.Count) { [math]::Round(($samples.gpuUtilizationPct | Measure-Object -Average).Average, 1) } else { 0 }
  maximumGpuTemperatureC = if ($samples.Count) { ($samples.gpuTemperatureC | Measure-Object -Maximum).Maximum } else { 0 }
  maximumGpuPowerW = if ($samples.Count) { ($samples.gpuPowerW | Measure-Object -Maximum).Maximum } else { 0 }
  maximumGpuMemoryUsedMiB = if ($samples.Count) { ($samples.gpuMemoryUsedMiB | Measure-Object -Maximum).Maximum } else { 0 }
  minimumProcessPrivateMemoryMiB = if ($samples.Count) { ($samples.processPrivateMemoryMiB | Measure-Object -Minimum).Minimum } else { 0 }
  maximumProcessPrivateMemoryMiB = if ($samples.Count) { ($samples.processPrivateMemoryMiB | Measure-Object -Maximum).Maximum } else { 0 }
  checkedMonotonic = $monotonic
  pauseResumePassed = $pausePassed
  stopPassed = $stopPassed
  apiFailures = $apiFailures
  error = $runError
  csvLog = $csvPath
}
$summary | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $summaryPath -Encoding utf8
$summary | ConvertTo-Json -Depth 4
if ($runError -or -not $monotonic -or -not $pausePassed -or -not $stopPassed -or $apiFailures -ne 0) {
  throw "Stability validation failed; see $summaryPath"
}
