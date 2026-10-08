# Downloads the third-party dependencies of the Codec port:
#   * nlohmann/json single header  -> third_party/nlohmann/json.hpp
#   * GoogleTest source release    -> third_party/googletest-1.17.0
#
# Usage:
#   powershell -File scripts/fetch_deps.ps1
#   powershell -File scripts/fetch_deps.ps1 -Proxy http://127.0.0.1:7890
param(
  [string]$Proxy = "",
  [switch]$Force
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$thirdParty = Join-Path $root "third_party"
New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null

$proxyArgs = @()
if ($Proxy -ne "") {
  $proxyArgs = @("--proxy", $Proxy)
  Write-Host "Using proxy $Proxy"
}

function Get-RemoteFile {
  param([string]$Url, [string]$Destination, [int]$MinimumBytes = 1024)
  if ((Test-Path $Destination) -and -not $Force) {
    Write-Host "already present: $Destination"
    return
  }
  Write-Host "downloading $Url"
  & curl.exe -sSL --max-time 300 @proxyArgs -o $Destination $Url
  if (-not (Test-Path $Destination) -or (Get-Item $Destination).Length -lt $MinimumBytes) {
    throw "failed to download $Url (use -Proxy http://127.0.0.1:7890 if the network needs a proxy)"
  }
  Write-Host ("  -> {0} ({1} bytes)" -f $Destination, (Get-Item $Destination).Length)
}

# --- nlohmann/json ---------------------------------------------------------
$nlohmannDir = Join-Path $thirdParty "nlohmann"
New-Item -ItemType Directory -Force -Path $nlohmannDir | Out-Null
Get-RemoteFile `
  -Url "https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp" `
  -Destination (Join-Path $nlohmannDir "json.hpp") `
  -MinimumBytes 500000

# --- GoogleTest ------------------------------------------------------------
$gtestDir = Join-Path $thirdParty "googletest-1.17.0"
if (-not (Test-Path (Join-Path $gtestDir "CMakeLists.txt"))) {
  $zip = Join-Path $thirdParty "googletest-1.17.0.zip"
  Get-RemoteFile `
    -Url "https://codeload.github.com/google/googletest/zip/refs/tags/v1.17.0" `
    -Destination $zip `
    -MinimumBytes 500000
  Expand-Archive -Path $zip -DestinationPath $thirdParty -Force
  Remove-Item $zip -Force
  Write-Host "  -> $gtestDir"
} else {
  Write-Host "already present: $gtestDir"
}

Write-Host "dependencies ready"
