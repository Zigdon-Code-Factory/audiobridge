# Phone wireless headset and microphone effects

The Windows server filters the returned phone microphone after Opus decoding and
before either Windows output path. Android does not need an update for this feature.
PC playback to the phone remains a separate encrypted network stream.

The native C++ pipeline adapts the audio processing in
[Open Broadcast](https://github.com/Zigdon-Code-Factory/open-broadcast), reviewed at
commit `72af5bf60c8c66bbe0b6bc68b27660e01625a37c`. It uses the same streaming
DeepFilterNet3/Silero approach without running Open Broadcast or Python on the PC
at runtime. Open Broadcast's video effects are not part of this integration.

## One-cable setup

1. Keep **Mode** on **Loopback**. For **PC to phone**, select the playback device
   used by Windows, your game, and your calling app (for example Voicemeeter Input).
2. Connect the phone and enable its microphone. Only the microphone-owning phone
   returns audio when several receivers are connected.
3. Set **Phone mic to PC** to **CABLE Input (VB-Audio Virtual Cable)**.
4. In Discord, Zoom, or your game, choose **CABLE Output** as the microphone.
   Choose the playback device from step 1 for that app's speakers.
5. Listen on the phone or headphones connected to the phone. Open Broadcast is
   not needed in this route; close/stop its processing if it also writes to CABLE Input.

One installed VB-CABLE is sufficient. Do not send other applications' playback to
CABLE Input, capture CABLE Input as PC-to-phone audio, or route the mic back into
its own Voicemeeter bus. Independent game/music/call mixes may use Voicemeeter's
existing virtual buses; they do not require a separate cable for each direction.

AudioBridge Microphone is an alternative only with a working installed driver and
service. These changes do not install drivers or change Windows default devices.
For a headset, use headphones to avoid acoustic echo. Noise removal is not echo
cancellation and does not eliminate feedback from a phone speaker into its mic.

## Controls

- **Noise removal** uses DeepFilterNet3 at 48 kHz. Strength limits attenuation to
  6/10/15/20/30 dB or Max. Lower values retain more room sound. Dry audio is delayed
  to align with the model before mixing; model delay is 32 ms plus hop/queue buffering.
- **Voice gate** uses Silero VAD. Expand the tuning section for detection threshold,
  0.1–1.5 second hold, and an optional 80 Hz Butterworth high-pass filter.
- **Record** captures up to 10 seconds of returned phone mic audio into memory.
  Stop to generate aligned Original/Processed versions. The playback bar provides
  play/pause and seeking. Switching versions preserves position; effect changes
  re-render the processed recording on a separate worker. Playback uses the PC's
  default output (and reaches the phone if that device is being streamed).
- Closing AudioBridge discards recordings. They are not saved or uploaded.

Settings persist in the existing `audiobridge_settings.txt` beside the executable.
The audio/network protocol and pairing records are unchanged.

## Build and verification

CMake fetches ONNX Runtime 1.23.2 for Windows x64 with an archive checksum. The
standard-library-only `voice_models.py` fetches pinned DeepFilterNet and Silero
models and verifies the model bytes before use. Python is needed at build time,
not at runtime. Keep `onnxruntime.dll` and `voice-models/` beside `audiobridge.exe`
when copying the build. Windows ARM and 32-bit builds are not supported by this
runtime package. Linux/macOS processing is unchanged.
Preserve `voice-models/licenses/` (Open Broadcast, DeepFilterNet and Silero notices)
and `licenses/onnxruntime/` when distributing the build.

```powershell
cmake --build windows/build --config Release --target audiobridge voice_effects_test
ctest --test-dir windows/build -C Release -R voice_ --output-on-failure
```

The real-model regression checks cover arbitrary phone packet sizes, bypass,
high-pass response, aligned strength mixing, VAD closure/reset, recording,
re-rendering, and missing-model passthrough. Inference is kept off the network
thread. The live input queue is bounded to 100 ms; overflow drops stale samples
and resets model state. Ownership changes reset model history and finish an
active recording so unrelated phones are not combined. The UI reports effect
load/inference failures and retains unprocessed microphone return.
Output is delivered in at most 512-sample batches to stay within the native
microphone's packet limit. Audio during output-device reconfiguration is dropped
rather than blocking the encrypted network callback.

This does not establish phone-to-PC latency, subjective quality, Bluetooth
routing, or acoustic echo performance; verify those with your phone and a call.

## Dependencies and model sources

- ONNX Runtime 1.23.2 (MIT): https://github.com/microsoft/onnxruntime/tree/v1.23.2
- DeepFilterNet / torchDF streaming export (MIT/Apache-2.0):
  https://github.com/grazder/DeepFilterNet/tree/408414f2bdb9ed772580558d1c3ec26c5dc002d7
- Silero VAD v6.2.3 (MIT): https://github.com/snakers4/silero-vad/tree/v6.2.3

The pinned model URLs and SHA-256 values are recorded in `voice_models.py`.
