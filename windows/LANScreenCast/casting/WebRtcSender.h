#pragma once
#include "MediaPipeline.h"
#include <atomic>
#include <functional>
#include <string>
namespace LANScreenCast::casting {
    void StreamWebRtc(std::wstring const& ip, VideoSettings settings, std::atomic_bool const& stop,
        std::shared_ptr<std::atomic_bool> controlPermission,
        std::function<void(std::wstring const&)> const& onStatus);
}
