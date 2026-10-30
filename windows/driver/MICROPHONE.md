# AudioBridge Microphone

This is the Windows 11 x64 capture-only driver and app-lifetime service. The
existing `audiobridge_vad` speaker/tap driver is independent and is not required.
The implementation stays in C++, using the project's existing PortCls/WaveRT
approach. It does not change Android's encrypted transport.

## Status

The app, service, and unsigned kernel driver compile. The INF passes WDK
InfVerif `/w`, and Inf2Cat produces the unsigned catalog with no signability
errors or warnings. Automated tests exercise the actual pipe session handler
with a fake driver, including process termination and audio-buffer behavior.

Validation on 2026-10-30: all four native CTest suites passed; the real-time IPC
soak passed for 180 seconds / 9,000 packets. The updated prefill/expiry queue
also passed its regression checks. PowerShell installer/build scripts passed
syntax checks. SYS and CAT signatures were confirmed as `NotSigned`.

**The driver has not been loaded or validated as a live Windows audio endpoint.**
Kernel-driver signing and the hardware validation below are required before
calling this a working VB-CABLE replacement. No Secure Boot, test-signing,
certificate-store, installed-driver, or service changes are made by the build.

## Behavior

1. AudioBridge opens a local pipe to `AudioBridgeMicService`. The client checks
   that the pipe server PID matches the installed, running service before sending
   microphone samples.
2. The service accepts one interactive local client. It creates the fixed
   `SWD\AudioBridge\Microphone` software device using hardware ID
   `AudioBridge\Microphone`. Windows binds the previously staged capture driver.
3. The service waits for PnP startup, then opens the exclusive SYSTEM-only
   `\\.\AudioBridgeMic` control handle. The app receives a success reply only
   after driver readiness. Other applications select **AudioBridge Microphone**
   as their recording input.
4. Authenticated phone mic packets are decoded in the app, converted to 48 kHz
   mono signed 16-bit PCM, and passed by a bounded worker queue to the service.
   The service copies fixed, validated messages through buffered IOCTLs. There
   are no writable mappings of kernel memory into user processes.
5. The capture driver clocks its WaveRT buffer from QPC. It prefills 12 ms to
   absorb arrival jitter, outputs silence during starvation, and bounds queued
   audio to 40 ms. A partial unprimed tail expires after 100 ms. The application
   queue is separately bounded to 40 ms. These are buffer bounds, **not measured
   end-to-end latency**.
6. Pipe EOF, invalid input, a failed driver write, three seconds without a
   message, or service shutdown closes the control handle and `HSWDEVICE`.
   The service also watches the app process while PnP startup is pending.
   Windows asynchronously removes the active endpoint. A disconnected device
   record and the staged driver remain installed. A stable instance ID retains
   settings across launches. Other apps may still need to reopen their input
   after removal/recreation; this must be checked per application.

The app prefers the native endpoint once ready unless the user chooses another
receiving device. If the service/driver is absent at startup, the existing
receiving-device route stays available. If the native route later fails, it
becomes silent and retries; it does not suddenly play microphone audio through
the default speakers. The GUI logs availability and Windows error codes.

## Build

Build the Windows app normally; CMake also defines `audiobridge_mic_service`.
The service uses the static MSVC runtime so installation does not require a
separate VC runtime package for that executable.

```powershell
cmake --build windows/build --config Release --target audiobridge audiobridge_mic_service
./windows/driver/build_microphone.ps1
```

The driver script requires MSVC Build Tools 2022, Windows SDK 10.0.26100.0, and
matching WDK headers/libraries/tools. It accepts `-WdkRoot` and `-Version`.
It detects either the installed WDK or a project-local Microsoft WDK NuGet
package at `windows/build/wdk/package/c`. The latter avoids a machine-wide WDK
installation. The development build used
`Microsoft.Windows.WDK.x64` version `10.0.26100.1` from NuGet, which contains the
10.0.26100.0 kit. See Microsoft's
[WDK NuGet instructions](https://learn.microsoft.com/en-us/windows-hardware/drivers/install-the-wdk-using-nuget).

Output: `windows/driver/build/microphone/audiobridge_mic.{sys,inf,cat}`.
The generated catalog is **unsigned**. The compiler uses `/kernel /W4 /WX`, and
the driver links with the security-cookie entry point, NX, and integrity flags.

## Installation after signing

For normal Windows systems, obtain a Microsoft-signed kernel package through
the supported driver-signing workflow. See
[Microsoft's release signing documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/signing-a-driver-for-public-release).
Use a dedicated driver-development VM for early kernel/Driver Verifier tests.
The production installer does not bypass kernel signing policy.

Run from an elevated PowerShell after placing the signed package in its output
directory:

```powershell
./windows/driver/install_microphone.ps1
```

The script verifies both SYS and INF against the catalog using `signtool /kp`,
stages the package, copies the service into a restricted Program Files directory,
and creates an automatic LocalSystem service. It never installs a permanent
root audio devnode and never removes VB-CABLE. The service sits idle without an
audio endpoint while AudioBridge is closed. Ordinary app launches do not need
elevation once setup is complete.

To stop using it, select the previous receiving device, close AudioBridge, and
stop/disable `AudioBridgeMicService`. Removing the driver package and service
is a separate administrator operation; no automatic driver-store deletion is
performed.

## Validation

```powershell
cmake --build windows/build/security-tests-vs --config Release --target microphone_test
ctest --test-dir windows/build/security-tests-vs -C Release --output-on-failure
./windows/build/security-tests-vs/Release/microphone_test.exe --soak 180
```

The soak sends 9,000 messages at 50 packets/second (48 kHz, 960 samples each)
through the production session handler and checks cleanup. It uses a fake
driver backend, so it cannot verify PnP, audio quality, kernel correctness,
endpoint enumeration, or real driver removal.

Before release, on a signed/test-signed development system:

- Confirm that the microphone is absent before launching AudioBridge, appears
  after launch, and remains absent after closing or forcibly terminating it.
  Repeat rapid launch/exit, service stop/restart, startup cancellation, and
  sleep/resume. Check for leaked handles, timers, and devices.
- Open the endpoint in shared-mode WASAPI and a voice application. Feed known
  PCM/tone data and verify sample format, channels, level, timing, absence of
  repeated stale samples, silence on mute/disconnect, and overflow recovery.
- Run encrypted Pixel mic audio for at least three minutes, including bursty
  delivery, disconnect/reconnect, mic toggle, and application/device changes.
  Measure underruns and latency and listen; packet delivery alone is insufficient.
- Use Driver Verifier and the applicable Windows audio HLK tests. Exercise
  malformed formats, IOCTL lengths, multiple capture attempts, pending I/O,
  PnP surprise removal, and application crashes while the endpoint is open.
- Check that an unelevated interactive app can use the service, while remote,
  noninteractive, low-integrity, and competing pipe-server attempts are denied.

Device lifetime follows the documented
[SwDeviceClose contract](https://learn.microsoft.com/en-us/windows/win32/api/swdevice/nf-swdevice-swdeviceclose).
