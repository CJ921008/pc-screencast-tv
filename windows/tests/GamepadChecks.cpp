#include "../LANScreenCast/casting/RemoteGamepad.h"
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace LANScreenCast::casting;
using namespace std::chrono_literals;

namespace {
void Require(bool condition, char const* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Probe {
    bool available = true, open = false;
    std::vector<GamepadReport> reports;
};

class TestDevice : public GamepadDevice {
    Probe& probe;
public:
    explicit TestDevice(Probe& value) : probe(value) {}
    bool Open() override { return probe.open = probe.available; }
    bool Update(GamepadReport const& state) override {
        if (!probe.open) return false;
        probe.reports.push_back(state);
        return true;
    }
    void Close() override { probe.open = false; }
};
}

int main() {
    try {
        auto permission = std::make_shared<std::atomic_bool>(false);
        Probe probe;
        bool desktopSuspended = false;
        RemoteGamepad pad(permission, std::make_unique<TestDevice>(probe),
            [&](bool suspended) { desktopSuspended = suspended; });
        auto start = RemoteGamepad::Clock::now();
        auto reply = pad.Receive(R"({"type":"start","epoch":1})", true, start);
        Require(reply && reply->state == "not_allowed" && !probe.open, "Unauthorized controller opened");

        *permission = true;
        probe.available = false;
        reply = pad.Receive(R"({"type":"start","epoch":2})", true, start);
        Require(reply && reply->state == "driver_unavailable" && !desktopSuspended,
            "Missing driver interrupted desktop controls");
        probe.available = true;
        reply = pad.Receive(R"({"type":"start","epoch":3})", true, start);
        Require(reply && reply->state == "active" && desktopSuspended, "Game mode did not suspend desktop controls");

        pad.Receive(R"({"type":"axes","epoch":3,"sequence":7,"axes":[-32768,32767,41,-61,255,20]})", false, start + 20ms);
        pad.Receive(R"({"type":"buttons","epoch":3,"buttons":4097})", true, start + 25ms);
        Require(probe.reports.back() == GamepadReport{4097,-32768,32767,41,-61,255,20},
            "Valid buttons, sticks and triggers did not reach virtual controller");
        auto count = probe.reports.size();
        pad.Receive(R"({"type":"axes","epoch":3,"sequence":6,"axes":[0,0,0,0,0,0]})", false, start + 40ms);
        pad.Receive(R"({"type":"axes","epoch":3,"sequence":8,"axes":[0,0,65535,0,256,0]})", false, start + 50ms);
        pad.Receive(R"({"type":"buttons","epoch":3,"buttons":0})", false, start + 60ms);
        Require(probe.reports.size() == count, "Stale or invalid data modified controller state");

        pad.Receive(R"({"type":"buttons","epoch":3,"buttons":0})", true, start + 70ms);
        Require(probe.reports.back().buttons == 0 && probe.reports.back().lt == 255,
            "Reliable release did not preserve axis state");
        Require(!pad.Expire(start + 519ms), "Controller expired early");
        reply = pad.Expire(start + 520ms);
        Require(reply && reply->state == "expired" && probe.reports.back() == GamepadReport{} &&
            !desktopSuspended, "Controller did not go neutral after 500 ms");
        count = probe.reports.size();
        pad.Receive(R"({"type":"buttons","epoch":3,"buttons":4096})", true, start + 550ms);
        pad.Receive(R"({"type":"start","epoch":3})", true, start + 570ms);
        Require(probe.reports.size() == count && !desktopSuspended, "A stopped session accepted a replayed button");

        pad.Receive(R"({"type":"start","epoch":4})", true, start + 600ms);
        pad.Receive(R"({"type":"buttons","epoch":4,"buttons":8192})", true, start + 620ms);
        *permission = false;
        reply = pad.Expire(start + 630ms);
        Require(reply && reply->state == "not_allowed" && probe.reports.back() == GamepadReport{}, "Revoke left a held button");
        pad.Close();
        Require(!probe.open && !desktopSuspended, "Close leaked the virtual controller");
        std::cout << "Gamepad mode, independent axes, reliable buttons, deadline and revocation passed.\n";
        return 0;
    } catch (std::exception const& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
