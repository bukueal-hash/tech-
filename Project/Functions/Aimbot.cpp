// Aimbot.cpp — baseline cache + ESP-matched screen aim, KmBox hardware output only.
#include "../Core/Engine.h"
#include "../Core/AimMath.hpp"
#include "../Core/AgentLog.h"
#include "../Core/AssetNames.h"
#include "../Functions/EspDraw.h"
#include "../Functions/RobotList.h"
#include "../Hardware/KmBox.h"
#include "../Input/KeyBind.h"
#include "../Interface/Utils/Visuals/visuals.hpp"
#include "../Core/IntervalTimer.h"
#include "../Interface/Utils/Variables/index.h"
#include "../ThirdParty/ImGui/imgui.h"
#include <iostream>
#include <fstream>
#include <random>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numbers>
#include <cfloat>

// ============================================
// RANDOM BONE SYSTEM (SEQUENCIAL)
// ============================================

// Hold-drift zone: while ON a target we only ever drift the crosshair between
// HIGH-VALUE bones on the body (head/neck/chest/spine). The old random-bone
// walked a fixed 2s Pelvis→Head ladder — predictable AND it dragged the aim to
// the worst-body thirds. This picker instead re-rolls to a random high-value
// bone on JITTERED timing, weighted toward head/chest, and NEVER offers
// Pelvis/limbs, so the crosshair always stays "on the target" in a way that
// looks like a human re-aiming, not a metronome.
static const UniBone g_HoldBones[] = {
    UniBone::Head,
    UniBone::Neck,
    UniBone::Spine3,
    UniBone::Chest,
    UniBone::Spine2
};
static constexpr int g_HoldBonesCount = sizeof(g_HoldBones) / sizeof(g_HoldBones[0]);
static constexpr float g_HoldWeights[] = { 0.40f, 0.20f, 0.13f, 0.20f, 0.07f }; // head-leaning

struct HoldDriftState {
    uint64_t targetKey = 0;
    int currentIndex = 0;
    float nextChangeTime = 0.f;
};
static HoldDriftState g_holdState;
static std::mt19937_64 g_holdRng{ std::random_device{}() };

static float HoldDriftDelay()
{
    // Jittered, NON-fixed cadence: 0.4-2.2s, leaner on the short side so the
    // drift stays alive but never on a beat a watcher could count.
    std::uniform_real_distribution<float> u(0.4f, 2.2f);
    return u(g_holdRng);
}

static UniBone GetHoldDriftBone(uint64_t targetKey, float currentTime, const BoneData& bones)
{
    // New target → pick a weighted starting bone (usually head) and arm first
    // drift for any jittered moment.
    if (g_holdState.targetKey != targetKey)
    {
        g_holdState.targetKey = targetKey;
        // Weighted initial pick.
        std::uniform_real_distribution<float> u(0.f, 1.f);
        float r = u(g_holdRng), acc = 0.f;
        int startIdx = 0;
        for (int i = 0; i < g_HoldBonesCount; ++i) {
            acc += g_HoldWeights[i];
            if (r <= acc) { startIdx = i; break; }
        }
        g_holdState.currentIndex = startIdx;
        g_holdState.nextChangeTime = currentTime + HoldDriftDelay();
    }

    if (currentTime >= g_holdState.nextChangeTime)
    {
        // Re-roll to a random weighted bone, avoiding the current one so it
        // actually drifts rather than sitting. Rarely it may stay put (human).
        std::uniform_real_distribution<float> u(0.f, 1.f);
        float r = u(g_holdRng), acc = 0.f;
        int idx = g_holdState.currentIndex;
        for (int i = 0; i < g_HoldBonesCount; ++i) {
            acc += g_HoldWeights[i];
            if (r <= acc) { idx = i; break; }
        }
        // Small chance to stay on the same bone (feels like holding aim), else
        // pick a different one (avoids self-repeats dominating the sequence).
        std::uniform_real_distribution<float> stay(0.f, 1.f);
        if (stay(g_holdRng) >= 0.25f && idx == g_holdState.currentIndex)
            idx = (g_holdState.currentIndex + 1) % g_HoldBonesCount;
        g_holdState.currentIndex = idx;
        g_holdState.nextChangeTime = currentTime + HoldDriftDelay();
    }

    const UniBone targetBone = g_HoldBones[g_holdState.currentIndex];

    // Nearest valid fallback so the drift never aims at a missing bone.
    if (!bones.valid.test(static_cast<size_t>(targetBone)))
    {
        for (int i = 0; i < g_HoldBonesCount; ++i)
        {
            const int checkIdx = (g_holdState.currentIndex + i) % g_HoldBonesCount;
            if (bones.valid.test(static_cast<size_t>(g_HoldBones[checkIdx])))
                return g_HoldBones[checkIdx];
        }
        return UniBone::Head;
    }

    return targetBone;
}

static bool TryBoneWorld(const BoneData& bones, UniBone bone, Vector3& outWorld)
{
    const size_t idx = static_cast<size_t>(bone);
    if (!bones.valid.test(idx))
        return false;
    outWorld = bones.bonesWorldDouble[idx];
    return true;
}

static bool ResolveAimBoneWorld(
    Engine& eng,
    const BoneData& bones,
    uint64_t key,
    float currentTime,
    const Vector3& screenCenter,
    float fovRadius,
    const Engine::CameraCache& cam,
    Vector3& outWorld)
{
    auto fallbackTorso = [&]() -> bool {
        if (TryBoneWorld(bones, UniBone::Chest, outWorld))
            return true;
        if (TryBoneWorld(bones, UniBone::Spine3, outWorld))
            return true;
        if (TryBoneWorld(bones, UniBone::Pelvis, outWorld))
            return true;
        if (TryBoneWorld(bones, UniBone::Neck, outWorld))
            return true;
        if (TryBoneWorld(bones, UniBone::Head, outWorld))
            return true;
        return false;
    };

    if (var::randombone) {
        // Hold-drift: re-aims to a random HIGH-VALUE bone on jittered timing,
        // weighted to head/chest, so the crosshair roams the target's body in a
        // human way and NEVER offers limbs/pelvis (never visibly "comes off").
        const UniBone bone = GetHoldDriftBone(key, currentTime, bones);
        Vector3 targetWorld{};
        if (!TryBoneWorld(bones, bone, targetWorld))
            return fallbackTorso();

        // Glide instead of teleport: snapping would YANK the crosshair one
        // bone spacing (15-80px) instantly — read as a robotic "jump". Ease
        // toward the newly selected bone (first-order lag, ~0.35s settle), so
        // the drift reads as a smooth human re-aim that stays on the body.
        static uint64_t s_rbKey = 0;
        static Vector3 s_rbPoint{};
        static float s_rbLastT = 0.f;
        if (s_rbKey != key) {
            s_rbKey = key;
            s_rbPoint = targetWorld;
            s_rbLastT = currentTime;
        }
        const float dt = (currentTime > s_rbLastT) ? (currentTime - s_rbLastT) : 0.f;
        s_rbLastT = currentTime;
        constexpr float kRbBlendSec = 0.35f;
        const float a = (dt > 0.f) ? (dt / kRbBlendSec) : 1.f;
        if (a >= 1.f) {
            s_rbPoint = targetWorld;
        } else {
            s_rbPoint.x += (targetWorld.x - s_rbPoint.x) * a;
            s_rbPoint.y += (targetWorld.y - s_rbPoint.y) * a;
            s_rbPoint.z += (targetWorld.z - s_rbPoint.z) * a;
        }
        outWorld = s_rbPoint;
        return true;
    }

    switch (var::aim_bone_mode) {
    case AimBoneMode::Head:
        if (TryBoneWorld(bones, UniBone::Head, outWorld))
            return true;
        break;
    case AimBoneMode::Chest:
        if (TryBoneWorld(bones, UniBone::Chest, outWorld))
            return true;
        break;
    case AimBoneMode::Pelvis:
        if (TryBoneWorld(bones, UniBone::Pelvis, outWorld))
            return true;
        break;
    case AimBoneMode::Arms:
        if (TryBoneWorld(bones, UniBone::UpperArmL, outWorld))
            return true;
        if (TryBoneWorld(bones, UniBone::UpperArmR, outWorld))
            return true;
        break;
    case AimBoneMode::Legs:
        if (TryBoneWorld(bones, UniBone::ThighL, outWorld))
            return true;
        if (TryBoneWorld(bones, UniBone::ThighR, outWorld))
            return true;
        break;
    case AimBoneMode::ClosestBone: {
        static const UniBone kClosestBones[] = {
            UniBone::Head, UniBone::Neck, UniBone::Chest, UniBone::Spine3,
            UniBone::Spine2, UniBone::Spine1, UniBone::Pelvis,
            UniBone::UpperArmL, UniBone::UpperArmR,
            UniBone::ThighL, UniBone::ThighR,
        };
        float bestDist = FLT_MAX;
        bool found = false;
        Vector3 bestWorld{};
        UniBone bestBone = UniBone::Chest;
        for (UniBone bone : kClosestBones) {
            Vector3 world{};
            if (!TryBoneWorld(bones, bone, world))
                continue;
            Vector3 screen{};
            if (!eng.ProjectWorldLocationToScreen(world, screen, cam))
                continue;
            const float dx = static_cast<float>(screen.x - screenCenter.x);
            const float dy = static_cast<float>(screen.y - screenCenter.y);
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > fovRadius)
                continue;
            if (dist < bestDist) {
                bestDist = dist;
                bestWorld = world;
                bestBone = bone;
                found = true;
            }
        }

        // Hysteresis: as the crosshair closes on bone A, bone B becomes closer
        // and the pick flip-flops — the aim point dithers between two bones
        // forever. Keep the current bone unless the challenger is 20% closer.
        static uint64_t s_cbKey = 0;
        static UniBone s_cbBone = UniBone::Chest;
        if (s_cbKey != key) {
            s_cbKey = key;
            s_cbBone = UniBone::Chest;
        }
        Vector3 stickyWorld{};
        if (TryBoneWorld(bones, s_cbBone, stickyWorld)) {
            Vector3 stickyScreen{};
            if (eng.ProjectWorldLocationToScreen(stickyWorld, stickyScreen, cam)) {
                const float sdx = static_cast<float>(stickyScreen.x - screenCenter.x);
                const float sdy = static_cast<float>(stickyScreen.y - screenCenter.y);
                const float stickyDist = std::sqrt(sdx * sdx + sdy * sdy);
                if (stickyDist <= fovRadius && bestDist >= stickyDist * 0.8f) {
                    outWorld = stickyWorld;
                    return true;
                }
            }
        }
        if (found) {
            s_cbBone = bestBone;
            outWorld = bestWorld;
            return true;
        }
        break;
    }
    }

    return fallbackTorso();
}

/** Soft fasten onto torso — no head acquisition snap (ban-risk / flips off body). */
static uint64_t g_playerAimLockedKey = 0;
static bool g_playerAimClosePhase = false;
/** 1 = locked-only candidate refresh this tick (cuts tickMs). */
static int g_aimFastPath = 0;

static float NormalizeYawDelta(float degrees)
{
    while (degrees > 180.f)
        degrees -= 360.f;
    while (degrees < -180.f)
        degrees += 360.f;
    return degrees;
}

static float CameraRotDeltaDeg(const Vector3& a, const Vector3& b)
{
    const float dp = std::abs(static_cast<float>(a.x - b.x));
    const float dy = std::abs(NormalizeYawDelta(static_cast<float>(a.y - b.y)));
    return std::sqrt(dp * dp + dy * dy);
}

static bool ResolvePlayerAimBoneWorldTwoPhase(
    Engine& eng,
    const BoneData& bones,
    uint64_t key,
    float currentTime,
    const Vector3& screenCenter,
    float fovRadius,
    const Engine::CameraCache& cam,
    Vector3& outWorld)
{
    (void)g_playerAimLockedKey;
    g_playerAimClosePhase = false;
    return ResolveAimBoneWorld(
        eng, bones, key, currentTime, screenCenter, fovRadius, cam, outWorld);
}

