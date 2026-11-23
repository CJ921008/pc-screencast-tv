#pragma once

#include "WebRtcSender.h"
#include <filesystem>
#include <string>
#include <string_view>

namespace LANScreenCast::casting {
struct RemoteIdentity {
    std::string certificate;
    std::string privateKey;
    std::string token;

    static RemoteIdentity Generate();
    std::string PairingCode() const;
    bool Authorizes(std::string_view message) const;
};

void ServeRemoteWebRtc(std::wstring const& ipv6, std::filesystem::path const& identityPath, bool resetPairing,
    VideoSettings settings, std::atomic_bool const& stop, std::shared_ptr<std::atomic_bool> controlPermission,
    std::function<void(std::wstring const&)> const& onStatus,
    std::function<void(std::wstring const&)> const& onPairingCode);
}
