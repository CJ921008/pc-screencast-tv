#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace LANScreenCast::casting {
struct GamepadReport {
    uint16_t buttons = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t lt = 0, rt = 0;
    bool operator==(GamepadReport const&) const = default;
};

class GamepadDevice {
public:
    virtual ~GamepadDevice() = default;
    virtual bool Open() = 0;
    virtual bool Update(GamepadReport const&) = 0;
    virtual void Close() = 0;
};

std::unique_ptr<GamepadDevice> CreateXboxGamepad();

class RemoteGamepad {
public:
    using Clock = std::chrono::steady_clock;
    struct Reply { uint32_t epoch; std::string_view state; };
    RemoteGamepad(std::shared_ptr<std::atomic_bool> allowed, std::unique_ptr<GamepadDevice> device,
        std::function<void(bool)> suspendDesktop);
    ~RemoteGamepad();
    std::optional<Reply> Receive(std::string_view message, bool reliable, Clock::time_point now);
    std::optional<Reply> Expire(Clock::time_point now);
    void Close();

private:
    std::optional<Reply> ExpireLocked(Clock::time_point now);
    Reply StopLocked(std::string_view state);
    std::shared_ptr<std::atomic_bool> permission;
    std::unique_ptr<GamepadDevice> device;
    std::function<void(bool)> suspendDesktop;
    std::mutex mutex;
    GamepadReport report;
    Clock::time_point deadline{};
    uint32_t newestEpoch = 0, activeEpoch = 0, newestSequence = 0;
    bool opened = false, closed = false;
};
}
