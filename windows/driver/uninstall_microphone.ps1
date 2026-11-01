#Requires -RunAsAdministrator
# Removes the AudioBridge Microphone service, device record, driver package and
# (with -RemoveTestCertificate) the machine trust for the local test certificate.
# VB-CABLE and the AudioBridge Virtual Speakers driver are not touched.
param([switch]$RemoveTestCertificate)
$ErrorActionPreference = "Stop"

$serviceName = "AudioBridgeMicService"
if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) {
    Stop-Service -Name $serviceName -Force -ErrorAction SilentlyContinue
    & sc.exe delete $serviceName | Out-Null
    Write-Host "Removed service $serviceName."
}
$destination = Join-Path $env:ProgramFiles "AudioBridge Microphone"
if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Recurse -Force }

# The service creates SWD\AudioBridge\Microphone on demand; drop its stored record.
Get-PnpDevice -InstanceId 'SWD\AUDIOBRIDGE\MICROPHONE' -ErrorAction SilentlyContinue | ForEach-Object {
    & pnputil.exe /remove-device $_.InstanceId | Out-Null
    Write-Host "Removed device $($_.InstanceId)."
}

# Find our staged oemNN.inf by its original file name.
$published = Get-WindowsDriver -Online -ErrorAction SilentlyContinue |
    Where-Object { $_.OriginalFileName -like '*\audiobridge_mic.inf' } | ForEach-Object { $_.Driver }
foreach ($oem in $published) {
    & pnputil.exe /delete-driver $oem /uninstall /force
    Write-Host "Deleted driver package $oem."
}

if ($RemoveTestCertificate) {
    foreach ($store in @("Root", "TrustedPublisher")) {
        Get-ChildItem "Cert:\LocalMachine\$store" |
            Where-Object { $_.Subject -eq "CN=AudioBridge Local Test Driver" } |
            ForEach-Object { Remove-Item -LiteralPath $_.PSPath; Write-Host "Removed test certificate from $store." }
    }
}
Write-Host "AudioBridge Microphone uninstalled."
