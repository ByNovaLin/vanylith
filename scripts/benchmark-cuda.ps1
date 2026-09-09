param(
  [Parameter(Mandatory = $true)]
  [string]$Executable,
  [ValidateRange(20, 3600)]
  [int]$DurationSeconds = 25,
  [ValidateRange(1, 300)]
  [int]$WarmupSeconds = 5,
  [ValidateSet("All", "Literal Prefix", "Literal Suffix", "Repeat Prefix", "Repeat Suffix")]
  [string]$Case = "All",
  [string]$DataDirectory = ""
)

$ErrorActionPreference = "Stop"

if ($WarmupSeconds -ge $DurationSeconds) {
  throw "WarmupSeconds must be smaller than DurationSeconds"
}

$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path
if (-not $DataDirectory) {
  $DataDirectory = Join-Path ([System.IO.Path]::GetDirectoryName($resolvedExecutable)) "benchmark-runtime"
}
$requestHeaders = @{ Origin = "http://127.0.0.1:8787" }

function Invoke-VanylithPost([string]$Path, $Body) {
  Invoke-RestMethod -Uri ("http://127.0.0.1:8787" + $Path) -Method Post `
    -Headers $requestHeaders -ContentType "application/json" `
    -Body ($Body | ConvertTo-Json -Compress)
}

function Measure-VanylithCase([string]$Name, $Task) {
  Invoke-VanylithPost "/api/task/start" $Task | Out-Null
  $samples = @()
  for ($second = 1; $second -le $DurationSeconds; ++$second) {
    Start-Sleep -Seconds 1
    $snapshot = Invoke-RestMethod "http://127.0.0.1:8787/api/status"
    if ($snapshot.status -ne "RUNNING") {
      throw "Benchmark case '$Name' left RUNNING state: $($snapshot.status) $($snapshot.error)"
    }
    $gpu = $snapshot.devices | Where-Object id -eq "cuda-0"
    $telemetry = (& nvidia-smi `
      --query-gpu=utilization.gpu,temperature.gpu,power.draw,memory.used,memory.total `
      --format=csv,noheader,nounits) -split "," | ForEach-Object { [double]$_.Trim() }
    if ($second -gt $WarmupSeconds) {
      $samples += [pscustomobject]@{
        rate = [double]$gpu.hashrate
        utilization = $telemetry[0]
        temperature = $telemetry[1]
        power = $telemetry[2]
        memoryUsed = $telemetry[3]
        memoryTotal = $telemetry[4]
        checked = [uint64]$snapshot.checked
      }
    }
  }
  Invoke-VanylithPost "/api/task/stop" @{} | Out-Null
  $result = [pscustomobject]@{
    case = $Name
    durationSeconds = $DurationSeconds
    warmupSeconds = $WarmupSeconds
    keysPerSecond = [math]::Round(($samples.rate | Measure-Object -Average).Average, 1)
    utilizationPct = [math]::Round(($samples.utilization | Measure-Object -Average).Average, 1)
    temperatureC = [math]::Round(($samples.temperature | Measure-Object -Average).Average, 1)
    powerW = [math]::Round(($samples.power | Measure-Object -Average).Average, 2)
    vramUsedMaxMiB = ($samples.memoryUsed | Measure-Object -Maximum).Maximum
    vramTotalMiB = ($samples.memoryTotal | Measure-Object -Maximum).Maximum
    checked = $samples[-1].checked
  }
  Invoke-VanylithPost "/api/task/reset" @{} | Out-Null
  return $result
}

$listener = Get-NetTCPConnection -LocalPort 8787 -State Listen -ErrorAction SilentlyContinue
if ($listener) {
  throw "Port 8787 is already in use by PID $($listener.OwningProcess)"
}

$process = Start-Process -FilePath $resolvedExecutable `
  -ArgumentList "--no-browser", "--data-dir", $DataDirectory `
  -WindowStyle Hidden -PassThru
try {
  $ready = $false
  for ($attempt = 0; $attempt -lt 80; ++$attempt) {
    try {
      Invoke-RestMethod "http://127.0.0.1:8787/api/status" | Out-Null
      $ready = $true
      break
    } catch {
      Start-Sleep -Milliseconds 250
    }
  }
  if (-not $ready) { throw "Vanylith API did not become ready" }

  Invoke-VanylithPost "/api/devices/config" @{ id = "cpu-0"; enabled = $false } | Out-Null
  Invoke-VanylithPost "/api/devices/config" @{ id = "cuda-0"; enabled = $true } | Out-Null

  $common = @{
    repeatPrefixLen = 10
    repeatSuffixLen = 10
    targetCount = 1000
  }
  $benchmarkCases = @(
    [pscustomobject]@{ Name = "Literal Prefix"; Task = ($common + @{
      matchMode = "specific"; matchPosition = "prefix"
      specificPrefix = "11111111111111111111"; specificSuffix = ""
    }) }
    [pscustomobject]@{ Name = "Literal Suffix"; Task = ($common + @{
      matchMode = "specific"; matchPosition = "suffix"
      specificPrefix = ""; specificSuffix = "11111111111111111111"
    }) }
    [pscustomobject]@{ Name = "Repeat Prefix"; Task = ($common + @{
      matchMode = "repeat"; matchPosition = "prefix"; specificPrefix = ""; specificSuffix = ""
    }) }
    [pscustomobject]@{ Name = "Repeat Suffix"; Task = ($common + @{
      matchMode = "repeat"; matchPosition = "suffix"; specificPrefix = ""; specificSuffix = ""
    }) }
  )
  if ($Case -ne "All") {
    $benchmarkCases = @($benchmarkCases | Where-Object Name -eq $Case)
  }
  $results = @($benchmarkCases | ForEach-Object {
    Measure-VanylithCase $_.Name $_.Task
  })
  $results | Format-Table -AutoSize
  $results | ConvertTo-Json -Depth 4
} finally {
  if (-not $process.HasExited) {
    Stop-Process -Id $process.Id -Force
  }
}
