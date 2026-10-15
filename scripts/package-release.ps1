# Builds a Release exe and zips it. Run from the repo root:  powershell -File scripts\package-release.ps1
# Packages ONLY build\bin\Release\island_loader.exe, so a Debug build (which needs Visual Studio's debug runtime DLLs)
# can never end up in a release zip by accident.
param([string]$Version = "")

$ErrorActionPreference = "Stop"

# Default: the version in CMakeLists.txt project(... VERSION x.y.z), so there is one place to bump.
if (-not $Version) {
  $m = Select-String -Path CMakeLists.txt -Pattern "project\(island-capsule VERSION ([0-9.]+)" | Select-Object -First 1
  if (-not $m) { throw "could not read the version from CMakeLists.txt; pass -Version" }
  $Version = $m.Matches[0].Groups[1].Value
}

cmake -S . -B build -DISLAND_CONSOLE=OFF
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build build --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$exe = "build\bin\Release\island_loader.exe"
if (-not (Test-Path $exe)) { throw "$exe not found" }

# A debug runtime import means the wrong configuration was packaged.
$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $exe))
$text = [System.Text.Encoding]::ASCII.GetString($bytes)
if ($text -match "(?i)(vcruntime140d|msvcp140d|ucrtbased)\.dll") { throw "$exe links the DEBUG runtime" }

$zip = "IslandCapsule-v$Version-windows-x64.zip"
Compress-Archive -Path $exe -DestinationPath $zip -Force
Write-Host "Created $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"
