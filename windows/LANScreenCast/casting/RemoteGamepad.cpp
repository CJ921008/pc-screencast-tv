#include "RemoteGamepad.h"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <utility>

namespace LANScreenCast::casting {
namespace {
using json = nlohmann::json;
constexpr auto Lease = std::chrono::milliseconds(500);
constexpr int64_t ButtonMask = 0xf3ff; // Standard XInput buttons, excluding reserved bits.

int64_t ReadInt(json const& value, int64_t low, int64_t high) {
    if (value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(high))
        throw std::out_of_range("controller value");
    if (!value.is_number_integer()) throw std::out_of_range("controller integer");
    auto number = value.get<int64_t>();
    if (number < low || number > high) throw std::out_of_range("controller value");
    return number;
}
}

RemoteGamepad::RemoteGamepad(std::shared_ptr<std::atomic_bool> allowed, std::unique_ptr<GamepadDevice> backend,
    std::function<void(bool)> suspend)
    : permission(std::move(allowed)), device(std::move(backend)), suspendDesktop(std::move(suspend)) {}

RemoteGamepad::~RemoteGamepad() { Close(); }

RemoteGamepad::Reply RemoteGamepad::StopLocked(std::string_view state) {
    auto stopped = activeEpoch;
    activeEpoch = 0;
    report = {};
    if (opened && !device->Update(report)) {
        device->Close();
        opened = false;
    }
    suspendDesktop(false);
    return {stopped, state};
}

std::optional<RemoteGamepad::Reply> RemoteGamepad::ExpireLocked(Clock::time_point now) {
    if (activeEpoch && (!*permission || now >= deadline))
        return StopLocked(*permission ? "expired" : "not_allowed");
    return {};
}

std::optional<RemoteGamepad::Reply> RemoteGamepad::Expire(Clock::time_point now) {
    std::lock_guard lock(mutex);
    return ExpireLocked(now);
}

std::optional<RemoteGamepad::Reply> RemoteGamepad::Receive(std::string_view raw, bool reliable, Clock::time_point now) {
    if (raw.empty() || raw.size() > 512) return {};
    std::lock_guard lock(mutex);
    if (closed) return {};
    auto expiry = ExpireLocked(now);
    try {
        const auto message = json::parse(raw);
        const auto& type = message.at("type").get_ref<std::string const&>();
        const auto epoch = static_cast<uint32_t>(ReadInt(message.at("epoch"), 1, UINT32_MAX));
        if (reliable && type == "start") {
            if (epoch <= newestEpoch) return expiry;
            newestEpoch = epoch;
            if (activeEpoch) StopLocked("stopped");
            if (!*permission) return Reply{epoch, "not_allowed"};
            auto opening = Clock::now();
            if (!opened) opened = device->Open();
            auto driverWait = Clock::now() - opening;
            if (!opened) return Reply{epoch, "driver_unavailable"};
            suspendDesktop(true);
            activeEpoch = epoch;
            newestSequence = 0;
            deadline = now + driverWait + Lease;
            return Reply{epoch, "active"};
        }
        if (!activeEpoch || activeEpoch != epoch) return expiry;

        if (reliable && type == "stop") return StopLocked("stopped");
        GamepadReport next = report;
        if (reliable && type == "buttons") {
            auto buttons = ReadInt(message.at("buttons"), 0, UINT16_MAX);
            if (buttons & ~ButtonMask) return {};
            next.buttons = static_cast<uint16_t>(buttons);
        } else if (!reliable && type == "axes") {
            auto sequence = static_cast<uint32_t>(ReadInt(message.at("sequence"), 1, UINT32_MAX));
            if (sequence <= newestSequence) return {};
            auto const& axes = message.at("axes");
            if (!axes.is_array() || axes.size() != 6) return {};
            next.lx = static_cast<int16_t>(ReadInt(axes[0], -32768, 32767));
            next.ly = static_cast<int16_t>(ReadInt(axes[1], -32768, 32767));
            next.rx = static_cast<int16_t>(ReadInt(axes[2], -32768, 32767));
            next.ry = static_cast<int16_t>(ReadInt(axes[3], -32768, 32767));
            next.lt = static_cast<uint8_t>(ReadInt(axes[4], 0, 255));
            next.rt = static_cast<uint8_t>(ReadInt(axes[5], 0, 255));
            newestSequence = sequence;
            deadline = now + Lease;
        } else {
            return {};
        }
        if (next != report) {
            if (!device->Update(next)) return StopLocked("driver_unavailable");
            report = next;
        }
    } catch (json::exception const&) {
    } catch (std::out_of_range const&) {
    }
    return expiry;
}

void RemoteGamepad::Close() {
    std::lock_guard lock(mutex);
    if (closed) return;
    closed = true;
    if (activeEpoch) StopLocked("stopped");
    if (opened) device->Close();
    opened = false;
}
}
