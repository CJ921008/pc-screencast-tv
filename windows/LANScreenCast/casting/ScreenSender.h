#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace LANScreenCast::casting
{
    // Simple local-network video path: LSC1, then big-endian length + JPEG frames.
    void StreamDesktop(std::wstring const& receiverIp, int targetFps, std::atomic_bool const& stop,
        std::function<void(std::wstring const&)> const& onStatus);
}
