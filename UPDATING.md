# Updates and release versions

## Using the update buttons

- **Android:** use **App updates** in the top bar, then **Check for updates** and
  **Update now**. The app downloads and verifies the APK, confirms audio shutdown,
  then opens Android's installer. Android may require **Allow from this source**
  and always retains its installation confirmation. Canceling keeps the installed
  version. No silent installation, uninstall, or pairing reset is performed.
- **Windows:** use **App updates** at the bottom of the window. **Update now**
  confirms the audio interruption, downloads the complete runtime, verifies it,
  closes AudioBridge normally, replaces runtime files, and restarts it. Keep
  `audiobridge-updater.exe` next to `audiobridge.exe`; install the entire ZIP,
  not just the main executable. Use a user-writable application folder.
- Older versions without update buttons need one manual installation of 0.2.0.
  Update the phone and PC separately. The updater never installs or changes a
  microphone driver, VB-CABLE, Windows defaults, or another device's permissions.

## Verification and recovery

Updates use the latest stable, non-draft release from the fixed public repository
`Zigdon-Code-Factory/audiobridge`. Only HTTPS and GitHub's download hosts are
accepted. Asset names, bounded size and GitHub's SHA-256 asset digest are checked.
The digest protects integrity relative to GitHub release metadata; it is not an
independent publisher signature or protection against a compromised GitHub account.

Android additionally checks package name, a strictly increasing Android build
number, matching release version and the installed APK's signing identity.
Downloaded APKs are held in private cache and shared read-only with the system
installer. An interrupted or rejected update does not replace the installed app.
Denial of install permission is recoverable through **Continue installation**.

Windows validates ZIP paths, file allowlists, duplicate entries, required runtime
files and unpacked-size limits. The helper verifies the download again, confirms
the original process identity and waits for it to exit. It backs up runtime files
and rolls back on copy failure. Pairings, settings, logs and drivers are outside
the replacement allowlist. A failed helper shows an error; its transaction folder
under `%TEMP%\AudioBridge-Update-*` contains `update-error.txt` and any backup.
If rollback is incomplete, restore that backup before reopening AudioBridge.
Downloaded transactions are retained for diagnosis; they can be deleted after
closing AudioBridge and confirming the update succeeded.

## Versioning and signing

The public release series is **0.2.0**, following the existing `v0.1.1` GitHub
release. Intermediate local builds used `1.0.x` display names. Android 0.2.0 has
`versionCode` **6**, higher than the previous build 5, so changing the display
version does not require uninstalling or clearing app data. Future releases must
increase both the semantic version and Android build number.

This sideload release retains the existing Android debug signing identity for
upgrade compatibility. It is not a production-key migration or Play Store
release. Do not replace that identity without an explicit signing/data-migration
plan. No signing keys belong in this repository. Windows artifacts are unsigned;
an initial manual download may trigger SmartScreen.

## Publishing

1. Update `common/app_version.h`, `windows/CMakeLists.txt`, the visible UI versions,
   and `flutter_app/pubspec.yaml`; increment the Android build number.
2. Build the Windows app and updater together and the Android release APK.
3. Run the host/UI/native regression suites, including `updater_test`.
4. Run `windows/package-release.ps1` with a fresh output directory. It bundles
   only explicit runtime files, Microsoft runtime DLLs, notices and documentation;
   it never includes pairings or settings.
5. Validate the ZIP with `updater_test.exe <zip-path>`, tag the tested commit, then
   create a GitHub release with `AudioBridge-Android.apk`,
   `AudioBridge-Windows-x64.zip` and `SHA256SUMS.txt`. GitHub must expose each
   installable asset's `sha256:` digest before the release is published as latest.

Tests cover fixtures and isolated file transactions. They do not establish
TalkBack/Switch Access conformance, hardware audio quality, all Windows antivirus
behaviors, or every OEM's Android installer behavior.
