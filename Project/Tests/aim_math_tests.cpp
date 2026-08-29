// Aim-smoothing / control-loop math suite — Pillar 1 (docs/aplus-plan.md)
// Exercises the REAL code from Core/AimMath.hpp, which Aimbot.cpp
// SendKmAimDelta calls. Grounded in the aim-shake analysis
// (tools/analyze_aim_shake.py): closeFrac curve, EMA, oscillation damping,
// burst cap, and the adaptive px-per-mouse learner.

#include "tests_main.hpp"
#include "Core/AimMath.hpp"

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

#include <cmath>

namespace {

bool approx(float a, float b, float eps = 1e-5f)
{
    return std::abs(a - b) <= eps;
}

} // namespace

TEST_CASE("CloseFractionForDist piecewise curve")
{
    // Boundaries are strict >.
    CHECK(approx(AimMath::CloseFractionForDist(0.f), 0.38f));
    CHECK(approx(AimMath::CloseFractionForDist(12.f), 0.38f));
    CHECK(approx(AimMath::CloseFractionForDist(12.01f), 0.48f));
    CHECK(approx(AimMath::CloseFractionForDist(28.f), 0.48f));
    CHECK(approx(AimMath::CloseFractionForDist(28.01f), 0.58f));
    CHECK(approx(AimMath::CloseFractionForDist(55.f), 0.58f));
    CHECK(approx(AimMath::CloseFractionForDist(55.01f), 0.68f));
    CHECK(approx(AimMath::CloseFractionForDist(100.f), 0.68f));
    CHECK(approx(AimMath::CloseFractionForDist(100.01f), 0.78f));
    CHECK(approx(AimMath::CloseFractionForDist(1000.f), 0.78f));
}

TEST_CASE("AcceleratedCloseFrac caps at 0.92")
{
    // norm = dist/100 clamped to 1; closeFrac + 0.12*norm^2, cap 0.92.
    CHECK(approx(AimMath::AcceleratedCloseFrac(0.38f, 0.f), 0.38f));
    CHECK(approx(AimMath::AcceleratedCloseFrac(0.38f, 50.f), 0.38f + 0.12f * 0.25f));
    CHECK(approx(AimMath::AcceleratedCloseFrac(0.78f, 100.f), 0.78f + 0.12f));
    // Cap: 0.78 + 0.12 = 0.90 < 0.92, but a base near the cap hits it.
    CHECK(approx(AimMath::AcceleratedCloseFrac(0.90f, 100.f), 0.92f));
    CHECK(approx(AimMath::AcceleratedCloseFrac(0.78f, 1000.f), 0.90f));
}

TEST_CASE("ApplyPullScale never drops below 0.90 factor")
{
    // closeFrac * max(pullScale, 0.90)
    CHECK(approx(AimMath::ApplyPullScale(0.5f, 0.0f), 0.5f * 0.90f));
    CHECK(approx(AimMath::ApplyPullScale(0.5f, 0.90f), 0.5f * 0.90f));
    CHECK(approx(AimMath::ApplyPullScale(0.5f, 1.0f), 0.5f));
}

TEST_CASE("PullProfileRamp clamps to [0.55, 1.6]")
{
    // ramp = clamp(1 + accel*(2t-1), 0.55, 1.6)
    CHECK(approx(AimMath::PullProfileRamp(0.f, 0.f), 1.f));
    CHECK(approx(AimMath::PullProfileRamp(0.f, 0.5f), 1.f));
    CHECK(approx(AimMath::PullProfileRamp(1.f, 1.f), 1.6f));   // fast-start cap
    CHECK(approx(AimMath::PullProfileRamp(1.f, 0.f), 0.55f));  // slow-start floor
    CHECK(approx(AimMath::PullProfileRamp(-1.f, 1.f), 0.55f)); // ease-off floor
    CHECK(approx(AimMath::PullProfileRamp(-1.f, 0.f), 1.6f));  // hurry-at-end cap
}

TEST_CASE("ApplyPullProfile clamps to [0.05, 0.97]")
{
    CHECK(approx(AimMath::ApplyPullProfile(0.5f, 1.f, 1.f, 1.f, 1.f), 0.5f));
    // Speed 1.5 * ramp 1.6 * sharp 1.3 * situ 1.6 on 0.5 → clamped to 0.97.
    CHECK(approx(AimMath::ApplyPullProfile(0.5f, 1.5f, 1.6f, 1.3f, 1.6f), 0.97f));
    // Tiny inputs → clamped up to 0.05.
    CHECK(approx(AimMath::ApplyPullProfile(0.01f, 0.5f, 0.55f, 0.5f, 0.5f), 0.05f));
}

TEST_CASE("OscDampPullFactor")
{
    CHECK(approx(AimMath::OscDampPullFactor(0.f, AimMath::kOscPullFactor), 1.f));
    CHECK(approx(AimMath::OscDampPullFactor(0.70f, AimMath::kOscPullFactor), 1.f - 0.70f * 0.5f));
}

