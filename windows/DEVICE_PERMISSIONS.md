# Per-device desktop controls

The local Windows owner chooses **Read only** or **Admin** separately for every
approved phone under **Paired devices**. Read only is the default for both new
pairings and existing four-field peer records. Missing, unrecognized or differently
cased role fields are read only. Existing installations therefore lose remote PC
media control until the desktop owner explicitly grants Admin. This is an
intentional safe migration, not a streaming incompatibility.

The fifth field of `approved_peers.txt` is exactly `admin` or `reader`:
`clientId|clientName|lastConnected|v2:key|role`. The key remains private. Grants are
bound to the authenticated ID and stored key, not the phone name, address or
client-supplied role. Replacing a key resets its role to read only. Reconnects with
the same key restore the role. Removing a pairing removes its grant. Role writes
use a flushed temporary file plus atomic Windows replacement; a failed write
restores the previous in-memory role and reports failure to the desktop selector.
The peer store remains a local owner-controlled file, not a defense against other
processes running as that same Windows user.

Readers can listen, adjust their own phone volume/mute/playback, use their own
microphone/source (subject to existing first-phone ownership), request metadata
and disconnect. Admin additionally controls PC media playback, capture mode,
input/output choice, codec, frame duration, jitter target and voice cleanup.
Only the desktop has a role-edit API. No remote command edits pairings or roles,
executes arbitrary code, accepts file paths or forwards generic GUI actions.

## Application and authorization

Commands are accepted only inside an authenticated active DTLS session. Both old
one-byte PC media commands and new settings requests pass through the same role
check and bounded main-thread queue. Role/key/session/permission-generation checks
run again immediately before application. A downgrade (even followed by a regrant)
invalidates already queued writes. The local role setter and application are
serialized; a write already executing completes before a subsequent downgrade.
Revocation uses the same serialization and disconnects the peer. A failed removal
write restores the saved pairing and role and reports failure; the live session
may already be disconnected. The UI verifies saved-peer absence before announcing
removal, including a late authoritative confirmation. Roles and
settings are included in fresh encrypted snapshots; local role changes push a
snapshot immediately when a snapshot is available. The GUI tick also publishes
snapshots at most every 500 ms, including desktop-side changes.

Audio mutations run on the existing GUI tick, never the receive thread. The DTLS
mutex is released during application so stopping a capture thread cannot deadlock
its final network send. PC media actions run on a temporary COM MTA worker and
return their actual Windows media-session result to the GUI task. Desktop device
selection failures roll back, and an unavailable/fallback route is never reported
as successful selection. Voice cleanup availability is reported independently of
saved preferences; unavailable effects cannot be edited remotely.

## Wire protocol v1

Existing audio, settings (0x08), session-status (0x0a), pause, microphone and
metadata packets are unchanged. Old clients ignore the added packet types and
continue streaming; already granted old clients can still use PC media controls.
Old servers do not publish control snapshots: new phones keep all PC editing and
PC media controls disabled, with an update/permission explanation. Linux using the
shared server header compiles with controls unsupported and PC writes fail closed;
this Windows/Android implementation does not add a Linux owner permission UI.

Client-to-server packet **0x0b** has exactly 16 payload bytes: four little-endian
32-bit fields, `requestId`, `settingsRevision`, `command`, `signedValue`.
Maximum accepted rate is ten control requests per second per session, with a
32-request global pending queue. Excess-rate packets are dropped rather than
amplified; queue-full requests receive an explicit busy rejection. Phones allow
one pending mutation, time out unconfirmed requests, and never automatically
retry a mutation. No transport completion is treated as application success.

| Command | Value |
| --- | --- |
| 0 | 0: request a fresh snapshot (also allowed for readers) |
| 1 | Jitter target, 0?150 ms |
| 2 | Frame duration: 2 (=2.5 ms), 5, 10 or 20 |
| 3 | Codec: 0 Opus, 1 raw PCM |
| 4 | Capture: 0 recording device, 1 system loopback |
| 5, 6 | Input/output index, 0 default, 1?8 enumerated choices |
| 7, 9, 12 | Noise suppression, gate, low cut: 0/1 |
| 8 | Noise strength: 0?100 |
| 10 | Gate threshold: 10?90 |
| 11 | Gate hold: 100?1500 ms |
| 16 | PC media: 0x10 play/pause, 0x11 next, 0x12 previous |

Settings writes require the latest nonzero revision, including device-list
generation; stale requests fail with a refresh instruction. Legacy media commands
are internally converted to allowlisted command 16 with request/revision zero.
New media requests use current revisions too. Only the first eight device choices
are offered remotely (plus default); the desktop retains its complete list.
Labels are bounded ASCII display names; endpoint IDs never leave the desktop.
The snapshot also identifies the current endpoint when it falls outside the first
eight choices and reports microphone-output availability. Noise strength is a
dB-like suppression value, not a percentage.

Server-to-client **0x0b** is bounded JSON:
`{"version":1,"admin":false,"settings":{"revision":1,...}}`.
It contains only audio preferences, capture/effect availability, bounded device
display choices and current selections. No peer list, identity keys, pairing
codes, logs or arbitrary desktop state is included.

Server-to-client **0x0c** acknowledges application, not transport:
`{"request":1,"ok":false,"message":"Settings changed; refresh and retry"}`.
The phone keeps confirmed values until both a successful application result and
a matching current snapshot arrive (media actions use the actual media result).
Failures, missing acknowledgements and permission changes are displayed locally;
pending editing is disabled. Permission snapshots expire after three seconds
without a fresh update, disabling PC writes until status recovers. These UI checks
are supplementary; the server remains authoritative for every write.

## Validation boundaries

Automated native tests use loopback, isolated test pairings and fake application
callbacks; Flutter/UI tests use fixtures. They do not establish audible quality,
driver functionality, physical microphone routing, actual PC media behavior,
Android background-service UX or performance with real phones. No test installs
an APK or stops the user's running server.
