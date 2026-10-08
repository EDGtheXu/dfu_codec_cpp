# verify_no_json.ps1 —— 验收"没有 nlohmann/json.hpp 也能构建运行"
#
# 做法：把 third_party/nlohmann/json.hpp 临时改名，用 -DCODEC_BUILD_JSON=OFF
# 只构建核 + TOML 层，跑 ctest；无论成功失败都在 finally 里把文件改回来。
#
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File build\verify_no_json.ps1
param(
  [string]$VcVars = "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat",
  [string]$BuildDir = "build-nojson",
  [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$header = Join-Path $root "third_party\nlohmann\json.hpp"
$stash = Join-Path $root "third_party\nlohmann\json.hpp.hidden"

if (-not (Test-Path $header)) { throw "not found: $header" }
if (Test-Path $stash) { throw "stash already exists, refusing to start: $stash" }

try {
  Move-Item $header $stash -Force
  Write-Host "=== nlohmann/json.hpp 已临时移走，配置只含核 + TOML 的构建 ==="
  $commands = @(
    "cmake -S `"$root`" -B `"$root\$BuildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCODEC_BUILD_JSON=OFF -DCODEC_BUILD_TOML=ON",
    "cmake --build `"$root\$BuildDir`"",
    "ctest --test-dir `"$root\$BuildDir`" --output-on-failure"
  )
  foreach ($command in $commands) {
    Write-Host "==> $command" -ForegroundColor Cyan
    cmd /c "call `"$VcVars`" >nul && $command"
    if ($LASTEXITCODE -ne 0) { throw "command failed with exit code $LASTEXITCODE" }
  }
  Write-Host "`nNO-JSON BUILD: PASSED (core + TOML built and tested without nlohmann)" -ForegroundColor Green
} finally {
  if (Test-Path $stash) { Move-Item $stash $header -Force }
  Write-Host "nlohmann/json.hpp restored"
}
