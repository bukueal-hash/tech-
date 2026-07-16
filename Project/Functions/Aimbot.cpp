// Aimbot.cpp — baseline cache + ESP-matched screen aim, KmBox hardware output only.
#include "../Core/Engine.h"
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

static const UniBone g_SequentialBones[] = {
    UniBone::Pelvis,
    UniBone::Spine1,
    UniBone::Spine2,
    UniBone::Spine3,
    UniBone::Chest,
    UniBone::Neck,
    UniBone::Head
};
static constexpr int g_SequentialBonesCount = sizeof(g_SequentialBones) / sizeof(g_SequentialBones[0]);

struct SequentialBoneState {
    uint64_t targetKey = 0;
    int currentIndex = 0;
    float lastChangeTime = 0.f;
    float changeInterval = 2.0f;
    bool ascending = true;
};
static SequentialBoneState g_seqBoneState;

static UniBone GetSequentialBone(uint64_t targetKey, float currentTime, const BoneData& bones)
{
    if (g_seqBoneState.targetKey != targetKey)
    {
        g_seqBoneState.targetKey = targetKey;
        g_seqBoneState.currentIndex = g_SequentialBonesCount - 1;
        g_seqBoneState.lastChangeTime = currentTime;
        g_seqBoneState.ascending = false;
    }

    const float elapsed = currentTime - g_seqBoneState.lastChangeTime;
    if (elapsed >= g_seqBoneState.changeInterval)
    {
        g_seqBoneState.lastChangeTime = currentTime;

        if (g_seqBoneState.ascending)
        {
            g_seqBoneState.currentIndex++;
            if (g_seqBoneState.currentIndex >= g_SequentialBonesCount)
            {
                g_seqBoneState.currentIndex = g_SequentialBonesCount - 2;
                g_seqBoneState.ascending = false;
            }
        }
        else
        {
            g_seqBoneState.currentIndex--;
            if (g_seqBoneState.currentIndex < 0)
            {
                g_seqBoneState.currentIndex = 1;
                g_seqBoneState.ascending = true;
            }
        }
    }

    const UniBone targetBone = g_SequentialBones[g_seqBoneState.currentIndex];

    if (!bones.valid.test(static_cast<size_t>(targetBone)))
    {
        for (int i = 0; i < g_SequentialBonesCount; i++)
        {
            const int checkIdx = (g_seqBoneState.currentIndex + i) % g_SequentialBonesCount;
            if (bones.valid.test(static_cast<size_t>(g_SequentialBones[checkIdx])))
                return g_SequentialBones[checkIdx];
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
        const UniBone bone = GetSequentialBone(key, currentTime, bones);
        if (TryBoneWorld(bones, bone, outWorld))
            return true;
        return fallbackTorso();
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
                found = true;
            }
        }
        if (found) {
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

/** Projected body soft-zone: crosshair inside = on-target (shoot-around); outside = fasten. */
struct SoftBodyScreenZone {
    bool valid = false;
    float cx = 0.f;
    float cy = 0.f;
    float halfW = 0.f;
    float halfH = 0.f;
};

static bool CrosshairInSoftBodyZone(
    const Vector3& screenCenter,
    const SoftBodyScreenZone& z)
{
    if (!z.valid || z.halfW < 4.f || z.halfH < 8.f)
        return false;
    const float dx = std::abs(static_cast<float>(screenCenter.x) - z.cx);
    const float dy = std::abs(static_cast<float>(screenCenter.y) - z.cy);
    return dx <= z.halfW && dy <= z.halfH;
}

static SoftBodyScreenZone BuildPlayerSoftBodyZone(
    Engine& eng,
    const BoneData& bones,
    const Vector3& worldFallback,
    const Engine::CameraCache& cam)
{
    SoftBodyScreenZone z{};
    Vector3 headW{};
    Vector3 feetW{};
    bool headOk = TryBoneWorld(bones, UniBone::Head, headW)
        || TryBoneWorld(bones, UniBone::Neck, headW);
    bool feetOk = TryBoneWorld(bones, UniBone::FootL, feetW)
        || TryBoneWorld(bones, UniBone::FootR, feetW)
        || TryBoneWorld(bones, UniBone::Pelvis, feetW);
    if (!headOk && IsPlausibleWorldPos(worldFallback)) {
        headW = worldFallback;
        headW.z += 90.0;
        headOk = true;
    }
    if (!feetOk && IsPlausibleWorldPos(worldFallback)) {
        feetW = worldFallback;
        feetW.z -= 90.0;
        feetOk = true;
    }
    if (!headOk || !feetOk)
        return z;

    Vector3 headS{};
    Vector3 feetS{};
    if (!eng.ProjectWorldLocationToScreen(headW, headS, cam)
        || !eng.ProjectWorldLocationToScreen(feetW, feetS, cam))
        return z;

    const float top = static_cast<float>((std::min)(headS.y, feetS.y));
    const float bot = static_cast<float>((std::max)(headS.y, feetS.y));
    const float midX = static_cast<float>((headS.x + feetS.x) * 0.5);
    const float h = bot - top;
    if (h < 16.f)
        return z;

    z.valid = true;
    z.cx = midX;
    z.cy = (top + bot) * 0.5f;
    z.halfH = h * 0.55f;
    z.halfW = h * 0.28f;
    if (z.halfW < 18.f)
        z.halfW = 18.f;
    return z;
}

static SoftBodyScreenZone BuildRobotSoftBodyZone(
    Engine& eng,
    const Engine::WorldCacheEntry& robot,
    const Engine::CameraCache& cam)
{
    SoftBodyScreenZone z{};
    Vector3 headW{};
    Vector3 feetW{};
    if (!EspDraw::ResolveBotHeadFeetWorld(robot, headW, feetW))
        return z;
    Vector3 headS{};
    Vector3 feetS{};
    if (!eng.ProjectWorldLocationToScreen(headW, headS, cam)
        || !eng.ProjectWorldLocationToScreen(feetW, feetS, cam))
        return z;
    const float top = static_cast<float>((std::min)(headS.y, feetS.y));
    const float bot = static_cast<float>((std::max)(headS.y, feetS.y));
    const float midX = static_cast<float>((headS.x + feetS.x) * 0.5);
    const float h = bot - top;
    if (h < 12.f)
        return z;
    z.valid = true;
    z.cx = midX;
    z.cy = (top + bot) * 0.5f;
    z.halfH = h * 0.55f;
    z.halfW = h * 0.32f;
    if (z.halfW < 14.f)
        z.halfW = 14.f;
    return z;
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
        if (isRobot)
            return fovScore;
        return lowHpScore * 1.5f + fovScore;
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
// MOTOR SYNERGY HUMANIZER (pixel jitter)
// ============================================

class MotorSynergyHumanizer {
private:
    struct HumanizerConfig {
        double ou_theta = 2.0;
        double ou_sigma = 0.3;
        double tremor_freq_min = 8.0;
        double tremor_freq_max = 12.0;
        double tremor_amp_min = 0.02;
        double tremor_amp_max = 0.05;
        double sdn_k = 0.005;
    };

    HumanizerConfig cfg;
    double ou_x = 0.0;
    double ou_y = 0.0;
    double tremor_freq = 10.0;
    double tremor_phase_x = 0.0;
    double tremor_phase_y = 0.0;
    double last_time_ms = 0.0;
    bool initialized = false;
    std::mt19937_64 rng{ std::random_device{}() };

    static double GetTimeMs()
    {
        using namespace std::chrono;
        static auto start = steady_clock::now();
        return duration_cast<duration<double, std::milli>>(steady_clock::now() - start).count();
    }

public:
    void Reset()
    {
        ou_x = 0.0;
        ou_y = 0.0;
        initialized = false;

        std::uniform_real_distribution<double> freqDist(cfg.tremor_freq_min, cfg.tremor_freq_max);
        std::uniform_real_distribution<double> phaseDist(0.0, 2.0 * std::numbers::pi);
        tremor_freq = freqDist(rng);
        tremor_phase_x = phaseDist(rng);
        tremor_phase_y = phaseDist(rng);
    }

    void ApplyToRotation(double& pitch, double& yaw, double movementSpeed)
    {
        const double current_time = GetTimeMs();
        if (!initialized) {
            last_time_ms = current_time;
            initialized = true;
            return;
        }

        const double dt_ms = current_time - last_time_ms;
        if (dt_ms <= 0.0 || dt_ms > 100.0) {
            last_time_ms = current_time;
            return;
        }
        last_time_ms = current_time;

        const double dt_s = dt_ms / 1000.0;
        const double t_s = current_time / 1000.0;
        std::normal_distribution<double> normal(0.0, 1.0);

        ou_x += -cfg.ou_theta * ou_x * dt_s + cfg.ou_sigma * std::sqrt(dt_s) * normal(rng);
        ou_y += -cfg.ou_theta * ou_y * dt_s + cfg.ou_sigma * std::sqrt(dt_s) * normal(rng);

        const double speed_factor = 1.0 / (1.0 + movementSpeed * 0.5);
        std::uniform_real_distribution<double> ampDist(cfg.tremor_amp_min, cfg.tremor_amp_max);
        const double tremor_amp = ampDist(rng);

        const double tremor_x = tremor_amp * speed_factor *
            std::sin(2.0 * std::numbers::pi * tremor_freq * t_s + tremor_phase_x);
        const double tremor_y = tremor_amp * speed_factor *
            std::sin(2.0 * std::numbers::pi * tremor_freq * t_s + tremor_phase_y);

        const double sdn_x = cfg.sdn_k * movementSpeed * normal(rng);
        const double sdn_y = cfg.sdn_k * movementSpeed * normal(rng);

        pitch += ou_x + tremor_x + sdn_x;
        yaw += ou_y + tremor_y + sdn_y;
    }
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
    Vector3 predicted = targetPos;

    for (int i = 0; i < iterations; i++)
    {
        Vector3 delta = {
            predicted.x - myPos.x,
            predicted.y - myPos.y,
            predicted.z - myPos.z
        };

        const float distance = static_cast<float>(std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z));

        if (distance < 1.0f)
            break;

        const float timeToHit = distance / bulletSpeed;

        predicted = {
            targetPos.x + targetVelocity.x * timeToHit,
            targetPos.y + targetVelocity.y * timeToHit,
            targetPos.z + targetVelocity.z * timeToHit
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

static void ExtrapolateRobotWorldPosToNow(Engine::WorldCacheEntry& entry)
{
    if (entry.lastVelocityUpdate <= 0.f)
        return;

    const uint64_t nowMs = NowMs();
    const float dtSec =
        (nowMs - static_cast<uint64_t>(entry.lastVelocityUpdate)) * 0.001f;
    if (dtSec <= 0.f || dtSec > 0.25f)
        return;

    const Vector3 vel = entry.cachedVelocity;
    const Vector3 delta{
        vel.x * dtSec,
        vel.y * dtSec,
        vel.z * dtSec
    };

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
    if (!IsPlausibleWorldPos(worldPos) || lastVelocityUpdate <= 0.f)
        return;
    const uint64_t nowMs = NowMs();
    const float dtSec =
        (nowMs - static_cast<uint64_t>(lastVelocityUpdate)) * 0.001f;
    if (dtSec <= 0.f || dtSec > 0.20f)
        return;
    worldPos.x += cachedVelocity.x * dtSec;
    worldPos.y += cachedVelocity.y * dtSec;
    worldPos.z += cachedVelocity.z * dtSec;
}

static float VelocityMag(const Vector3& v)
{
    return static_cast<float>(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
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
    std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
    for (const auto& [key, actor] : playerCache)
    {
        if (!actor.Drawing) continue;
        // Do not hard-skip health<=0 — same soft policy as ShouldDrawPlayerEsp
        // (HealthInfo@PS+0x530 is wrong; health often reads 0 while boxes still draw).
        if (var::visiblecheck && !actor.isVisible) continue;
        if (actor.Distance > var::aimbot_distance) continue;

        Vector3 aimPos{};
        Vector3 worldPos{};

        const auto& bones = actor.boneData;
        if (!ResolvePlayerAimBoneWorldTwoPhase(
                *this, bones, key, currentTime, screenCenter, fovRadius, g_aimProjCam, worldPos))
        {
            worldPos = actor.WorldPos;
            worldPos.z += 160.0;
        }

        // Match ESP PositionRefresh lead so bones aren't left behind moving pawns.
        ExtrapolatePlayerAimWorldToNow(
            worldPos, actor.cachedVelocity, actor.lastVelocityUpdate);

        Vector3 velocity = actor.cachedVelocity;
        if (VelocityMag(velocity) < 5.f) {
            const Vector3 dmaVel = GetActorVelocity(actor.APawn);
            if (VelocityMag(dmaVel) > VelocityMag(velocity))
                velocity = dmaVel;
        }

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
            continue;

        const float dx = static_cast<float>(aimPos.x - screenCenter.x);
        const float dy = static_cast<float>(aimPos.y - screenCenter.y);
        const float distToCenter = std::sqrt(dx * dx + dy * dy);

        if (!AimStickyBypassFov(key)
            && distToCenter > EffectiveAimFovForKey(key, fovRadius))
            continue;

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
    }
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
// Soft fasten (not ban-risk hard snap): enough mouse budget to reach body, capped.
constexpr float kMaxAimStepFraction = 1.50f;
constexpr float kHumanizerSkipDistPx = 40.f;
/** Per-tick view rotation jump — reload / flinch; don't fight the animation. */
constexpr float kAimViewShakeSuppressDeg = 1.35f;
/** ESP-frame camera only when live view is stable (avoids stale-cam jitter). */
constexpr float kAimEspFrameCamMaxDriftDeg = 0.65f;
/** When crosshair is already on the model, keep tracking tiny (shoot-around). */
constexpr float kOnBodyPullScale = 0.12f;

float SendKmAimDelta(float dx, float dy, float pullScale = 1.f, float* outGain = nullptr)
{
    const float dist = hypotf(dx, dy);
    if (dist <= var::aim_deadzone_px)
        return 0.f;

    float speed = var::aim_hardware_speed;
    if (speed < 1.f)
        speed = 1.f;
    const float smooth = (std::max)(var::smoothness, 1.f);
    float sens = var::aim_sensitivity;
    if (sens < 0.25f)
        sens = 0.25f;

    float gain = (speed / 10.f) / (smooth * sens);
    if (gain < 0.06f)
        gain = 0.06f;
    if (gain > 10.f)
        gain = 10.f;

    // Progressive fasten while off-body — not a teleport.
    if (dist > 20.f)
        gain *= 1.05f + (std::min)(dist, 220.f) / 220.f;

    if (var::aim_algorithm == AimAlgorithm::Accelerated) {
        const float norm = (std::min)(dist / 100.f, 1.f);
        gain *= (0.8f + 0.45f * norm * norm);
    }

    if (pullScale < 0.f)
        pullScale = 0.f;
    if (pullScale > 1.f)
        pullScale = 1.f;
    gain *= pullScale;

    if (outGain)
        *outGain = gain;

    if (gain < 0.01f)
        return gain;

    int remX = static_cast<int>(std::round(dx * gain));
    int remY = static_cast<int>(std::round(dy * gain));

    const float maxStep = dist * kMaxAimStepFraction * (std::max)(pullScale, 0.15f);
    if (maxStep > 0.5f) {
        const float mag = hypotf(static_cast<float>(remX), static_cast<float>(remY));
        if (mag > maxStep) {
            const float scale = maxStep / mag;
            remX = static_cast<int>(std::round(static_cast<float>(remX) * scale));
            remY = static_cast<int>(std::round(static_cast<float>(remY) * scale));
        }
    }

    if (remX == 0 && remY == 0 && dist > 0.5f && pullScale > 0.05f) {
        remX = (dx > 0.f) ? 1 : ((dx < 0.f) ? -1 : 0);
        remY = (dy > 0.f) ? 1 : ((dy < 0.f) ? -1 : 0);
    }
    if (remX == 0 && remY == 0)
        return gain;

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
    return gain;
}

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
    // #region agent log
    int lockedInCand = -1;
    int isRobot = -1;
    float distPx = -1.f;
    float velMag = -1.f;
    float stickyFov = -1.f;
    int dropReason = 0; // 0 none, 1 notInCand, 2 graceExpire, 3 noBest, 4 kmbox
    int onBody = -1;    // 1 crosshair in soft body zone
    int zoneOk = 0;
    float pullScale = 1.f;
    float bodyEdge = -1.f; // 0 center .. 1 edge of soft zone
    // #endregion
};
static AimDebugSnapshot s_aimDbg;

// #region agent log
static void WriteAimTrackNdjson(const AimDebugSnapshot& d, bool suppress)
{
    static auto s_last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    // Always emit on drop; otherwise throttle.
    const bool force = d.dropReason != 0;
    if (!force
        && s_last.time_since_epoch().count() != 0
        && now - s_last < std::chrono::milliseconds(250))
        return;
    s_last = now;
    std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
    if (!f)
        return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"soft-body\",\"hypothesisId\":\"G\""
      << ",\"location\":\"Aimbot.cpp:AimAssistence\",\"message\":\"aim_lock_track\""
      << ",\"data\":{\"candidates\":" << d.candidates
      << ",\"locked\":" << d.locked
      << ",\"inCand\":" << d.lockedInCand
      << ",\"dx\":" << d.lastDx
      << ",\"dy\":" << d.lastDy
      << ",\"distPx\":" << d.distPx
      << ",\"gain\":" << d.lastGain
      << ",\"kmbox\":" << d.kmbox
      << ",\"grace\":" << d.grace
      << ",\"drop\":" << d.dropReason
      << ",\"isRobot\":" << d.isRobot
      << ",\"velMag\":" << d.velMag
      << ",\"worldAgeMs\":" << d.worldAgeMs
      << ",\"posSrc\":" << static_cast<int>(d.posSrc)
      << ",\"sticky\":" << (var::sticky_target_lock ? 1 : 0)
      << ",\"stickyFovPx\":" << d.stickyFov
      << ",\"predict\":" << (var::predict ? 1 : 0)
      << ",\"smooth\":" << var::smoothness
      << ",\"hwSpeed\":" << var::aim_hardware_speed
      << ",\"fovDeg\":" << var::aimbot_fov
      << ",\"graceMs\":" << var::aim_loss_of_sight_grace_ms
      << ",\"suppress\":" << (suppress ? 1 : 0)
      << ",\"onBody\":" << d.onBody
      << ",\"zoneOk\":" << d.zoneOk
      << ",\"pullScale\":" << d.pullScale
      << ",\"bodyEdge\":" << d.bodyEdge
      << "},\"timestamp\":" << ms << "}\n";
}
// #endregion

} // namespace

void Engine::AimAssistence()
{
    static uint64_t lockedTarget = 0;
    static uint64_t previousTarget = 0;
    static float lastSwitchTime = 0.f;
    static float lastTargetScore = 0.f;
    static bool lockedIsRobot = false;
    static MotorSynergyHumanizer humanizer;
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
    const bool suppressAimOutput = viewShakeDeg >= kAimViewShakeSuppressDeg;

    EspRenderFrame espFrame{};
    {
        std::shared_lock<std::shared_mutex> lock(m_espFrameMutex);
        espFrame = m_lastEspFrame;
    }

    CameraCache aimProjCam = aimCam;
    if (!suppressAimOutput
        && espFrame.valid
        && IsPlausibleWorldPos(espFrame.camera.Location)
        && espFrame.camera.FOV > 1.f && espFrame.camera.FOV < 179.f
        && CameraRotDeltaDeg(aimCam.Rotation, espFrame.camera.Rotation)
            <= kAimEspFrameCamMaxDriftDeg)
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
    const float bulletSpeed = var::aim_bullet_speed_cm_s > 0.f
        ? var::aim_bullet_speed_cm_s
        : 80000.f;
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

    // #region agent log
    s_aimDbg.candidates = static_cast<int>(allTargets.size());
    // #endregion

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
                    std::shared_lock<std::shared_mutex> plock(m_playerCacheMutex);
                    if (const auto it = playerCache.find(static_cast<uintptr_t>(target.entityKey));
                        it != playerCache.end()
                        && VelocityMag(it->second.cachedVelocity) >= 5.f)
                        s_graceVelocity = it->second.cachedVelocity;
                    else
                        s_graceVelocity = GetActorVelocity(
                            static_cast<uintptr_t>(target.entityKey));
                }
                break;
            }
        }
        // #region agent log
        s_aimDbg.lockedInCand = lockedInCandidates ? 1 : 0;
        // #endregion
        if (!lockedInCandidates) {
            // #region agent log
            s_aimDbg.dropReason = 1;
            // #endregion
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
                        grace.worldPos.x += s_graceVelocity.x * dtSec;
                        grace.worldPos.y += s_graceVelocity.y * dtSec;
                        grace.worldPos.z += s_graceVelocity.z * dtSec;
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
                    // #region agent log
                    s_aimDbg.dropReason = 2;
                    WriteAimTrackNdjson(s_aimDbg, suppressAimOutput);
                    // #endregion
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
                // #region agent log
                s_aimDbg.dropReason = 1;
                WriteAimTrackNdjson(s_aimDbg, suppressAimOutput);
                // #endregion
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
            bestTarget = &target;
            break;
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
        // #region agent log
        s_aimDbg.dropReason = 3;
        WriteAimTrackNdjson(s_aimDbg, suppressAimOutput);
        // #endregion
        return;
    }

    s_aimDbg.candidates = static_cast<int>(allTargets.size());
    // #region agent log
    s_aimDbg.isRobot = bestTarget->isRobot ? 1 : 0;
    s_aimDbg.distPx = bestTarget->distToCenter;
    {
        const Vector3& v = s_graceVelocity;
        s_aimDbg.velMag = static_cast<float>(
            std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z));
    }
    s_aimDbg.stickyFov = fovRadius + (var::sticky_target_lock
        ? (std::max)(0.f, var::aim_sticky_fov_bias_px)
        : 0.f);
    // #endregion

    if (bestTarget->worldPos.x == 0.0 && bestTarget->worldPos.y == 0.0 && bestTarget->worldPos.z == 0.0)
        return;

    if (lockedTarget != bestTarget->entityKey)
    {
        const float sinceLastSwitch = currentTime - lastSwitchTime;
        const bool allowSwitch = !var::sticky_target_lock
            || lockedTarget == 0
            || bestTarget->score > lastTargetScore * 1.2f
            || sinceLastSwitch > 0.45f;

        if (allowSwitch)
        {
            previousTarget = lockedTarget;
            lockedTarget = bestTarget->entityKey;
            lockedIsRobot = bestTarget->isRobot;
            lastSwitchTime = currentTime;
            lastTargetScore = bestTarget->score;
            humanizer.Reset();
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
            // #region agent log
            s_aimDbg.dropReason = 4;
            WriteAimTrackNdjson(s_aimDbg, suppressAimOutput);
            // #endregion
        }
        return;
    }
    kmboxFailStreak = 0;
    kmboxFailLogged = false;
    s_aimDbg.kmbox = 1;

    float dx = static_cast<float>(bestTarget->aimPos.x - screenCenter.x);
    float dy = static_cast<float>(bestTarget->aimPos.y - screenCenter.y);

    // Soft body zone: fasten from outside; free shoot-around once crosshair is on model.
    SoftBodyScreenZone bodyZone{};
    float pullScale = 1.f;
    int onBody = 0;
    float bodyEdge = -1.f;
    if (!bestTarget->isRobot) {
        std::shared_lock<std::shared_mutex> plock(m_playerCacheMutex);
        if (const auto it = playerCache.find(static_cast<uintptr_t>(bestTarget->entityKey));
            it != playerCache.end()) {
            bodyZone = BuildPlayerSoftBodyZone(
                *this, it->second.boneData, it->second.WorldPos, aimProjCam);
        }
    } else {
        std::shared_lock<std::shared_mutex> rlock(m_robotCacheMutex);
        if (const auto it = robotCache.find(static_cast<uintptr_t>(bestTarget->entityKey));
            it != robotCache.end()) {
            bodyZone = BuildRobotSoftBodyZone(*this, it->second, aimProjCam);
        }
    }

    if (bodyZone.valid && CrosshairInSoftBodyZone(screenCenter, bodyZone)) {
        onBody = 1;
        const float ox = std::abs(static_cast<float>(screenCenter.x) - bodyZone.cx)
            / (std::max)(bodyZone.halfW, 1.f);
        const float oy = std::abs(static_cast<float>(screenCenter.y) - bodyZone.cy)
            / (std::max)(bodyZone.halfH, 1.f);
        bodyEdge = (std::max)(ox, oy);
        if (bodyEdge < 0.72f) {
            // On model — do not magnet to one bone; allow shoot-around.
            pullScale = kOnBodyPullScale;
            dx = 0.f;
            dy = 0.f;
        } else {
            // Near silhouette edge — soft inward retention so lock doesn't fall off.
            dx = bodyZone.cx - static_cast<float>(screenCenter.x);
            dy = bodyZone.cy - static_cast<float>(screenCenter.y);
            pullScale = 0.30f;
        }
    }

    s_aimDbg.lastDx = dx;
    s_aimDbg.lastDy = dy;
    s_aimDbg.posSrc = bestTarget->aimPosSrc;
    s_aimDbg.worldAgeMs = bestTarget->aimWorldAgeMs;
    // #region agent log
    s_aimDbg.onBody = onBody;
    s_aimDbg.zoneOk = bodyZone.valid ? 1 : 0;
    s_aimDbg.pullScale = pullScale;
    s_aimDbg.bodyEdge = bodyEdge;
    s_aimDbg.distPx = hypotf(
        static_cast<float>(bestTarget->aimPos.x - screenCenter.x),
        static_cast<float>(bestTarget->aimPos.y - screenCenter.y));
    // #endregion

    if (var::humanizer && !bestTarget->isRobot
        && pullScale > 0.5f
        && hypotf(dx, dy) >= kHumanizerSkipDistPx) {
        const float movePx = hypotf(dx, dy);
        double jitterX = 0.0;
        double jitterY = 0.0;
        humanizer.ApplyToRotation(
            jitterX,
            jitterY,
            static_cast<double>(movePx) * 0.02);
        dx += static_cast<float>(jitterX * 2.5);
        dy += static_cast<float>(jitterY * 2.5);
    }

    // Reload / flinch moves the view — skip hardware pull for this tick (keep lock).
    if (!suppressAimOutput)
        s_aimDbg.lastGain = SendKmAimDelta(dx, dy, pullScale, &s_aimDbg.lastGain);

    // Triggerbot: fire when locked and within deadzone (or very close), if hardware
    // can report physical buttons and user isn't already holding LMB.
    if (var::enable_triggerbot && g_kmbox.FiringProxyAvailable()) {
        const float onTargetPx = (std::max)(var::aim_deadzone_px, 4.f);
        const float distPx = hypotf(
            static_cast<float>(bestTarget->aimPos.x - screenCenter.x),
            static_cast<float>(bestTarget->aimPos.y - screenCenter.y));
        const bool onBodyFire = onBody != 0;
        if ((distPx <= onTargetPx || onBodyFire) && !g_kmbox.IsPhysicalLeftDown()) {
            static auto s_lastTriggerClick = std::chrono::steady_clock::time_point{};
            const auto nowTp = std::chrono::steady_clock::now();
            if (s_lastTriggerClick.time_since_epoch().count() == 0
                || std::chrono::duration_cast<std::chrono::milliseconds>(
                       nowTp - s_lastTriggerClick).count() >= 80) {
                g_kmbox.LeftClick();
                s_lastTriggerClick = nowTp;
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
                << " onBody=" << s_aimDbg.onBody
                << " pull=" << s_aimDbg.pullScale
                << " edge=" << s_aimDbg.bodyEdge
                << " zone=" << s_aimDbg.zoneOk
                << " viewShakeDeg=" << viewShakeDeg
                << " suppress=" << (suppressAimOutput ? 1 : 0)
                << " src=" << srcTag
                << " worldAgeMs=" << s_aimDbg.worldAgeMs
                << " camFov=" << aimProjCam.FOV
                << " frameValid=" << (espFrame.valid ? 1 : 0)
                << std::endl;
        }
    }

    // #region agent log
    WriteAimTrackNdjson(s_aimDbg, suppressAimOutput);
    // #endregion
}