static float ScoreAimTarget(
    float distToCenter,
    float fovRadius,
    float worldDistanceM,
    float health,
    float maxHealth,
    int weaponQuality,
    bool isRobot)
{
    const float fovScore = fovRadius - distToCenter;
    const float distScore = (worldDistanceM > 0.1f)
        ? (1000.f / worldDistanceM)
        : 1000.f;
    float hpPct = 100.f;
    if (maxHealth > 1.f)
        hpPct = std::clamp((health / maxHealth) * 100.f, 0.f, 100.f);
    else if (health > 0.f)
        hpPct = std::clamp(health, 0.f, 100.f);
    const float lowHpScore = 100.f - hpPct;

    switch (var::aimbot_priority) {
    case AimbotPriority::Fov:
        return fovScore;
    case AimbotPriority::Distance:
        return distScore;
    case AimbotPriority::Threat: {
        float threat = fovScore * 0.5f + distScore * 0.5f;
        if (!isRobot && weaponQuality > 0)
            threat += static_cast<float>(weaponQuality) * 40.f;
        return threat;
    }
    case AimbotPriority::LowHealth:
        if (maxHealth > 1.f || health > 0.f)
            return lowHpScore * 2.f + fovScore * 0.25f;
        return fovScore;
    case AimbotPriority::FovDistance:
    default:
        // Closest to crosshair wins — no player HP bonus (logs: chosePlayerOverCloserRobot).
        (void)isRobot;
        (void)lowHpScore;
        return fovScore + distScore * 0.02f;
    }
}

static uint64_t g_aimStickyKey = 0;
static float g_aimStickyExtraFovPx = 0.f;

static float EffectiveAimFovForKey(uint64_t key, float baseFov)
{
    if (g_aimStickyKey != 0 && key == g_aimStickyKey && g_aimStickyExtraFovPx > 0.f)
        return baseFov + g_aimStickyExtraFovPx;
    return baseFov;
}

// Sticky lock must retain the locked target even when menu sticky FOV bias is 0
// (otherwise movers leave the tiny FOV cone and unlock / grace at empty space).
static bool AimStickyBypassFov(uint64_t key)
{
    return var::sticky_target_lock
        && g_aimStickyKey != 0
        && key == g_aimStickyKey;
}

// ============================================
// HUMANIZER — realistic mouse movement model
// ============================================
//
// Real human aim has three components:
//   1. Hand tremor: 8-12 Hz sinusoidal, always present, amplitude scales
//      with distance (far = sloppy, close = precise)
//   2. Micro-corrections: small random offsets that drift and correct over
//      ~100-200ms, simulating the eye-hand feedback loop
//   3. Speed penalty: fast sweeping movements are less precise than slow
//      fine-tuning near the target
//
// The old OU+tremor+SDN model produced uniform jitter regardless of
// distance. This model modulates all three components by error magnitude
// so the aim is visibly sloppier at distance and tighter on target.

// Humanizer v2 — the old model (pure tremor + drift + speed noise) was
// sub-pixel and read as invisible high-frequency noise, not human aim.
// Real aim has THREE visible tells this model now adds on top of the noise:
//
//   1. REACTION DELAY: a lock does not engage instantly. On a new lock (or a
//      large error jump) the pull ramps in over a jittered 120-350ms window,
//      scaled up for far targets. Kills the "snaps the millisecond an enemy
//      peeks" tell.
//   2. SETTLING OVERSHOOT: a real flick is loose — it sweeps in fast, blows
//      past, settles back, and only then holds. Modelled as a damped
//      2nd-order response whose overshoot amplitude scales with remaining
//      error, gives a visible arcing-in plus a decaying oscillation on the
//      approach axis rather than a mechanical linear glide.
//   3. MICRO RE-AIM: once settled, the crosshair occasionally drifts off and
//      re-fixes itself every 600-1600ms (the "double-check" tell), instead
//      of locking on a pixel and never moving again.
// Tremor + drift + speed noise are retained underneath for naturalness.
class Humanizer {
public:
    void Reset()
    {
        m_driftX = 0.0;
        m_driftY = 0.0;
        m_lastTimeMs = 0.0;
        m_initialized = false;
        m_reactStartMs = GetTimeMs();
        m_lastJumpMs = m_reactStartMs;
        m_reaimEndMs = m_reactStartMs; // micro re-aim armed
        std::uniform_real_distribution<double> phaseDist(0.0, 2.0 * std::numbers::pi);
        m_tremorPhaseX = phaseDist(m_rng);
        m_tremorPhaseY = phaseDist(m_rng);
        // Randomize the settling spring phase so the overshoot curve differs
        // per lock-on (avoids a repeatable weave pattern).
        std::uniform_real_distribution<double> springDist(0.0, 2.0 * std::numbers::pi);
        m_springPhase = springDist(m_rng);
        // Reaction delay centered on the user's slider (var::humanizer_react_ms),
        // jittered 0.45x-1.0x per lock so engagements don't share a fixed delay.
        {
            const double base = (std::max)(0.0, static_cast<double>(var::humanizer_react_ms));
            std::uniform_real_distribution<double> reactDist(base * 0.45, (std::max)(base, 1.0));
            m_reactMs = reactDist(m_rng);
        }
        // Every other knob is ALSO randomized per lock so no two engagements
        // share a character: overshoot strength, the flick frequency, how fast
        // that overshoot decays, the X/Y weave leak, and micro-re-aim cadence.
        {
            // Per-lock overshoot fraction: user slider is the center; each lock
            // draws 0.55x..1.5x of it (never identical twice).
            std::uniform_real_distribution<double> ovDist(
                0.55 * var::humanizer_overshoot, 1.5 * var::humanizer_overshoot + 0.02);
            m_overshoot = (std::clamp)(ovDist(m_rng), 0.0, 0.6);
            std::normal_distribution<double> nd(0.0, 1.0);
            m_springFreq = (std::clamp)(kSpringFreq * (1.0 + nd(m_rng) * 0.35), 1.6, 4.2);
            m_springDamp = (std::clamp)(kSpringDamp * (1.0 + nd(m_rng) * 0.3), 4.0, 14.0);
            m_springCross = (std::clamp)(kSpringCross * (1.0 + nd(m_rng) * 0.4), 0.1, 0.65);
            m_reaimProb = (std::clamp)(kReaimProb * (0.4 + std::uniform_real_distribution<double>(0.0, 1.0)(m_rng)), 0.02, 0.4);
            m_reaimHoldMs = (std::clamp)(kReaimHoldMs * (0.6 + std::uniform_real_distribution<double>(0.0, 1.0)(m_rng)), 120.0, 500.0);
        }

        // Last applied delta, for debug logging.
        m_lastJx = 0.0;
        m_lastJy = 0.0;
        m_lastPhase = 0; // 0 none, 1 reacting, 2 settling, 3 held
    }

    // Called each aim tick BEFORE Apply so the caller can log it. Error
    // jumps (target teleport / bone switch) re-arm the reaction ramp.
    void NotifyError(double dx, double dy)
    {
        const double error = std::sqrt(dx * dx + dy * dy);
        const double now = GetTimeMs();
        // A sudden large error after being close = the target moved / we lost
        // the lock axis — treat like a fresh lock (short, scaled reaction).
        if (error > kSettleOverJumpsRe && now - m_lastJumpMs > 150.0) {
            m_lastJumpMs = now;
            m_reactStartMs = now;
            {
                const double base = (std::max)(0.0, static_cast<double>(var::humanizer_react_ms));
                const double fracFar = (std::min)(error / kReactFarErr, 1.0);
                std::uniform_real_distribution<double> reactDist(
                    base * 0.45, (std::max)(base * (0.6 + 0.4 * fracFar), 1.0));
                m_reactMs = reactDist(m_rng);
            }
            m_tSettleBase = now;
            m_springPhase = std::uniform_real_distribution<double>(0.0, 2.0 * std::numbers::pi)(m_rng);
        }
    }

    // dx/dy = screen-space error (px crosshair→target). Adds humanizing
    // offset to dx/dy in place. Larger error = more visible slop.
    void Apply(float& dx, float& dy)
    {
        const double now = GetTimeMs();
        if (!m_initialized) {
            m_lastTimeMs = now;
            m_tSettleBase = now;
            m_initialized = true;
            return;
        }
        const double dtMs = now - m_lastTimeMs;
        if (dtMs <= 0.0 || dtMs > 100.0) {
            m_lastTimeMs = now;
            return;
        }
        m_lastTimeMs = now;
        const double dtS = dtMs / 1000.0;
        const double tS = now / 1000.0;
        const double error = std::sqrt(dx * dx + dy * dy);

        // ── Phase selection ──────────────────────────────────────────
        // Reacting while within the reaction ramp and error is still big.
        // Settling (decaying overshoot spring) until error smalls down.
        // Held: fine tremor + drift + micro re-aim only.
        int phase = 3; // held
        const double sinceGate = now - m_lastJumpMs;
        if (error > kSettleGateErr) {
            phase = (sinceGate < m_reactMs) ? 1 : 2; // reacting → settling
        }

        // ── 1. Hand tremor (scaled, both phases) ────────────────────
        constexpr double kTremorFreq = 10.0;  // Hz, physiological range
        constexpr double kTremorBaseAmp = 0.3;
        constexpr double kTremorScale = 0.009;
        const double tremorAmp = kTremorBaseAmp + error * kTremorScale;
        const double tremorX = tremorAmp * std::sin(2.0 * std::numbers::pi * kTremorFreq * tS + m_tremorPhaseX);
        const double tremorY = tremorAmp * std::sin(2.0 * std::numbers::pi * kTremorFreq * 0.97 * tS + m_tremorPhaseY);

        // ── 2. Micro-drift correction (random walk) ──────────────────
        constexpr double kDriftTheta = 6.0;
        constexpr double kDriftSigma = 0.15;
        constexpr double kDriftMax = 1.5;
        std::normal_distribution<double> norm(0.0, 1.0);
        m_driftX += -kDriftTheta * m_driftX * dtS + kDriftSigma * std::sqrt(dtS) * norm(m_rng);
        m_driftY += -kDriftTheta * m_driftY * dtS + kDriftSigma * std::sqrt(dtS) * norm(m_rng);
        const double driftR = std::sqrt(m_driftX * m_driftX + m_driftY * m_driftY);
        if (driftR > kDriftMax) { const double s = kDriftMax / driftR; m_driftX *= s; m_driftY *= s; }
        const double driftScale = (std::min)(error / 30.0, 1.0);

        // ── 3. Speed penalty (proportional to error) ─────────────────
        constexpr double kSpeedNoiseK = 0.004;
        const double speedNoiseX = kSpeedNoiseK * error * norm(m_rng);
        const double speedNoiseY = kSpeedNoiseK * error * norm(m_rng);

        // ── 4. Settling overshoot spring (visible loose flick) ───────
        // A damped sinusoid along the dominant error axis. Amplitude scales
        // with error and decays over ~450ms; frequency ~2-3Hz (human flick
        // correction cadence). Direction flips once past the target.
        double settleX = 0.0, settleY = 0.0;
        if (phase == 2) {
            const double sinceSettle = (now - m_tSettleBase) / 1000.0;
            const double amp = (std::min)(error * m_overshoot, kOvershootMaxPx);
            const double damp = std::exp(-m_springDamp / 1000.0 * (now - m_tSettleBase));
            const double osc = amp * std::sin(2.0 * std::numbers::pi * m_springFreq * sinceSettle + m_springPhase) * damp;
            // Inject mostly along the dominant axis; a fraction leaks to the
            // other axis so the weave is slightly diagonal (more human).
            const double eMag = error > 1e-6 ? error : 1.0;
            settleX = static_cast<double>(dx) / eMag * osc * (1.0 - m_springCross);
            settleY = static_cast<double>(dy) / eMag * osc * m_springCross;
        }

        // ── 5. Reaction ease-in (ramp the pull up, don't freeze) ─────
        double react = 1.0;
        if (phase == 1) {
            react = (std::clamp)((sinceGate) / m_reactMs, 0.0, 1.0);
            react = 0.15 + 0.85 * react; // starts at 15% pull, ramps to 100%
        }

        // ── 6. Micro re-aim (settled locks randomly drift & re-fix) ───
        double reaim = 0.0;
        if (phase == 3 && error < kReaimErr) {
            const double rngV = std::uniform_real_distribution<double>(0.0, 1.0)(m_rng);
            const double nowMs = now;
            if (nowMs >= m_reaimEndMs) {
                if (rngV < m_reaimProb) {
                    // Fire a re-aim: small random nudge that decays over ~220ms.
                    m_reaimEndMs = nowMs + m_reaimHoldMs + std::uniform_real_distribution<double>(0.0, 1.0)(m_rng) * kReaimJitterMs;
                    m_reaimStartMs = nowMs;
                    m_reaimNx = std::normal_distribution<double>(0.0, kReaimAmp)(m_rng);
                    m_reaimNy = std::normal_distribution<double>(0.0, kReaimAmp)(m_rng);
                }
            }
            const double sinceR = nowMs - m_reaimStartMs;
            if (sinceR >= 0.0 && sinceR < m_reaimHoldMs) {
                const double env = std::exp(-kReaimDecay / 1000.0 * sinceR);
                reaim = env * (std::abs(m_reaimNx) + std::abs(m_reaimNy));
                settleX += m_reaimNx * env;
                settleY += m_reaimNy * env;
            }
        }

        // ── Combine (scaled by the user's intensity slider) ──────────
        const double intensity = (std::clamp)(static_cast<double>(var::humanizer_intensity), 0.0, 3.0);
        m_lastJx = ((settleX + reaim) * react + tremorX + m_driftX * driftScale + speedNoiseX) * intensity;
        m_lastJy = ((settleY + reaim) * react + tremorY + m_driftY * driftScale + speedNoiseY) * intensity;
        m_lastPhase = phase;
        dx += static_cast<float>(m_lastJx);
        dy += static_cast<float>(m_lastJy);
    }

