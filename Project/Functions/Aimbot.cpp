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
        if (actor.health <= 0.f) continue;
        if (var::visiblecheck && !actor.isVisible) continue;
        if (actor.Distance > var::aimbot_distance) continue;

        Vector3 aimPos{};
        Vector3 worldPos{};

        const auto& bones = actor.boneData;

        if (var::randombone)
        {
            const UniBone targetBone = GetSequentialBone(key, currentTime, bones);

            if (bones.valid.test(static_cast<size_t>(targetBone)))
                worldPos = bones.bonesWorldDouble[static_cast<size_t>(targetBone)];
            else if (bones.valid.test(static_cast<size_t>(UniBone::Head)))
                worldPos = bones.bonesWorldDouble[static_cast<size_t>(UniBone::Head)];
            else if (bones.valid.test(static_cast<size_t>(UniBone::Neck)))
                worldPos = bones.bonesWorldDouble[static_cast<size_t>(UniBone::Neck)];
            else
            {
                worldPos = actor.WorldPos;
                worldPos.z += 160.0;
            }
        }
        else
        {
            if (bones.valid.test(static_cast<size_t>(UniBone::Head)))
                worldPos = bones.bonesWorldDouble[static_cast<size_t>(UniBone::Head)];
            else if (bones.valid.test(static_cast<size_t>(UniBone::Neck)))
                worldPos = bones.bonesWorldDouble[static_cast<size_t>(UniBone::Neck)];
            else
            {
                worldPos = actor.WorldPos;
                worldPos.z += 160.0;
            }
        }

        if (var::predict)
        {
            Vector3 camLocation{};
            {
                std::shared_lock<std::shared_mutex> camLock(m_cameraMutex);
                camLocation = g_Camera.Location;
            }
            const Vector3 velocity = GetActorVelocity(actor.APawn);
            worldPos = PredictPosition(worldPos, velocity, camLocation, bulletSpeed, 3);
        }

        if (!ProjectWorldLocationToScreen(worldPos, aimPos, g_aimProjCam))
            continue;

        const float dx = static_cast<float>(aimPos.x - screenCenter.x);
        const float dy = static_cast<float>(aimPos.y - screenCenter.y);
        const float distToCenter = std::sqrt(dx * dx + dy * dy);

        if (distToCenter > fovRadius)
            continue;

        const float healthScore = (100.f - actor.health) * 1.5f;
        const float distanceScore = fovRadius - distToCenter;
        const float totalScore = healthScore + distanceScore;

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

        if (!GetRobotAimScreenFromDraw(*this, resolvedEntry, g_aimProjCam, aimPos, worldPos))
            return;

        const float dx = static_cast<float>(aimPos.x - screenCenter.x);
        const float dy = static_cast<float>(aimPos.y - screenCenter.y);
        const float distToCenter = std::sqrt(dx * dx + dy * dy);

        if (distToCenter > fovRadius)
            return;

        const float totalScore = fovRadius - distToCenter;

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
constexpr float kMaxAimStepFraction = 1.0f;
constexpr float kHumanizerSkipDistPx = 40.f;

