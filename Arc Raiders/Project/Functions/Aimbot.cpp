// Aimbot.cpp
#include "../Core/Engine.h"
#include <random>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <numbers>

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
    // Se mudou de alvo, reseta para Head (começa do topo)
    if (g_seqBoneState.targetKey != targetKey)
    {
        g_seqBoneState.targetKey = targetKey;
        g_seqBoneState.currentIndex = g_SequentialBonesCount - 1; // Começa no Head
        g_seqBoneState.lastChangeTime = currentTime;
        g_seqBoneState.ascending = false; // Começa descendo
    }

    // Verifica se é hora de trocar de bone
    float elapsed = currentTime - g_seqBoneState.lastChangeTime;
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
                g_seqBoneState.currentIndex = 1; // Volta um
                g_seqBoneState.ascending = true; // Começa a subir
            }
        }
    }

    // Pega o bone atual
    UniBone targetBone = g_SequentialBones[g_seqBoneState.currentIndex];

    // Verifica se o bone existe, se não, procura o próximo válido
    if (!bones.valid.test((size_t)targetBone))
    {
        // Procura o bone válido mais próximo
        for (int i = 0; i < g_SequentialBonesCount; i++)
        {
            int checkIdx = (g_seqBoneState.currentIndex + i) % g_SequentialBonesCount;
            if (bones.valid.test((size_t)g_SequentialBones[checkIdx]))
            {
                return g_SequentialBones[checkIdx];
            }
        }
        // Fallback para Head
        return UniBone::Head;
    }

    return targetBone;
}

// ============================================
// MOTOR SYNERGY HUMANIZER (CORRIGIDO)
// ============================================

class MotorSynergyHumanizer {
private:
    // Configuração do motor_synergy (valores reduzidos para rotação)
    motor_synergy::config cfg;

    // Estado do Ornstein-Uhlenbeck
    double ou_x = 0.0;
    double ou_y = 0.0;

    // Tremor
    double tremor_freq = 10.0;
    double tremor_phase_x = 0.0;
    double tremor_phase_y = 0.0;

    // Timing
    double last_time_ms = 0.0;
    bool initialized = false;

    // RNG
    std::mt19937_64 rng{ std::random_device{}() };

    double GetTimeMs() {
        using namespace std::chrono;
        static auto start = steady_clock::now();
        return duration_cast<duration<double, std::milli>>(steady_clock::now() - start).count();
    }

public:
    MotorSynergyHumanizer()
    {
        // Configuração MUITO sutil para rotação (graus são sensíveis)
        cfg.ou_theta = 2.0;        // Reversão mais lenta
        cfg.ou_sigma = 0.3;        // Drift BEM menor
        cfg.tremor_freq_min = 8.0;
        cfg.tremor_freq_max = 12.0;
        cfg.tremor_amp_min = 0.02; // Tremor MUITO sutil
        cfg.tremor_amp_max = 0.05;
        cfg.sdn_k = 0.005;         // SDN bem baixo
    }

    void Reset()
    {
        ou_x = 0.0;
        ou_y = 0.0;
        initialized = false;

        // Randomiza parâmetros
        std::uniform_real_distribution<double> freqDist(cfg.tremor_freq_min, cfg.tremor_freq_max);
        std::uniform_real_distribution<double> phaseDist(0.0, 2.0 * std::numbers::pi);

        tremor_freq = freqDist(rng);
        tremor_phase_x = phaseDist(rng);
        tremor_phase_y = phaseDist(rng);
    }

    void ApplyToRotation(double& pitch, double& yaw, double movementSpeed)
    {
        double current_time = GetTimeMs();

        if (!initialized)
        {
            last_time_ms = current_time;
            initialized = true;
            return;
        }

        double dt_ms = current_time - last_time_ms;
        if (dt_ms <= 0.0 || dt_ms > 100.0)
        {
            last_time_ms = current_time;
            return;
        }
        last_time_ms = current_time;

        double dt_s = dt_ms / 1000.0;
        double t_s = current_time / 1000.0;

        std::normal_distribution<double> normal(0.0, 1.0);

        // 1. Ornstein-Uhlenbeck drift (MUITO sutil)
        ou_x += -cfg.ou_theta * ou_x * dt_s + cfg.ou_sigma * std::sqrt(dt_s) * normal(rng);
        ou_y += -cfg.ou_theta * ou_y * dt_s + cfg.ou_sigma * std::sqrt(dt_s) * normal(rng);

        // 2. Tremor fisiológico (suprimido com velocidade)
        // Quanto mais rápido move, menos tremor (proprioceptive suppression)
        double speed_factor = 1.0 / (1.0 + movementSpeed * 0.5);

        // Amplitude do tremor baseada na config
        std::uniform_real_distribution<double> ampDist(cfg.tremor_amp_min, cfg.tremor_amp_max);
        double tremor_amp = ampDist(rng);

        double tremor_x = tremor_amp * speed_factor *
            std::sin(2.0 * std::numbers::pi * tremor_freq * t_s + tremor_phase_x);
        double tremor_y = tremor_amp * speed_factor *
            std::sin(2.0 * std::numbers::pi * tremor_freq * t_s + tremor_phase_y);

        // 3. Signal-dependent noise (proporcional à velocidade)
        double sdn_x = cfg.sdn_k * movementSpeed * normal(rng);
        double sdn_y = cfg.sdn_k * movementSpeed * normal(rng);

        // Aplica humanização (valores já são pequenos na config)
        pitch += ou_x + tremor_x + sdn_x;
        yaw += ou_y + tremor_y + sdn_y;
    }
};

