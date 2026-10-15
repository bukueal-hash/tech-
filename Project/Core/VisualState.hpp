#pragma once

// State-driven draw policy shared by the ESP pass and the radar pass.
//
// Why this exists: opened-container and dead-Arc styling used to be decided
// independently inside each surface, so the radar silently dropped the
// opened check and drew looted crates exactly like fresh ones. Every call
// site now asks this header the same question and gets the same Decision,
// which is what keeps the two surfaces from drifting apart again.
//
// Pure logic only: no ImGui, no DMA, no config globals. That keeps it in
// Core (unit-testable) and lets a test drive the whole mode matrix without
// a device or a game session.

#include <cstdint>

namespace VisualState {

// How a matched state should be drawn.
enum class Mode : int {
    Normal = 0,  // draw as usual
    Dim    = 1,  // scale alpha by dimAlpha
    Grey   = 2,  // replace RGB with kGreyRgb, keep alpha
    Hide   = 3,  // do not draw at all
};

// Neutral grey used by Mode::Grey. Single source of truth so the ESP label
// colour and the radar blip colour can never disagree about what "grey" is.
inline constexpr unsigned kGreyRgb = 0x8C8C8Cu;  // 140,140,140

struct Decision {
    bool hide = false;       // caller must not draw
    bool grey = false;       // caller replaces RGB with kGreyRgb, keeps alpha
    float alphaMul = 1.f;    // caller multiplies its alpha by this
};

inline float Clamp01(float v)
{
    return v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
}

// Translate a mode into a draw decision. Alpha 0 means invisible, so it is
// reported as Hide rather than making the caller draw something it cannot see.
inline Decision Apply(Mode mode, float dimAlpha)
{
    Decision d;
    switch (mode) {
    case Mode::Hide:
        d.hide = true;
        break;
    case Mode::Grey:
        d.grey = true;
        break;
    case Mode::Dim:
        d.alphaMul = Clamp01(dimAlpha);
        if (d.alphaMul <= 0.f)
            d.hide = true;
        break;
    case Mode::Normal:
    default:
        break;
    }
    return d;
}

// openedState: -1 = not probed yet, 0 = closed, 1 = opened.
// A container we have never read must never be styled — that is the case
// that would otherwise blank out containers on a slow first probe.
inline Decision ForOpened(int8_t openedState, Mode mode, float dimAlpha)
{
    if (openedState <= 0)
        return Decision{};  // closed or unknown -> Normal
    return Apply(mode, dimAlpha);
}

inline Decision ForDead(bool dead, Mode mode, float dimAlpha)
{
    if (!dead)
        return Decision{};
    return Apply(mode, dimAlpha);
}

}  // namespace VisualState
