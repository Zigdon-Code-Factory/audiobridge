# Installs or updates the AudioBridge Virtual Speakers root-enumerated WaveRT device.
# Run from an elevated (Administrator) PowerShell prompt after building & signing .\build\audiobridge_vad.sys.

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$InfPath   = Join-Path $ScriptDir "build\audiobridge_vad.inf"

if (-not (Test-Path $InfPath)) {
    throw "Driver package not found at '$InfPath'. Run .\build_driver.ps1 first."
}

$KitsRoot = "C:\Program Files (x86)\Windows Kits\10"
$DevGen = Get-ChildItem "$KitsRoot\Tools" -Recurse -Filter "devgen.exe" -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match "x64" } | Select-Object -First 1

if ($DevGen) {
    Write-Host "[INSTALL] Creating root-enumerated software device node Root\AudioBridgeVAD..."
    & $DevGen.FullName /add /bus ROOT /hardwareid "Root\AudioBridgeVAD"
}

Write-Host "[INSTALL] Staging and installing $InfPath via pnputil..."
pnputil.exe /add-driver "$InfPath" /install
Write-Host "[OK] AudioBridge Virtual Speakers installed. AudioBridge Server will automatically attach to \\.\AudioBridgeTap."
