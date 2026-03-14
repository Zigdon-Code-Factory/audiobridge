# AudioBridge - Communication Specification v1.0

## Overview
Stream system audio from a Windows PC to an Android phone over local WiFi with sub-50ms latency.

## Architecture
```
[Windows] WASAPI Loopback → Opus Encode → UDP → [Android] Opus Decode → Oboe Playback
```

## Discovery Protocol (UDP Broadcast)
- **Port:** 4011 (discovery), 4012 (audio stream)
- **Flow:**
  1. Android app broadcasts `AB_DISCOVER` to `255.255.255.255:4011` every 2 seconds
  2. Windows server responds with `AB_OFFER|<server_name>|<stream_port>` to sender
  3. Android sends `AB_CONNECT|<client_name>` to server's stream port (4012)
  4. Server responds `AB_ACCEPT` and begins streaming
  5. Either side can send `AB_DISCONNECT` to stop

## Audio Stream Protocol (UDP)

### Packet Format (binary, little-endian)
```
Offset  Size   Field
0       1      Version (0x01)
1       1      Type (0x01=audio, 0x02=keepalive, 0x03=control)
2       4      Sequence number (uint32, wrapping)
6       8      Timestamp (microseconds since stream start, uint64)
14      2      Payload length (uint16)
16      N      Opus-encoded audio data
```

**Total header: 16 bytes**

### Audio Parameters
- **Codec:** Opus
- **Sample rate:** 48000 Hz
- **Channels:** 2 (stereo)
- **Frame size:** 480 samples (10ms) — optimized for low latency
- **Bitrate:** 128 kbps
- **Application mode:** OPUS_APPLICATION_RESTRICTED_LOWDELAY

### Keepalive
- Server sends keepalive (type 0x02, no payload) every 500ms when no audio
- Client sends keepalive every 1000ms
- Disconnect after 5 seconds of no packets from either side

### Control Messages (type 0x03)
Payload is a single byte command:
- `0x01` = pause
- `0x02` = resume  
- `0x03` = disconnect

## Latency Budget (target <50ms total)
| Stage | Target |
|---|---|
| WASAPI capture | ~5ms |
| Opus encode (10ms frame) | ~1ms |
| Network (LAN UDP) | ~1ms |
| Jitter buffer | ~10-20ms |
| Opus decode | ~1ms |
| Oboe playback | ~5-10ms |
| **Total** | **~23-38ms** |

## Jitter Buffer (Android)
- Adaptive: 10-30ms range
- Start at 20ms (2 frames)
- Shrink if consistently early, grow if drops detected
- On underrun: insert silence, grow buffer by 1 frame

## Volume Control
- Server captures at system volume (WASAPI loopback)
- Client handles playback volume independently via Android system volume

## Error Handling
- Out-of-order packets: play if within jitter buffer window, drop if too old
- Packet loss: Opus PLC (packet loss concealment) handles gaps
- No retransmission (UDP, latency-critical)

## File Structure

### Windows App (`windows/`)
- C++ console app (or tray app)
- Dependencies: WASAPI (Windows SDK), libopus
- Build: CMake

### Flutter App (`flutter_app/`)
- Dart + FFI to native C for Opus decode + Oboe playback
- Platform channels for UDP socket management
- Dependencies: opus (via FFI), oboe (native Android)
