#define NOMINMAX
#include "RemoteInput.h"
#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>
#include <vector>
#pragma comment(lib, "User32.lib")

namespace LANScreenCast::casting {
namespace {
using json = nlohmann::json;
constexpr auto lease = std::chrono::milliseconds(1500);

INPUT Mouse(DWORD flags, LONG x = 0, LONG y = 0, DWORD data = 0) {
    INPUT event{};
    event.type = INPUT_MOUSE;
    event.mi.dwFlags = flags;
    event.mi.dx = x;
    event.mi.dy = y;
    event.mi.mouseData = data;
    return event;
}

INPUT Key(WORD code, DWORD flags = 0, WORD unicode = 0) {
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wVk = code;
    event.ki.wScan = unicode;
    event.ki.dwFlags = flags;
    return event;
}

int BoundedInt(json const& message, char const* field, int min, int max) {
    auto const& value = message.at(field);
    if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<uint64_t>() > uint64_t(max)))
        throw std::out_of_range(field);
    auto number = value.get<int64_t>();
    if (number < min || number > max) throw std::out_of_range(field);
    return static_cast<int>(number);
}

bool SupportedKey(int code) {
    return (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') ||
        (code >= VK_F1 && code <= VK_F12) || (code >= VK_PRIOR && code <= VK_DOWN) ||
        code == VK_BACK || code == VK_TAB || code == VK_RETURN || code == VK_ESCAPE ||
        code == VK_SPACE || code == VK_INSERT || code == VK_DELETE || code == VK_LWIN;
}

bool ExtendedKey(int code) {
    return (code >= VK_PRIOR && code <= VK_DOWN) || code == VK_INSERT ||
        code == VK_DELETE || code == VK_LWIN;
}

LONG AbsoluteCoordinate(LONG point, int origin, int extent) {
    // Use the center of the target pixel within the virtual desktop's 16-bit range.
    return static_cast<LONG>(std::clamp(((2LL * (point - origin) + 1) * 32768) / extent, 0LL, 65535LL));
}

INPUT MoveTo(POINT point) {
    int width = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN));
    int height = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    return Mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK,
        AbsoluteCoordinate(point.x, GetSystemMetrics(SM_XVIRTUALSCREEN), width),
        AbsoluteCoordinate(point.y, GetSystemMetrics(SM_YVIRTUALSCREEN), height));
}

INPUT ReadPoint(json const& message, RECT desktop) {
    POINT point{
        desktop.left + static_cast<LONG>(
            static_cast<int64_t>(desktop.right - desktop.left - 1) * BoundedInt(message, "x", 0, 10000) / 10000),
        desktop.top + static_cast<LONG>(
            static_cast<int64_t>(desktop.bottom - desktop.top - 1) * BoundedInt(message, "y", 0, 10000) / 10000),
    };
    return MoveTo(point);
}
}

RemoteInput::RemoteInput(RECT bounds, std::shared_ptr<std::atomic_bool> allowed, Inject send)
    : desktop(bounds), permission(std::move(allowed)), inject(std::move(send)) {
    if (!inject) inject = [](std::span<INPUT> events) {
        return SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT));
    };
}

RemoteInput::~RemoteInput() { Close(); }

bool RemoteInput::Send(std::span<INPUT> events) {
    bool ok = inject(events) == events.size();
    blocked = !ok;
    return ok;
}

void RemoteInput::SendKeys(std::span<INPUT> events) {
    auto count = std::min<size_t>(inject(events), events.size());
    blocked = count != events.size();
    if (!blocked) return;
    // A partial SendInput may leave a chord pressed. Release every inserted key in reverse order.
    for (size_t index = count; index > 0; --index) {
        auto key = events[index - 1];
        if (key.ki.dwFlags & KEYEVENTF_KEYUP) continue;
        key.ki.dwFlags |= KEYEVENTF_KEYUP;
        inject(std::span(&key, 1));
    }
}

void RemoteInput::ReleaseLocked() {
    if (dragging) {
        auto release = Mouse(MOUSEEVENTF_LEFTUP);
        if (Send(std::span(&release, 1))) dragging = false;
    }
}

void RemoteInput::Expire(Clock::time_point now) {
    std::lock_guard lock(mutex);
    if (closed || !*permission || now >= expires) ReleaseLocked();
}

void RemoteInput::Close() {
    std::lock_guard lock(mutex);
    closed = true;
    ReleaseLocked();
}

