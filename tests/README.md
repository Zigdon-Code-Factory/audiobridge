# Regression tests

Windows phone microphone effects have a real-model test in the server build:
`cmake --build windows/build --config Release --target voice_effects_test`, then
`ctest --test-dir windows/build -C Release -R voice_ --output-on-failure`.
With Node.js available, this includes the embedded UI's bridge and control-isolation test.
See [voice effects](../windows/VOICE_EFFECTS.md) for routing and runtime dependencies.

### Desktop layout and interaction checks

The optional Edge/Playwright harness renders the production embedded HTML with
fixture native-bridge responses. It does not connect phones, record real audio,
or modify pairings. Install its development-only dependencies outside the source tree:

```powershell
npm install --prefix windows/build/ui-review --no-audit --no-fund playwright @axe-core/playwright
$env:PLAYWRIGHT_MODULE = "$PWD/windows/build/ui-review/node_modules/playwright"
$env:AXE_MODULE = "$PWD/windows/build/ui-review/node_modules/@axe-core/playwright"
node tests/ui_layout_test.cjs
```

It checks compact keyboard tabs, initial-viewport access to routing and recording,
rejected-control rollback, local operation feedback, microphone prerequisites,
persistent recording access, unchanged pairing announcements, removal focus,
native capture confirmation, sample availability/reload, visible pairing alerts,
setup recovery, delayed keyboard focus, per-device permission selection/pending/failure
rollback and focus retention, authoritative permission changes and late-save outcomes,
confirmed voice/recording actions, pairing completion focus and outage disablement,
recording completion focus, verified pairing removal, startup-model explanations,
compact endpoint guidance, device-management access and grouped recovery links,
and WCAG AA rules across 45 scenarios. Full-page and
viewport screenshots plus the scan report go to `windows/build/ui-review/screenshots/`;
set `UI_REVIEW_OUTPUT` to use another folder. These checks do not replace native
WebView, screen-reader, Windows scaling, or hardware audio validation.

Run native PCM boundary tests with a C++17 compiler and CMake:

```sh
cmake -S tests -B windows/build/playback-tests
cmake --build windows/build/playback-tests --config Release
ctest --test-dir windows/build/playback-tests -C Release --output-on-failure
```

Assertions remain enabled in Release. These tests use the production playback cursor, checking exact samples and output bounds for 5/10/20 ms and mixed packet durations with differently sized output callbacks. They require no Android SDK or audio device and do not exercise Opus or Oboe.

When the apps' mbedTLS checkout exists at `windows/build/_deps/mbedtls-src`, the same CMake project also builds `secure_transport_test` and, on Windows, `server_security_test`. Use `-DMBEDTLS_SOURCE_DIR=/path/to/mbedtls` for another checkout. Local UDP loopback access is required.

The security tests exercise X25519/HKDF agreement, the comparison code and identity commitment, invalid keys, legacy key rejection, DTLS cookies and retransmission, all application packet types, tampering, duplicate/reordered records, previous-session replay, wrong PSKs/identities, and plaintext rejection. The Windows integration test runs the production server on temporary ports and verifies approval/denial, the encrypted READY barrier, mic and metadata delivery, authenticated reconnect, and resistance to plaintext disconnect/control injection. It stores test peer records beside the test executable and revokes its generated peer when finished.

`peer_control_test` runs the production DTLS server on ephemeral loopback ports,
without opening audio devices. Its separately isolated `peer-control/Release`
directory contains its temporary pairing store. It tests safe four-field peer
migration, invalid-role fallback, per-key role persistence and restore, independent
connected identities with the same advertised name, reader denial (including old
media commands), admin success, live downgrade and downgrade/regrant queue
invalidation, self-escalation/unknown-command rejection, payload/value validation,
per-session rate limiting, negative application acknowledgements, and continued
reader pause/resume/audio delivery. Build it with the security tests and run
`ctest --test-dir windows/build/security-tests-vs -C Release -R peer_control --output-on-failure`.
See [device permissions](../windows/DEVICE_PERMISSIONS.md) for migration and wire semantics.

On Windows, `multistream_test` runs eight independent DTLS clients against the
production server. It verifies exact PCM samples at all four frame sizes, Opus
and metadata fan-out, per-client sequence numbers, pause isolation, pending
pairing while audio continues, slot limits, wrong-key isolation, microphone
ownership/handoff, individual disconnect/revocation and timeout isolation.

For a manual soak alongside a physical phone, build `lan_stream_probe` and run:

```powershell
./windows/build/security-tests-vs/Release/lan_stream_probe.exe SERVER_IP 4012 180
```

Compare its printed six-digit code with the desktop approval prompt. Each run
uses a fresh temporary identity. It counts encrypted audio delivery, nonzero PCM
and sequence gaps without saving audio; it is not an audible playback test.
Revoke its `LAN test receiver` pairing in the desktop UI after testing. Multiple
probe processes exercise concurrent receivers; physical phones are still needed
to evaluate their playback and Wi-Fi behavior.

