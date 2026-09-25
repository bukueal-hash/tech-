// BotMotion render-time interpolation suite.
//
// The bug these lock down: paint used to snap to the newest position sample and
// extrapolate from it, so the box teleported once per sample and jumped
// BACKWARD whenever a sample landed late (the "choppy / off target" bot ESP).
// The properties that have to hold: the rendered track is monotonic in the
// direction of travel, and every per-tick step stays inside the allowed motion
// budget (the bot's own speed plus the correction budget), including at a
// sample-arrival boundary where snaping used to produce its largest jump.

#include "tests_main.hpp"
#include "Core/Vector.hpp"
#include "Core/BotMotion.hpp"

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

BotMotion::Sample SampleAt(double x, uint64_t ms)
{
    return BotMotion::Sample{Vector3(x, 0.0, 0.0), ms};
}

std::vector<double> PaintTicks(
    const BotMotion::Sample& prev,
    const BotMotion::Sample& cur,
    const Vector3& velocity,
    uint64_t from,
    uint64_t to)
{
    std::vector<double> out;
    for (uint64_t now = from; now < to; now += 16) {
        const BotMotion::RenderState s =
            BotMotion::ResolveRenderPos(prev, cur, velocity, now);
        REQUIRE(s.valid);
        out.push_back(s.pos.x);
    }
    return out;
}

double MaxStep(const std::vector<double>& xs)
{
    double worst = 0.0;
    for (size_t i = 1; i < xs.size(); ++i)
        worst = (std::max)(worst, std::abs(xs[i] - xs[i - 1]));
    return worst;
}

bool Monotonic(const std::vector<double>& xs)
{
    for (size_t i = 1; i < xs.size(); ++i) {
        if (xs[i] < xs[i - 1])
            return false;
    }
    return true;
}

// The paint loop, reduced to what decides smoothness: resolve the render
// position from the sample pair, then glide toward it with the correction
// limiter carrying the previous rendered anchor.
struct PaintSim {
    BotMotion::Sample prev{};
    BotMotion::Sample cur{};
    Vector3 velocity{};
    Vector3 rendered{};
    uint64_t renderedMs = 0;
    bool hasRendered = false;
    std::vector<double> xs;

    void OnSample(double x, uint64_t ms)
    {
        prev = cur;
        cur = SampleAt(x, ms);
    }

    void Tick(uint64_t now)
    {
        const BotMotion::RenderState s =
            BotMotion::ResolveRenderPos(prev, cur, velocity, now);
        Vector3 target = s.valid ? s.pos : cur.pos;
        const uint64_t dtMs = (hasRendered && now > renderedMs)
            ? now - renderedMs : 0;
        if (hasRendered && dtMs > 0) {
            target = BotMotion::SmoothCorrection(
                rendered, target, dtMs, BotMotion::SpeedOf(velocity));
        }
        rendered = target;
        renderedMs = now;
        hasRendered = true;
        xs.push_back(target.x);
    }
};

} // namespace

TEST_CASE("BotMotion interpolates between dense samples")
{
    // 50ms apart: inside kInterpMaxIntervalMs, so the two samples straddle the
    // paint tick and interpolation is exact.
    const auto prev = SampleAt(0.0, 1000);
    const auto cur = SampleAt(25.0, 1050);
    const Vector3 velocity{500.0, 0.0, 0.0};  // 25cm / 50ms

    // One interval of lag, no extrapolation.
    const auto tick = BotMotion::ResolveRenderPos(prev, cur, velocity, 1075);
    CHECK(tick.intervalMs == 50);
    CHECK(tick.delayMs == 50);
    CHECK(tick.leadMs == 0);
    CHECK(tick.pos.x == doctest::Approx(0.0 + 25.0 * 0.5));

    const auto atCur = BotMotion::ResolveRenderPos(prev, cur, velocity, 1100);
    CHECK(atCur.pos.x == doctest::Approx(25.0));
}

TEST_CASE("BotMotion extrapolates across sparse samples")
{
    // 400ms apart, as measured on the bot path. A full-interval delay would
    // park the box 200cm behind, so this mode extrapolates the whole gap and
    // lands on the body.
    const auto prev = SampleAt(0.0, 1000);
    const auto cur = SampleAt(200.0, 1400);
    const Vector3 velocity{500.0, 0.0, 0.0};  // 200cm / 400ms

    const auto atCur = BotMotion::ResolveRenderPos(prev, cur, velocity, 1600);
    CHECK(atCur.intervalMs == 400);
    CHECK(atCur.delayMs == 0);
    CHECK(atCur.leadMs == 200);
    CHECK(atCur.pos.x == doctest::Approx(300.0));  // truth at t=1600

    // The lead is capped at one interval plus slack, so a stale sample cannot
    // send the box a second into the future.
    const auto late = BotMotion::ResolveRenderPos(prev, cur, velocity, 9000);
    CHECK(late.leadMs == 400 + BotMotion::kLeadSlackMs);
    CHECK(late.pos.x == doctest::Approx(200.0 + 500.0 * 0.500));
}

