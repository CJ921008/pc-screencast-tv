# LAN ScreenCast signaling v2

WebRTC uses WebSocket TCP port **47475**, path **/signaling**. JPEG compatibility uses the unchanged LSC1 TCP port **47474**. A shared receiver lock prevents these paths from running simultaneously.

Each JSON message carries protocolVersion=2, type, UUID requestId, Unix-millisecond timestamp, and payload. All payloads carry a sessionId; errors before a valid session may use an empty ID. See signaling.schema.json for message payload constraints.

Flow: hello (computerName) → user accepts → capabilities → offer (SDP + video settings) → answer → ICE candidates → video. The receiver reserves the session while acceptance is pending and replies BUSY to another sender. Rejection returns REJECTED. Version errors return VERSION_MISMATCH. ICE is queued until remote SDP is set. No public STUN/TURN services are used.

Both peers send ping every two seconds and answer pong; six seconds without signaling traffic ends the session. Human acceptance expires after 30 seconds; after acceptance the media handshake has ten seconds. disconnect and socket close release the reservation and all media resources.

The offer contains H.264 sendonly video, RTP packetization-mode=1, Constrained Baseline, Level 3.1 for 720p30 or Level 4.0 for 1080p30. Media uses UDP ICE plus DTLS/SRTP. RTCP NACK and PLI provide retransmission and keyframe recovery. SPS/PPS are sent with IDR frames; encoded frame timestamps share one monotonic capture clock.

stats reports actual rendered fps, received bitrate (bits/second), RTT (milliseconds), packet loss percent, and decoder implementation. Windows logs encoder name, hardware mode, dimensions, process CPU percent, capture/encode durations, and dropped queued frames.

## IPv6 remote connection

Android opens `wss://[WINDOWS_IPV6]:47476/remote`. The pairing code is `LSC1.<certificate SHA-256>.<random token>` using unpadded base64url for the two 32-byte fields. Android validates the exact certificate pin during TLS before sending a text JSON message with `type: "remote_auth"`, `version: 1` and `token`. On acceptance, Windows sends the existing v2 `hello` and both endpoints reuse v2 signaling on that socket. On rejection, Windows may send a `remote_error` object with `version: 1` and a `code` such as `PAIRING_REQUIRED` or `BUSY` before closing. Only one authenticated session is active at a time.

Windows binds the public IPv6 entered by the user, including for the host ICE candidates in UDP 47500–47509. Data channel control requires a separate permission on Windows. The access token and certificate private key are protected using Windows DPAPI. Resetting pairing rotates the certificate and the token.
