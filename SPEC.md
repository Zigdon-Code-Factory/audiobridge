# AudioBridge - Communication Specification v2.0

## Overview
Stream system audio from a Windows/Linux/macOS PC to an Android phone over local WiFi with sub-50ms latency. All audio and control traffic is encrypted via DTLS.

## Architecture
```
[Server] Audio Capture → Opus Encode → DTLS → UDP → [Android] DTLS → Opus Decode → Oboe Playback
```

## Discovery Protocol (UDP Broadcast, plaintext)
- **Port:** 4011 (discovery), 4012 (audio stream)
- Discovery is unencrypted — it only reveals server name and MAC on the local network.
- **Flow:**
  1. Android app broadcasts `AB_DISCOVER` to `255.255.255.255:4011` every 2 seconds
  2. Server responds with `AB_OFFER|<server_name>|<stream_port>|<mac_address>` to sender
  3. Client proceeds to Connection Protocol on port 4012

## Connection Protocol (UDP, port 4012)
Only discovery, public pairing material, and accept/reject messages are plaintext. There is no plaintext application mode. Both apps must be updated; previously exposed keys require fresh pairing.

1. Client generates a fresh X25519 key and sends `AB_CONNECT2|<name>|<id>|<commitment>|<pair-or-resume>`.
2. The commitment is SHA-256 of `AB_PAIR2_COMMIT|<name>|<id>|<client-public-key>`. Public keys are 32 bytes encoded as lowercase hex. Names and IDs are bounded to 128 bytes and cannot contain control characters or pipes.
3. For pairing, the server sends `AB_KEY2|<server-public-key>` before the client reveals `AB_KEY2|<client-public-key>`. The client locks the first server key; the server verifies the earlier commitment. Retries reuse the same transcript. New attempts use fresh keys.
4. Both derive an X25519 shared secret, rejecting invalid/all-zero results. HKDF-SHA-256 uses the hash of `AB_PAIR2|<name>|<id>|<client-public-key>|<server-public-key>` as salt and `AudioBridge v2 PSK and SAS` as info. The first 32 output bytes are the PSK; the next four bytes, interpreted big-endian modulo 1,000,000, produce the six-digit comparison code.
5. The code is displayed independently on both devices. The user must compare all six digits and approve on the PC within 30 seconds. Approval is bound to that exchange, and denied/expired requests cannot save keys. The commitment prevents choosing an attacker key offline after learning the other side's code. The code itself is never sent across the network.
6. An approved pairing or known-peer reconnect receives `AB_ACCEPT2`, containing no secret. Both sides perform a DTLS 1.2 PSK handshake. The server binds the identity lookup to the expected client ID and that connection's key.
7. The client sends encrypted `AB_READY2` after completing its handshake. The server acknowledges it inside DTLS, then saves the pairing and starts streaming. The client retries READY if the acknowledgement is lost; the server acknowledges repeats. No audio floods a peer still completing its handshake.
8. Every application packet, including mic audio, metadata, controls, keepalive and disconnect, goes through this DTLS session. Peer IP and port must match, and only authenticated traffic refreshes the liveness timer.

### Stored keys and reconnects
- Persisted keys are `v2:<64 hex digits>`. Unversioned legacy keys are ignored because earlier builds sent them in plaintext.
- Server file: `clientId|clientName|lastConnected(unix)|v2:psk_hex`; client storage remains app-private server history.
- Known peers prove possession using DTLS; a UUID or matching IP alone grants no access. A wrong/missing key never triggers an unencrypted fallback.
- A new unauthenticated connection cannot evict a live authenticated client. Reconnect after an abrupt loss may need to wait for the five-second timeout.
- Legacy `AB_CONNECT` requests are rejected. Plaintext disconnects and binary application packets are discarded.

## Audio Stream Protocol (DTLS over UDP)

### Packet Format (binary, little-endian)
Packets are encrypted inside DTLS records. The inner (plaintext) format:
```
Offset  Size   Field
0       1      Version (0x01)
1       1      Type (see packet types below)
2       4      Sequence number (uint32, wrapping)
6       8      Timestamp (microseconds since stream start, uint64)
14      2      Payload length (uint16)
16      N      Payload (Opus audio data, control command, or JSON)
```

**Total header: 16 bytes** (unchanged from v1)

### Packet Types
| Type | Value | Direction | Description |
|------|-------|-----------|-------------|
| Audio | 0x01 | Server→Client | Opus-encoded speaker audio |
| Keepalive | 0x02 | Bidirectional | Heartbeat (no payload) |
| Control | 0x03 | Client→Server | Single-byte command |
| Mic Audio | 0x04 | Client→Server | Opus-encoded microphone audio |
| Media Info | 0x05 | Server→Client | JSON now-playing metadata |
| Ping | 0x06 | Bidirectional | RTT measurement (timestamp in header) |
| Pong | 0x07 | Bidirectional | RTT response (echoed timestamp) |
| Settings | 0x08 | Server→Client | Jitter, frame quantum, codec and QoS |
| PCM Audio | 0x09 | Server→Client | Raw 16-bit stereo PCM |