// ============================================
// FUNÇÕES AUXILIARES
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

    uintptr_t rootComponent = Memory::read<uintptr_t>(actor + Offsets::RootComponent);
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

        float distance = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

        if (distance < 1.0f)
            break;

        float timeToHit = distance / bulletSpeed;

        predicted = {
            targetPos.x + targetVelocity.x * timeToHit,
            targetPos.y + targetVelocity.y * timeToHit,
            targetPos.z + targetVelocity.z * timeToHit
        };
    }

    return predicted;
}

bool Engine::GetAimPoint2D(const PlayerCacheEntry& actor, Vector3& out)
{
    const auto& bones = actor.boneData;

    if (bones.valid.test((size_t)UniBone::Head))
    {
        out = bones.bonesDouble[(size_t)UniBone::Head];
        out.z = 0.0;
        return true;
    }

    if (bones.valid.test((size_t)UniBone::Neck))
    {
        out = bones.bonesDouble[(size_t)UniBone::Neck];
        out.z = 0.0;
        return true;
    }

    out.x = actor.ScreenTop.x + (actor.ScreenBottom.x - actor.ScreenTop.x) * 0.28;
    out.y = actor.ScreenTop.y + (actor.ScreenBottom.y - actor.ScreenTop.y) * 0.28;
    out.z = 0.0;
    return true;
}

bool Engine::GetAimPoint2DPredicted(
    const PlayerCacheEntry& actor,
    float bulletSpeed,
    Vector3& out)
{
    const auto& bones = actor.boneData;
    Vector3 targetWorldPos;

    if (bones.valid.test((size_t)UniBone::Head))
    {
        targetWorldPos = bones.bonesWorldDouble[(size_t)UniBone::Head];
    }
    else if (bones.valid.test((size_t)UniBone::Neck))
    {
        targetWorldPos = bones.bonesWorldDouble[(size_t)UniBone::Neck];
    }
    else
    {
        targetWorldPos = actor.WorldPos;
        targetWorldPos.z += 160.0;
    }

    if (targetWorldPos.x == 0.0 && targetWorldPos.y == 0.0 && targetWorldPos.z == 0.0)
    {
        return GetAimPoint2D(actor, out);
    }

    Vector3 velocity = GetActorVelocity(actor.APawn);

    Vector3 predictedWorldPos = PredictPosition(
        targetWorldPos,
        velocity,
        g_Camera.Location,
        bulletSpeed,
        3
    );

    if (!ProjectWorldLocationToScreen(predictedWorldPos, out))
    {
        return GetAimPoint2D(actor, out);
    }

    out.z = 0.0;
    return true;
}

bool Engine::GetRobotAimPoint2D(
    const WorldCacheEntry& robot,
    float fovRadius,
    Vector3& outScreenPos,
    Vector3& outWorldPos,
    int32_t& outPartID,
    int32_t& outResistGroup)
{
    Vector3 screenCenter{
        GetSystemMetrics(SM_CXSCREEN) * 0.5,
        GetSystemMetrics(SM_CYSCREEN) * 0.5,
        0.0
    };

    Vector3 screen;
    if (!ProjectWorldLocationToScreen(robot.WorldPos, screen))
        return false;

    double dx = screen.x - screenCenter.x;
    double dy = screen.y - screenCenter.y;
    float dist = (float)std::sqrt(dx * dx + dy * dy);

    if (dist > fovRadius)
        return false;

    outScreenPos = screen;
    outWorldPos = robot.WorldPos;
    outPartID = -1;
    outResistGroup = 0;
    return true;
}

