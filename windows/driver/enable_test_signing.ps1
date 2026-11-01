#Requires -RunAsAdministrator
# Turns Windows test signing on (or off with -Disable) so the self-signed
# AudioBridge Microphone driver can load. Requires Secure Boot to be OFF in the
# firmware (BIOS/UEFI) setup first; Windows refuses this change otherwise.
#
# Side effects while enabled: a "Test Mode" desktop watermark, and some kernel
# anti-cheat software (Vanguard, FACEIT, Ricochet, ...) refuses to run.
param([switch]$Disable)
$ErrorActionPreference = "Stop"

function Test-SecureBoot {
    try { return [bool](Confirm-SecureBootUEFI) } catch { return $false } # Legacy BIOS: not applicable.
}

function Suspend-BitLockerOnce {
    # Changing boot configuration makes BitLocker ask for the recovery key on the
    # next boot unless protection is suspended for that boot.
    $volume = Get-BitLockerVolume -MountPoint $env:SystemDrive -ErrorAction SilentlyContinue
    if ($volume -and $volume.ProtectionStatus -eq 'On') {
        Suspend-BitLocker -MountPoint $env:SystemDrive -RebootCount 1 | Out-Null
        Write-Host "BitLocker suspended for the next reboot only."
    }
}

if ($Disable) {
    Suspend-BitLockerOnce
    & bcdedit.exe /set "{current}" testsigning off
    if ($LASTEXITCODE -ne 0) { throw "bcdedit failed ($LASTEXITCODE)." }
    Write-Host "Test signing disabled. Reboot; you may then re-enable Secure Boot in firmware setup."
    Write-Host "Run uninstall_microphone.ps1 first - the test-signed driver will not load afterwards."
    return
}

if (Test-SecureBoot) {
    Write-Host @"
Secure Boot is ON, so Windows will not allow test signing.

  1. (Recommended) Back up your BitLocker recovery key: manage-bde -protectors -get $env:SystemDrive
  2. Run: Suspend-BitLocker -MountPoint $env:SystemDrive -RebootCount 1
  3. Reboot into firmware setup (Settings > System > Recovery > Advanced startup >
     Troubleshoot > Advanced options > UEFI Firmware Settings) and disable Secure Boot.
  4. Boot Windows and run this script again.
"@
    exit 1
}

$current = (& bcdedit.exe /enum "{current}") -join "`n"
if ($current -match '(?im)^testsigning\s+Yes') {
    Write-Host "Test signing is already enabled. Next: .\install_microphone.ps1 -TestSigned"
    return
}
Suspend-BitLockerOnce
& bcdedit.exe /set "{current}" testsigning on
if ($LASTEXITCODE -ne 0) { throw "bcdedit failed ($LASTEXITCODE). Is Secure Boot really disabled?" }
Write-Host "Test signing enabled. Reboot, then run: .\install_microphone.ps1 -TestSigned"
