# LAN ScreenCast signaling v2

WebRTC uses WebSocket TCP port **47475**, path **/signaling**. JPEG compatibility uses the unchanged LSC1 TCP port **47474**. A shared receiver lock prevents these paths from running simultaneously.

Each JSON message carries protocolVersion=2, type, UUID requestId, Unix-millisecond timestamp, and payload. All payloads carry a sessionId; errors before a valid session may use an empty ID. See signaling.schema.json for message payload constraints.

Flow: hello (computerName) → user accepts → capabilities → offer (SDP + video settings) → answer → ICE candidates → video. The receiver reserves the session while acceptance is pending and replies BUSY to another sender. Rejection returns REJECTED. Version errors return VERSION_MISMATCH. ICE is queued until remote SDP is set. No public STUN/TURN services are used.

Both peers send ping every two seconds and answer pong; six seconds without signaling traffic ends the session. Human acceptance expires after 30 seconds; after acceptance the media handshake has ten seconds. disconnect and socket close release the reservation and all media resources.

The offer contains H.264 sendonly video, RTP packetization-mode=1, Constrained Baseline, Level 3.1 for 720p30 or Level 4.0 for 1080p30. Media uses UDP ICE plus DTLS/SRTP. RTCP NACK and PLI provide retransmission and keyframe recovery. SPS/PPS are sent with IDR frames; encoded frame timestamps share one monotonic capture clock.

stats reports actual rendered fps, received bitrate (bits/second), RTT (milliseconds), packet loss percent, and decoder implementation. Windows logs encoder name, hardware mode, dimensions, process CPU percent, capture/encode durations, and dropped queued frames.
