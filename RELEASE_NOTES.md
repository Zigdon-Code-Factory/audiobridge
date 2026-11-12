# AudioBridge 0.2.1

Session-based Android self-updates, built on the 0.2.0 overhaul.

## New in 0.2.1

- Android updates use PackageInstaller sessions and request installation without
  another prompt on supported, eligible self-updates. Android can still require
  source permission or its system confirmation; these checks are not bypassed.
- Durable installation status, authenticated session callbacks, explicit
  cancellation, and recovery after activity/process replacement.
- Verified cached downloads can be reused, and an update-complete notification
  can reopen AudioBridge when notifications are permitted.
- Windows and Android both report version 0.2.1. Android build 7 upgrades build 6
  without uninstalling or clearing app data.

## Highlights

- Redesigned Windows routing, microphone cleanup and recording workflows, compact
  task tabs, clearer readiness/recovery states and persistent recording controls.
- Per-device **Read only / Admin** access. Only the desktop owner grants Admin;
  authorized phones can change PC audio settings and control PC media.
- Mobile accessibility improvements: labeled/toggled controls, keyboard recovery,
  reduced-motion support, large-text layouts and persistent local feedback.
- Safer asynchronous microphone ownership, timeout, reconnect, playback and
  shutdown handling, with explicit uncertainty instead of unconfirmed success.
- **App updates** on both platforms: verified downloads, retained settings and
  pairings, Windows restart/install and Android's normal system confirmation.

## Installation and compatibility

- **Windows x64:** extract the complete `AudioBridge-Windows-x64.zip` into a
  user-writable folder and run `audiobridge.exe`. Keep its updater, runtime DLLs
  and `voice-models` folder together. WebView2 is required.
- **Android:** install `AudioBridge-Android.apk` over the existing app; no uninstall
  is needed. The display version is 0.2.1; Android build 7 upgrades build 6.
  This sideload build retains the existing signing identity.
- For older builds, install this release once manually to get the update buttons.
  Future phone and Windows updates remain separate.
- A calling-app microphone still needs a working VB-CABLE or AudioBridge virtual
  microphone setup. This release does not install drivers or alter Windows defaults.

## Validation limits

Automated coverage includes permissions, transport, audio processing, desktop
browser fixtures, Flutter accessibility/workflows and updater integrity, path
safety, rollback and data preservation. Fixture passes are not accessibility or
audio-quality certification. TalkBack/Switch Access and all OEM installer flows
remain outside the automated coverage.
