# AudioBridge Windows Server

Captures system audio via WASAPI loopback, encodes to Opus, and streams over UDP to an AudioBridge client (e.g., Android app).

## Requirements

- Windows 10 or later
- CMake 3.20+
- Visual Studio 2019+ (or Build Tools with MSVC)
- Git (for FetchContent to download libopus)

## Build

```powershell
# From the windows/ directory:
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

Or with Ninja (faster):

```powershell
# Open "Developer Command Prompt for VS" or "x64 Native Tools Command Prompt"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The executable will be at `build/Release/audiobridge.exe` (VS) or `build/audiobridge.exe` (Ninja).

## Usage

```powershell
audiobridge.exe
```

The server will:
1. Start capturing system audio from the default output device
2. Listen on UDP port 4011 for discovery broadcasts
3. Listen on UDP port 4012 for client connections
4. When a client connects, stream Opus-encoded audio to it
5. Print status updates to the console

Press `Ctrl+C` to exit.

## Protocol

See `../SPEC.md` for the full AudioBridge protocol specification.

## Firewall

You may need to allow UDP ports 4011 and 4012 through Windows Firewall:

```powershell
netsh advfirewall firewall add rule name="AudioBridge Discovery" dir=in action=allow protocol=UDP localport=4011
netsh advfirewall firewall add rule name="AudioBridge Stream" dir=in action=allow protocol=UDP localport=4012
```