TEST_CASE("painted track stays smooth across sample arrivals")
{
    // 500cm/s bot, samples every 400ms: the case that produced the jump. With
    // only a 120ms delay the track extrapolates most of the interval, so each
    // arrival corrects a large prediction error.
    PaintSim sim;
    sim.velocity = Vector3(500.0, 0.0, 0.0);
    sim.OnSample(0.0, 1000);
    sim.Tick(1000);
    for (uint64_t now = 1016; now < 2600; now += 16) {
        if ((now - 1000) % 400 == 0)
            sim.OnSample(500.0 * 0.001 * static_cast<double>(now - 1000), now);
        sim.Tick(now);
    }

    CHECK(Monotonic(sim.xs));
    // One tick may move the bot its own 8cm plus the correction budget
    // (800cm/s * 16ms = 12.8cm) — but never the ~90cm teleport the snap
    // produced (assertions below the budget would fail on the old behaviour).
    const double budget = BotMotion::AllowedStepCm(500.0, 16) + 1e-9;
    CHECK(MaxStep(sim.xs) <= budget);
    CHECK(MaxStep(sim.xs) < 21.0);

    // The track keeps up: 500cm/s from t=1000 means the body is at 796cm by the
    // last painted tick, and the extrapolated lead has to land the box there
    // rather than an interval behind it.
    const double trueAtEnd = 500.0 * 0.001 * (2592.0 - 1000.0);
    CHECK(trueAtEnd == doctest::Approx(796.0));
    CHECK(std::abs(sim.xs.back() - trueAtEnd) < 40.0);
}

TEST_CASE("BotMotion stays smooth on a slow sampler")
{
    // 2400ms spacing, the measured RobotList pass interval.
    const Vector3 velocity{100.0, 0.0, 0.0};
    const auto prev = SampleAt(0.0, 10000);
    const auto cur = SampleAt(240.0, 12400);
    const auto ticks = PaintTicks(prev, cur, velocity, 12400, 14800);

    CHECK(Monotonic(ticks));
    // Sparse samples extrapolate, so the motion is carried by the velocity
    // tail — even when the velocity is right, and bounded by the correction
    // budget when it is not.
    CHECK(MaxStep(ticks) <= BotMotion::AllowedStepCm(100.0, 16) + 1e-9);
    CHECK(MaxStep(ticks) < 2.0);
}

TEST_CASE("painted track still advances with no velocity estimate")
{
    // The live probe reported speed:0 for 86% of bot samples, so this is the
    // current worst case: sparse samples AND no velocity to extrapolate with.
    // The box cannot lead the body, but it must still move forward smoothly
    // instead of teleporting when each sample lands.
    PaintSim sim;
    sim.velocity = Vector3(0.0, 0.0, 0.0);
    sim.OnSample(0.0, 1000);
    sim.Tick(1000);
    for (uint64_t now = 1016; now < 2600; now += 16) {
        if ((now - 1000) % 400 == 0)
            sim.OnSample(500.0 * 0.001 * static_cast<double>(now - 1000), now);
        sim.Tick(now);
    }

    CHECK(Monotonic(sim.xs));
    CHECK(MaxStep(sim.xs) <= BotMotion::AllowedStepCm(0.0, 16) + 1e-9);
    // It keeps moving: it is not parked at the first sample.
    CHECK(sim.xs.back() > sim.xs.front() + 100.0);
}

TEST_CASE("BotMotion holds before the older sample and survives one sample")
{
    // Dense pair, clock behind both samples: hold the older one, never invent
    // motion from a velocity we do not have yet.
    const auto held = BotMotion::ResolveRenderPos(
        SampleAt(10.0, 1000), SampleAt(12.0, 1050),
        Vector3(0.0, 0.0, 0.0), 900);
    CHECK(held.pos.x == doctest::Approx(10.0));
    CHECK(held.leadMs == 0);

    // Only one sample (prevSampleMs == 0): fall back to the newest sample and,
    // once stale, a bounded velocity lead.
    const auto one = BotMotion::ResolveRenderPos(
        BotMotion::Sample{}, SampleAt(70.0, 1000),
        Vector3(500.0, 0.0, 0.0), 1060);
    CHECK(one.valid);
    CHECK(one.intervalMs == 0);
    CHECK(one.leadMs == 60);
    CHECK(one.pos.x == doctest::Approx(70.0 + 500.0 * 0.060));

    // No sample at all: invalid, caller keeps its own fallback.
    const auto none = BotMotion::ResolveRenderPos(
        BotMotion::Sample{}, BotMotion::Sample{}, Vector3{}, 1000);
    CHECK_FALSE(none.valid);
}

TEST_CASE("SmoothCorrection bounds a correction but passes normal motion")
{
    // A step inside the budget is taken as-is (normal motion is not damped).
    const Vector3 prev(0.0, 0.0, 0.0);
    CHECK(BotMotion::SmoothCorrection(prev, Vector3(8.0, 0.0, 0.0), 16, 500.0)
        .x == doctest::Approx(8.0));

    // A jump-sized correction is limited to one budget step.
    const Vector3 glided =
        BotMotion::SmoothCorrection(prev, Vector3(90.0, 0.0, 0.0), 16, 500.0);
    CHECK(glided.x == doctest::Approx(BotMotion::AllowedStepCm(500.0, 16)));
    CHECK(glided.x < 90.0);

    // Zero elapsed time cannot advance the track (no divide-by-zero blowup).
    CHECK(BotMotion::SmoothCorrection(prev, Vector3(90.0, 0.0, 0.0), 0, 500.0)
        .x == doctest::Approx(0.0));

    // Repeated glosses converge on the target.
    Vector3 at = prev;
    for (int i = 0; i < 20; ++i)
        at = BotMotion::SmoothCorrection(at, Vector3(90.0, 0.0, 0.0), 16, 500.0);
    CHECK(at.x == doctest::Approx(90.0));
}