float SendKmAimDelta(float dx, float dy, float* outGain = nullptr)
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
    if (gain < 0.05f)
        gain = 0.05f;
    if (gain > 8.f)
        gain = 8.f;

    // Extra pull when lagging behind a moving target (large pixel error).
    if (dist > 24.f)
        gain *= 1.f + (std::min)(dist, 160.f) / 160.f;

    if (var::aim_algorithm == AimAlgorithm::Accelerated) {
        const float norm = (std::min)(dist / 120.f, 1.f);
        gain *= (0.5f + 0.5f * norm * norm);
    }

    if (outGain)
        *outGain = gain;

    int remX = static_cast<int>(std::round(dx * gain));
    int remY = static_cast<int>(std::round(dy * gain));

    const float maxStep = dist * kMaxAimStepFraction;
    if (maxStep > 0.5f) {
        const float mag = hypotf(static_cast<float>(remX), static_cast<float>(remY));
        if (mag > maxStep) {
            const float scale = maxStep / mag;
            remX = static_cast<int>(std::round(static_cast<float>(remX) * scale));
            remY = static_cast<int>(std::round(static_cast<float>(remY) * scale));
        }
    }

    if (remX == 0 && remY == 0 && dist > 0.5f) {
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
};
static AimDebugSnapshot s_aimDbg;

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

    s_aimDbg = {};

    const bool playerAimEnabled = var::enable_aimbot;
    const bool robotAimEnabled = var::robotAimEnabled;

    if (!playerAimEnabled && !robotAimEnabled)
    {
        lockedTarget = 0;
        previousTarget = 0;
        humanizer.Reset();
        kmboxFailStreak = 0;
        return;
    }

    const int bindAim = var::aim_hold_key ? var::aim_hold_key : VK_SHIFT;
    const bool keyPressed = KeyBindIsHeld(bindAim);

    if (!keyPressed)
    {
        lockedTarget = 0;
        previousTarget = 0;
        humanizer.Reset();
        kmboxFailStreak = 0;
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
        return;
    }

    if (!IsValidPointer(sPlayerController) || !IsValidPointer(sAcknowledgedPawn))
    {
        lockedTarget = 0;
        previousTarget = 0;
        humanizer.Reset();
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

    EspRenderFrame espFrame{};
    {
        std::shared_lock<std::shared_mutex> lock(m_espFrameMutex);
        espFrame = m_lastEspFrame;
    }

    CameraCache aimProjCam = aimCam;
    if (espFrame.valid
        && IsPlausibleWorldPos(espFrame.camera.Location)
        && espFrame.camera.FOV > 1.f && espFrame.camera.FOV < 179.f)
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

    const float currentTime = GetTimeSeconds();
    const float bulletSpeed = var::aim_bullet_speed_cm_s > 0.f
        ? var::aim_bullet_speed_cm_s
        : 80000.f;

    std::vector<AimTarget> allTargets;
    allTargets.reserve(64);

    if (playerAimEnabled)
        AimAssistPlayer(screenCenter, fovRadius, bulletSpeed, currentTime, allTargets);

    if (robotAimEnabled)
        AimAssistRobot(screenCenter, fovRadius, currentTime, allTargets);

    g_aimEspFrame = nullptr;

    if (lockedTarget != 0) {
        bool lockedInCandidates = false;
        for (const AimTarget& target : allTargets) {
            if (target.entityKey == lockedTarget) {
                lockedInCandidates = true;
                break;
            }
        }
        if (!lockedInCandidates) {
            lockedTarget = 0;
            previousTarget = 0;
            humanizer.Reset();
            s_aimDbg.locked = 0;
            return;
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

        float priorityBonus = 0.f;
        const bool priorizePlayers = true;
        if (!target.isRobot && priorizePlayers)
            priorityBonus = 1000.f;

        const float adjustedScore = target.score + priorityBonus;

        if (adjustedScore > bestScore ||
            (adjustedScore == bestScore &&
                target.distToCenter < (bestTarget ? bestTarget->distToCenter : FLT_MAX)))
        {
            bestScore = adjustedScore;
            bestTarget = &target;
        }
    }

    if (!bestTarget)
    {
        lockedTarget = 0;
        previousTarget = 0;
        humanizer.Reset();
        s_aimDbg.locked = 0;
        return;
    }

    s_aimDbg.candidates = static_cast<int>(allTargets.size());

    if (bestTarget->worldPos.x == 0.0 && bestTarget->worldPos.y == 0.0 && bestTarget->worldPos.z == 0.0)
        return;

    if (lockedTarget != bestTarget->entityKey)
    {
        const float sinceLastSwitch = currentTime - lastSwitchTime;

        if (lockedTarget == 0 ||
            bestTarget->score > lastTargetScore * 1.2f ||
            sinceLastSwitch > 0.45f)
        {
            previousTarget = lockedTarget;
            lockedTarget = bestTarget->entityKey;
            lockedIsRobot = bestTarget->isRobot;
            lastSwitchTime = currentTime;
            lastTargetScore = bestTarget->score;
            humanizer.Reset();
        }
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
        }
        return;
    }
    kmboxFailStreak = 0;
    kmboxFailLogged = false;
    s_aimDbg.kmbox = 1;

    float dx = static_cast<float>(bestTarget->aimPos.x - screenCenter.x);
    float dy = static_cast<float>(bestTarget->aimPos.y - screenCenter.y);

    s_aimDbg.lastDx = dx;
    s_aimDbg.lastDy = dy;
    s_aimDbg.posSrc = bestTarget->aimPosSrc;
    s_aimDbg.worldAgeMs = bestTarget->aimWorldAgeMs;

    if (var::humanizer && !bestTarget->isRobot && hypotf(dx, dy) >= kHumanizerSkipDistPx) {
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

    s_aimDbg.lastGain = SendKmAimDelta(dx, dy, &s_aimDbg.lastGain);

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
                << " src=" << srcTag
                << " worldAgeMs=" << s_aimDbg.worldAgeMs
                << " camFov=" << aimProjCam.FOV
                << " frameValid=" << (espFrame.valid ? 1 : 0)
                << std::endl;
        }
    }
}
