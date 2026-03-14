# AudioBridge Android Receiver

Discovers and connects to an AudioBridge Windows server on the local network, receiving Opus-encoded audio over UDP and playing it back with ultra-low latency via Oboe.

## Requirements

- Flutter 3.22+ with Android SDK
- Android NDK (installed via Flutter — `flutter doctor` will verify)
- Android device or emulator running API 27+ (recommended for low-latency AAudio)
  - API 21+ is supported but falls back to OpenSL ES with higher latency

## Architecture

```
UDP Discovery (4011) → AB_CONNECT (4012) → Opus packets → JNI → native_audio.cpp
                                                                    ├── Opus decode
                                                                    ├── Adaptive jitter buffer (10-30ms)
                                                                    └── Oboe exclusive low-latency output
```

## Build & Run

```bash
# From the flutter_app/ directory:
flutter pub get
flutter run          # debug on connected device
flutter build apk    # release APK
```

The native C++ code (Opus decoder + Oboe player) is built automatically by the NDK via CMake — no manual steps needed. Oboe and Opus are fetched via CMake FetchContent.

## Native Dependencies

Built automatically from source via `android/app/src/main/cpp/CMakeLists.txt`:

- **Oboe 1.9.0** — low-latency audio output (AAudio on API 27+, OpenSL ES fallback)
- **Opus 1.5.2** — audio codec (decode + packet loss concealment)

## Protocol

See `../SPEC.md` for the full AudioBridge protocol specification.

## Notes

- Playback volume is controlled via Android system volume
- The app uses Oboe's exclusive mode for lowest possible latency
- Jitter buffer adapts between 1-3 frames (10-30ms) based on network conditions
- Opus PLC (packet loss concealment) fills gaps on packet loss
