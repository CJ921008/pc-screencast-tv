# LAN ScreenCast signaling v2

WebRTC uses WebSocket TCP port **47475**, path **/signaling**. JPEG compatibility uses the unchanged LSC1 TCP port **47474**. A shared receiver lock prevents these paths from running simultaneously.

Each JSON message carries protocolVersion=2, type, UUID requestId, Unix-millisecond timestamp, and payload. All payloads carry a sessionId; errors before a valid session may use an empty ID. See signaling.schema.json for message payload constraints.

Flow: hello (computerName) → user accepts → capabilities → offer (SDP + video settings) → answer → ICE candidates → media. The receiver reserves the session while acceptance is pending and replies BUSY to another sender. Rejection returns REJECTED. Version errors return VERSION_MISMATCH. ICE is queued until remote SDP is set. No public STUN/TURN services are used.

Both peers send ping every two seconds and answer pong; six seconds without signaling traffic ends the session. Human acceptance expires after 30 seconds; after acceptance the media handshake has ten seconds. disconnect and socket close release the reservation and all media resources.

The offer contains H.264 sendonly video, RTP packetization-mode=1, Constrained Baseline, Level 3.1 for 720p30 or Level 4.0 for 1080p30. Media uses UDP ICE plus DTLS/SRTP. RTCP NACK and PLI provide retransmission and keyframe recovery. SPS/PPS are sent with IDR frames.

Receivers may advertise optional `opus: true` in `capabilities` when their audio receiver supports Opus 48 kHz stereo. The sender then adds sendonly Opus (payload type 111, SSRC 43, 20 ms frames, 128 kbps) alongside H.264 (payload type 102, SSRC 42). Both use BUNDLE on the same PeerConnection, CNAME `lanscreencast`, and stream ID `screen`. Missing or false `opus` preserves video-only negotiation, so protocolVersion remains 2. JPEG has no audio path.

Windows captures the default rendering endpoint using shared WASAPI loopback and converts to interleaved stereo float PCM at 48 kHz. Endpoint changes and capture errors trigger retry while sending silence. Samples are bounded and late audio is dropped after a sender stall. Video capture timestamps and audio QPC timestamps use a common monotonic origin; RTCP sender reports map both RTP clocks (90 kHz video, 48 kHz audio) to one fixed NTP epoch, including capture/encode delays. Android uses media audio output without recording, applies local mute and audio focus, and releases playback on disconnect.

stats reports actual rendered fps, received bitrate (bits/second), RTT (milliseconds), packet loss percent, and decoder implementation. Windows logs encoder name, hardware mode, dimensions, process CPU percent, capture/encode durations, and dropped queued frames.

## IPv6 remote connection

Android opens `wss://[WINDOWS_IPV6]:47476/remote`. The pairing code is `LSC1.<certificate SHA-256>.<random token>` using unpadded base64url for the two 32-byte fields. Android validates the exact certificate pin during TLS before sending a text JSON message with `type: "remote_auth"`, `version: 1` and `token`. On acceptance, Windows sends the existing v2 `hello` and both endpoints reuse v2 signaling on that socket. On rejection, Windows may send a `remote_error` object with `version: 1` and a `code` such as `PAIRING_REQUIRED` or `BUSY` before closing. Only one authenticated session is active at a time.

Windows binds the public IPv6 entered by the user, including for the host ICE candidates in UDP 47500–47509. Data channel control requires a separate permission on Windows. The access token and certificate private key are protected using Windows DPAPI. Resetting pairing rotates the certificate and the token.


## Physical keyboards and mice

A sender supporting hardware input adds `hardwareInput: true` to its `view` message on the authorized `lsc-input-v1` data channel. Receivers use it to enable hardware-event forwarding. Legacy receivers keep using their existing touch and text commands. All inputs use the same channel and foreground lease, including over the authenticated public IPv6 connection.

- `key_state` has an integer `scanCode` (PC set 1 make code, with bit 8 representing the E0 prefix), a boolean `down`, and an optional boolean `repeat`. A repeat applies only to an already held key.
- `mouse_button` has a `button` of `left`, `right`, or `middle`, a boolean `down`, and optional `x` and `y` in the existing 0–10000 content coordinates.
- `wheel` accepts optional coordinates and an optional `horizontal` boolean. `delta` is in Windows wheel units.
- `hardware_reset` releases held physical keyboard and mouse input when the receiver loses focus, opens its local control menu, or disconnects an active input device. Pause, lease expiry, permission revocation, and channel close release all tracked input.