TEST_CASE("EmaStep converges and respects alpha")
{
    // alpha=1 → instant; alpha=0 → unchanged.
    CHECK(approx(AimMath::EmaStep(0.f, 1.f, 1.f), 1.f));
    CHECK(approx(AimMath::EmaStep(0.5f, 1.f, 0.f), 0.5f));

    // Repeated steps converge toward the sample (0.48 alpha).
    float e = 0.f;
    for (int i = 0; i < 100; ++i)
        e = AimMath::EmaStep(e, 1.f, 0.48f);
    CHECK(approx(e, 1.f, 1e-3f));
}

TEST_CASE("TickScaleFromTickMs clamps to [1, 10]")
{
    CHECK(approx(AimMath::TickScaleFromTickMs(8.f), 1.f));
    CHECK(approx(AimMath::TickScaleFromTickMs(0.f), 1.f));
    CHECK(approx(AimMath::TickScaleFromTickMs(80.f), 10.f));
    CHECK(approx(AimMath::TickScaleFromTickMs(16.f), 2.f));
}

TEST_CASE("MaxMouseForDist")
{
    CHECK(approx(AimMath::MaxMouseForDist(35.f, 1.f), 95.f));
    CHECK(approx(AimMath::MaxMouseForDist(40.f, 1.f), 95.f));
    CHECK(approx(AimMath::MaxMouseForDist(40.01f, 1.f), 140.f));
    CHECK(approx(AimMath::MaxMouseForDist(100.f, 10.f), 1400.f));
}

TEST_CASE("ClampMouseBurst scales down to cap")
{
    // Below cap: unchanged.
    auto [a, b] = AimMath::ClampMouseBurst(30.f, 40.f, 95.f);
    CHECK(approx(a, 30.f));
    CHECK(approx(b, 40.f));

    // Above cap: magnitude scaled to exactly the cap.
    auto [c, d] = AimMath::ClampMouseBurst(100.f, 0.f, 50.f);
    CHECK(approx(c, 50.f));
    CHECK(approx(d, 0.f));

    // Zero vector stays zero.
    auto [e, f] = AimMath::ClampMouseBurst(0.f, 0.f, 1.f);
    CHECK(approx(e, 0.f));
    CHECK(approx(f, 0.f));
}

TEST_CASE("OscDampState window logic")
{
    AimMath::OscDampState st;

    // 3 flips inside one 500ms window → damp ramps by one step.
    st.tick(1000, true);
    st.tick(1100, true);
    st.tick(1200, true);
    CHECK(st.flips == 3);
    CHECK(approx(st.damp, 0.f));

    // Window expires at 1500+500 → ≥3 flips → +0.15.
    st.tick(1500, false);
    CHECK(approx(st.damp, 0.15f));
    CHECK(st.flips == 0);

    // A clean window (0 flips) decays by recover step.
    st.tick(2000, false);
    CHECK(approx(st.damp, 0.10f));

    // Repeated ringing windows climb to the cap.
    for (int i = 0; i < 10; ++i) {
        st.tick(2500 + i * 600, true);
        st.tick(2600 + i * 600, true);
        st.tick(2700 + i * 600, true);
        st.tick(2800 + i * 600, false);  // expire window
    }
    CHECK(approx(st.damp, AimMath::kOscDampMax));
}

TEST_CASE("EmaAlphaWithDamp")
{
    // No damp → base alpha; full damp → base * (1 - 0.35).
    CHECK(approx(AimMath::EmaAlphaWithDamp(0.48f, 0.f), 0.48f));
    CHECK(approx(AimMath::EmaAlphaWithDamp(0.48f, 0.70f), 0.48f * (1.f - 0.70f * 0.35f)));
}

TEST_CASE("px-per-mouse learning")
{
    // Clamp sample to [0.50, 1.25].
    CHECK(approx(AimMath::ClampPxPerMouseSample(0.1f), 0.50f));
    CHECK(approx(AimMath::ClampPxPerMouseSample(5.f), 1.25f));
    CHECK(approx(AimMath::ClampPxPerMouseSample(0.8f), 0.8f));

    // Slow EMA: 0.92/0.08 blend.
    CHECK(approx(AimMath::LearnPxPerMouse(0.70f, 1.0f), 0.70f * 0.92f + 1.0f * 0.08f));

    // Overshoot raise: clamp(current * 1.05, max 1.25).
    CHECK(approx(AimMath::OvershootRaisePxPerMouse(0.70f), 0.735f));
    CHECK(approx(AimMath::OvershootRaisePxPerMouse(1.24f), 1.25f));
}

TEST_CASE("Constants match the shake-analysis invariants")
{
    // The floor/ceil that ended the limit-cycle (see Aimbot.cpp comment).
    CHECK(AimMath::kMousePxPerCountMin == 0.50f);
    CHECK(AimMath::kMousePxPerCountMax == 1.25f);
    CHECK(AimMath::kOscWindowMs == 500);
    CHECK(AimMath::kOscFlipsPerWindow == 3);
}