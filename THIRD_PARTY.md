# Third-party dependencies in 0.3.0

The Windows build uses vcpkg baseline 2750401336fb7c95f6619657a46a7e798661341c and x64-windows-static-md. libdatachannel is overridden to 0.24.5 with WebSocket and SRTP support; transitive versions are locked by the baseline.

| Component | Version | License / source |
|---|---|---|
| libdatachannel | 0.24.5 | MPL-2.0, https://github.com/paullouisageneau/libdatachannel/tree/v0.24.5 |
| libjuice, libsrtp, usrsctp, OpenSSL, plog, nlohmann-json | vcpkg baseline | Each package's upstream license is included under licenses in the Windows CI artifact |
| WebRTC Android SDK | 144.7559.15 | WebRTC BSD-style license and bundled third-party licenses, https://github.com/webrtc-sdk/android/releases/tag/v144.7559.15 |
| Java-WebSocket | 1.6.0 | MIT, https://github.com/TooTallNate/Java-WebSocket/tree/v1.6.0 |
| AndroidX / Kotlin | Gradle locked versions | Upstream Apache-2.0 licenses |
| Windows App SDK / C++/WinRT | project NuGet versions | Upstream Microsoft/MIT licenses |

libdatachannel is statically linked without modifying its source. Its source and license are available at the pinned upstream tag. Android WebRTC uses the published org.webrtc AAR; it is a community-maintained precompiled distribution, not a Google Maven artifact. H.264 encoder/decoder implementations come from the operating system/device codec providers.

The Windows artifact includes this file and the copyright/license files supplied by every installed vcpkg package. Android dependency source/license references are also included as an APK asset.