void RemoteInput::Receive(std::string_view raw) {
    if (raw.empty() || raw.size() > 4096 || !*permission) return;
    std::lock_guard lock(mutex);
    if (closed || !*permission) return;

    try {
        auto message = json::parse(raw);
        auto const& type = message.at("type").get_ref<std::string const&>();
        auto now = Clock::now();
        if (type == "resume") {
            if (now >= expires) ReleaseLocked();
            expires = now + lease;
            return;
        }
        if (type == "pause") {
            expires = {};
            ReleaseLocked();
            return;
        }
        if (now >= expires) {
            ReleaseLocked();
            return;
        }

        if (type == "move") {
            POINT point{};
            if (!GetPhysicalCursorPos(&point)) return;
            point.x = std::clamp(point.x + BoundedInt(message, "x", -1000, 1000), desktop.left, desktop.right - 1);
            point.y = std::clamp(point.y + BoundedInt(message, "y", -1000, 1000), desktop.top, desktop.bottom - 1);
            auto event = MoveTo(point);
            Send(std::span(&event, 1));
        } else if (type == "point") {
            auto event = ReadPoint(message, desktop);
            Send(std::span(&event, 1));
        } else if (type == "drag") {
            if (message.at("down").get<bool>()) {
                if (!dragging && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) {
                    std::array<INPUT, 2> events{};
                    size_t count = 0;
                    if (message.contains("x") || message.contains("y")) events[count++] = ReadPoint(message, desktop);
                    events[count++] = Mouse(MOUSEEVENTF_LEFTDOWN);
                    dragging = Send(std::span(events).first(count));
                }
            } else {
                ReleaseLocked();
            }
        } else if (type == "click") {
            auto const& button = message.at("button").get_ref<std::string const&>();
            if (button != "left" && button != "right") return;
            bool right = button == "right";
            if (dragging || (GetAsyncKeyState(right ? VK_RBUTTON : VK_LBUTTON) & 0x8000)) return;
            std::array<INPUT, 3> events{};
            size_t count = 0;
            if (message.contains("x") || message.contains("y")) events[count++] = ReadPoint(message, desktop);
            events[count++] = Mouse(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
            events[count++] = Mouse(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
            auto sent = inject(std::span(events).first(count));
            blocked = sent != count;
            if (sent == count - 1) inject(std::span(&events[count - 1], 1));
        } else if (type == "wheel") {
            auto amount = BoundedInt(message, "delta", -960, 960);
            auto event = Mouse(MOUSEEVENTF_WHEEL, 0, 0, static_cast<DWORD>(amount));
            Send(std::span(&event, 1));
        } else if (type == "key") {
            int code = BoundedInt(message, "code", 1, 254);
            int modifiers = BoundedInt(message, "modifiers", 0, 15);
            if (!SupportedKey(code)) return;
            constexpr std::array<WORD, 4> keys{VK_CONTROL, VK_SHIFT, VK_MENU, VK_LWIN};
            std::vector<INPUT> events;
            for (int bit = 0; bit < 4; ++bit) {
                if ((modifiers & (1 << bit)) && keys[bit] != code && !(GetAsyncKeyState(keys[bit]) & 0x8000))
                    events.push_back(Key(keys[bit], ExtendedKey(keys[bit]) ? KEYEVENTF_EXTENDEDKEY : 0));
            }
            DWORD flags = ExtendedKey(code) ? KEYEVENTF_EXTENDEDKEY : 0;
            events.push_back(Key(static_cast<WORD>(code), flags));
            events.push_back(Key(static_cast<WORD>(code), flags | KEYEVENTF_KEYUP));
            auto modifierCount = events.size() - 2;
            while (modifierCount > 0) {
                auto up = events[--modifierCount];
                up.ki.dwFlags |= KEYEVENTF_KEYUP;
                events.push_back(up);
            }
            SendKeys(events);
        } else if (type == "text") {
            auto const& utf8 = message.at("text").get_ref<std::string const&>();
            int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                static_cast<int>(utf8.size()), nullptr, 0);
            if (size <= 0 || size > 256) return;
            std::wstring text(size, L'\0');
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                static_cast<int>(utf8.size()), text.data(), size)) return;
            std::vector<INPUT> events;
            events.reserve(text.size() * 2);
            for (wchar_t character : text) {
                if (character == L'\r') continue;
                if (character == L'\n' || character == L'\t') {
                    WORD code = character == L'\n' ? VK_RETURN : VK_TAB;
                    events.push_back(Key(code));
                    events.push_back(Key(code, KEYEVENTF_KEYUP));
                } else if (character >= L' ') {
                    events.push_back(Key(0, KEYEVENTF_UNICODE, character));
                    events.push_back(Key(0, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, character));
                }
            }
            if (!events.empty()) SendKeys(events);
        }
    } catch (json::exception const&) {
        // Malformed control messages do not affect the video session.
    } catch (std::out_of_range const&) {}
}
}