### Audio Parameters
- **Codec:** Opus
- **Sample rate:** 48000 Hz
- **Channels:** 2 (stereo server→client), 1 (mono client→server mic)
- **Frame size:** 480 samples (10ms) — optimized for low latency
- **Bitrate:** 128 kbps
- **Application mode:** OPUS_APPLICATION_RESTRICTED_LOWDELAY

### Keepalive
- Server sends keepalive (type 0x02, no payload) every 500ms when no audio
- Client sends keepalive every 1000ms
- Disconnect after 5 seconds without authenticated application packets

### Control Messages (type 0x03)
Payload is a single byte command:
- `0x01` = pause
- `0x02` = resume
- `0x03` = disconnect
- `0x10` = media play/pause
- `0x11` = media next track
- `0x12` = media previous track
- `0x13` = request media info

## Security (DTLS)

### Threat Model
The transport protects audio and application data on shared LAN/Wi-Fi networks against:
- **Passive eavesdropping:** prevents anyone on the LAN from capturing audio
- **Packet injection:** DTLS integrity checks reject forged packets
- **Replay attacks:** DTLS sequence numbers and anti-replay window

DTLS does **not** protect against:
- Incorrect approval of mismatching pairing codes (an active first-pair attacker has roughly a one-in-a-million chance per comparison attempt)
- Traffic analysis, discovery spoofing, or denial of service
- Later compromise of a stored PSK: the selected PSK-only DTLS suite does not provide forward secrecy for reconnections
- An attacker with access to the peer storage files on either device

### Implementation
- **Library:** mbedTLS 3.x (fetched via CMake, Apache 2.0 license)
- **Protocol:** DTLS 1.2 with PSK cipher suites
- **Cipher suite:** TLS_PSK_WITH_AES_128_GCM_SHA256
- **PSK length:** 32 bytes (256-bit), derived via X25519 and HKDF-SHA-256; ephemeral keys use mbedTLS entropy/CTR-DRBG
- **DTLS handshake timeout:** 250-2000 ms retransmission backoff within a 10-second connection deadline
- **Connection timeout:** 10 seconds for handshake to complete, then normal 5s keepalive timeout

### Performance impact
Encryption introduces no codec lookahead. Actual CPU cost, network latency and underruns must be measured on the target hardware; no microsecond latency guarantee is assumed. Mic packets enter a bounded queue with a nonblocking enqueue from the Oboe callback; a worker performs DTLS sends. DTLS state is serialized with receive and teardown.

Each AES-GCM DTLS 1.2 application record adds 37 bytes: a 13-byte record header, an 8-byte explicit nonce and a 16-byte authentication tag. A 480-byte PCM payload plus the 16-byte app header therefore occupies 533 UDP payload bytes (561 bytes including IPv4/UDP headers).

## Latency Budget (target <50ms total)
| Stage | Target |
|---|---|
| Audio capture | ~5ms |
| Opus encode (10ms frame) | ~1ms |
| DTLS encrypt | <0.01ms |
| Network (LAN UDP) | ~1ms |
| DTLS decrypt | <0.01ms |
| Jitter buffer | ~10-20ms |
| Opus decode | ~1ms |
| Oboe playback | ~5-10ms |
| **Total** | **~23-38ms** |

## Jitter Buffer (Android)
- Adaptive: 0-30ms range
- Start at 10ms (1 frame)
- Shrink if consistently early, grow if drops detected
- On underrun: Opus PLC, grow buffer by 1 frame

## Volume Control
- Server captures at system volume (loopback)
- Client has independent volume slider (0-100%, applied in native audio callback)

## Error Handling
- Out-of-order packets: play if within jitter buffer window, drop if too old
- Packet loss: Opus PLC (packet loss concealment) handles gaps
- No retransmission (UDP, latency-critical)
- DTLS handshake failure: log error, disconnect, client can retry

## Peer Persistence
- **File:** `approved_peers.txt` (next to server executable)
- **Format:** `clientId|clientName|lastConnected(unix)|v2:psk_hex`
- **Expiry:** 30-day sliding window from last connection
- **Legacy format** (no PSK): `clientId|clientName|lastConnected(unix)` — triggers re-pairing for DTLS

## File Structure

### Windows App (`windows/`)
- C++ GUI app (webview-based)
- Dependencies: WASAPI, libopus, mbedTLS, webview
- Build: CMake

### Linux App (`linux/`)
- C++ GUI app (webview-based)
- Dependencies: PulseAudio, libopus, mbedTLS, webview
- Build: CMake

### macOS App (`mac/`)
- C++ GUI app (webview-based)
- Dependencies: CoreAudio, libopus, mbedTLS, webview
- Build: CMake

### Flutter App (`flutter_app/`)
- Dart + FFI to native C for Opus decode + Oboe playback
- DTLS handled in native C++ layer (mbedTLS linked into the NDK build)
- Platform channels for UDP socket management
- Dependencies: opus (via FFI), oboe (native Android), mbedTLS (native)
