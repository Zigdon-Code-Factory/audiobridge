# Regression tests

Run native PCM boundary tests with a C++17 compiler and CMake:

```sh
cmake -S tests -B windows/build/playback-tests
cmake --build windows/build/playback-tests --config Release
ctest --test-dir windows/build/playback-tests -C Release --output-on-failure
```

Assertions remain enabled in Release. These tests use the production playback cursor, checking exact samples and output bounds for 5/10/20 ms and mixed packet durations with differently sized output callbacks. They require no Android SDK or audio device and do not exercise Opus or Oboe.

Run the application smoke and history tests:

```sh
cd flutter_app
flutter test --no-pub
```

For hardware validation, exercise all three frame sizes and change them during playback, then test reconnect, mute/unmute, jitter settings, and device changes. See REVIEW.md for remaining issues and build limitations.
