param(
    [switch]$TestSign
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$OutDir = Join-Path $ScriptDir "build"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# Locate latest installed Windows Driver Kit (WDK)
$KitsRoot = "C:\Program Files (x86)\Windows Kits\10"
$WdkVersions = Get-ChildItem "$KitsRoot\Include" -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "km\portcls.h") } |
    Sort-Object Name -Descending

if (-not $WdkVersions -or $WdkVersions.Count -eq 0) {
    Write-Error @"
Windows Driver Kit (WDK) kernel headers (km\portcls.h) were not found in '$KitsRoot\Include'.
Install the matching WDK for Windows 11 SDK 10.0.26100.0:
  winget install Microsoft.WindowsWDK.10.0.26100
Then re-run .\build_driver.ps1
"@
    exit 1
}

$Ver = $WdkVersions[0].Name
Write-Host "[WDK] Using Windows Driver Kit version: $Ver"

$VcVars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $VcVars)) {
    $VcVars = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
}

$KmInc     = "$KitsRoot\Include\$Ver\km"
$SharedInc = "$KitsRoot\Include\$Ver\shared"
$KmLib     = "$KitsRoot\Lib\$Ver\km\x64"
$SrcFile   = Join-Path $ScriptDir "audiobridge_vad.cpp"
$ObjFile   = Join-Path $OutDir "audiobridge_vad.obj"
$SysFile   = Join-Path $OutDir "audiobridge_vad.sys"
$InfOut    = Join-Path $OutDir "audiobridge_vad.inf"

Copy-Item (Join-Path $ScriptDir "audiobridge_vad.inf") $InfOut -Force

$Cmd = @"
call "$VcVars" >nul && ^
cl.exe /nologo /c /Zp8 /Gy /GF /GR- /EHs-c- /Zl /GS /O2 /W4 /WX- ^
  /D_WIN64 /D_AMD64_ /DAMD64 /D_KERNEL_MODE=1 /DNTDDI_VERSION=0x0A00000C ^
  /I"$KmInc" /I"$SharedInc" /I"$ScriptDir" ^
  /Fo"$ObjFile" "$SrcFile" && ^
link.exe /nologo /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:DriverEntry /NODEFAULTLIB ^
  /RELEASE /OPT:REF /OPT:ICF /INTEGRITYCHECK ^
  /LIBPATH:"$KmLib" ^
  ntoskrnl.lib hal.lib portcls.lib stdunk.lib ksguid.lib BufferOverflowFastFailK.lib ^
  /OUT:"$SysFile" "$ObjFile"
"@

cmd.exe /c $Cmd
if ($LASTEXITCODE -ne 0) {
    throw "Kernel driver build failed with exit code $LASTEXITCODE"
}

Write-Host "[OK] Built $SysFile"

if ($TestSign) {
    $CertName = "AudioBridge Local Test Driver"
    $Cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=$CertName" } | Select-Object -First 1
    if (-not $Cert) {
        $Cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=$CertName" -CertStoreLocation "Cert:\CurrentUser\My"
    }
    $Inf2Cat = Get-ChildItem "$KitsRoot\bin" -Recurse -Filter "Inf2Cat.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($Inf2Cat) {
        & $Inf2Cat.FullName /driver:"$OutDir" /os:10_X64
    }
    $SignTool = Get-ChildItem "$KitsRoot\bin" -Recurse -Filter "signtool.exe" -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match "x64" } | Select-Object -First 1
    if ($SignTool) {
        & $SignTool.FullName sign /v /s My /n $CertName /fd SHA256 "$SysFile"
        $CatFile = Join-Path $OutDir "audiobridge_vad.cat"
        if (Test-Path $CatFile) {
            & $SignTool.FullName sign /v /s My /n $CertName /fd SHA256 "$CatFile"
        }
    }
    Write-Host "[OK] Test-signed driver package in $OutDir"
}
