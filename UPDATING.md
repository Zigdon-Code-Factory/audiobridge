# Updates and release versions

## Using the update buttons

- **Android:** use **App updates** in the top bar, then **Check for updates** and
  **Update now**. The app downloads and verifies the APK, confirms audio shutdown,
  then streams it into an owned Android PackageInstaller session. On Android 12+
  it requests eligible self-updates without another approval prompt. Android may
  require **Allow from this source** or a system installation confirmation.
  Canceling keeps the installed
  version. No silent installation, uninstall, or pairing reset is performed.
- **Windows:** use **App updates** at the bottom of the window. **Update now**
  confirms the audio interruption, downloads the complete runtime, verifies it,
  closes AudioBridge normally, replaces runtime files, and restarts it. Keep
  `audiobridge-updater.exe` next to `audiobridge.exe`; install the entire ZIP,
  not just the main executable. Use a user-writable application folder.
- Older versions without update buttons need one manual installation of a current release.
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
Downloaded APKs are held in private cache and streamed into the owned system
installation session. Callback intents target a non-exported receiver and must
match both the active session ID and a private per-session identity. Session
receipts survive process/activity replacement; completion is announced only after
the installed build is verified. System confirmation intents must resolve to a
system application before they can be opened, and are opened only while
AudioBridge is in the foreground. An interrupted or rejected update does not
replace the installed app.
Denial of install permission is recoverable through **Continue installation**.
Active installations can be canceled from the update dialog. Android remains
authoritative about whether additional approval is required; no root, Shizuku or
system permission bypass is used.

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

The public release is **0.2.1**, following the 0.2.0 overhaul. Intermediate local
builds used `1.0.x` display names. Android 0.2.1 has
`versionCode` **7**, higher than the previous build 6, so changing the display
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
6. For the live Android upgrade test, an internal bootstrap can be built from the
   new source using the previous version/build number. Keep that APK out of the
   public release. Upgrade from it to the final published APK through the in-app
   updater, not ADB, to exercise the new session installer.

Tests cover fixtures and isolated file transactions. They do not establish
TalkBack/Switch Access conformance, hardware audio quality, all Windows antivirus
behaviors, or every OEM's Android installer behavior.
