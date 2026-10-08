# Decompiles the DataFixerUpper jar that the port is based on into
# reference/dfu-6.0.8/ for study.
#
# Those sources are Mojang's; they are NOT versioned (see .gitignore) and are not
# part of the build -- this script exists so the provenance of the port can be
# reproduced.
#
# Usage:
#   powershell -File scripts/decompile_reference.ps1
#   powershell -File scripts/decompile_reference.ps1 -Proxy http://127.0.0.1:7890
param(
  [string]$Jar = "$env:USERPROFILE\.gradle\caches\modules-2\files-2.1\com.mojang\datafixerupper\6.0.8\3ba4a30557a9b057760af4011f909ba619fc5125\datafixerupper-6.0.8.jar",
  [string]$Proxy = "",
  [string]$Output = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
if ($Output -eq "") { $Output = Join-Path $root "reference\dfu-6.0.8" }

if (-not (Test-Path $Jar)) {
  throw "DataFixerUpper jar not found: $Jar`nPass -Jar <path> (it lives in the Gradle module cache)."
}

$java = Get-Command java -ErrorAction SilentlyContinue
if ($null -eq $java) {
  throw "no 'java' on PATH; install a JRE or pass one via JAVA_HOME"
}

$proxyArgs = @()
if ($Proxy -ne "") { $proxyArgs = @("--proxy", $Proxy) }

$thirdParty = Join-Path $root "third_party"
New-Item -ItemType Directory -Force -Path $thirdParty | Out-Null
$decompiler = Join-Path $thirdParty "vineflower.jar"
if (-not (Test-Path $decompiler)) {
  Write-Host "downloading the Vineflower decompiler"
  & curl.exe -sSL --max-time 300 @proxyArgs -o $decompiler `
    "https://repo1.maven.org/maven2/org/vineflower/vineflower/1.11.1/vineflower-1.11.1.jar"
  if ((Get-Item $decompiler).Length -lt 100000) {
    throw "failed to download the decompiler (use -Proxy http://127.0.0.1:7890 if needed)"
  }
}

New-Item -ItemType Directory -Force -Path $Output | Out-Null
Write-Host "decompiling $Jar -> $Output"
& $java.Source -jar $decompiler -s --folder $Jar $Output
if ($LASTEXITCODE -ne 0) { throw "decompiler failed with exit code $LASTEXITCODE" }

$count = (Get-ChildItem -Recurse -File $Output | Measure-Object).Count
Write-Host "done: $count files (not versioned -- see .gitignore)"