For hardware validation, update both apps, compare the six-digit codes, and approve only a match. Exercise PC audio, phone mic, metadata and media controls for at least three minutes with DTLS shown on both devices. Check native logs for disconnects, authentication errors, packet loss, decode errors and underruns. Test a reconnect without another pairing prompt. Do not infer end-to-end latency or audible quality solely from successful packet tests.

On Windows, `microphone_test` exercises the bounded microphone FIFO, prefill and
stale-tail expiry, real named-pipe audio transfer, malformed messages, driver
failure, cancellation, and abrupt client-process termination. It uses the
production service session handler with a fake driver backend. Run
`microphone_test.exe --soak 180` for a three-minute, 9,000-packet IPC soak.
This does not validate the kernel driver or an actual Windows recording endpoint;
see `windows/driver/MICROPHONE.md` for signing and hardware validation.

Run the Flutter smoke tests:

```sh
cd flutter_app
flutter test --no-pub
```

The mobile accessibility suite checks labeled native buttons, semantic roles and
states, volume units and increase/decrease actions, 48dp Android touch targets,
text contrast, keyboard activation, reduced-motion discovery/pairing and 200%
text reflow at a 320px viewport. Pairing codes are announced digit by digit and
connection/permission outcomes use stable live regions. The PC settings tests
cover delayed success, rejection and timeout focus recovery, deliberate focus
changes, meaningful current endpoint names and noise-strength dB units. Connected
workflow regressions also exercise reduced-motion expansion, microphone stop
failure/unknown-state recovery, retained keyboard position, non-live spoken media
progress, long-duration timestamp reflow and unmuting from zero volume. They
also cover source-selection rollback, manual/native disconnect during pending
microphone work, ownership revocation during startup (including stop failures),
and device-name editing with 200% text and a constrained keyboard viewport.
Failure-boundary tests cover late microphone starts, initial ownership cleanup,
persistent startup failures, bounded disconnect with verified shutdown/recovery,
confirmed local mute/volume state, inline media errors, blank-name validation,
and rejection of older permission snapshots within a connection.

Run targeted static analysis alongside the widget suite:

```sh
flutter analyze --no-pub lib/phone_controls.dart lib/desktop_controls.dart test/accessibility_test.dart test/desktop_controls_test.dart test/mobile_workflow_accessibility_test.dart
```

These fixture checks are not a screen-reader certification. Verify TalkBack
reading/focus order, spoken feedback, Android display/font scaling, Bluetooth
routes and background notifications on a physical phone before release.

The desktop-control widget/controller suite also checks old-server/read-only
disablement, admin controls, application acknowledgement plus snapshot confirmation
(transport send completion is insufficient), failure rollback, pending-write
exclusion, connected downgrade, stale permission expiry and reconnect reset.

For hardware validation, exercise all four frame sizes (2.5/5/10/20 ms) with PCM
and Opus during playback, then test reconnect, mute/unmute, jitter settings, and
device changes. See REVIEW.md for remaining issues and build limitations.

The repeatable Android foreground-control test uses a real phone and LAN server:

```sh
cd flutter_app
flutter test integration_test/phone_controls_test.dart -d DEVICE_SERIAL --dart-define=TEST_SERVER=SERVER_NAME --dart-define=SOAK_SECONDS=180 --reporter expanded
```

Allow AudioBridge's microphone/notification/Bluetooth permissions. The Flutter
control test expects the phone to be the first/only receiver so it owns mic
return. Test a secondary phone's disabled mic state and ownership handoff
separately. The Flutter
test installer can remove existing app data; compare and approve a fresh pairing
code on the PC if prompted. The test never auto-approves a code. It exercises
device-name cancel/blank/save/restore, scan start/stop, discovery, latency details,
ten mic toggles, an injected mic-start failure plus real retry, mute/unmute,
volume endpoints/intermediate values, all mic-source selections, media controls,
metadata refresh when available, disconnect, three saved-key reconnects, and a
three-minute encrypted connection soak. Native state is checked for mic, mute,
volume and shutdown; physical Bluetooth routing needs a connected headset.

Test notification Mic On/Off, Play/Pause, Stop, tap-to-open, background recording,
and hardware volume keys separately on the release APK. After an integration
build, use `flutter build apk --release` **without `--no-pub`** so Flutter
regenerates plugin registration without the debug-only integration-test plugin.
Install that APK with `adb install -r` to preserve data where available.

For an actual recording endpoint, `audio_endpoint_probe` measures levels without
recording audio. With the phone mic enabled and then disabled, respectively:

```powershell
./windows/build/security-tests-vs/Release/audio_endpoint_probe.exe "CABLE Output" 5
./windows/build/security-tests-vs/Release/audio_endpoint_probe.exe "CABLE Output" 5 --silence
```

Use `"AudioBridge Microphone"` instead after the native driver is signed and
installed. A passing signal test establishes nonzero PCM delivery, not audible
quality or correct Bluetooth routing. The capture probe may report a startup
discontinuity on opening a WASAPI stream; assess subsequent capture separately.
