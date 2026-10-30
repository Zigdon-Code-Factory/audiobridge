# Regression tests

Run native PCM boundary tests with a C++17 compiler and CMake:

```sh
cmake -S tests -B windows/build/playback-tests
cmake --build windows/build/playback-tests --config Release
ctest --test-dir windows/build/playback-tests -C Release --output-on-failure
```

Assertions remain enabled in Release. These tests use the production playback cursor, checking exact samples and output bounds for 5/10/20 ms and mixed packet durations with differently sized output callbacks. They require no Android SDK or audio device and do not exercise Opus or Oboe.

When the apps' mbedTLS checkout exists at `windows/build/_deps/mbedtls-src`, the same CMake project also builds `secure_transport_test` and, on Windows, `server_security_test`. Use `-DMBEDTLS_SOURCE_DIR=/path/to/mbedtls` for another checkout. Local UDP loopback access is required.

The security tests exercise X25519/HKDF agreement, the comparison code and identity commitment, invalid keys, legacy key rejection, DTLS cookies and retransmission, all application packet types, tampering, duplicate/reordered records, previous-session replay, wrong PSKs/identities, and plaintext rejection. The Windows integration test runs the production server on temporary ports and verifies approval/denial, the encrypted READY barrier, mic and metadata delivery, authenticated reconnect, and resistance to plaintext disconnect/control injection. It stores test peer records beside the test executable and revokes its generated peer when finished.

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
