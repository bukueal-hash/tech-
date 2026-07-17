#include "MyMakcu.h"

#include "makcu_vendor/makcu.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

std::string g_portNormalized;

/** HID-scale safety: one-frame relative move (typical game / device step). */
static int32_t ClampMoveDelta(int v) {
    constexpr int32_t kMaxAbs = 8192;
    if (v > kMaxAbs)
        return kMaxAbs;
    if (v < -kMaxAbs)
        return -kMaxAbs;
    return static_cast<int32_t>(v);
}
makcu::Device g_device;

std::string NormalizeComPort(std::string p) {
    while (!p.empty() && (p.front() == ' ' || p.front() == '\t'))
        p.erase(p.begin());
    while (!p.empty() && (p.back() == ' ' || p.back() == '\t'))
        p.pop_back();
    if (p.empty())
        return {};

    std::string upper = p;
    std::transform(upper.begin(), upper.end(), upper.begin(),
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    if (upper.size() >= 4 && upper.rfind("COM", 0) == 0)
        return upper;

    bool allDigit = !p.empty() && std::all_of(p.begin(), p.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    if (allDigit)
        return "COM" + p;

    return p;
}

} // namespace

std::string MyMakcu::AutoDetectComPort() {
    try {
        return makcu::Device::findFirstDevice();
    } catch (...) {
        return {};
    }
}

void MyMakcu::SetComPort(const std::string& port) {
    g_portNormalized = NormalizeComPort(port);
}

bool MyMakcu::Initialize() {
    g_device.disconnect();
    if (g_portNormalized.empty())
        return false;
    return g_device.connect(g_portNormalized);
}

void MyMakcu::Move(int x, int y) {
    if (!g_device.isConnected())
        return;
    g_device.mouseMove(ClampMoveDelta(x), ClampMoveDelta(y));
}

void MyMakcu::LeftClick() {
    if (!g_device.isConnected())
        return;
    // Manual KmBox settings test only; aimbot has no auto-fire path.
    g_device.mouseDown(makcu::MouseButton::LEFT);
    std::this_thread::sleep_for(std::chrono::milliseconds(22));
    g_device.mouseUp(makcu::MouseButton::LEFT);
}
