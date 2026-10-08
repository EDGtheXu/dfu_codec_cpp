# Downloads the third-party dependencies of the Codec port:
#   * nlohmann/json single header  -> third_party/nlohmann/json.hpp
#   * tinytoml single header       -> third_party/tinytoml/toml/toml.h  (TOML support)
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

# --- tinytoml (TOML support, see include/codec_toml.hpp) --------------------
# Header-only, simplified BSD; upstream has no releases, so we pin the header by
# SHA-256 and refuse to continue if master changes: re-pin deliberately.
$tinytomlDir = Join-Path $thirdParty "tinytoml"
$tinytomlTomlDir = Join-Path $tinytomlDir "toml"
New-Item -ItemType Directory -Force -Path $tinytomlTomlDir | Out-Null
$tinytomlHeader = Join-Path $tinytomlTomlDir "toml.h"
Get-RemoteFile `
  -Url "https://raw.githubusercontent.com/mayah/tinytoml/master/include/toml/toml.h" `
  -Destination $tinytomlHeader `
  -MinimumBytes 40000
Get-RemoteFile `
  -Url "https://raw.githubusercontent.com/mayah/tinytoml/master/LICENSE" `
  -Destination (Join-Path $tinytomlDir "LICENSE") `
  -MinimumBytes 512
$tinytomlExpected = "C08C1E1F5ABB82C53955350DF6F5E97A34B21C6C31F0A00B86445069C0C7B238"
$tinytomlActual = (Get-FileHash $tinytomlHeader -Algorithm SHA256).Hash
if ($tinytomlActual -ne $tinytomlExpected) {
  throw ("tinytoml master changed: expected SHA-256 $tinytomlExpected, got $tinytomlActual. " +
         "Review the new upstream header, re-run the tests, then update the pin in this script.")
}
Write-Host "  -> $tinytomlHeader (SHA-256 verified)"

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
