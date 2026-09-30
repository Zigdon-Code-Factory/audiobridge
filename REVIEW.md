# Logic review — 2026-09-07

## Implemented

- Android decodes up to 960 stereo frames (20 ms), records the decoded length, and consumes PCM through a callback-owned cursor. Two 5 ms packets can fill a 10 ms callback; a 20 ms packet survives across callbacks. Mixed packet sizes work while settings change.
- Windows frame-size updates use a validated atomic value, loaded once per capture batch.
- Jitter targets round up to whole packets and support 150 ms at all supported durations (20 ms packets round to 160 ms).
- Flutter disposal now stops discovery before disposing its animation and clears the native event handler. The new smoke test exposed the previous cleanup failure.

## Remaining findings (not changed)

1. **P1 — Microphone bypasses encryption and is dropped by the Windows server in DTLS mode.** `flutter_app/android/app/src/main/cpp/native_audio.cpp:665` sends microphone packets directly through the recorder's UDP socket. `windows/src/network.cpp:510` only processes plaintext binary packets when DTLS is inactive. Route mic packets through the connection manager's encrypted send path; queue them off the audio callback to avoid network blocking.
2. **P1 — Android decoder has concurrent owners.** `native_audio.cpp:291` decodes network packets while `native_audio.cpp:409` invokes concealment on the same decoder from the playback callback. Stop/restart also destroys that decoder. Give decoding and concealment one thread owner and synchronize lifecycle changes. A mutex in the real-time callback alone would introduce blocking and would not repair packet ordering.
3. **P1 — macOS drops capture remainders.** `mac/src/audio_capture.cpp:154` only emits complete 480-frame chunks from each CoreAudio callback. A 256-frame callback emits nothing; a 512-frame callback loses 32 frames. Preserve remainders across callbacks, as on Windows.
4. **P2 — Packet sequence generation still races.** `windows/src/network.cpp:617` increments a plain sequence counter before the send lock. Audio, GUI settings, media, and network responses can call header generation concurrently. Allocate sequence values atomically or serialize packet construction; synchronize resets as well. Linux/macOS copies use the same plain counter pattern.
5. **P2 — Logging performs per-packet I/O in the streaming path.** `common/dtls_session.cpp:320`, `:351`, and `:372` log successful sends/receives; `windows/src/main.cpp:151` makes stdout unbuffered. At 5 ms this adds hundreds of synchronous log writes per second. Keep errors and periodic aggregate counters; gate packet tracing behind an opt-in debug setting.
6. **P2 — Windows capture does avoidable allocation/copy work.** `windows/src/audio_capture.cpp:442` allocates a vector for every audio packet, after individual deque pushes/pops. Reuse storage and batch copies, then measure capture-thread time and underruns before/after.

The source review covered the audio, network/DTLS, application lifecycle, UI bindings, Android service, and serving/build/test code across the platform implementations. These are static findings, not hardware reproductions or an exhaustive certification. Generated platform scaffolding, fetched dependencies, and release binaries were not audited as application logic.

## Validation

- Native playback regression executable: passed 20 combinations covering 5/10/20 ms packets, mixed packet durations, callback sizes 192/240/480/512/960, exact sample ordering, guard samples, and cursor reset/empty behavior.
- Flutter tests: 2 passed (startup/native cleanup and history serialization). Replaced the stale template test referencing nonexistent MyApp.
- Android arm64 release native library: built successfully using the existing SDK/CMake cache.
- Windows audio_capture.cpp: compiled successfully with MSVC and the Windows SDK.
- Full Windows CMake build: blocked by the installed Visual Studio instance not being registered with its installer. No full executable link verified.
- Flutter analysis: 70 existing warning/info diagnostics, chiefly unused fields/helpers and deprecated withOpacity calls; no analyzer errors reported.
- No physical-device listening, timing, packet-loss, Bluetooth, Linux, or macOS runtime tests were performed. Native PCM tests do not exercise Opus, Oboe scheduling, or networking.
