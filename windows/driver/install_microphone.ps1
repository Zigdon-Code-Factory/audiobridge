#Requires -RunAsAdministrator
param(
    [string]$PackagePath = "$PSScriptRoot/build/microphone",
    [string]$ServicePath = "$PSScriptRoot/../build/secure-release/audiobridge_mic_service.exe"
)
$ErrorActionPreference = "Stop"
$PackagePath = (Resolve-Path -LiteralPath $PackagePath).Path
$ServicePath = (Resolve-Path -LiteralPath $ServicePath).Path
$inf = Join-Path $PackagePath "audiobridge_mic.inf"
$sys = Join-Path $PackagePath "audiobridge_mic.sys"
$cat = Join-Path $PackagePath "audiobridge_mic.cat"
foreach ($file in @($inf, $sys, $cat)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing signed driver package file: $file" }
}
$signTool = Get-ChildItem "${env:ProgramFiles(x86)}/Windows Kits/10/bin" -Recurse -Filter signtool.exe |
    Where-Object { $_.FullName -match 'x64' } | Sort-Object FullName -Descending | Select-Object -First 1
if (-not $signTool) { throw "Windows SDK signtool is required to verify the kernel driver before installation." }
foreach ($file in @($sys, $inf)) {
    & $signTool.FullName verify /kp /c $cat $file
    if ($LASTEXITCODE -ne 0) { throw "Kernel signing verification failed for $file. This installer does not enable test signing." }
}
& pnputil.exe /add-driver $inf /install
if ($LASTEXITCODE -notin @(0,3010)) { throw "Driver staging failed ($LASTEXITCODE)." }
$serviceName = "AudioBridgeMicService"
$existing = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($existing) {
    Stop-Service -Name $serviceName -Force
    $existing.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(20))
}
# Store the SYSTEM service in an administrator-writable location, never launch
# it directly from the user's checkout or download directory.
$destination = Join-Path $env:ProgramFiles "AudioBridge Microphone"
if ((Test-Path -LiteralPath $destination) -and
    ((Get-Item -LiteralPath $destination).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw "Service destination must not be a reparse point."
}
New-Item -ItemType Directory -Force -Path $destination | Out-Null
& icacls.exe $destination /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)F' '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-32-545:(OI)(CI)RX'
if ($LASTEXITCODE -ne 0) { throw "Cannot secure the service directory." }
$installedExe = Join-Path $destination "audiobridge_mic_service.exe"
if ((Test-Path -LiteralPath $installedExe) -and
    ((Get-Item -LiteralPath $installedExe).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw "Service executable must not be a reparse point."
}
Copy-Item -LiteralPath $ServicePath -Destination $installedExe -Force
$binaryPath = '"' + $installedExe + '"'
if ($existing) {
    & sc.exe config $serviceName binPath= $binaryPath start= auto obj= LocalSystem
    if ($LASTEXITCODE -ne 0) { throw "Service update failed." }
} else {
    New-Service -Name $serviceName -BinaryPathName $binaryPath -DisplayName "AudioBridge Microphone" -StartupType Automatic | Out-Null
}
& sc.exe description $serviceName "Hosts the AudioBridge microphone only while the AudioBridge app is connected."
Start-Service -Name $serviceName
Write-Host "Installed. Open AudioBridge; select AudioBridge Microphone as the input in your voice apps."
Write-Host "Closing AudioBridge removes the active endpoint. VB-CABLE is not changed by this installer."
