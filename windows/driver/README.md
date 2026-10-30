# AudioBridge Virtual Speakers (`audiobridge_vad.sys`)

For the app-lifetime **AudioBridge Microphone** capture driver, service, build,
signing status, and validation steps, see [MICROPHONE.md](MICROPHONE.md).
The speaker/tap driver documented below is independent.

Custom Windows 10/11 **WaveRT Virtual Audio Speaker Driver** that replaces Voicemeeter / VB-Cable and eliminates Windows `audiodg.exe`'s hardcoded `10 ms` (`480`-frame) user-mode loopback APO tap.

## How It Cuts Capture Latency to 1.0–2.67 ms

1. **Native WaveRT Notification Support (`IMiniportWaveRTStreamNotification`):**
   - Exposes **"AudioBridge Virtual Speakers"** (`48 kHz` stereo 32-bit IEEE float & 16-bit PCM).
   - `audiobridge.exe` opens a silent companion `IAudioClient3::InitializeSharedAudioStream(..., 128, ...)` stream on the endpoint, switching Windows `audiodg.exe` from `480`-frame (`10.0 ms`) ticks down to **`128`-frame (`2.67 ms`)** ticks (or `48`-frame `1.0 ms` in Exclusive mode).
2. **Direct Zero-Copy Kernel Tap (`\\.\AudioBridgeTap`):**
   - Standard WASAPI `AUDCLNT_STREAMFLAGS_LOOPBACK` buffers `480` frames (`10.0 ms`) inside `audiodg.exe` even when the render engine runs at `128` frames.
   - Instead, `audiobridge_vad.sys` copies each freshly rendered `48–128`-frame WaveRT DMA quantum directly into a page-aligned shared-memory ring buffer (`AudioBridgeTapSharedBuffer` in `common/audiobridge_tap.h`) mapped into `audiobridge.exe` via `IOCTL_AUDIOBRIDGE_MAP_BUFFER`, and wakes `audiobridge.exe` immediately (`< 0.1 ms`) via `KeSetEvent` (`IOCTL_AUDIOBRIDGE_REGISTER_EVENT`).
3. **Automatic Detection & Seamless Fallback:**
   - `AudioCapture::initialize()` automatically checks for `\\.\AudioBridgeTap` when capturing `AudioBridge Virtual Speakers` (or default output). When loaded, it uses the direct `2.67 ms` kernel tap; when not installed yet, it automatically falls back to event-driven WASAPI Loopback.

## Building & Signing on Windows 11

1. Install the matching Windows Driver Kit (`WDK`) for Windows SDK `10.0.26100.0`:
   ```powershell
   winget install Microsoft.WindowsWDK.10.0.26100
   ```
2. Compile `audiobridge_vad.sys`:
   ```powershell
   .\build_driver.ps1 -TestSign
   ```
3. **Driver Signature Note (Secure Boot + BitLocker):**
   - Windows 11 with **Secure Boot** enabled requires kernel `.sys` drivers to be **Microsoft Attestation Signed** via the Windows Hardware Developer Program (Partner Center + EV / Azure Trusted Signing certificate) — no Secure Boot changes or BitLocker prompts required once attestation-signed.
   - For local kernel testing without an attestation signature, disable Secure Boot (suspend BitLocker first with `Suspend-BitLocker -MountPoint "C:" -RebootCount 1` so BitLocker does not prompt for a recovery key!), run `bcdedit /set testsigning on` as Administrator, reboot, and run `.\install_driver.ps1`.