    // Debug inspection: the jitter this class injected on the last tick.
    struct PhaseOut { int phase = 0; float jx = 0.f; float jy = 0.f; float react = 0.f; bool reaim = false; };
    PhaseOut Last() const
    {
        PhaseOut o;
        o.phase = m_lastPhase;
        o.jx = static_cast<float>(m_lastJx);
        o.jy = static_cast<float>(m_lastJy);
        o.reaim = m_reaimStartMs > m_lastJumpMs && (m_lastPhase == 3);
        return o;
    }

private:
    static double GetTimeMs()
    {
        using namespace std::chrono;
        static auto start = steady_clock::now();
        return duration_cast<std::chrono::duration<double, std::milli>>(steady_clock::now() - start).count();
    }

    // Tunables (expose as menu sliders later; cheap to keep local for now).
    static constexpr double kReactMinMs = 120.0;
    static constexpr double kReactMaxMs = 350.0;
    static constexpr double kReactFarErr = 140.0;    // error at which reaction hits max
    static constexpr double kSettleGateErr = 12.0;    // above this → settling/overshoot visible
    static constexpr double kSettleOverJumpsRe = 30.0; // error jump that re-arms reaction
    static constexpr double kOvershootFrac = 0.18;    // overshoot amplitude as fraction of error
    static constexpr double kOvershootMaxPx = 34.0;   // cap on overshoot px
    static constexpr double kSpringFreq = 2.6;         // Hz, flick correction cadence
    static constexpr double kSpringDamp = 8.0;         // 1/s decay of the overshoot
    static constexpr double kSpringCross = 0.35;       // leak to secondary axis (diagonal weave)
    static constexpr double kReaimErr = 9.0;           // only re-aim once this close
    static constexpr double kReaimProb = 0.12;         // chance to start a re-aim each poll
    static constexpr double kReaimAmp = 1.2;           // px re-aim nudge (gaussian sigma)
    static constexpr double kReaimHoldMs = 220.0;      // re-aim decay window
    static constexpr double kReaimJitterMs = 1400.0;   // extra random gap before next poll
    static constexpr double kReaimDecay = 12.0;        // 1/s env decay

    double m_driftX = 0.0;
    double m_driftY = 0.0;
    double m_tremorPhaseX = 0.0;
    double m_tremorPhaseY = 0.0;
    double m_lastTimeMs = 0.0;
    bool m_initialized = false;
    std::mt19937_64 m_rng{ std::random_device{}() };

    // Per-lock randomized character (drawn in Reset so each engagement differs).
    double m_overshoot = 0.18;
    double m_springFreq = 2.6;
    double m_springDamp = 8.0;
    double m_springCross = 0.35;
    double m_reaimProb = 0.12;
    double m_reaimHoldMs = 220.0;

    // Reaction / settling / re-aim state.
    double m_reactStartMs = 0.0;
    double m_reactMs = 150.0;
    double m_lastJumpMs = 0.0;
    double m_tSettleBase = 0.0;
    double m_springPhase = 0.0;
    double m_reaimEndMs = 0.0;
    double m_reaimStartMs = -1.0;
    double m_reaimNx = 0.0;
    double m_reaimNy = 0.0;

    // Last-tick debug output.
    double m_lastJx = 0.0;
    double m_lastJy = 0.0;
    int m_lastPhase = 0;
};

// ============================================
// HELPERS
// ============================================

static float GetTimeSeconds()
{
    using namespace std::chrono;
    static auto start = steady_clock::now();
    return duration_cast<duration<float>>(steady_clock::now() - start).count();
}

Vector3 Engine::GetActorVelocity(uintptr_t actor)
{
    if (!actor)
        return { 0.0, 0.0, 0.0 };

    const Vector3 repVel = Memory::read<Vector3>(actor + Offsets::ReplicatedMovement + Offsets::RepMov_LinearVelocity);
    if (repVel.x != 0.0 || repVel.y != 0.0 || repVel.z != 0.0)
        return repVel;

    const uintptr_t pioneerMove = Memory::read<uintptr_t>(actor + Offsets::PioneerCharacterMovement);
    if (pioneerMove && Memory::IsValidPtrFast2(pioneerMove))
        return Memory::read<Vector3>(pioneerMove + Offsets::ComponentVelocity);

    const uintptr_t movement = Memory::read<uintptr_t>(actor + Offsets::CharacterMovement);
    if (movement && Memory::IsValidPtrFast2(movement))
        return Memory::read<Vector3>(movement + Offsets::ComponentVelocity);

    const uintptr_t rootComponent = Memory::read<uintptr_t>(actor + Offsets::RootComponent);
    if (!rootComponent)
        return { 0.0, 0.0, 0.0 };

    return Memory::read<Vector3>(rootComponent + Offsets::ComponentVelocity);
}

Vector3 Engine::PredictPosition(
    const Vector3& targetPos,
    const Vector3& targetVelocity,
    const Vector3& myPos,
    float bulletSpeed,
    int iterations)
{
    // Iterative intercept solver: estimate time-to-hit, extrapolate target,
    // re-estimate. 3 iterations converges to <1px error for realistic speeds.
    //
    // Velocity staleness: PositionRefreshPass updates cached velocity every
    // ~16ms, but the aim thread runs at 4ms.  If the cached velocity is >50ms
    // old, we attenuate it to avoid predicting based on stale movement data
    // (the target may have changed direction since the last refresh).
    Vector3 predicted = targetPos;
    const float velMag = static_cast<float>(std::sqrt(
        targetVelocity.x * targetVelocity.x
        + targetVelocity.y * targetVelocity.y
        + targetVelocity.z * targetVelocity.z));

    for (int i = 0; i < iterations; i++) {
        Vector3 delta = {
            predicted.x - myPos.x,
            predicted.y - myPos.y,
            predicted.z - myPos.z
        };
        const float distance = static_cast<float>(std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));
        if (distance < 1.0f)
            break;
        const float timeToHit = distance / bulletSpeed;
        // For distant, fast-moving targets, apply mild velocity attenuation
        // on the final iteration to prevent overshooting when velocity is stale.
        const float atten = (i == iterations - 1 && velMag > 200.f && timeToHit > 0.08f)
            ? 0.85f : 1.0f;
        predicted = {
            targetPos.x + targetVelocity.x * timeToHit * atten,
            targetPos.y + targetVelocity.y * timeToHit * atten,
            targetPos.z + targetVelocity.z * timeToHit * atten
        };
    }
    return predicted;
}

