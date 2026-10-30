param([string]$WdkRoot = "", [string]$Version = "10.0.26100.0")
$ErrorActionPreference = "Stop"
$sdk = "${env:ProgramFiles(x86)}\Windows Kits\10"
if (-not $WdkRoot) {
    $localKit = Join-Path $PSScriptRoot "../build/wdk/package/c"
    $WdkRoot = if (Test-Path "$sdk/Include/$Version/km/portcls.h") { $sdk } else { $localKit }
}
$WdkRoot = (Resolve-Path -LiteralPath $WdkRoot).Path
if (-not (Test-Path "$WdkRoot/Include/$Version/km/portcls.h")) { throw "WDK $Version not found. See MICROPHONE.md." }
$vcvars = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "MSVC Build Tools 2022 not found." }
$out = Join-Path $PSScriptRoot "build/microphone"
New-Item -ItemType Directory -Force $out | Out-Null
$src = Join-Path $PSScriptRoot "microphone.cpp"
# This script only compiles and validates. It never changes boot configuration,
# installs certificates, signs binaries, stages drivers, or creates devices.
$command = @"
call "$vcvars" >nul && cl.exe /nologo /c /std:c++17 /kernel /Zp8 /Gy /GF /GR- /EHs-c- /Zl /GS /O2 /W4 /WX /D_WIN64 /D_AMD64_ /DAMD64 /DNTDDI_VERSION=0x0A00000C /D_WIN32_WINNT=0x0A00 /I"$WdkRoot\Include\$Version\km" /I"$sdk\Include\$Version\shared" /I"$sdk\Include\$Version\um" /Fo"$out\microphone.obj" "$src" && link.exe /nologo /DRIVER /SUBSYSTEM:NATIVE,10.00 /ENTRY:GsDriverEntry /NODEFAULTLIB /RELEASE /OPT:REF /OPT:ICF /INTEGRITYCHECK /NXCOMPAT /DYNAMICBASE /LIBPATH:"$WdkRoot\Lib\$Version\km\x64" ntoskrnl.lib hal.lib portcls.lib stdunk.lib ksguid.lib wdmsec.lib BufferOverflowFastFailK.lib /OUT:"$out\audiobridge_mic.sys" "$out\microphone.obj"
"@
& cmd.exe /d /c $command
if ($LASTEXITCODE -ne 0) { throw "Microphone driver build failed ($LASTEXITCODE)." }
Copy-Item -LiteralPath "$PSScriptRoot/audiobridge_mic.inf" -Destination $out -Force
$infverif = Get-ChildItem "$WdkRoot" -Recurse -Filter InfVerif.exe | Where-Object { $_.FullName -match 'x64' } | Select-Object -First 1
if ($infverif) {
    & $infverif.FullName /w "$out/audiobridge_mic.inf"
    if ($LASTEXITCODE -ne 0) { throw "INF validation failed ($LASTEXITCODE)." }
}
$inf2cat = Get-ChildItem "$WdkRoot" -Recurse -Filter Inf2Cat.exe | Select-Object -First 1
if (-not $inf2cat) { throw "WDK Inf2Cat tool is missing." }
& $inf2cat.FullName "/driver:$out" /os:10_CO_X64 /uselocaltime
if ($LASTEXITCODE -ne 0) { throw "Driver catalog generation failed ($LASTEXITCODE)." }
Write-Host "Built unsigned microphone driver: $out"