// ============================================
// AIM ASSIST PLAYER (COM RANDOM BONE)
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

        // ========== RANDOM BONE (SEQUENCIAL) ==========
        if (var::randombone)
        {
            UniBone targetBone = GetSequentialBone(key, currentTime, bones);

            if (bones.valid.test((size_t)targetBone))
            {
                worldPos = bones.bonesWorldDouble[(size_t)targetBone];
            }
            else
            {
                // Fallback
                if (bones.valid.test((size_t)UniBone::Head))
                    worldPos = bones.bonesWorldDouble[(size_t)UniBone::Head];
                else if (bones.valid.test((size_t)UniBone::Neck))
                    worldPos = bones.bonesWorldDouble[(size_t)UniBone::Neck];
                else
                {
                    worldPos = actor.WorldPos;
                    worldPos.z += 160.0;
                }
            }
        }
        else
        {
            // Comportamento padrão: Head > Neck > WorldPos
            if (bones.valid.test((size_t)UniBone::Head))
            {
                worldPos = bones.bonesWorldDouble[(size_t)UniBone::Head];
            }
            else if (bones.valid.test((size_t)UniBone::Neck))
            {
                worldPos = bones.bonesWorldDouble[(size_t)UniBone::Neck];
            }
            else
            {
                worldPos = actor.WorldPos;
                worldPos.z += 160.0;
            }
        }

        // Prediction (se ativado)
        if (var::predict)
        {
            Vector3 velocity = GetActorVelocity(actor.APawn);
            worldPos = PredictPosition(worldPos, velocity, g_Camera.Location, bulletSpeed, 3);
        }

        // Projeta para screen
        if (!ProjectWorldLocationToScreen(worldPos, aimPos))
            continue;

        float dx = aimPos.x - screenCenter.x;
        float dy = aimPos.y - screenCenter.y;
        float distToCenter = std::sqrt(dx * dx + dy * dy);

        if (distToCenter > fovRadius)
            continue;

        float healthScore = (100.f - actor.health) * 1.5f;
        float distanceScore = (fovRadius - distToCenter);
        float totalScore = healthScore + distanceScore;

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
    if (!var::enable_aimbot)
        return;

    std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
    for (const auto& [key, robot] : robotCache)
    {
        if (!robot.Drawing) continue;
        if (robot.Distance > var::aimbot_distance) continue;

        uint8_t destroyed = Memory::read<uint8_t>(robot.APawn + Offsets::bIsBreaked);
        if (destroyed != 0)
            continue;

        Vector3 aimPos{};
        Vector3 worldPos{};
        int32_t partID = -1;
        int32_t resistGroup = 0;

        if (!GetRobotAimPoint2D(robot, fovRadius, aimPos, worldPos, partID, resistGroup))
            continue;

        float dx = aimPos.x - screenCenter.x;
        float dy = aimPos.y - screenCenter.y;
        float distToCenter = std::sqrt(dx * dx + dy * dy);

        if (distToCenter > fovRadius)
            continue;

        float totalScore = (fovRadius - distToCenter);

        AimTarget target;
        target.entityKey = key;
        target.aimPos = aimPos;
        target.worldPos = worldPos;
        target.distToCenter = distToCenter;
        target.score = totalScore;
        target.isRobot = true;
        target.partID = partID;
        target.resistGroup = resistGroup;

        targets.push_back(target);
    }
}

// ============================================
// ROTATION-BASED AIM ASSISTANCE
// ============================================

static Vector3 CalcAngle(Vector3 MeLocation, Vector3 TargetLocation)
{
    Vector3 DeltaPos = TargetLocation - MeLocation;

    double Distance = std::sqrt(DeltaPos.x * DeltaPos.x + DeltaPos.y * DeltaPos.y + DeltaPos.z * DeltaPos.z);

    if (Distance < 1.0)
        return Vector3(0, 0, 0);

    double x = -((std::acos(DeltaPos.z / Distance) * (180.0 / 3.14159265358979323846)) - 90.0);
    double y = std::atan2(DeltaPos.y, DeltaPos.x) * (180.0 / 3.14159265358979323846);
    double z = 0.0;

    return Vector3(x, y, z);
}

static Vector3 SmoothAim(const Vector3& target, const Vector3& current, float smoothness)
{
    // Garante smoothness mínimo
    if (smoothness < 1.0f) smoothness = 1.0f;

    // Calcula a diferença entre target e current
    double diffX = target.x - current.x;
    double diffY = target.y - current.y;

    // Normaliza os ângulos para -180 a 180 (PITCH - X)
    while (diffX > 180.0) diffX -= 360.0;
    while (diffX < -180.0) diffX += 360.0;

    // Normaliza os ângulos para -180 a 180 (YAW - Y)
    while (diffY > 180.0) diffY -= 360.0;
    while (diffY < -180.0) diffY += 360.0;

    // Factor: quanto maior smoothness, menor o passo
    double factor = 1.0 / (double)smoothness;

    // Calcula nova rotação
    double newX = current.x + diffX * factor;
    double newY = current.y + diffY * factor;

    // Normaliza o resultado (PITCH)
    while (newX > 180.0) newX -= 360.0;
    while (newX < -180.0) newX += 360.0;

    // Normaliza o resultado (YAW)
    while (newY > 180.0) newY -= 360.0;
    while (newY < -180.0) newY += 360.0;

    return Vector3(newX, newY, 0.0);
}

