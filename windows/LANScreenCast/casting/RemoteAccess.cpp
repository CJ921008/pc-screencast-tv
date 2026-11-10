#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <dpapi.h>
#include "RemoteAccess.h"
#include "logging/FileLogger.h"
#include <rtc/rtc.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Ws2_32.lib")

namespace LANScreenCast::casting {
namespace {
using Clock = std::chrono::steady_clock;
using Socket = std::shared_ptr<rtc::WebSocket>;
using json = nlohmann::json;
constexpr uint16_t SignalingPort = 47476;

std::string Protect(std::string const& data, bool encrypt) {
    DATA_BLOB input{static_cast<DWORD>(data.size()), reinterpret_cast<BYTE*>(const_cast<char*>(data.data()))};
    DATA_BLOB output{};
    auto ok = encrypt ? CryptProtectData(&input, L"LAN ScreenCast remote identity", nullptr, nullptr, nullptr,
        CRYPTPROTECT_UI_FORBIDDEN, &output)
        : CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok) throw std::runtime_error("无法保护或读取远程配对信息");
    std::unique_ptr<BYTE, decltype(&LocalFree)> buffer(output.pbData, LocalFree);
    return {reinterpret_cast<char const*>(output.pbData), output.cbData};
}

RemoteIdentity LoadIdentity(std::filesystem::path const& path, bool reset) {
    if (!reset && std::filesystem::exists(path)) {
        auto size = std::filesystem::file_size(path);
        if (size == 0 || size > 16384) throw std::runtime_error("远程配对信息无效，请重置配对");
        std::ifstream file(path, std::ios::binary);
        std::string encrypted(static_cast<size_t>(size), '\0');
        if (!file.read(encrypted.data(), encrypted.size())) throw std::runtime_error("无法读取远程配对信息");
        auto value = json::parse(Protect(encrypted, false));
        return {value.at("certificate").get<std::string>(), value.at("privateKey").get<std::string>(),
            value.at("token").get<std::string>()};
    }
    auto identity = RemoteIdentity::Generate();
    auto encrypted = Protect(json{{"certificate", identity.certificate}, {"privateKey", identity.privateKey},
        {"token", identity.token}}.dump(), true);
    auto temp = path;
    temp += L".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(encrypted.data(), encrypted.size());
        file.close();
        if (!file) throw std::runtime_error("无法保存远程配对信息");
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("无法更新远程配对信息");
    return identity;
}

struct RemoteInbox {
    struct Pending { Socket socket; Clock::time_point deadline; };
    std::mutex mutex;
    std::vector<Pending> pending;
    Socket next;
    bool busy = false;
    bool closed = false;
};

void CloseSocket(Socket const& peer) {
    try { peer->forceClose(); } catch (std::exception const&) {}
}

void ReplyError(Socket const& peer, std::string const& code) {
    try {
        if (peer->isOpen()) peer->send(json{{"type", "remote_error"}, {"version", 1}, {"code", code}}.dump());
        peer->close();
    } catch (std::exception const&) { CloseSocket(peer); }
}

std::wstring Wide(std::string_view message) {
    auto length = static_cast<int>(message.size());
    int size = MultiByteToWideChar(CP_UTF8, 0, message.data(), length, nullptr, 0);
    std::wstring result(size, 0);
    if (size) MultiByteToWideChar(CP_UTF8, 0, message.data(), length, result.data(), size);
    return result;
}
}