namespace {

enum class RobotAimPosSrc : uint8_t {
    None = 0,
    Frame = 1,
    Cache = 2,
};

static uint64_t NowMs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

static float VelocityMag(const Vector3& v)
{
    return static_cast<float>(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
}

/** Shared aim lead ramp (velMag>80/+0.055s, >400/+0.035s). maxDtSec is intentional
 *  per-category (robot 0.25, player 0.20). Returns false if no lead applied. */
static bool ComputeVelocityLeadDelta(
    Vector3& outDelta,
    const Vector3& vel,
    float lastVelocityUpdate,
    float maxDtSec)
{
    outDelta = {};
    if (lastVelocityUpdate <= 0.f)
        return false;
    const uint64_t nowMs = NowMs();
    float dtSec =
        (nowMs - static_cast<uint64_t>(lastVelocityUpdate)) * 0.001f;
    if (dtSec <= 0.f || dtSec > maxDtSec)
        return false;
    const float velMag = VelocityMag(vel);
    if (velMag > 80.f)
        dtSec += 0.055f;
    if (velMag > 400.f)
        dtSec += 0.035f;
    outDelta = { vel.x * dtSec, vel.y * dtSec, vel.z * dtSec };
    return true;
}

static void ExtrapolateRobotWorldPosToNow(Engine::WorldCacheEntry& entry)
{
    Vector3 delta{};
    if (!ComputeVelocityLeadDelta(
            delta, entry.cachedVelocity, entry.lastVelocityUpdate, 0.25f))
        return;

    if (IsPlausibleWorldPos(entry.WorldPos)) {
        entry.WorldPos.x += delta.x;
        entry.WorldPos.y += delta.y;
        entry.WorldPos.z += delta.z;
    }
    if (entry.hasBotHeadWorldPos && IsPlausibleWorldPos(entry.BotHeadWorldPos)) {
        entry.BotHeadWorldPos.x += delta.x;
        entry.BotHeadWorldPos.y += delta.y;
        entry.BotHeadWorldPos.z += delta.z;
    }
    if (IsPlausibleWorldPos(entry.CenterWorldPos)) {
        entry.CenterWorldPos.x += delta.x;
        entry.CenterWorldPos.y += delta.y;
        entry.CenterWorldPos.z += delta.z;
    }
    for (int i = 0; i < entry.BotPartCount; ++i) {
        if (!IsPlausibleWorldPos(entry.BotPartPos[i]))
            continue;
        entry.BotPartPos[i].x += delta.x;
        entry.BotPartPos[i].y += delta.y;
        entry.BotPartPos[i].z += delta.z;
    }
}

// Lead aim bones to "now" using PositionRefresh velocity — GetActorVelocity was
// ~0 in aim_lock_track (velMag=1) so predict never caught strafing players while
// ESP WorldPos already extrapolated ahead of uneqtrapolated bones.
static void ExtrapolatePlayerAimWorldToNow(
    Vector3& worldPos,
    const Vector3& cachedVelocity,
    float lastVelocityUpdate)
{
    if (!IsPlausibleWorldPos(worldPos))
        return;
    Vector3 delta{};
    if (!ComputeVelocityLeadDelta(delta, cachedVelocity, lastVelocityUpdate, 0.20f))
        return;
    worldPos.x += delta.x;
    worldPos.y += delta.y;
    worldPos.z += delta.z;
}

static float RobotWorldAgeMs(const Engine::WorldCacheEntry& entry)
{
    if (entry.lastVelocityUpdate <= 0.f)
        return 0.f;
    const uint64_t nowMs = NowMs();
    return static_cast<float>(
        nowMs - static_cast<uint64_t>(entry.lastVelocityUpdate));
}

static bool ResolveRobotAimEntry(
    uint64_t actorKey,
    const Engine::WorldCacheEntry& cacheEntry,
    const Engine::EspRenderFrame* frame,
    Engine::WorldCacheEntry& outEntry,
    RobotAimPosSrc& outSrc)
{
    // Cache is refreshed every ~10 ms (PositionRefreshPass + RobotList scatter).
    outEntry = cacheEntry;
    outSrc = RobotAimPosSrc::Cache;

    if (frame && frame->valid) {
        for (const Engine::EspFrameWorld& frameRobot : frame->robots) {
            if (frameRobot.actorKey != actorKey)
                continue;

            const Engine::WorldCacheEntry& fr = frameRobot.entry;
            if (IsPlausibleWorldPos(fr.WorldPos))
                outEntry.WorldPos = fr.WorldPos;
            if (IsPlausibleWorldPos(fr.CenterWorldPos))
                outEntry.CenterWorldPos = fr.CenterWorldPos;
            if (fr.hasBotHeadWorldPos && IsPlausibleWorldPos(fr.BotHeadWorldPos))
            {
                outEntry.BotHeadWorldPos = fr.BotHeadWorldPos;
                outEntry.hasBotHeadWorldPos = true;
            }
            outEntry.BotPartCount = fr.BotPartCount;
            for (int i = 0; i < fr.BotPartCount && i < Engine::WorldCacheEntry::kMaxBotParts; ++i)
                outEntry.BotPartPos[i] = fr.BotPartPos[i];
            // Keep cacheEntry velocity / lastVelocityUpdate for live lead.
            outSrc = RobotAimPosSrc::Frame;
            break;
        }
    }

    // Every aim tick (2 ms): lead to now — frame path used to skip this entirely.
    ExtrapolateRobotWorldPosToNow(outEntry);
    return IsPlausibleWorldPos(outEntry.WorldPos);
}

const Engine::EspRenderFrame* g_aimEspFrame = nullptr;
Engine::CameraCache g_aimProjCam{};

static bool GetRobotAimScreenFromDraw(
    Engine& eng,
    const Engine::WorldCacheEntry& robot,
    const Engine::CameraCache& cam,
    Vector3& outScreenPos,
    Vector3& outWorldPos)
{
    return EspDraw::ResolveBotHeartScreenPoint(eng, cam, robot, outScreenPos, outWorldPos);
}

} // namespace

bool Engine::GetRobotAimPoint2D(
    const WorldCacheEntry& robot,
    float fovRadius,
    Vector3& outScreenPos,
    Vector3& outWorldPos,
    int32_t& outPartID,
    int32_t& outResistGroup)
{
    (void)fovRadius;
    if (!GetRobotAimScreenFromDraw(*this, robot, g_aimProjCam, outScreenPos, outWorldPos))
        return false;
    outPartID = -1;
    outResistGroup = 0;
    return true;
}

// ============================================
// AIM ASSIST — baseline cache loops
// ============================================

void Engine::AimAssistPlayer(
    const Vector3& screenCenter,
    float fovRadius,
    float bulletSpeed,
    float currentTime,
    std::vector<AimTarget>& targets)
{
    // Prefer EspRenderFrame.players (same snapshot as paint). Cache fallback only.
    static int s_playerFullScan = 0;
    const bool wantFull = (g_playerAimLockedKey == 0) || ((++s_playerFullScan % 4) == 0);
    g_aimFastPath = wantFull ? 0 : 1;

    auto consider = [&](uintptr_t key, const PlayerCacheEntry& actor) {
        if (!actor.Drawing) return;
        if (actor.Distance > var::aimbot_distance) return;
        if (var::aim_vis_mode == AimVisMode::VisibleOnly && !actor.isVisible)
            return;

        Vector3 aimPos{};
        Vector3 worldPos{};

        const auto& bones = actor.boneData;
        if (!ResolvePlayerAimBoneWorldTwoPhase(
                *this, bones, key, currentTime, screenCenter, fovRadius, g_aimProjCam, worldPos))
        {
            // Skip — actor.WorldPos (root CTW) oscillates ±100cm on this build,
            // using it as fallback causes violent shaking.
            return;
        }

        ExtrapolatePlayerAimWorldToNow(
            worldPos, actor.cachedVelocity, actor.lastVelocityUpdate);

        Vector3 velocity = actor.cachedVelocity;

        if (var::predict)
        {
            Vector3 camLocation{};
            {
                std::shared_lock<std::shared_mutex> camLock(m_cameraMutex);
                camLocation = g_Camera.Location;
            }
            worldPos = PredictPosition(worldPos, velocity, camLocation, bulletSpeed, 3);
        }

        if (!ProjectWorldLocationToScreen(worldPos, aimPos, g_aimProjCam))
            return;

        const float dx = static_cast<float>(aimPos.x - screenCenter.x);
        const float dy = static_cast<float>(aimPos.y - screenCenter.y);
        const float distToCenter = std::sqrt(dx * dx + dy * dy);

        if (!AimStickyBypassFov(key)
            && distToCenter > EffectiveAimFovForKey(key, fovRadius))
            return;

        const float totalScore = ScoreAimTarget(
            distToCenter,
            fovRadius,
            actor.Distance,
            actor.health,
            actor.maxhealth,
            actor.weaponQuality,
            false);

        AimTarget target;
        target.entityKey = key;
        target.aimPos = aimPos;
        target.worldPos = worldPos;
        target.distToCenter = distToCenter;
        target.score = totalScore;
        target.isRobot = false;

        targets.push_back(target);
    };

    if (g_aimEspFrame && g_aimEspFrame->valid && !g_aimEspFrame->players.empty()) {
        if (!wantFull && g_playerAimLockedKey != 0) {
            for (const EspFramePlayer& fp : g_aimEspFrame->players) {
                if (fp.actorKey == static_cast<uintptr_t>(g_playerAimLockedKey)) {
                    consider(fp.actorKey, fp.entry);
                    return;
                }
            }
        }
        for (const EspFramePlayer& fp : g_aimEspFrame->players)
            consider(fp.actorKey, fp.entry);
        return;
    }

    std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
    if (!wantFull && g_playerAimLockedKey != 0) {
        if (const auto it = playerCache.find(static_cast<uintptr_t>(g_playerAimLockedKey));
            it != playerCache.end())
            consider(it->first, it->second);
        return;
    }

    for (const auto& [key, actor] : playerCache)
        consider(key, actor);
}

void Engine::AimAssistRobot(
    const Vector3& screenCenter,
    float fovRadius,
    float currentTime,
    std::vector<AimTarget>& targets)
{
    (void)currentTime;

    if (!var::robotAimEnabled)
        return;

    auto tryAddRobot = [&](uintptr_t key, const WorldCacheEntry& robot) {
        if (!key || !IsValidPointer(key))
            return;
        if (IsCachedPlayer(key))
            return;
        if (!robot.Drawing)
            return;
        if (robot.Distance > var::aimbot_distance)
            return;
        if (var::aim_vis_mode == AimVisMode::VisibleOnly && !robot.isVisible)
            return;

        // Constructable bots (Fireball, Pop, Surveyor) use @0x1210 — not generic @0x1220.
        if (ReadBotBrokenFlag(key) != 0 && !var::show_dead_bots)
            return;

        std::string fname;
        if (robot.ActorName.empty()
            || robot.ActorName == kBotStructAdmissionToken
            || !IsAcceptedBotEspLabel(*this, robot.ActorName, std::string{})) {
            fname = GetActorFNameStringCached(key);
            if (fname.empty())
                fname = GetActorFNameString(key);
        }
        if (ResolveBotDrawLabel(key, robot.ActorName, fname).empty())
            return;

        Vector3 aimPos{};
        Vector3 worldPos{};
        int32_t partID = -1;
        int32_t resistGroup = 0;

        WorldCacheEntry resolvedEntry{};
        RobotAimPosSrc posSrc = RobotAimPosSrc::None;
        if (!ResolveRobotAimEntry(key, robot, g_aimEspFrame, resolvedEntry, posSrc))
            return;

        if (!GetRobotAimPoint2D(resolvedEntry, fovRadius, aimPos, worldPos, partID, resistGroup))
            return;

        const float dx = static_cast<float>(aimPos.x - screenCenter.x);
        const float dy = static_cast<float>(aimPos.y - screenCenter.y);
        const float distToCenter = std::sqrt(dx * dx + dy * dy);

        if (!AimStickyBypassFov(key)
            && distToCenter > EffectiveAimFovForKey(key, fovRadius))
            return;

        const float totalScore = ScoreAimTarget(
            distToCenter,
            fovRadius,
            robot.Distance,
            robot.health,
            robot.maxhealth,
            0,
            true);

        AimTarget target;
        target.entityKey = key;
        target.aimPos = aimPos;
        target.worldPos = worldPos;
        target.distToCenter = distToCenter;
        target.score = totalScore;
        target.isRobot = true;
        target.partID = partID;
        target.resistGroup = resistGroup;
        target.aimPosSrc = static_cast<uint8_t>(posSrc);
        target.aimWorldAgeMs = RobotWorldAgeMs(resolvedEntry);

        targets.push_back(target);
    };

    static int s_robotFullScan = 0;
    const bool wantFull = (g_playerAimLockedKey == 0) || ((++s_robotFullScan % 4) == 0);
    if (!wantFull && g_playerAimLockedKey != 0) {
        g_aimFastPath = 1;
        if (g_aimEspFrame) {
            for (const EspFrameWorld& item : g_aimEspFrame->robots) {
                if (item.actorKey == static_cast<uintptr_t>(g_playerAimLockedKey)) {
                    tryAddRobot(item.actorKey, item.entry);
                    return;
                }
            }
        }
        {
            std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
            if (const auto it = robotCache.find(static_cast<uintptr_t>(g_playerAimLockedKey));
                it != robotCache.end())
                tryAddRobot(it->first, it->second);
        }
        return;
    }

    // Prefer ESP frame robots (same pool + fresh scatter positions as on-screen boxes).
    if (g_aimEspFrame) {
        for (const EspFrameWorld& item : g_aimEspFrame->robots)
            tryAddRobot(item.actorKey, item.entry);
        return;
    }

    std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
    for (const auto& [key, robot] : robotCache)
        tryAddRobot(key, robot);
}

// ============================================
// KmBox hardware aim output
// ============================================

namespace {

constexpr int kKmAimChunkPx = 512;
/** Per-tick view rotation jump — reload / flinch; don't fight the animation.
 *  Only applied when already near the bone (see AimAssistence) so catch-up
 *  isn't cancelled by our own aim-driven camera motion. */
constexpr float kAimViewShakeSuppressDeg = 2.5f;
/** ESP-frame camera only when live view is stable (avoids stale-cam jitter). */
constexpr float kAimEspFrameCamMaxDriftDeg = 0.65f;
// Constants moved to Core/AimMath.hpp (Pillar 1 extraction — tests lock them).

struct AimDebugSnapshot {
    int candidates = 0;
    uint64_t locked = 0;
    float lastDx = 0.f;
    float lastDy = 0.f;
    float lastGain = 0.f;
    int kmbox = 0;
    uint8_t posSrc = 0;
    float worldAgeMs = 0.f;
    int grace = 0;
    // Used by [debugAim] console overlay + aim suppress path (always on).
    int lockedInCand = -1;
    int isRobot = -1;
    float distPx = -1.f;
    float distanceM = -1.f;
    float velMag = -1.f;
    int dropReason = 0; // 0 none, 1 notInCand, 2 graceExpire, 3 noBest, 4 kmbox
    float pullScale = 1.f;
    // Aim-shake probe (debug_aim_shake): what was commanded and how the
    // control loop is behaving tick to tick.
    float cmdX = 0.f;          // last mouse counts actually sent
    float cmdY = 0.f;
    float pxPerMouse = 0.f;    // learned screen-px per mouse count
    float closeFrac = 0.f;     // fraction of error closed per tick
    float tickMs = 0.f;        // time between aim ticks
    // Humanizer output (set in AimAssistence just before SendKmAimDelta, so
    // the aim_shake probe can show what the model injected last tick).
    int hnPhase = 0;           // 0 none, 1 reacting, 2 settling, 3 held
    float hnJx = 0.f;          // last injected jitter (px), X
    float hnJy = 0.f;          // last injected jitter (px), Y
    float hnReact = 0.f;       // reaction ease-in scalar (0..1) this tick
    // Per-lock randomized pull profile (surfaced so the log can confirm the
    // speed/asym differ per engagement rather than staying fixed).
    float ppSpeed = 0.f;
    float ppAccel = 0.f;
    float ppAsym = 0.f;
    float ppSharp = 0.f;
    float ppSitu = 0.f;        // live environment bias (density * type * distance)
    uint64_t ppStartMs = 0;
};
static AimDebugSnapshot s_aimDbg;

// Shake counters persist across ticks (s_aimDbg resets each tick); printed
// and cleared by the 500ms debug block.
static int s_dbgFlipX = 0;      // error sign flips since last print (X)
static int s_dbgFlipY = 0;      // error sign flips since last print (Y)
static int s_dbgOvershoot = 0;  // adaptive-gain overshoot events
static int s_dbgSwitches = 0;   // target lock switches since last print
static bool s_dbgSuppress = false; // last tick's suppress decision
static float s_dbgShakeDeg = 0.f;  // last tick's view shake

// Adaptive mouse↔screen: post-fix3 sent rem≈distPx*gain but only closed ~10px
// while rem was ~120 → treat mouse counts as weaker than screen pixels.
static float s_pxPerMouse = AimMath::kMousePxPerCountInit;
static float s_prevErrX = 0.f;
static float s_prevErrY = 0.f;
static float s_prevCmdX = 0.f;
static float s_prevCmdY = 0.f;
static uint64_t s_adaptKey = 0;
static float s_filtMx = 0.f;
static float s_filtMy = 0.f;
static float s_lastAimTickMs = 8.f;

static bool s_inDeadzone = false;

// OscDampState moved to Core/AimMath.hpp (Pillar 1 extraction).
static AimMath::OscDampState s_oscDampX;
static AimMath::OscDampState s_oscDampY;

static void ResetAimMotionState()
{
    s_filtMx = 0.f;
    s_filtMy = 0.f;
    s_prevCmdX = 0.f;
    s_prevCmdY = 0.f;
    s_adaptKey = 0;
    s_inDeadzone = false;
    s_oscDampX = {};
    s_oscDampY = {};
}

// ── AUTO PREDICTION (no bullet-speed slider) ───────────────────────
// Self-learning lead scale. `PredictPosition` leads by velocity*timeToHit
// where timeToHit = distance/bulletSpeed; an uninformed fixed bullet speed
// leads too much or too little. Instead we learn a multiplier on effective
// bullet speed from the aim loop's own behavior on MOVING locked targets:
//   • over-lead (aim sits too far ahead) → the loop overshoots → crank
//     effective speed up (scale down) so we lead less;
//   • under-lead (target slips ahead) → error stays big while close → crank
//     effective speed down (scale up) so we lead more.
// s_leadScale multiplies the user bullet-speed BASELINE, so the slider stops
// mattering — it starts at 1.0 and converges per fight. Persists per lock.
static float s_leadScale = 1.0f;
static uint64_t s_leadKey = 0;
static int s_leadOverCount = 0;      // overshoot events in current window
static int s_leadUnderCount = 0;     // under-lead (err not shrinking) samples
static uint64_t s_leadWinMs = 0;     // window start
static float s_leadErrAcc = 0.f;     // accumulated residual while close+moving
static int s_leadErrN = 0;

// ── Randomized per-lock pull profile ───────────────────────────────
// Every time the aim locks a (new) target it draws a fresh set of random
// parameters that shape how that ONE engagement feels: how fast it pulls,
// whether it accelerates or eases in, per-axis asymmetry, and a duration
// over which the speed ramps. This means no two pulls are ever identical —
// the crosshair speed profile differs per engagement instead of following
// one fixed gain curve. The profile is held stable for the whole lock and
// only re-rolled when the target switches.
struct PullProfile {
    float speed = 1.f;         // overall pull-speed multiplier (0.6x..1.4x)
    float accel = 0.f;         // -1 (slow start, hurry at end) .. +1 (fast start, ease off)
    float asym = 1.f;          // X/Y asymmetry: 0.8..1.25 (per-axis speed split)
    float durationSec = 0.4f;  // how long the ramp spans
    float sharp = 1.f;         // closeFrac sharpness multiplier near target
    uint64_t startMs = 0;      // when this profile began
};
static PullProfile s_pullProf;
static std::mt19937_64 s_pullRng{ std::random_device{}() };

// Roll a fresh profile for a new lock. Reuses the platform RNG but keeps its
// own stream so it never hints at the humanizer's pattern.
static void ResetPullProfile()
{
    s_pullProf.startMs = NowMs();
    std::uniform_real_distribution<float> u;
    std::normal_distribution<float> n(0.f, 1.f);
    s_pullProf.speed = std::clamp(1.0f + n(s_pullRng) * 0.18f, 0.62f, 1.42f);
    s_pullProf.accel = std::clamp(n(s_pullRng) * 0.6f, -0.75f, 0.85f);
    s_pullProf.asym = std::clamp(1.0f + n(s_pullRng) * 0.14f, 0.82f, 1.25f);
    s_pullProf.durationSec = std::clamp(0.25f + u(s_pullRng) * 0.55f, 0.2f, 0.8f);
    s_pullProf.sharp = std::clamp(0.8f + n(s_pullRng) * 0.25f, 0.55f, 1.3f);
}

// Situational pull-speed bias — reads the live environment (set by
// AimAssistence into s_aimDbg) so the SAME randomized profile behaves
// differently depending on the fight:
//   • lone target       → patient, careful pull
//   • a few hostiles    → normal speed
//   • many hostiles     → fast, snappy (threat pressure)
//   • player vs bot     → players pull faster than predictable bots
//   • close vs far      → point-blank snaps, long range eases in
// The per-lock random character is layered ON TOP, so each pull is still never
// identical even in the same situation.
static float SituationalPullFactor()
{
    // Threat density from how many targets sit in the aim pool.
    const int cands = s_aimDbg.candidates;
    float density;
    if (cands <= 1)      density = 0.80f;   // lone — patient
    else if (cands <= 3) density = 1.00f;   // a couple — normal
    else if (cands <= 6) density = 1.18f;   // cluster — quick
    else                 density = 1.30f;   // mass — snap

    // Players are the real threat; bots are predictable.
    const float type = (s_aimDbg.isRobot == 1) ? 0.86f : 1.14f;

    // World distance (meters): small far target wants a careful settle.
    const float dm = (std::max)(s_aimDbg.distanceM, 0.f);
    float distance;
    if (dm <= 0.1f)      distance = 1.0f;    // unknown — neutral
    else if (dm < 15.f)  distance = 1.24f;   // point-blank — instant
    else if (dm < 40.f)  distance = 1.05f;   // close-mid
    else if (dm < 90.f)  distance = 0.90f;   // mid-far — ease in
    else                 distance = 0.70f;   // long range — slow & careful

    return (std::clamp)(density * type * distance, 0.45f, 1.6f);
}

float SendKmAimDelta(float dx, float dy, float pullScale = 1.f, float* outGain = nullptr)
{
    const float dist = hypotf(dx, dy);

    // New lock target → fresh oscillation state AND a fresh randomized pull
    // profile (the error jumps arbitrarily on a switch and would otherwise
    // look like flips; the pull profile must differ per engagement).
    if (s_adaptKey != s_aimDbg.locked || s_pullProf.startMs == 0) {
        s_oscDampX = {};
        s_oscDampY = {};
        ResetPullProfile();
        // New lock = fresh lead-learning window (don't carry a previous fight's
        // overshoot/trailing bias into the next engagement).
        s_leadWinMs = 0;
        s_leadOverCount = 0;
        s_leadErrAcc = 0.f;
        s_leadErrN = 0;
    }

    // Shake probe: an error sign flip between consecutive commands means the
    // pull crossed over the target — the limit-cycle that feels like shaking.
    if (s_prevErrX * dx < 0.f && std::fabs(s_prevErrX) > 3.f && std::fabs(dx) > 3.f)
        ++s_dbgFlipX;
    if (s_prevErrY * dy < 0.f && std::fabs(s_prevErrY) > 3.f && std::fabs(dy) > 3.f)
        ++s_dbgFlipY;
    // Oscillation damping (always on): count crossovers per axis in a 500ms
    // window; ≥3 in a window means the loop is ringing — ramp damping, which
    // eases closeFrac and smooths the EMA below until the flips stop.
    {
        const bool xFlip = s_prevErrX * dx < 0.f
            && std::fabs(s_prevErrX) > 3.f && std::fabs(dx) > 3.f;
        const bool yFlip = s_prevErrY * dy < 0.f
            && std::fabs(s_prevErrY) > 3.f && std::fabs(dy) > 3.f;
        const uint64_t flipNowMs = NowMs();
        s_oscDampX.tick(flipNowMs, xFlip);
        s_oscDampY.tick(flipNowMs, yFlip);
    }
    // Deadzone with hysteresis: once inside, stay inside until error exceeds
    // deadzone + 2 px. Prevents the aim from oscillating in/out at the boundary.
    const float dzThreshold = s_inDeadzone
        ? (var::aim_deadzone_px + 2.0f)
        : var::aim_deadzone_px;
    if (dist <= dzThreshold) {
        s_inDeadzone = true;
        s_filtMx = 0.f;
        s_filtMy = 0.f;
        return 0.f;
    }
    s_inDeadzone = false;

    // Learn px-per-mouse from last command vs this frame's error change.
    if (s_adaptKey != 0 && s_adaptKey == s_aimDbg.locked) {
        const float cmdMag = hypotf(s_prevCmdX, s_prevCmdY);
        if (cmdMag > 4.f) {
            const float dErrX = s_prevErrX - dx;
            const float dErrY = s_prevErrY - dy;
            const float along = (dErrX * s_prevCmdX + dErrY * s_prevCmdY) / cmdMag;
            // Misattribution guard: a target strafing across the crosshair
            // shrinks/raises error WITHOUT our pull — learning (either branch)
            // from that poisons the scale. Only learn when the error actually
            // moved along the commanded direction by a meaningful amount.
            const bool errMovedAlongCmd = along > 1.0f;
            const float errMagNow = hypotf(dx, dy);
            const float errMagPrev = hypotf(s_prevErrX, s_prevErrY);
            if (along > 0.75f && errMovedAlongCmd) {
                const float sample =
                    AimMath::ClampPxPerMouseSample(along / cmdMag);
                // Slow EMA (0.92/0.08, was 0.82/0.18): fast adaptation chased
                // per-tick noise and swung the scale itself into oscillation.
                s_pxPerMouse = AimMath::LearnPxPerMouse(s_pxPerMouse, sample);
            } else if (along < -1.f && cmdMag > 8.f
                && errMagNow > errMagPrev + 2.f
                && ((s_prevErrX * dx < 0.f && std::fabs(s_prevErrX) > 3.f
                     && std::fabs(dx) > 3.f)
                    || (s_prevErrY * dy < 0.f && std::fabs(s_prevErrY) > 3.f
                        && std::fabs(dy) > 3.f))) {
                // Overshoot: the error crossed the target and GREW after our
                // command — the mouse is STRONGER than learned. Raise the
                // scale so the next tick sends FEWER counts. The old code
                // lowered it (more counts), which fed the Y limit cycle the
                // aim-shake probe measured (flipY 22, overshoot 29). The sign
                // flip requirement keeps target motion — which also grows
                // errMag — from being misattributed as a weak mouse.
                s_pxPerMouse = AimMath::OvershootRaisePxPerMouse(s_pxPerMouse);
                ++s_dbgOvershoot;
            }
        }

        // ── AUTO PREDICTION lead estimator ─────────────────────────
        // Only learn while LOCKED, tracking a MOVING target, and we've pulled
        // real mouse (errMag meaningful). Over-lead shows up as loop overshoot;
        // under-lead shows up as residual error that refuses to shrink while
        // close. Aggregate over a ~450ms window, then nudge s_leadScale slowly
        // (multiplies the baseline bullet speed). Bounded + per-lock.
        {
            const bool targetMoving = s_aimDbg.velMag > 80.f;
            const float errMag = hypotf(dx, dy);
            const uint64_t nowL = NowMs();
            if (g_kmbox.kmboxConfig.initialized && targetMoving) {
                if (s_leadWinMs == 0)
                    s_leadWinMs = nowL;
                if (s_dbgOvershoot > 0)
                    s_leadOverCount = (std::max)(s_leadOverCount, s_dbgOvershoot);
                // Under-lead: close but residual stays stubbornly above a
                // small threshold while the target moves → we trail it.
                if (errMag > 5.f && errMag < 45.f)
                    s_leadErrAcc += errMag, ++s_leadErrN;
                if (nowL - s_leadWinMs >= 300) {
                    const float avgErr = s_leadErrN > 0 ? s_leadErrAcc / s_leadErrN : 0.f;
                    if (s_leadOverCount >= 1) {
                        // Overshooting the leader → aim too far ahead → less lead.
                        s_leadScale = (std::clamp)(s_leadScale * 1.05f, 0.5f, 2.0f);
                    } else if (avgErr > 10.f && s_leadErrN >= 6) {
                        // Stuck off-center on a moving target → trail → more lead.
                        s_leadScale = (std::clamp)(s_leadScale * 0.95f, 0.5f, 2.0f);
                    }
                    s_leadOverCount = 0;
                    s_leadErrAcc = 0.f;
                    s_leadErrN = 0;
                    s_leadWinMs = nowL;
                }
            } else {
                s_leadWinMs = 0;
                s_leadOverCount = 0;
                s_leadErrAcc = 0.f;
                s_leadErrN = 0;
            }
        }
        s_leadKey = s_aimDbg.locked;
    }
    s_pxPerMouse = AimMath::ClampPxPerMouseSample(s_pxPerMouse);

    // Single clean gain — the old three-setting interaction (speed/smooth/sensitivity)
    // was confusing and the three sliders fought each other. One internal gain that
    // the adaptive px/mouse converter learns around.  Higher = faster pull.
    constexpr float kBaseGain = 1.0f;
    const float userBias = kBaseGain;

    // Fraction of remaining screen error to close this tick — smooth, not snap.
    float closeFrac = AimMath::CloseFractionForDist(dist);

    if (var::aim_algorithm == AimAlgorithm::Accelerated)
        closeFrac = AimMath::AcceleratedCloseFrac(closeFrac, dist);

    if (pullScale < 0.f)
        pullScale = 0.f;
    if (pullScale > 1.f)
        pullScale = 1.f;
    closeFrac = AimMath::ApplyPullScale(closeFrac, pullScale);

    // Per-lock randomized pull profile: speed varies WITHIN the engagement via
    // a jittered ramp, and base speed/sharpness differ per target — so no two
    // pulls are ever the same curve. accel<0 = slow start then hurry; accel>0 =
    // fast start then ease off.
    // Environment bias driven by the LIVE situation (enemy density, players vs
    // bots, world distance) — layered under the per-lock randomness so the same
    // situation always baselines correctly but no two pulls are identical.
    const float situ = SituationalPullFactor();
    {
        const float elapsed = static_cast<float>(NowMs() - s_pullProf.startMs) / 1000.f;
        const float t = elapsed / s_pullProf.durationSec;
        const float ramp = AimMath::PullProfileRamp(s_pullProf.accel, t);
        closeFrac = AimMath::ApplyPullProfile(
            closeFrac, s_pullProf.speed, ramp, s_pullProf.sharp, situ);
    }
    s_aimDbg.ppSitu = situ;

    // Convert desired screen close → mouse counts via learned scale.
    // Oscillation damping eases the pull per axis while the loop rings.
    const float invScale = 1.f / s_pxPerMouse;
    const float dampPullX = AimMath::OscDampPullFactor(s_oscDampX.damp, AimMath::kOscPullFactor);
    const float dampPullY = AimMath::OscDampPullFactor(s_oscDampY.damp, AimMath::kOscPullFactor);
    // Per-lock asymmetry: split the per-axis speed (X faster than Y or vice
    // versa), which rotates the approach vector slightly per engagement and
    // makes the path feel less machine-perfect.
    const float asymX = s_pullProf.asym;
    const float asymY = 1.f / s_pullProf.asym;
    float mx = dx * closeFrac * invScale * userBias * dampPullX * asymX;
    float my = dy * closeFrac * invScale * userBias * dampPullY * asymY;

    s_aimDbg.pxPerMouse = s_pxPerMouse;
    s_aimDbg.closeFrac = closeFrac * (std::max)(pullScale, 0.90f);
    s_aimDbg.ppSpeed = s_pullProf.speed;
    s_aimDbg.ppAccel = s_pullProf.accel;
    s_aimDbg.ppAsym = s_pullProf.asym;
    s_aimDbg.ppSharp = s_pullProf.sharp;
    s_aimDbg.ppStartMs = s_pullProf.startMs;

    // Cap mouse burst so one late tick cannot slam (choppy). Scale cap with
    // recent tickMs so slow ticks still catch movers.
    const float tickScale = AimMath::TickScaleFromTickMs(s_lastAimTickMs);
    const float maxMouse = AimMath::MaxMouseForDist(dist, tickScale);
    std::tie(mx, my) = AimMath::ClampMouseBurst(mx, my, maxMouse);

    // Light EMA — kills dy sign-flip chatter without lagging far catch-up.
    // Damping lowers the alpha further (stronger smoothing) while ringing.
    const float filtA = dist > 35.f ? 0.72f : 0.48f;
    const float filtAX = AimMath::EmaAlphaWithDamp(filtA, s_oscDampX.damp);
    const float filtAY = AimMath::EmaAlphaWithDamp(filtA, s_oscDampY.damp);
    s_filtMx = AimMath::EmaStep(s_filtMx, mx, filtAX);
    s_filtMy = AimMath::EmaStep(s_filtMy, my, filtAY);
    mx = s_filtMx;
    my = s_filtMy;

    const float gainOut = closeFrac * invScale * userBias;
    if (outGain)
        *outGain = gainOut;


    int remX = static_cast<int>(std::round(mx));
    int remY = static_cast<int>(std::round(my));

    // Nudge at least 1 count toward error — but only when well outside the
    // deadzone. The old threshold (deadzone + 0.5) caused limit-cycle
    // oscillation: nudge → overshoot → nudge back → overshoot → shake.
    // Gate at deadzone + 3 px so the 1-count nudge never fires right at the
    // boundary where it would bounce.
    if (remX == 0 && remY == 0 && dist > var::aim_deadzone_px + 3.0f) {
        remX = (dx > 0.f) ? 1 : ((dx < 0.f) ? -1 : 0);
        remY = (dy > 0.f) ? 1 : ((dy < 0.f) ? -1 : 0);
        if (remX == 0 && remY == 0)
            remX = (std::abs(dx) >= std::abs(dy)) ? ((dx >= 0.f) ? 1 : -1) : 0;
    }

    if (remX == 0 && remY == 0)
        return gainOut;

    // Shake probe: log the crossover event with full loop state, throttled.
    if (var::debug_aim_shake && (s_dbgFlipX > 0 || s_dbgFlipY > 0)) {
        static uint64_t s_shakeLastLogMs = 0;
        const uint64_t nowMsShake = NowMs();
        if (nowMsShake - s_shakeLastLogMs >= 150) {
            s_shakeLastLogMs = nowMsShake;
            std::cout << "[debugShake] flipX=" << s_dbgFlipX
                << " flipY=" << s_dbgFlipY
                << " overshoot=" << s_dbgOvershoot
                << " err=(" << dx << "," << dy << ")"
                << " prevErr=(" << s_prevErrX << "," << s_prevErrY << ")"
                << " cmd=(" << remX << "," << remY << ")"
                << " pxPerMouse=" << s_pxPerMouse
                << " closeFrac=" << closeFrac
                << " distPx=" << s_aimDbg.distPx
                << " tickMs=" << s_lastAimTickMs
                << " shakeDeg=" << s_dbgShakeDeg
                << " suppress=" << (s_dbgSuppress ? 1 : 0)
                << " damp=(" << s_oscDampX.damp << "," << s_oscDampY.damp << ")"
                << std::endl;
            {
                std::ofstream lf(kArcVerifyPath, std::ios::app);
                if (lf) {
                    lf << "{\"location\":\"Aimbot.cpp\"," 
                       << "\"message\":\"aim_shake\"," 
                       << "\"data\":{\"flipX\":" << s_dbgFlipX
                       << ",\"flipY\":" << s_dbgFlipY
                       << ",\"overshoot\":" << s_dbgOvershoot
                       << ",\"err\":[" << dx << "," << dy << "]"
                       << ",\"prevErr\":[" << s_prevErrX << "," << s_prevErrY << "]"
                       << ",\"cmd\":[" << remX << "," << remY << "]"
                       << ",\"pxPerMouse\":" << s_pxPerMouse
                       << ",\"closeFrac\":" << closeFrac
                       << ",\"distPx\":" << s_aimDbg.distPx
                       << ",\"tickMs\":" << s_lastAimTickMs
                       << ",\"shakeDeg\":" << s_dbgShakeDeg
                       << ",\"suppress\":" << (s_dbgSuppress ? 1 : 0)
                       << ",\"damp\":[" << s_oscDampX.damp << "," << s_oscDampY.damp << "]"
                       << ",\"hn\":[" << s_aimDbg.hnPhase << "," << s_aimDbg.hnJx << "," << s_aimDbg.hnJy << "]"
                       << ",\"pp\":[" << s_aimDbg.ppSpeed << "," << s_aimDbg.ppAccel << "," << s_aimDbg.ppAsym << "," << s_aimDbg.ppSharp << "," << s_aimDbg.ppSitu << "," << s_leadScale << "]"
                       << ",\"velMag\":" << s_aimDbg.velMag
                       << ",\"posSrc\":" << static_cast<int>(s_aimDbg.posSrc)
                       << ",\"distM\":" << s_aimDbg.distanceM
                       << "},\"ts\":" << nowMsShake << "}\n";
                }
            }
        }
    }

    s_prevErrX = dx;
    s_prevErrY = dy;
    s_prevCmdX = static_cast<float>(remX);
    s_prevCmdY = static_cast<float>(remY);
    s_adaptKey = s_aimDbg.locked;
    s_aimDbg.cmdX = static_cast<float>(remX);
    s_aimDbg.cmdY = static_cast<float>(remY);

    while (remX != 0 || remY != 0) {
        int stepX = remX;
        int stepY = remY;
        if (stepX > kKmAimChunkPx)
            stepX = kKmAimChunkPx;
        else if (stepX < -kKmAimChunkPx)
            stepX = -kKmAimChunkPx;
        if (stepY > kKmAimChunkPx)
            stepY = kKmAimChunkPx;
        else if (stepY < -kKmAimChunkPx)
            stepY = -kKmAimChunkPx;

        g_kmbox.MoveAim(stepX, stepY);
        remX -= stepX;
        remY -= stepY;
    }
    return gainOut;
}


} // namespace

void Engine::AimAssistence()
{
    static uint64_t lockedTarget = 0;
    static uint64_t previousTarget = 0;
    static float lastSwitchTime = 0.f;
    static float lastTargetScore = 0.f;
    static bool lockedIsRobot = false;
    static Humanizer humanizer;
    static int kmboxFailStreak = 0;
    static bool kmboxFailLogged = false;
    static AimTarget s_graceTarget{};
    static uint64_t s_graceUntilMs = 0;
    static bool s_graceActive = false;
    static Vector3 s_graceVelocity{};
    static uint64_t s_graceStampMs = 0;


    s_aimDbg = {};
    g_aimStickyKey = 0;
    g_aimStickyExtraFovPx = 0.f;
    g_aimFastPath = 0;

    const bool playerAimEnabled = var::enable_aimbot;
    const bool robotAimEnabled = var::robotAimEnabled;

    if (!playerAimEnabled && !robotAimEnabled)
    {
        lockedTarget = 0;
        previousTarget = 0;
        humanizer.Reset();
        kmboxFailStreak = 0;
        s_graceActive = false;
        return;
    }

    const int bindAim = var::aim_hold_key ? var::aim_hold_key : VK_SHIFT;
    const bool keyPressed = KeyBindIsHeld(bindAim);

    // Shake probe: measure aim tick cadence. Slow/irregular ticks make the
    // pull stutter; combined with flip/overshoot counts it separates "control
    // loop oscillation" from "DMA timing jitter".
    if (keyPressed) {
        static std::chrono::steady_clock::time_point s_lastAimInvoke{};
        const auto invokeNow = std::chrono::steady_clock::now();
        if (s_lastAimInvoke.time_since_epoch().count() != 0)
            s_aimDbg.tickMs =
                std::chrono::duration<float, std::milli>(invokeNow - s_lastAimInvoke).count();
        s_lastAimInvoke = invokeNow;
    }

    if (!keyPressed)
    {
        lockedTarget = 0;
        previousTarget = 0;
        g_playerAimClosePhase = false;
        humanizer.Reset();
        kmboxFailStreak = 0;
        s_graceActive = false;
        return;
    }

    uintptr_t sGWorld, sPersistentLevel, sAcknowledgedPawn, sPlayerController;
    {
        std::shared_lock<std::shared_mutex> slock(m_stateMutex);
        sGWorld = GWorld;
        sPersistentLevel = PersistentLevel;
        sAcknowledgedPawn = AcknowledgedPawn;
        sPlayerController = PlayerController;
    }

    if (!IsValidPointer(sGWorld) || !IsValidPointer(sPersistentLevel) || !sAcknowledgedPawn)
    {
        lockedTarget = 0;
        previousTarget = 0;
        humanizer.Reset();
        s_graceActive = false;
        return;
    }

    if (!IsValidPointer(sPlayerController) || !IsValidPointer(sAcknowledgedPawn))
    {
        lockedTarget = 0;
        previousTarget = 0;
        humanizer.Reset();
        s_graceActive = false;
        return;
    }

    try
    {
        const Vector3 testRead = Memory::read<Vector3>(sPlayerController + Offsets::ControlRotation);
        if (std::isnan(testRead.x) || std::isnan(testRead.y) || std::isnan(testRead.z))
            return;
    }
    catch (...)
    {
        return;
    }

    CameraCache aimCam{};
    {
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        aimCam = g_Camera;
    }

    static Vector3 s_lastLiveViewRot{};
    static bool s_haveLastLiveView = false;
    float viewShakeDeg = 0.f;
    if (s_haveLastLiveView)
        viewShakeDeg = CameraRotDeltaDeg(aimCam.Rotation, s_lastLiveViewRot);
    s_lastLiveViewRot = aimCam.Rotation;
    s_haveLastLiveView = true;
    // Provisional: far catch-up must not be cancelled by aim-driven cam motion.
    // Final suppress is decided after we know bone distPx (below).
    bool suppressAimOutput = false;

    std::shared_ptr<const EspRenderFrame> espFrameShared;
    {
        // Never block publish — same try_lock + last-good policy as paint.
        static std::shared_ptr<const EspRenderFrame> s_lastGoodAimFrame;
        std::shared_lock<std::shared_mutex> lock(m_espFrameMutex, std::try_to_lock);
        if (lock.owns_lock() && m_espFrameShared && m_espFrameShared->valid)
            s_lastGoodAimFrame = m_espFrameShared;
        espFrameShared = s_lastGoodAimFrame;
    }
    // Bind to a valid frame or an immutable empty one; no deep copy either way.
    static const EspRenderFrame kInvalidAimFrame{};
    const EspRenderFrame& espFrame = espFrameShared ? *espFrameShared : kInvalidAimFrame;

    CameraCache aimProjCam = aimCam;
    // Per-axis drift check: reject ESP frame camera if EITHER axis exceeds the
    // threshold. The old RSS check (sqrt(pitch^2 + yaw^2)) rejected the frame
    // camera during diagonal look movements even when each axis was within
    // tolerance — causing unnecessary fallback to the live camera.
    const float pitchDrift = std::abs(static_cast<float>(
        aimCam.Rotation.x - espFrame.camera.Rotation.x));
    const float yawDrift = std::abs(static_cast<float>(
        NormalizeYawDelta(static_cast<float>(aimCam.Rotation.y - espFrame.camera.Rotation.y))));
    if (viewShakeDeg < kAimViewShakeSuppressDeg
        && espFrame.valid
        && IsPlausibleWorldPos(espFrame.camera.Location)
        && espFrame.camera.FOV > 1.f && espFrame.camera.FOV < 179.f
        && pitchDrift <= kAimEspFrameCamMaxDriftDeg
        && yawDrift <= kAimEspFrameCamMaxDriftDeg)
        aimProjCam = espFrame.camera;

    g_aimProjCam = aimProjCam;
    g_aimEspFrame = espFrame.valid ? &espFrame : nullptr;

    if (!IsPlausibleWorldPos(aimProjCam.Location))
    {
        g_aimEspFrame = nullptr;
        return;
    }

    if (aimProjCam.FOV <= 1.f || aimProjCam.FOV > 179.f)
    {
        g_aimEspFrame = nullptr;
        return;
    }

    const Vector3 screenCenter = GetProjectionScreenCenter();
    if (screenCenter.x <= 0.0 || screenCenter.y <= 0.0)
    {
        g_aimEspFrame = nullptr;
        return;
    }

    const float screenW = static_cast<float>(screenCenter.x * 2.0);
    const float fovRadius = Visuals::AimbotFovRadiusPx(var::aimbot_fov, aimProjCam.FOV, screenW);
    if (fovRadius < 4.f)
    {
        g_aimEspFrame = nullptr;
        return;
    }

    if (var::sticky_target_lock && lockedTarget != 0) {
        g_aimStickyKey = lockedTarget;
        g_aimStickyExtraFovPx = (std::max)(0.f, var::aim_sticky_fov_bias_px);
    }

    g_playerAimLockedKey = lockedTarget;

    const float currentTime = GetTimeSeconds();
    // AUTO prediction: the user slider is only a baseline — the learned
    // s_leadScale (multiplies effective bullet speed) is what actually tunes
    // lead per fight, so the slider stops mattering. Higher scale = faster
    // effective bullet = LESS lead (fixes overshoot); lower = MORE lead
    // (fixes trailing a strafer). Clamped, persists per lock.
    const float baseSpeed = var::aim_bullet_speed_cm_s > 0.f
        ? var::aim_bullet_speed_cm_s
        : 80000.f;
    const float bulletSpeed = baseSpeed * s_leadScale;
    const uint64_t nowMs = NowMs();

    std::vector<AimTarget> allTargets;
    allTargets.reserve(64);

    if (playerAimEnabled)
        AimAssistPlayer(screenCenter, fovRadius, bulletSpeed, currentTime, allTargets);

    if (robotAimEnabled)
        AimAssistRobot(screenCenter, fovRadius, currentTime, allTargets);

    g_aimEspFrame = nullptr;
    g_aimStickyKey = 0;
    g_aimStickyExtraFovPx = 0.f;


    if (lockedTarget != 0) {
        bool lockedInCandidates = false;
        for (const AimTarget& target : allTargets) {
            if (target.entityKey == lockedTarget) {
                lockedInCandidates = true;
                s_graceTarget = target;
                s_graceActive = false;
                s_graceUntilMs = 0;
                s_graceStampMs = nowMs;
                s_graceVelocity = {};
                if (target.isRobot) {
                    std::shared_lock<std::shared_mutex> rlock(m_robotCacheMutex);
                    if (const auto it = robotCache.find(static_cast<uintptr_t>(target.entityKey));
                        it != robotCache.end())
                        s_graceVelocity = it->second.cachedVelocity;
                } else {
                    // Prefer cached velocity (PositionRefreshPass, no DMA on aim thread).
                    // GetActorVelocity does 2-4 Memory::read calls — too expensive for the
                    // 4ms aim budget and unnecessary when PositionRefreshPass already
                    // extrapolates positions with the same velocity data.
                    std::shared_lock<std::shared_mutex> plock(m_playerCacheMutex);
                    if (const auto it = playerCache.find(static_cast<uintptr_t>(target.entityKey));
                        it != playerCache.end())
                        s_graceVelocity = it->second.cachedVelocity;
                    else
                        s_graceVelocity = {};
                }
                break;
            }
        }
        s_aimDbg.lockedInCand = lockedInCandidates ? 1 : 0;
        if (!lockedInCandidates) {
            s_aimDbg.dropReason = 1;
            if (var::aim_loss_of_sight_grace_enabled
                && var::aim_loss_of_sight_grace_ms > 0
                && s_graceTarget.entityKey == lockedTarget) {
                if (!s_graceActive) {
                    s_graceActive = true;
                    s_graceUntilMs = nowMs + static_cast<uint64_t>(var::aim_loss_of_sight_grace_ms);
                    if (s_graceStampMs == 0)
                        s_graceStampMs = nowMs;
                }
                if (nowMs <= s_graceUntilMs) {
                    // Lead grace world pos each tick — do not replay frozen screen aimPos.
                    AimTarget grace = s_graceTarget;
                    const float dtSec = (s_graceStampMs > 0)
                        ? static_cast<float>(nowMs - s_graceStampMs) * 0.001f
                        : 0.f;
                    if (dtSec > 0.f && dtSec < 0.6f) {
                        // Cap per-tick displacement to prevent overshoot when target
                        // changes direction mid-grace (e.g. strafe reversal). At 400 cm/s
                        // and 4ms tick, the normal displacement is 1.6cm — the 8cm cap
                        // allows sprint but catches a 180-degree reversal (which would
                        // otherwise overshoot by vel*dt before the next cache refresh
                        // corrects it).
                        constexpr float kMaxGraceStepCm = 8.f;
                        float stepX = static_cast<float>(s_graceVelocity.x * dtSec);
                        float stepY = static_cast<float>(s_graceVelocity.y * dtSec);
                        float stepZ = static_cast<float>(s_graceVelocity.z * dtSec);
                        const float stepMag = std::sqrt(stepX * stepX + stepY * stepY + stepZ * stepZ);
                        if (stepMag > kMaxGraceStepCm) {
                            const float scale = kMaxGraceStepCm / stepMag;
                            stepX *= scale;
                            stepY *= scale;
                            stepZ *= scale;
                        }
                        grace.worldPos.x += stepX;
                        grace.worldPos.y += stepY;
                        grace.worldPos.z += stepZ;
                    }
                    s_graceTarget.worldPos = grace.worldPos;
                    s_graceStampMs = nowMs;
                    Vector3 screen{};
                    if (ProjectWorldLocationToScreen(grace.worldPos, screen, g_aimProjCam)) {
                        grace.aimPos = screen;
                        const float dx = static_cast<float>(screen.x - screenCenter.x);
                        const float dy = static_cast<float>(screen.y - screenCenter.y);
                        grace.distToCenter = std::sqrt(dx * dx + dy * dy);
                    }
                    allTargets.push_back(grace);
                    s_aimDbg.grace = 1;
                } else {
                    lockedTarget = 0;
                    previousTarget = 0;
                    humanizer.Reset();
                    s_graceActive = false;
                    s_graceVelocity = {};
                    s_graceStampMs = 0;
                    s_aimDbg.locked = 0;
                    s_aimDbg.grace = 0;
                    return;
                }
            } else {
                lockedTarget = 0;
                previousTarget = 0;
                humanizer.Reset();
                s_graceActive = false;
                s_graceVelocity = {};
                s_graceStampMs = 0;
                s_aimDbg.locked = 0;
                return;
            }
        }
    }

    AimTarget* bestTarget = nullptr;
    float bestScore = -FLT_MAX;

    for (auto& target : allTargets)
    {
        if (lockedTarget == target.entityKey)
        {
            // Sticky retention FOV only — still pick highest score (closest FOV).
            // Do not break early (post-fix3: stuck on farther player).
        }

        if (target.score > bestScore ||
            (target.score == bestScore &&
                target.distToCenter < (bestTarget ? bestTarget->distToCenter : FLT_MAX)))
        {
            bestScore = target.score;
            bestTarget = &target;
        }
    }

    if (!bestTarget)
    {
        lockedTarget = 0;
        previousTarget = 0;
        humanizer.Reset();
        s_graceActive = false;
        s_aimDbg.locked = 0;
        return;
    }

    s_aimDbg.candidates = static_cast<int>(allTargets.size());
    s_aimDbg.isRobot = bestTarget->isRobot ? 1 : 0;
    s_aimDbg.distPx = bestTarget->distToCenter;
    s_aimDbg.distanceM = bestTarget->distanceM;
    {
        const Vector3& v = s_graceVelocity;
        s_aimDbg.velMag = static_cast<float>(
            std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    }

    if (bestTarget->worldPos.x == 0.0 && bestTarget->worldPos.y == 0.0 && bestTarget->worldPos.z == 0.0)
        return;

    if (lockedTarget != bestTarget->entityKey)
    {
        const float sinceLastSwitch = currentTime - lastSwitchTime;

        // Anti flip-flop: with two players near the crosshair the score ranking
        // flips with per-tick noise, and the old `sinceLastSwitch > 0.45` rule
        // then let whichever one won that instant STEAL the lock every 450ms —
        // the aim visibly swapped back and forth. Now a switch needs ALL of:
        // dwell elapsed, challenger clearly better than the LOCKED target's
        // LIVE score (not a stale snapshot), and not bouncing straight back to
        // the target we just left.
        float lockedScoreNow = -FLT_MAX;
        if (lockedTarget != 0) {
            for (const AimTarget& t : allTargets) {
                if (t.entityKey == lockedTarget) {
                    lockedScoreNow = t.score;
                    break;
                }
            }
        }

        constexpr float kTargetSwitchDwellSec = 0.35f;
        constexpr float kTargetSwitchScoreMargin = 1.25f;
        constexpr float kTargetSwitchNoBounceSec = 1.5f;

        const bool dwellElapsed = sinceLastSwitch >= kTargetSwitchDwellSec;
        const bool challengerClearlyBetter =
            bestTarget->score > lockedScoreNow * kTargetSwitchScoreMargin;
        const bool bounceBack =
            previousTarget != 0
            && bestTarget->entityKey == previousTarget
            && sinceLastSwitch < kTargetSwitchNoBounceSec;

        const bool allowSwitch = lockedTarget == 0
            || (dwellElapsed && challengerClearlyBetter && !bounceBack);

        if (allowSwitch)
        {
            ++s_dbgSwitches;
            previousTarget = lockedTarget;
            lockedTarget = bestTarget->entityKey;
            lockedIsRobot = bestTarget->isRobot;
            lastSwitchTime = currentTime;
            lastTargetScore = bestTarget->score;
            humanizer.Reset();
            ResetAimMotionState();
            s_graceActive = false;
            s_graceTarget = *bestTarget;
        } else if (lockedTarget != 0) {
            // Keep hysteresis lock; re-find locked entry if present.
            for (auto& target : allTargets) {
                if (target.entityKey == lockedTarget) {
                    bestTarget = &target;
                    break;
                }
            }
        }
    } else {
        s_graceTarget = *bestTarget;
    }

    (void)previousTarget;
    (void)lockedIsRobot;

    s_aimDbg.locked = lockedTarget;

    if (!g_kmbox.EnsureReady()) {
        ++kmboxFailStreak;
        s_aimDbg.kmbox = 0;
        if (kmboxFailStreak >= 3) {
            if (!kmboxFailLogged) {
                std::cout << "[debugAim] kmbox not ready — clearing lock" << std::endl;
                kmboxFailLogged = true;
            }
            lockedTarget = 0;
            previousTarget = 0;
            humanizer.Reset();
            s_graceActive = false;
        }
        return;
    }
    kmboxFailStreak = 0;
    kmboxFailLogged = false;
    s_aimDbg.kmbox = 1;

    float dx = static_cast<float>(bestTarget->aimPos.x - screenCenter.x);
    float dy = static_cast<float>(bestTarget->aimPos.y - screenCenter.y);

    float pullScale = 1.f;
    float targetVelMag = 0.f;
    if (!bestTarget->isRobot) {
        std::shared_lock<std::shared_mutex> plock(m_playerCacheMutex);
        if (const auto it = playerCache.find(static_cast<uintptr_t>(bestTarget->entityKey));
            it != playerCache.end())
            targetVelMag = VelocityMag(it->second.cachedVelocity);
    } else {
        std::shared_lock<std::shared_mutex> rlock(m_robotCacheMutex);
        if (const auto it = robotCache.find(static_cast<uintptr_t>(bestTarget->entityKey));
            it != robotCache.end())
            targetVelMag = VelocityMag(it->second.cachedVelocity);
    }

    s_aimDbg.lastDx = dx;
    s_aimDbg.lastDy = dy;
    s_aimDbg.posSrc = bestTarget->aimPosSrc;
    s_aimDbg.worldAgeMs = bestTarget->aimWorldAgeMs;
    s_aimDbg.pullScale = pullScale;
    s_aimDbg.distPx = hypotf(
        static_cast<float>(bestTarget->aimPos.x - screenCenter.x),
        static_cast<float>(bestTarget->aimPos.y - screenCenter.y));
    if (targetVelMag > 0.f)
        s_aimDbg.velMag = targetVelMag;

    // Only suppress reload/flinch jitter when already close — never cancel catch-up.
    if (viewShakeDeg >= kAimViewShakeSuppressDeg && s_aimDbg.distPx < 35.f) {
        suppressAimOutput = true;
    }

    if (var::humanizer) {
        humanizer.NotifyError(dx, dy);
        humanizer.Apply(dx, dy);
        const Humanizer::PhaseOut hn = humanizer.Last();
        s_aimDbg.hnPhase = hn.phase;
        s_aimDbg.hnJx = hn.jx;
        s_aimDbg.hnJy = hn.jy;
    } else {
        s_aimDbg.hnPhase = 0;
        s_aimDbg.hnJx = 0.f;
        s_aimDbg.hnJy = 0.f;
    }

    // Reload / flinch moves the view — skip hardware pull for this tick (keep lock).
    s_dbgSuppress = suppressAimOutput;
    s_dbgShakeDeg = viewShakeDeg;
    if (!suppressAimOutput)
        s_aimDbg.lastGain = SendKmAimDelta(dx, dy, pullScale, &s_aimDbg.lastGain);

    // ============================================
    // TRIGGERBOT
    // ============================================
    {
        static uint64_t s_lastFireMs = 0;
        static bool s_isHolding = false;

        const bool trigEnabled = var::enable_triggerbot;
        const bool aimLocked = (lockedTarget != 0);

        // Hotkey check
        bool trigKeyActive = false;
        if (var::trigger_hold_mode == 2) {
            trigKeyActive = true; // Always on
        } else {
            const int bindTrig = var::trigger_hold_key ? var::trigger_hold_key : VK_SHIFT;
            if (var::trigger_hold_mode == 1) {
                // Toggle
                static bool trigToggled = false;
                static bool trigPrevHeld = false;
                const bool trigNowHeld = KeyBindIsHeld(bindTrig);
                if (trigNowHeld && !trigPrevHeld)
                    trigToggled = !trigToggled;
                trigPrevHeld = trigNowHeld;
                trigKeyActive = trigToggled;
            } else {
                // Hold
                trigKeyActive = KeyBindIsHeld(bindTrig);
            }
        }

        if (trigEnabled && aimLocked && trigKeyActive && !suppressAimOutput) {
            const float trigDistPx = hypotf(
                static_cast<float>(bestTarget->aimPos.x - screenCenter.x),
                static_cast<float>(bestTarget->aimPos.y - screenCenter.y));

            if (trigDistPx <= var::trigger_deadzone_px) {
                if (true) {
                    const uint64_t nowMsTrig = NowMs();
                    if (var::trigger_auto_hold) {
                        if (!s_isHolding) {
                            g_kmbox.HoldStart();
                            s_isHolding = true;
                        }
                    } else {
                        if (nowMsTrig - s_lastFireMs >= static_cast<uint64_t>(var::trigger_fire_delay_ms)) {
                            const int holdMs = (var::trigger_fire_delay_ms > 0 && var::trigger_fire_delay_ms < 50)
                                ? var::trigger_fire_delay_ms
                                : 22;
                            g_kmbox.Click(holdMs);
                            s_lastFireMs = nowMsTrig;
                        }
                    }
                }
            } else {
                if (s_isHolding) {
                    g_kmbox.HoldEnd();
                    s_isHolding = false;
                }
            }
        } else {
            if (s_isHolding) {
                g_kmbox.HoldEnd();
                s_isHolding = false;
            }
        }
    }

    if (var::show_debug_overlay) {
        static IntervalTimer aimDebugTimer(500);
        if (aimDebugTimer.fire()) {
            const char* srcTag = "none";
            if (s_aimDbg.posSrc == 1)
                srcTag = "frame";
            else if (s_aimDbg.posSrc == 2)
                srcTag = "cache";

            std::cout << "[debugAim] candidates=" << s_aimDbg.candidates
                << " locked=" << s_aimDbg.locked
                << " dx=" << s_aimDbg.lastDx
                << " dy=" << s_aimDbg.lastDy
                << " gain=" << s_aimDbg.lastGain
                << " kmbox=" << s_aimDbg.kmbox
                << " grace=" << s_aimDbg.grace
                << " inCand=" << s_aimDbg.lockedInCand
                << " robot=" << s_aimDbg.isRobot
                << " distPx=" << s_aimDbg.distPx
                << " vel=" << s_aimDbg.velMag
                << " drop=" << s_aimDbg.dropReason
                << " pull=" << s_aimDbg.pullScale
                << " viewShakeDeg=" << viewShakeDeg
                << " suppress=" << (suppressAimOutput ? 1 : 0)
                << " src=" << srcTag
                << " worldAgeMs=" << s_aimDbg.worldAgeMs
                << " camFov=" << aimProjCam.FOV
                << " frameValid=" << (espFrame.valid ? 1 : 0)
                << std::endl;

            if (var::debug_aim_shake) {
                std::cout << "[debugShakeSum] flips=(" << s_dbgFlipX
                    << "," << s_dbgFlipY << ")"
                    << " overshoot=" << s_dbgOvershoot
                    << " switches=" << s_dbgSwitches
                    << " cmd=(" << s_aimDbg.cmdX << "," << s_aimDbg.cmdY << ")"
                    << " pxPerMouse=" << s_aimDbg.pxPerMouse
                    << " closeFrac=" << s_aimDbg.closeFrac
                    << " tickMs=" << s_aimDbg.tickMs
                    << " damp=(" << s_oscDampX.damp << "," << s_oscDampY.damp << ")"
                    << std::endl;
                {
                    const uint64_t sumTs = NowMs();
                    std::ofstream lf(kArcVerifyPath, std::ios::app);
                    if (lf) {
                        lf << "{\"location\":\"Aimbot.cpp\"," 
                           << "\"message\":\"aim_shake_sum\"," 
                           << "\"data\":{\"flipX\":" << s_dbgFlipX
                           << ",\"flipY\":" << s_dbgFlipY
                           << ",\"overshoot\":" << s_dbgOvershoot
                           << ",\"switches\":" << s_dbgSwitches
                           << ",\"cmd\":[" << s_aimDbg.cmdX << "," << s_aimDbg.cmdY << "]"
                           << ",\"pxPerMouse\":" << s_aimDbg.pxPerMouse
                           << ",\"closeFrac\":" << s_aimDbg.closeFrac
                           << ",\"tickMs\":" << s_aimDbg.tickMs
                           << ",\"damp\":[" << s_oscDampX.damp << "," << s_oscDampY.damp << "]"
                           << "},\"ts\":" << sumTs << "}\n";
                    }
                }
                s_dbgFlipX = 0;
                s_dbgFlipY = 0;
                s_dbgOvershoot = 0;
                s_dbgSwitches = 0;
            }

            uint64_t dmaExec = 0, dmaPrep = 0, dmaLast = 0;
            DmaScatterStats_Get(dmaExec, dmaPrep, dmaLast);
            std::cout << "[debugDma] exec=" << dmaExec
                << " prep=" << dmaPrep
                << " lastBatch=" << dmaLast
                << std::endl;
        }
    }

}
