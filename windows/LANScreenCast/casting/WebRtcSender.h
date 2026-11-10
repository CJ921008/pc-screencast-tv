#pragma once
#include "MediaPipeline.h"
#include <atomic>
#include <functional>
#include <string>
namespace rtc { class WebSocket; }
namespace LANScreenCast::casting {
    inline constexpr unsigned short RemoteMediaFirstPort = 47500;
    inline constexpr unsigned short RemoteMediaLastPort = 47509;
    void StreamWebRtc(std::wstring const& ip, VideoSettings settings, std::atomic_bool const& stop,
        std::shared_ptr<std::atomic_bool> controlPermission,
        std::function<void(std::wstring const&)> const& onStatus);
    void StreamWebRtcSocket(std::shared_ptr<rtc::WebSocket> const& ws, std::string const& lanUrl,
        std::string const& bindIPv6, VideoSettings settings, std::atomic_bool const& stop,
        std::shared_ptr<std::atomic_bool> controlPermission,
        std::function<void(std::wstring const&)> const& onStatus);
}
