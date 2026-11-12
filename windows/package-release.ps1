param(
    [string]$Version = '0.2.1',
    [string]$BuildDirectory = (Join-Path $PSScriptRoot 'build/release-0.2.1'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'build/release-artifacts/v0.2.1'),
    [string]$VisualStudioDirectory = ''
)
$ErrorActionPreference = 'Stop'
$repository = Split-Path $PSScriptRoot -Parent
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$desktopVersion = (Get-Item -LiteralPath (Join-Path $build 'audiobridge.exe')).VersionInfo.ProductVersion
$androidMetadata = Get-Content -LiteralPath (Join-Path $repository 'flutter_app/build/app/outputs/apk/release/output-metadata.json') -Raw | ConvertFrom-Json
if ($desktopVersion -ne $Version -or $androidMetadata.elements[0].versionName -ne $Version) {
    throw 'Build versions do not match the requested release. Rebuild both platforms first.'
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $output) { throw 'Use a fresh output directory to avoid mixing release artifacts.' }
$stage = Join-Path $output 'windows'
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$runtimeFiles = @('audiobridge.exe', 'audiobridge-updater.exe', 'onnxruntime.dll',
    'voice-models/deepfilternet3_streaming.onnx', 'voice-models/silero_vad.onnx')
foreach ($relative in $runtimeFiles) {
    $destination = Join-Path $stage $relative
    New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $build $relative) -Destination $destination
}
$licenses = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'licenses/onnxruntime') -Destination $licenses -Recurse
Copy-Item -LiteralPath (Join-Path $build 'voice-models/licenses') -Destination (Join-Path $licenses 'voice-models') -Recurse
$dependencies = @{
    opus = 'opus-src/COPYING'
    mbedtls = 'mbedtls-src/LICENSE'
    webview = 'webview-src/LICENSE'
}
foreach ($name in $dependencies.Keys) {
    $destination = Join-Path $licenses $name
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot ('build/_deps/' + $dependencies[$name])) -Destination (Join-Path $destination 'LICENSE')
}
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio = $VisualStudioDirectory
if (!$visualStudio) { $visualStudio = & $vswhere -latest -products '*' -property installationPath }
if (!$visualStudio) { throw 'Specify -VisualStudioDirectory when Build Tools is not registered with vswhere.' }
$redistRoot = Join-Path $visualStudio 'VC/Redist/MSVC'
$redist = Get-ChildItem -LiteralPath $redistRoot -Directory | Where-Object Name -Match '^\d+\.\d+' | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
foreach ($name in @('msvcp140.dll', 'msvcp140_1.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    Copy-Item -LiteralPath (Join-Path $redist.FullName "x64/Microsoft.VC143.CRT/$name") -Destination (Join-Path $stage $name)
}
Copy-Item -LiteralPath (Join-Path $repository 'RELEASE_NOTES.md') -Destination (Join-Path $stage 'README.md')
Copy-Item -LiteralPath (Join-Path $repository 'UPDATING.md') -Destination $stage
foreach ($name in @('VOICE_EFFECTS.md', 'DEVICE_PERMISSIONS.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination $stage
}
$archive = Join-Path $output 'AudioBridge-Windows-x64.zip'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in Get-ChildItem -LiteralPath $stage -File -Recurse) {
        $relative = $file.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $file.FullName, $relative, [IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally { $zip.Dispose() }
$apk = Join-Path $output 'AudioBridge-Android.apk'
Copy-Item -LiteralPath (Join-Path $repository 'flutter_app/build/app/outputs/flutter-apk/app-release.apk') -Destination $apk
$checksums = foreach ($file in @($archive, $apk)) {
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($file))"
}
[IO.File]::WriteAllLines((Join-Path $output 'SHA256SUMS.txt'), $checksums, [Text.Encoding]::ASCII)
$allArchive = Join-Path $output "AudioBridge-$Version-All.zip"
$allZip = [IO.Compression.ZipFile]::Open($allArchive, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($file in @($archive, $apk, (Join-Path $output 'SHA256SUMS.txt'))) {
        [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($allZip, $file, [IO.Path]::GetFileName($file), [IO.Compression.CompressionLevel]::NoCompression) | Out-Null
    }
    [IO.Compression.ZipFileExtensions]::CreateEntryFromFile($allZip, (Join-Path $repository 'RELEASE_NOTES.md'), 'README.md', [IO.Compression.CompressionLevel]::Optimal) | Out-Null
} finally { $allZip.Dispose() }
$allHash = (Get-FileHash -LiteralPath $allArchive -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::AppendAllText((Join-Path $output 'SHA256SUMS.txt'), "$allHash  $([IO.Path]::GetFileName($allArchive))" + [Environment]::NewLine, [Text.Encoding]::ASCII)
Write-Output "AudioBridge $Version release artifacts: $output"