void ServeRemoteWebRtc(std::wstring const& ipv6, std::filesystem::path const& identityPath, bool resetPairing,
    VideoSettings settings, std::atomic_bool const& stop, std::shared_ptr<std::atomic_bool> controlPermission,
    std::function<void(std::wstring const&)> const& onStatus,
    std::function<void(std::wstring const&)> const& onPairingCode) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("Winsock initialization failed");
    struct Runtime { ~Runtime() { WSACleanup(); } } runtime;
    in6_addr address{};
    char ip[INET6_ADDRSTRLEN]{};
    if (InetPtonW(AF_INET6, ipv6.c_str(), &address) != 1 || (address.u.Byte[0] & 0xe0) != 0x20 ||
        !InetNtopA(AF_INET6, &address, ip, sizeof(ip)))
        throw std::runtime_error("请输入电脑网卡的公网 IPv6 地址（不带方括号）");

    auto identity = std::make_shared<RemoteIdentity>(LoadIdentity(identityPath, resetPairing));
    auto code = identity->PairingCode();
    rtc::WebSocketServerConfiguration config;
    config.port = SignalingPort;
    config.bindAddress = ip;
    config.enableTls = true;
    config.certificatePemFile = identity->certificate;
    config.keyPemFile = identity->privateKey;
    config.connectionTimeout = std::chrono::seconds(5);
    config.maxMessageSize = 1024 * 1024;

    auto inbox = std::make_shared<RemoteInbox>();
    rtc::WebSocketServer server(config);
    server.onClient([inbox, identity](Socket peer) {
        bool admitted = false;
        {
            std::lock_guard lock(inbox->mutex);
            if (!inbox->closed && inbox->pending.size() < 8) {
                inbox->pending.push_back({peer, Clock::now() + std::chrono::seconds(5)});
                admitted = true;
            }
        }
        if (!admitted) { CloseSocket(peer); return; }
        peer->onMessage([inbox = std::weak_ptr(inbox), peer = std::weak_ptr(peer), identity](rtc::message_variant message) {
            auto state = inbox.lock();
            auto socket = peer.lock();
            if (!state || !socket || !socket->isOpen()) return;
            auto raw = std::get_if<std::string>(&message);
            if (socket->path() != "/remote" || !raw || !identity->Authorizes(*raw)) {
                ReplyError(socket, "PAIRING_REQUIRED");
                return;
            }
            bool authorized = false;
            {
                std::lock_guard lock(state->mutex);
                auto found = std::find_if(state->pending.begin(), state->pending.end(),
                    [&](auto const& candidate) { return candidate.socket == socket; });
                if (!state->closed && !state->busy && found != state->pending.end() && found->deadline > Clock::now()) {
                    state->next = socket;
                    state->busy = true;
                    state->pending.erase(found);
                    authorized = true;
                }
            }
            if (!authorized) ReplyError(socket, "BUSY");
        });
    });
    std::jthread expiry([inbox](std::stop_token cancel) {
        while (!cancel.stop_requested()) {
            std::vector<Socket> expired;
            {
                std::lock_guard lock(inbox->mutex);
                auto now = Clock::now();
                std::erase_if(inbox->pending, [&](auto const& candidate) {
                    if (!candidate.socket->isClosed() && candidate.deadline > now) return false;
                    expired.push_back(candidate.socket);
                    return true;
                });
            }
            for (auto const& peer : expired) CloseSocket(peer);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    struct Cleanup {
        rtc::WebSocketServer& server;
        std::shared_ptr<RemoteInbox> state;
        ~Cleanup() {
            std::vector<RemoteInbox::Pending> pending;
            Socket next;
            {
                std::lock_guard lock(state->mutex);
                state->closed = true;
                pending.swap(state->pending);
                next = std::move(state->next);
            }
            server.stop();
            for (auto const& candidate : pending) CloseSocket(candidate.socket);
            if (next) CloseSocket(next);
        }
    } cleanup{server, inbox};

    onPairingCode(Wide(code));
    onStatus(L"远程接入已开启 · 等待已配对手机");
    while (!stop) {
        std::wstring waiting = L"远程接入已开启 · 等待已配对手机";
        Socket peer;
        {
            std::lock_guard lock(inbox->mutex);
            peer = std::move(inbox->next);
        }
        if (!peer) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (peer->isOpen()) {
            try {
                StreamWebRtcSocket(peer, {}, ip, settings, stop, controlPermission, onStatus);
            } catch (std::exception const& error) {
                auto detail = Wide(error.what());
                logging::FileLogger::Write(L"WARN", L"REMOTE", L"SESSION_ENDED", detail);
                waiting = L"远程会话结束 · " + detail + L" · 等待重连";
                ReplyError(peer, "SESSION_FAILED");
            }
        }
        CloseSocket(peer);
        {
            std::lock_guard lock(inbox->mutex);
            inbox->busy = false;
        }
        if (!stop) onStatus(waiting);
    }
}
}
