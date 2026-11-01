#Requires -RunAsAdministrator
param(
    [string]$PackagePath = "$PSScriptRoot/build/microphone",
    [string]$ServicePath = "",
    # Accept a package signed by build_microphone.ps1 -TestSign. Requires test
    # signing to be enabled (enable_test_signing.ps1) and trusts only the
    # certificate exported next to the package, on this machine.
    [switch]$TestSigned
)
$ErrorActionPreference = "Stop"
$PackagePath = (Resolve-Path -LiteralPath $PackagePath).Path
if (-not $ServicePath) {
    $ServicePath = @("$PSScriptRoot/../build/secure-release/audiobridge_mic_service.exe",
                     "$PSScriptRoot/../build/Release/audiobridge_mic_service.exe") |
        Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if (-not $ServicePath) { throw "audiobridge_mic_service.exe not found. Build it or pass -ServicePath." }
}
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
if ($TestSigned) {
    $bcd = (& bcdedit.exe /enum "{current}") -join "`n"
    if ($bcd -notmatch '(?im)^testsigning\s+Yes') {
        throw "Test signing is not enabled. Run .\enable_test_signing.ps1 and reboot first."
    }
    $cer = Join-Path $PackagePath "audiobridge_test.cer"
    if (-not (Test-Path -LiteralPath $cer -PathType Leaf)) { throw "Missing $cer. Rebuild with build_microphone.ps1 -TestSign." }
    $certificate = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 $cer
    if ($certificate.Subject -ne "CN=AudioBridge Local Test Driver") { throw "Unexpected test certificate: $($certificate.Subject)" }
    # Root: chain trust for the self-signed cert. TrustedPublisher: no PnP prompt.
    foreach ($store in @("Root", "TrustedPublisher")) {
        Import-Certificate -FilePath $cer -CertStoreLocation "Cert:\LocalMachine\$store" | Out-Null
    }
    Write-Host "Trusted test certificate $($certificate.Thumbprint) on this machine."
}
foreach ($file in @($sys, $inf)) {
    # /kp = kernel-mode (Microsoft) policy; /pa = Authenticode chain to a trusted root.
    $policy = if ($TestSigned) { "/pa" } else { "/kp" }
    & $signTool.FullName verify $policy /c $cat $file
    if ($LASTEXITCODE -ne 0) {
        throw "Signature verification failed for $file." + $(if ($TestSigned) { "" } else { " For a self-signed build use -TestSigned." })
    }
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
