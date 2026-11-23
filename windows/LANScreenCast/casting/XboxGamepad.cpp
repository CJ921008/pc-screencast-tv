#include <windows.h>
#include <ViGEm/Client.h>
#include "RemoteGamepad.h"

#pragma comment(lib, "ViGEmClient.lib")
#pragma comment(lib, "Setupapi.lib")

namespace LANScreenCast::casting {
namespace {
class XboxGamepad final : public GamepadDevice {
public:
    ~XboxGamepad() override { Close(); }
    bool Open() override {
        if (target) return true;
        client = vigem_alloc();
        if (!client || !VIGEM_SUCCESS(vigem_connect(client))) {
            Close();
            return false;
        }
        connected = true;
        target = vigem_target_x360_alloc();
        if (!target || !VIGEM_SUCCESS(vigem_target_add(client, target))) {
            Close();
            return false;
        }
        attached = true;
        return true;
    }

    bool Update(GamepadReport const& report) override {
        return attached && VIGEM_SUCCESS(vigem_target_x360_update(client, target, {
            report.buttons, report.lt, report.rt, report.lx, report.ly, report.rx, report.ry
        }));
    }

    void Close() override {
        if (attached) {
            Update({});
            vigem_target_remove(client, target);
            attached = false;
        }
        if (target) {
            vigem_target_free(target);
            target = nullptr;
        }
        if (client) {
            if (connected) vigem_disconnect(client);
            vigem_free(client);
            client = nullptr;
            connected = false;
        }
    }

private:
    PVIGEM_CLIENT client = nullptr;
    PVIGEM_TARGET target = nullptr;
    bool connected = false, attached = false;
};
}

std::unique_ptr<GamepadDevice> CreateXboxGamepad() { return std::make_unique<XboxGamepad>(); }
}
