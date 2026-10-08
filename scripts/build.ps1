param(
  [string]$Config = "Release",
  [switch]$RunTests,
  [switch]$Fresh
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vcvars = "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
  throw "vcvars64.bat not found at $vcvars"
}

$buildDir = Join-Path $root "build"
if ($Fresh -and (Test-Path $buildDir)) {
  Remove-Item -Recurse -Force $buildDir
}

$commands = @(
  "cmake -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Config",
  "cmake --build `"$buildDir`""
)
if ($RunTests) {
  $commands += "ctest --test-dir `"$buildDir`" --output-on-failure"
}

foreach ($command in $commands) {
  Write-Host "==> $command" -ForegroundColor Cyan
  cmd /c "call `"$vcvars`" >nul && $command"
  if ($LASTEXITCODE -ne 0) {
    throw "command failed with exit code $LASTEXITCODE"
  }
}
