#pragma once

#include <windows.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>

namespace LANScreenCast::casting {
class RemoteInput {
public:
    using Clock = std::chrono::steady_clock;
    using Inject = std::function<UINT(std::span<INPUT>)>;

    RemoteInput(RECT desktop, std::shared_ptr<std::atomic_bool> permission, Inject inject = {});
    ~RemoteInput();
    void Receive(std::string_view message);
    void Expire(Clock::time_point now);
    void Close();
    bool InputBlocked() const { return blocked.load(); }

private:
    void ReleaseLocked();
    bool Send(std::span<INPUT> events);
    void SendKeys(std::span<INPUT> events);

    RECT desktop;
    std::shared_ptr<std::atomic_bool> permission;
    Inject inject;
    std::mutex mutex;
    Clock::time_point expires{};
    bool closed = false;
    bool dragging = false;
    std::atomic_bool blocked{false};
};
}
