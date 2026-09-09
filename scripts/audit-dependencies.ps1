param(
  [Parameter(Mandatory = $true)]
  [string]$Executable,
  [string]$Dumpbin = ""
)

$ErrorActionPreference = "Stop"
$resolvedExecutable = (Resolve-Path -LiteralPath $Executable).Path

if (-not $Dumpbin) {
  $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
  if ($command) {
    $Dumpbin = $command.Source
  } else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
      $installation = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      if ($installation) {
        $Dumpbin = Get-ChildItem -Path (Join-Path $installation "VC\Tools\MSVC") `
          -Filter dumpbin.exe -Recurse | Where-Object FullName -Match `
          "bin\\Hostx64\\x64\\dumpbin.exe$" | Sort-Object FullName -Descending | `
          Select-Object -First 1 -ExpandProperty FullName
      }
    }
  }
}
if (-not $Dumpbin -or -not (Test-Path -LiteralPath $Dumpbin)) {
  throw "dumpbin.exe was not found. Install the Visual Studio C++ build tools to audit imports."
}

$raw = & $Dumpbin /nologo /dependents $resolvedExecutable 2>&1
if ($LASTEXITCODE -ne 0) { throw "dumpbin /dependents failed for $resolvedExecutable" }
$dependencies = @($raw | ForEach-Object {
  if ($_ -match "^\s+([A-Za-z0-9_.-]+\.dll)\s*$") { $Matches[1].ToLowerInvariant() }
} | Sort-Object -Unique)

$forbidden = @($dependencies | Where-Object {
  $_ -match "^(cudart|cublas|cufft|curand|cusolver|cusparse|nvrtc).*\.dll$" -or
  $_ -match "^(vcruntime|msvcp|concrt).*\.dll$" -or $_ -eq "nvml.dll"
})
$allowedSystemDependencies = @(
  "advapi32.dll",
  "bcrypt.dll",
  "gdi32.dll",
  "kernel32.dll",
  "ole32.dll",
  "shell32.dll",
  "user32.dll",
  "ws2_32.dll"
)
$unexpected = @($dependencies | Where-Object { $_ -notin $allowedSystemDependencies })

[pscustomobject]@{
  executable = $resolvedExecutable
  dumpbin = $Dumpbin
  dependencies = $dependencies
  forbiddenRedistributableDependencies = $forbidden
  unexpectedDependencies = $unexpected
  cudaRuntimeStrategy = "CUDA Runtime statically linked; NVIDIA Driver loaded from Windows"
} | Format-List

if ($forbidden.Count -ne 0 -or $unexpected.Count -ne 0) {
  $failures = @($forbidden + $unexpected | Sort-Object -Unique)
  throw "Portable dependency audit failed: $($failures -join ', ')"
}