void Engine::AimAssistence()
{
    static uint64_t lockedTarget = 0;
    static uint64_t previousTarget = 0;
    static float lastSwitchTime = 0.f;
    static float lastTargetScore = 0.f;
    static bool lockedIsRobot = false;
    static MotorSynergyHumanizer humanizer;

    bool playerAimEnabled = var::enable_aimbot;
    bool robotAimEnabled = true;

    if (!playerAimEnabled && !robotAimEnabled)
    {
        lockedTarget = 0;
        previousTarget = 0;
        humanizer.Reset();
        return;
    }

    auto bindAim = VK_SHIFT;
    bool keyPressed = (GetAsyncKeyState(bindAim) & 0x8000) != 0;

    if (!keyPressed)
    {
        lockedTarget = 0;
        previousTarget = 0;
        humanizer.Reset();
        return;
    }

    // ========== VALIDAÇÃO CRÍTICA ==========
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
        FNameCache::Instance().Clear();
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
        Vector3 testRead = Memory::read<Vector3>(sPlayerController + Offsets::ControlRotation);
        if (std::isnan(testRead.x) || std::isnan(testRead.y) || std::isnan(testRead.z))
        {
            return;
        }
    }
    catch (...)
    {
        return;
    }

    Vector3 screenCenter{
        GetSystemMetrics(SM_CXSCREEN) * 0.5,
        GetSystemMetrics(SM_CYSCREEN) * 0.5,
        0.0
    };

    float currentTime = GetTimeSeconds();
    float playerFov = var::aimbot_fov;
    float robotFov = var::aimbot_fov;
    float bulletSpeed = 42000.0f;

    std::vector<AimTarget> allTargets;
    allTargets.reserve(64);

    if (playerAimEnabled)
    {
        AimAssistPlayer(screenCenter, playerFov, bulletSpeed, currentTime, allTargets);
    }

    if (robotAimEnabled)
    {
        AimAssistRobot(screenCenter, robotFov, currentTime, allTargets);
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
        bool priorizePlayers = true;
        if (!target.isRobot && priorizePlayers)
        {
            priorityBonus = 1000.f;
        }

        float adjustedScore = target.score + priorityBonus;

        if (adjustedScore > bestScore ||
            (adjustedScore == bestScore && target.distToCenter < (bestTarget ? bestTarget->distToCenter : FLT_MAX)))
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
        return;
    }

    if (bestTarget->worldPos.x == 0.0 && bestTarget->worldPos.y == 0.0 && bestTarget->worldPos.z == 0.0)
    {
        return;
    }

    bool isNewTarget = (bestTarget->entityKey != previousTarget && previousTarget != 0) ||
        (lockedTarget == 0 && bestTarget->entityKey != 0);

    if (lockedTarget != bestTarget->entityKey)
    {
        float sinceLastSwitch = currentTime - lastSwitchTime;

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

    // ========== ROTATION-BASED AIM ==========

    Vector3 targetWorldPos = bestTarget->worldPos;
    Vector3 cameraLocation = g_Camera.Location;

    if (cameraLocation.x == 0.0 && cameraLocation.y == 0.0 && cameraLocation.z == 0.0)
    {
        return;
    }

    Vector3 targetRotation = CalcAngle(cameraLocation, targetWorldPos);

    if (std::isnan(targetRotation.x) || std::isnan(targetRotation.y))
    {
        return;
    }

    if (!IsValidPointer(sPlayerController))
    {
        return;
    }

    Vector3 currentRotation = Memory::read<Vector3>(sPlayerController + Offsets::ControlRotation);

    if (std::isnan(currentRotation.x) || std::isnan(currentRotation.y))
    {
        return;
    }

    float smoothness = var::smoothness;

    Vector3 newRotation = SmoothAim(targetRotation, currentRotation, smoothness);

    // ========== MOTOR SYNERGY HUMANIZER ==========
    if (var::humanizer)
    {
        // Calcula velocidade do movimento para o humanizer
        double movementSpeed = std::sqrt(
            std::pow(targetRotation.x - currentRotation.x, 2) +
            std::pow(targetRotation.y - currentRotation.y, 2)
        );

        // Aplica humanização (tremor, drift, SDN)
        humanizer.ApplyToRotation(newRotation.x, newRotation.y, movementSpeed);
    }

    if (std::isnan(newRotation.x) || std::isnan(newRotation.y))
    {
        return;
    }

    if (!IsValidPointer(sPlayerController))
    {
        return;
    }

    Memory::write<Vector3>(sPlayerController + Offsets::ControlRotation, newRotation);
}