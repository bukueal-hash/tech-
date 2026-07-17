#include "InputBind.h"

#include "../DMA/DmaKeyboard.h"
#include "DmaGamepad.h"
#include "Controller.h"

#include <Windows.h>

namespace {

struct GpMap {
    int code;
    uint32_t mask;
    const char* label;
};

// Canonical gamepad bind codes (256+) — no overlap with Windows VK codes.
static const GpMap kGpBinds[] = {
    {256, XINPUT_A_BTN, "Pad A"},
    {257, XINPUT_B_BTN, "Pad B"},
    {258, XINPUT_X_BTN, "Pad X"},
    {259, XINPUT_Y_BTN, "Pad Y"},
    {260, XINPUT_LB_BTN, "Pad LB"},
    {261, XINPUT_RB_BTN, "Pad RB"},
    {262, XINPUT_SHARE_BTN, "Pad Share"},
    {263, XINPUT_START_BTN, "Pad Start"},
    {264, XINPUT_L3_BTN, "Pad L3"},
    {265, XINPUT_R3_BTN, "Pad R3"},
    {266, XINPUT_LT_BTN, "Pad LT"},
    {267, XINPUT_RT_BTN, "Pad RT"},
    {268, XINPUT_DPAD_UP_BTN, "Pad D-Up"},
    {269, XINPUT_DPAD_DOWN_BTN, "Pad D-Down"},
    {270, XINPUT_DPAD_RIGHT_BTN, "Pad D-Right"},
    {271, XINPUT_DPAD_LEFT_BTN, "Pad D-Left"},
};

// Legacy saved codes that collide with VK_F4..VK_F16 — still resolve to the same pad mask.
static const struct {
    int legacy;
    uint32_t mask;
} kLegacyGp[] = {
    {115, XINPUT_A_BTN},
    {116, XINPUT_B_BTN},
    {117, XINPUT_X_BTN},
    {118, XINPUT_Y_BTN},
    {119, XINPUT_LB_BTN},
    {120, XINPUT_RB_BTN},
    {121, XINPUT_SHARE_BTN},
    {122, XINPUT_START_BTN},
    {123, XINPUT_L3_BTN},
    {124, XINPUT_R3_BTN},
    {126, XINPUT_LT_BTN},
    {127, XINPUT_RT_BTN},
    {243, XINPUT_DPAD_UP_BTN},
    {245, XINPUT_DPAD_DOWN_BTN},
    {244, XINPUT_DPAD_RIGHT_BTN},
    {246, XINPUT_DPAD_LEFT_BTN},
};

static bool IsKeyboardBindCode(int code)
{
    return code > 0 && code < 256;
}

static bool IsKeyboardOrMouseDown(int code)
{
    if (!IsKeyboardBindCode(code))
        return false;

    if (g_dmaKeyboard.IsReady()) {
        if (g_dmaKeyboard.IsKeyDown(static_cast<uint32_t>(code)))
            return true;
    }
    if ((GetAsyncKeyState(code) & 0x8000) != 0)
        return true;
    return false;
}

static bool IsGamepadMaskDown(uint32_t mask)
{
    if (mask == XINPUT_LT_BTN || mask == XINPUT_RT_BTN) {
        DmaPadRaw raw{};
        if (DmaGamepad::ReadRaw(raw)) {
            if (mask == XINPUT_LT_BTN)
                return raw.lt > 128;
            return raw.rt > 128;
        }
    } else {
        DmaPadRaw raw{};
        if (DmaGamepad::ReadRaw(raw)) {
            return (raw.buttons & static_cast<uint16_t>(mask)) != 0;
        }
    }

    if (!g_controller.IsReady())
        return false;
    return g_controller.IsButtonDown(mask);
}

static uint32_t MaskForBindCode(int code)
{
    for (const auto& e : kGpBinds) {
        if (e.code == code)
            return e.mask;
    }
    for (const auto& e : kLegacyGp) {
        if (e.legacy == code)
            return e.mask;
    }
    return 0u;
}

} // namespace

bool ImGuiTryBindGamepadKey(int* outCode)
{
    if (!outCode)
        return false;

    for (const auto& e : kGpBinds) {
        if (IsGamepadMaskDown(e.mask)) {
            *outCode = e.code;
            return true;
        }
    }

    return false;
}

bool InputBindCodeIsGamepad(int code)
{
    return MaskForBindCode(code) != 0u;
}

bool InputBindIsDown(int code)
{
    if (code == 0)
        return false;

    // Keyboard / mouse VK: always check DMA + local state (fixes legacy VK/gamepad overlap).
    if (IsKeyboardOrMouseDown(code))
        return true;

    const uint32_t mask = MaskForBindCode(code);
    if (mask != 0u && IsGamepadMaskDown(mask))
        return true;

    return false;
}

const char* InputBindCodeLabel(int code)
{
    for (const auto& e : kGpBinds) {
        if (e.code == code)
            return e.label;
    }
    for (const auto& e : kLegacyGp) {
        if (e.legacy != code)
            continue;
        for (const auto& b : kGpBinds) {
            if (b.mask == e.mask)
                return b.label;
        }
    }
    return "?";
}
