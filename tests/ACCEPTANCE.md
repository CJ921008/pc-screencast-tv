# 0.3.0 video acceptance

Build and signing success are separate from physical-device performance.

## Automated

- Android: ./gradlew :app:testDebugUnitTest :app:assembleDebug. Five signaling-state tests cover early ICE, invalid order, rejection/reset, bounded ICE queue, and JPEG/WebRTC exclusion.
- Protocol: pip install jsonschema==4.23.0; python protocol/check_schema.py. Ten message payloads plus invalid version, resolution, and type.
- Windows: build the app, then CMake windows/tests with the same vcpkg toolchain and installed directory; run ctest. The test generates 60 moving NV12 frames, requests IDRs, checks timestamps, encodes and decodes with software H.264, and sends the result over a real WebRTC loopback.

## Physical Windows and receiver

1. Install both 0.3.0 packages; allow Windows private-network access. Start the receiver, enter its IP on Windows, choose H.264/WebRTC, 720p, 30 FPS, and accept on the receiver.
2. Run a continuous moving test pattern or scrolling content for ten minutes, then repeat at 1080p30. Record encoder and decoder names, hardware/software mode, sent/rendered FPS, bitrate, CPU, RTT, packet loss, encodeMs, captureMs and queueDropped from the logs.
3. Hardware acceptance: mean rendered FPS >=27 for each supported resolution, first frame <=3000 ms after acceptance, no cursor flashing, garbled text, crashes, or growing frame backlog. Software fallback defaults to 720p and reports measured performance without this hardware guarantee.
4. To measure end-to-end latency, record the same millisecond clock on the computer and receiving screen in one camera view. Compute P95 across at least 100 samples; initial target <=250 ms. Unaligned clocks and encodeMs are not end-to-end measurements.
5. Stop/start three times, reject a request, attempt a second sender while acceptance is pending and while streaming, disconnect Wi-Fi, close each app, change display mode, and retry manually. Verify cleanup and an actionable status. Test forced software encoding in the native test and JPEG compatibility on the same receiver.
6. For comparison, run the same moving content at 720p30 in JPEG mode and record the same metrics; do not compare static-screen FPS to dynamic content.

## Browser-to-Android integration diagnostic

Serve the repository with python -m http.server 8765, then open tests/webrtc-browser.html?ip=RECEIVER_IP&width=1280 in Chrome via http://localhost:8765/. Accept the request on Android. Repeat with width=1920.

The page transmits a moving 30 FPS canvas using H.264 and the actual v2 signaling endpoint. window.results records connection state, receiver statistics and errors. Verify DECODER, FIRST_FRAME_MS, RESOLUTION and STATS in Android logs, then call window.stopTest() and check that the receiver returns to waiting. Local-network browser permission may be required. This verifies Android WebRTC reception, not the Windows Media Foundation hardware encoder.

All physical-device results must name the actual devices and modes. Unsupported or unavailable tests must be reported as pending.
