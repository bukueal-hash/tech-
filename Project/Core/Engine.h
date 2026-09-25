#pragma once
#include "Vector.hpp"
#include "Cache.hpp"
#include "Offsets.h"
#include "PlayerChain.hpp"
#include "SteamDecrypt.hpp"
#include "BotTypes.h"
#include "CrateContents.hpp"
#include "LoadoutFormat.hpp"
#include "ReviveBadge.hpp"
#include "LookArrow.hpp"
#include "SquadRoster.hpp"
#include "VisionCone.hpp"
#include "PartDamage.hpp"
#include "RaidClock.hpp"
#include "ActivityFeed.hpp"
#include "RadarProjection.hpp"
#include "EspFramePolicy.hpp"

#include <atomic>
#include <algorithm>
#include <cmath>
#include <limits>
#include <random>
#include <deque>
#include <chrono>
#include <d3d9.h>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include "../ThirdParty/ImGui/imgui.h"
#include "../Interface/Utils/Threads/SyncedThread.h"
#include "../Interface/Utils/Variables/index.h"
#include <memory>

#define M_PI	3.14159265358979323846

inline bool IsGameplayCameraFov(float fov)
{
    return fov >= 50.f && fov <= 120.f;
}

inline bool IsUsableCameraFov(float fov)
{
    return fov > 1.f && fov < 179.f;
}

inline bool IsPlausibleWorldPos(const Vector3& p)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        return false;
    const double mag =
        static_cast<double>(p.x) * p.x +
        static_cast<double>(p.y) * p.y +
        static_cast<double>(p.z) * p.z;
    if (mag < 100.0 || mag > 1.0e18)
        return false;
    constexpr double kMaxAxis = 5.0e6;
    return std::fabs(static_cast<double>(p.x)) < kMaxAxis
        && std::fabs(static_cast<double>(p.y)) < kMaxAxis
        && std::fabs(static_cast<double>(p.z)) < kMaxAxis;
}

namespace EngineProjection {
inline void RotationGetAxes(const Vector3& rot, Vector3& axisX, Vector3& axisY, Vector3& axisZ)
{
    constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
    const double sp = std::sin(rot.x * kDegToRad);
    const double cp = std::cos(rot.x * kDegToRad);
    const double sy = std::sin(rot.y * kDegToRad);
    const double cy = std::cos(rot.y * kDegToRad);
    const double sr = std::sin(rot.z * kDegToRad);
    const double cr = std::cos(rot.z * kDegToRad);

    axisX = Vector3{ cy * cp, sy * cp, sp };
    axisY = Vector3{ cy * sp * sr - sy * cr, sy * sp * sr + cy * cr, -cp * sr };
    axisZ = Vector3{ -(cy * sp * cr + sy * sr), cy * sr - sy * sp * cr, cp * cr };
}

inline bool ProjectWorldLocationToScreen(
    const Vector3& worldLocation,
    Vector3& screen,
    const Vector3& cameraLocation,
    const Vector3& cameraRotation,
    float cameraFov,
    double overlayW,
    double overlayH)
{
    if (worldLocation.x == 0.0 &&
        worldLocation.y == 0.0 &&
        worldLocation.z == 0.0)
        return false;

    if (cameraFov <= 1.0f || cameraFov > 179.0f)
        return false;

    Vector3 axisX{};
    Vector3 axisY{};
    Vector3 axisZ{};
    RotationGetAxes(cameraRotation, axisX, axisY, axisZ);

    const Vector3 delta = worldLocation - cameraLocation;
    const double transformedX =
        delta.x * axisY.x + delta.y * axisY.y + delta.z * axisY.z;
    const double transformedY =
        delta.x * axisZ.x + delta.y * axisZ.y + delta.z * axisZ.z;
    const double transformedZ =
        delta.x * axisX.x + delta.y * axisX.y + delta.z * axisX.z;

    if (transformedZ < 1.0)
        return false;

    const double centerX = overlayW * 0.5;
    const double centerY = overlayH * 0.5;

    constexpr double kPi = 3.14159265358979323846;
    const double tanHalfFov = std::tan(static_cast<double>(cameraFov) * kPi / 360.0);
    if (tanHalfFov < 0.001)
        return false;

    const double scale = centerY / tanHalfFov;
    screen.x = centerX + (transformedX * scale) / transformedZ;
    screen.y = centerY - (transformedY * scale) / transformedZ;

    if (!std::isfinite(screen.x) || !std::isfinite(screen.y))
        return false;
    if (screen.x < -overlayW * 0.5 || screen.x > overlayW * 1.5 ||
        screen.y < -overlayH * 0.5 || screen.y > overlayH * 1.5)
        return false;

    return true;
}
} // namespace EngineProjection

class Engine {
public:
    struct WorldCacheEntry;

    struct HumanizerDebugState {
        int phase = 0;              // 0 off, 1 reacting, 2 settling, 3 held
        float injectedX = 0.f;      // last injected screen-space offset, px
        float injectedY = 0.f;
        bool limiterActive = false; // raw offset exceeded the safety cap
        uint64_t stampMs = 0;       // last aim tick that published this state
        bool valid = false;
    };
    void PublishHumanizerDebug(const HumanizerDebugState& state);
    HumanizerDebugState GetHumanizerDebug() const;

    // FStowedWeaponInfo — 0x40 bytes per stowed weapon slot in InventoryComponent
    // (CL-1315578). Pack(1) without pad put WeaponQuality at +0x30 — wrong.
#pragma pack(push, 1)
    struct StowedWeaponInfo {
        uint64_t Item = 0;              // +0x00 ItemBase*
        uint64_t ItemDataAsset = 0;     // +0x08 ItemDataAsset*
        uint64_t StowedActor = 0;       // +0x10 StowedWeaponActor*
        uint64_t WeaponVisual = 0;      // +0x18 WeaponVisualsDataAsset*
        uint64_t QualityVisual = 0;     // +0x20 WeaponQualityVisualizationAsset*
        uint64_t WeaponVisualMods = 0;  // +0x28 FArrayProperty (ptr)
        uint64_t _pad30 = 0;            // +0x30 pad → quality at +0x38
        int32_t  WeaponQuality = 0;     // +0x38 int32 (0-3 = I–IV)
        uint8_t  bShouldBeStowed = 0;   // +0x3C
        uint8_t  bSlotHidden = 0;       // +0x3D
        uint8_t  bUsePrimary = 0;       // +0x3E
        uint8_t  WantedSlot = 0;        // +0x3F
    };
#pragma pack(pop)
    static_assert(sizeof(StowedWeaponInfo) == Offsets::StowedInfo_Size,
        "FStowedWeaponInfo must match StowedWeaponLayout::STRUCT_SIZE (0x40)");

    /** Try plaintext then decrypt_object_ptr for InventoryComponent chain. */
    static uintptr_t ResolveInventoryPtr(uintptr_t raw);
    /** Read stowed weapon info, equipped weapon, armor from a player's inventory. */
    void ReadPlayerInventory(uintptr_t pawn, std::string& outWeaponName, int& outWeaponQuality,
        int& outWeaponClip,
        std::string& outStowed0, int& outStowedQ0,
        std::string& outStowed1, int& outStowedQ1,
        float& outArmorPlates, float& outArmorPerPlate,
        int& outArmorTier, std::string& outArmorName,
        bool* outResolved = nullptr);

    // ---- Player intel (features: DBNO/revive, look arrows, squads) ----------
    /** bIsDBNO from FPlayerHealthInfo; picks the coherent health block first. */
    bool ReadPlayerDbnoState(uintptr_t playerState, bool& outBrokenArmor,
        bool* outResolved = nullptr);
    /** Defib (preferred) or bleedout timer pair from the pawn's instance
     *  components. false = no plausible timer this pass. */
    bool ReadReviveTimer(uintptr_t pawn, float& outElapsed, float& outTotal);
    /** SteamID64 via PlatformIdComponent -> FUniqueNetIdRepl -> payload. */
    uint64_t ReadPlayerSteamId(uintptr_t playerState, bool* outResolved = nullptr);
    /** Full kit readout (InventoryComponent: stowed tool actor, safe pouch
     *  item + ItemBase::QUALITY_LEVEL, belt/backpack ItemLimit slot counts).
     *  Best-effort - misses just leave the KitParts segments empty. */
    void ReadPlayerKit(uintptr_t pawn, LoadoutFormat::KitParts& outKit);
    /** PlayerState status bits (1=bot, 2=spectator, 4=finished round). The
     *  flags are only reported when the PioneerPlayerState block is coherent
     *  (its component slots read as null-or-valid pointers). */
    uint8_t ReadPlayerStatusTags(uintptr_t playerState);
    /** Container dispenser state: dispenser port count (0 = none/unresolved)
     *  plus whether the socket-loot mesh resolved on the container. */
    int ReadContainerDispenserPorts(uintptr_t actor, bool& outSocketMeshOk);
    /** Item rarity (0-3) through the pickup resolution chain (data asset ->
     *  PickupDataAsset::RESOLVED_ITEM_CLASS -> CDO -> ItemBase::QUALITY_LEVEL).
     *  -1 = unresolved. */
    int ReadItemRarityFromPickup(uintptr_t actor);

    // ---- Bot intel (features: vision cones, per-part damage) ----------------
    /** AIStateService first, AISenseConfigSight second (floats win). Fills
     *  sight radius/half-angle + alertness/combat phase bytes. */
    bool ReadBotVision(uintptr_t actor, float& outRadiusCm, float& outHalfAngleDeg,
        uint8_t& outAlertness, uint8_t& outCombatPhase);
    /** HealthService::PART_HP_ARRAY fractions + style-driver destroyed count.
     *  Returns the fractions written (capped), each gated into [-1, 4]. */
    int ReadBotPartHp(uintptr_t actor, float* out, int cap, int& outDestroyedParts);

    // ---- Raid intel (features: raid dashboard, activity feed, map radar) -----
    struct RaidHudState {
        double timeLeftS = -1.0;   // FStageInfo::TimeLeft seconds; -1 = unknown
        double graceS = -1.0;
        int32_t gamePhase = -1;
        int32_t enemyCount = -1;
        int32_t pickupCount = -1;
        uint64_t stampMs = 0;
        bool valid = false;
    };
    /** 1 Hz worker-side refresh of the raid clock/counts + minimap bounds.
     *  Paint never reads DMA - it consumes GetRaidHud()/GetRadarBounds(). */
    void RefreshRaidDashboard();
    RaidHudState GetRaidHud() const;
    RadarProjection::Bounds GetRadarBounds(bool& outValid) const;
    /** Scanners push state transitions; the feed panel renders the timeline. */
    void PushActivity(uint64_t key, const char* text);
    ActivityFeed::Feed GetActivityFeed() const;
    void RenderRaidHud();
    void RenderActivityFeed();

    mutable std::shared_mutex m_raidHudMutex;
    RaidHudState m_raidHud{};
    mutable std::shared_mutex m_feedMutex;
    ActivityFeed::Feed m_activityFeed;
    mutable std::shared_mutex m_radarBoundsMutex;
    RadarProjection::Bounds m_radarBounds{};
    bool m_radarBoundsValid = false;
    uintptr_t m_miniMapActor = 0;
    void RefreshRadarBounds();

    /**
     * Crate contents preview: resolve pickup SpawnItems first, then inspect the
     * owned UItemContainerComponent used by world crates. Each accepted item
     * contributes amount / data asset / display name / rarity to out. Every
     * read is sanity-gated; a miss just means fewer stacks on the label.
     */
    int ReadCrateContents(uintptr_t actor, CrateContents::Stack* out, int cap);

private:
    mutable std::mutex m_humanizerDebugMutex;
    HumanizerDebugState m_humanizerDebug{};

    std::atomic<bool> entityStarted{ false };

    std::atomic<bool> m_workerThreadsStarted{ false };
    std::unique_ptr<SyncedThread> m_worldThread;
    std::unique_ptr<SyncedThread> m_entityThread;
    std::unique_ptr<SyncedThread> m_worldEspThread;
    std::unique_ptr<SyncedThread> m_robotEspThread;
    std::unique_ptr<SyncedThread> m_aimThread;
    std::unique_ptr<SyncedThread> m_positionThread;
    std::unique_ptr<SyncedThread> m_cameraThread;
    std::unique_ptr<SyncedThread> m_frameBuilderThread;

public:
    // Thread synchronization (paint debug overlay uses try_lock on these)
    mutable std::shared_mutex m_stateMutex;       // Protects state pointers (GWorld, PersistentLevel, etc.)
    mutable std::shared_mutex m_playerCacheMutex;  // Protects playerCache
    mutable std::shared_mutex m_containerCacheMutex; // Protects containerCache
    mutable std::shared_mutex m_itemCacheMutex;    // Protects itemCache
    mutable std::shared_mutex m_robotCacheMutex;   // Protects robotCache
    std::atomic<uint64_t> m_worldGeneration{ 0 };    // Incremented on world change

private:
    std::atomic<bool> m_raidRaw{ false };
    std::atomic<bool> m_espRaidActive{ false };
    /** Scanners may run; ESP/aim only draw after caches prove healthy. */
    std::atomic<bool> m_espDrawReady{ false };
    bool m_raidEnterPending{ false };
    bool m_partyEnterPending{ false };
    std::chrono::steady_clock::time_point m_raidDebSince{};
    std::chrono::steady_clock::time_point m_partyDebSince{};
    std::chrono::steady_clock::time_point m_raidArmedSince{};

    /** Party settle then raid settle before arming scanners.
     *  Was 5s+5s — log proof (raid-gate TheDam_02_P): raw/ok_enter already true
     *  at party_wait, so ESP sat idle ~10s after the map was ready. */
    static constexpr std::chrono::milliseconds kPartyEnterDelayMs{ 500 };
    /** User spec: map found -> wait 10s before ESP scanning starts. */
    static constexpr std::chrono::milliseconds kRaidEnterDelayMs{ 10000 };
    /** Live-verified (debug-c190fb.log TheDam burst): VMM read stalls of ~8.5s
     *  where GWorld resolvers return 0 for every attempt. With a 5s grace the
     *  gate dropped, ClearEspCaches() wiped everything, and re-arm+rescan made
     *  ESP vanish/refill patch-by-patch (user-visible "flicker"). 12s rides
     *  out the observed stalls on last-known data instead of wiping. */
    static constexpr std::chrono::milliseconds kRaidRawFalseGraceMs{ 12000 };
    /** Soft draw-ready if not all three caches fill (solo / empty pocket). */
    static constexpr std::chrono::milliseconds kEspCacheReadySoftMs{ 12000 };
    static constexpr int kPartyMinPlayers{ 3 };

    std::chrono::steady_clock::time_point m_raidFalseSince{};
    /** Last time the world pointer resolved non-zero (CheckWorldTransition).
     *  HandleWorldLost uses it to hold through transient read stalls instead of
     *  nuking camera + caches on every VMM hiccup (user-visible flicker). */
    std::chrono::steady_clock::time_point m_lastGoodWorldTime{};

    void TickRaidGate();
    void TickEspCacheReadiness();
    int CountGameStatePlayerArray() const;
    void ClearEspCaches();
    /** PC cache, camera, FName/decrypt — same class of reset as process restart for raid transitions. */
    void ResetRaidTransitionState();

public:
    struct CameraCache
    {
        Vector3 Location;
        Vector3 Rotation;
        float FOV;
    };

    struct FVector3d
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    // POV layout: Location at +0x00, Rotation at +0x28, FOV at +0x48
    // (internal FMinimalViewInfo offsets; PCM base offsets in Offsets.h).
    struct FMinimalViewInfo
    {
        FVector3d Location;         // 0x00
        char pad_to_rotation[0x10]; // 0x18–0x27
        FVector3d Rotation;         // 0x28
        char pad_to_fov[0x08];      // 0x40–0x47
        float FOV;                  // 0x48
        // DesiredFOV / AspectRatio / PostProcessSettings omitted (not read)
    };

    struct CameraProbeSnapshot;

    BoneData boneData;

    void Update();
    void EntityList();
    void RobotList();
    void ContainerList();
    void ItemList();
    void UserConfirmGroundItemPicked(uintptr_t key);
    struct GroundPickupHudRow {
        uintptr_t key = 0;
        std::string name;
        float distM = -1.f;
        uint8_t worldCategory = 0;
        std::string fname;
    };
    void CollectDrawingGroundPickups(std::vector<GroundPickupHudRow>& out) const;

    struct WorldScanContext {
        uintptr_t gWorld = 0;
        uintptr_t persistentLevel = 0;
        uintptr_t actors = 0;
        uintptr_t acknowledgedPawn = 0;
        std::vector<uint64_t> currentActors;
        CameraCache camera{};
    };
    bool GatherWorldScanContext(WorldScanContext& ctx);
    void FinalizeWorldCacheMap(
        std::unordered_map<uintptr_t, WorldCacheEntry>& cache,
        const CameraCache& cam,
        int& outDrawing);

    void StartWorkerThreads();
    void StopWorkerThreads();

    void RenderEsp();
    void RenderFovCircle();
    void RenderOverlayCrosshair();
    void RenderRadar(bool interactive = false);

    struct EspFramePlayer;
    struct EspFrameWorld;
    struct EspRenderFrame;
    struct PlayerCacheEntry;

    bool CollectEspRenderFrame(EspRenderFrame& out);
    void BuildEspRenderFrameWorker();
    void PositionRefreshPass();

    bool ShouldDrawPlayerEsp(const PlayerCacheEntry& entry) const;
    bool ShouldDrawRobotEsp(uintptr_t actorKey, const WorldCacheEntry& entry) const;
    bool ShouldDrawWorldEsp(const WorldCacheEntry& entry) const;

    bool BuildCameraCacheFromPovReads(
        bool pcmOk,
        float povFov,
        float defFov,
        float jsonFov,
        const FVector3d& povLoc,
        const FVector3d& povRot,
        const Vector3& pawnPos,
        bool pawnOk,
        const Vector3& ctrlRot,
        bool hasPc,
        CameraCache& outCamera,
        CameraProbeSnapshot* outProbe = nullptr) const;
    bool InitConsts();
    void AimAssistence();
    void ReleaseAimOutputs();

    // ── The player chain (Core/PlayerChain.hpp) ───────────────────────────────
    // One ladder, one state, per-hop diagnostics. ResolvePlayerChain walks the
    // hops in a fixed order and records which named rung answered each one;
    // Update() only seeds it from the previous tick and publishes the result.
    // The seed/retain policy stays in Update (it is per-tick state, not order).
    void ResolvePlayerChain(uint64_t base, const PlayerChain::Seed& seed,
        uintptr_t& outActors, int& outActorCount);
    PlayerChain::State GetChainState() const;
    std::string GetChainTrace() const;

    // World primitives the ladder's world rungs are built from. They used to be
    // file-local helpers in Update.cpp, which is why the resolution order could
    // not live in one place.
    uintptr_t ReadWorldSlot(uint64_t base);
    uintptr_t WorldSlotInner(uintptr_t slot);
    bool LevelLooksOwnedByWorld(uintptr_t level, uintptr_t world);
    uintptr_t ResolvePersistentLevel(uintptr_t world);
    uintptr_t WorldFromGameStateGlobal(uint64_t base);

    /**
     * The AActor an object belongs to: walk UObject::Outer (the four encrypted
     * slots at obj+0x20 — see OuterLink in Core/SteamDecrypt.hpp) until a hop
     * lands on an actor. 0 when the chain ends first, so callers keep their own
     * fallbacks.
     */
    uintptr_t ResolveOwningActor(uintptr_t obj, int maxHops = 8);
    /**
     * Structural "is this an AActor": it has a live RootComponent and that
     * component's own Outer decrypts back to it. No class name is consulted.
     */
    static bool LooksLikeActor(uintptr_t obj);
    /** CL-1315578: UWorld::LevelCollections → collection.GameState. */
    uintptr_t ResolveGameStateFromWorld(uintptr_t uworldAddr);
    bool ResolveLevelActors(uintptr_t persistentLevel, uintptr_t& outActorsData, int& outActorCount);
    /**
     * LocalPlayer rungs from the controller: the dump-sourced +0x4B0 decrypt
     * (accepted only with the 0xA0 back-pointer), then a legacy slot scan.
     * Reports the rung it used, so the ladder can name it in the panel.
     */
    uintptr_t ResolveLocalPlayerFromController(uintptr_t playerController,
        PlayerChain::Rung* outRung = nullptr, bool* outBackRef = nullptr);
    bool ResolveLocalPlayerChainFromActors(uintptr_t persistentLevel, uintptr_t actorsArray, int actorCount,
        uintptr_t gameInstance, uintptr_t& outController, uintptr_t& outPawn);
    /**
     * Camera manager from the level's actors (FName scan / PCOwner match).
     * The ladder passes the world, level and controller it just resolved, so the
     * scan never depends on what the *previous* tick published; 0 keeps the
     * published value, which is what the other callers want.
     */
    uintptr_t GetCameraManagerFromActors(uintptr_t worldOverride = 0,
        uintptr_t levelOverride = 0, uintptr_t pcOverride = 0);
    bool ControllerHasValidPcm(uintptr_t pc);
    /**
     * Dump-verified local-controller identity: APlayerController::
     * bIsLocalPlayerController (0xD64 bit 0), the offset the 20260922 SDK reflects
     * for both Engine.PlayerController and Angelscript.PioneerPlayerController.
     * Works without a pawn or a PlayerCameraManager.Confirmed additionally demands
     * a live PlayerState so a coincidental bit read can't pass.
     */
    bool IsLocalPlayerController(uintptr_t pc);
    bool IsLocalPlayerControllerConfirmed(uintptr_t pc);
    bool ResolvePcFromLevelCameraManager(uintptr_t level, uintptr_t actorsData,
        uintptr_t& outPc, uintptr_t& outPawn, uintptr_t& outPcm);
    /**
     * Shared PC→PCM discovery ladder (Esp frame + RefreshCameraFromViewTarget).
     * nocacheFov: Utils uses read_nocache for DefaultFOV; Esp uses cached read.
     * Updates pc/pawn/pcm by reference when level scan recovers them.
     */
    bool ResolvePlayerCameraManagerLadder(
        uintptr_t& pc,
        uintptr_t& pawn,
        uintptr_t& pcm,
        uintptr_t level,
        uintptr_t actors,
        bool nocacheFov);

    void UpdateCamera();
    void SetProjectionViewport(float width, float height);
    Vector3 GetProjectionScreenCenter() const;
    bool RefreshCameraFromViewTarget();
    /** Lean render-path POV: three nocache reads from stored PCM. No rediscovery. */
    bool TryBuildCameraFromPcmPov(uintptr_t pcm, CameraCache& outCamera) const;

    struct CameraProbeSnapshot {
        char src[32]{};
        bool ok = false;
        bool povLocOk = false;
        bool povRotOk = false;
        bool pawnOk = false;
        float povRotX = 0.f;
        float povRotY = 0.f;
        float povRotZ = 0.f;
        float ctrlRotX = 0.f;
        float ctrlRotY = 0.f;
        float ctrlRotZ = 0.f;
        float pubRotX = 0.f;
        float pubRotY = 0.f;
        float pubRotZ = 0.f;
        float pubLocX = 0.f;
        float pubLocY = 0.f;
        float pubLocZ = 0.f;
        float povLocX = 0.f;
        float povLocY = 0.f;
        float povLocZ = 0.f;
        float pawnX = 0.f;
        float pawnY = 0.f;
        float pawnZ = 0.f;
        float fov = 0.f;
    };

    void DbgStoreCameraProbe(const CameraProbeSnapshot& probe);
    bool ProjectWorldLocationToScreen(Vector3 world_location, Vector3& screen);
    bool ProjectWorldLocationToScreen(Vector3 world_location, Vector3& screen, const CameraCache& camera);

    uintptr_t GetActorSkeletalMesh(uintptr_t actor) const;
    /** Mesh that owns the encrypted bone block (Embark preferred). */
    uintptr_t GetActorBoneMesh(uintptr_t actor);

    bool IsValidPointer(uintptr_t ptr) const;
    bool IsUsermodePtr(uintptr_t ptr);
    std::string getEntityType(const std::string& actorName);
    bool getAllowType(const std::string& actorName, int category = 0) const;
    bool getAllowWorldEntry(const WorldCacheEntry& entry) const;

    int32_t GetActorFNameId(uint64_t actor_base);
    std::string GetActorFNameString(uint64_t actor_base);

    std::string GetActorFNameStringCached(uintptr_t actor_base);
    std::string GetActorClassFName(uintptr_t actor_base);

    void ClearFNameCache();

    std::string GetEnglishItemName(uint64_t actor);
    std::string GetWeaponName(const std::string& internal_name);
    /** True for real loadout weapons; false for Unarmed, blueprints, recipes, junk. */
    bool IsPlayerWeaponEspLabel(const std::string& label);

    struct EngineStateSnapshot {
        uintptr_t gWorld = 0;
        uintptr_t gWorldRaw = 0;       // direct read at base+UWorld (even if validation fails)
        int gWorldFailStep = 0;       // 0=ok, 1=null slot, 2=bad world, 3=no level, 4=no actors
        uintptr_t persistentLevel = 0;
        uintptr_t actors = 0;
        uintptr_t playerController = 0;
        uintptr_t acknowledgedPawn = 0;
        uintptr_t rootComponent = 0;
        uintptr_t playerCameraManager = 0;
        uintptr_t owningGameInstance = 0;
        uintptr_t localPlayer = 0;

        // The whole chain, as the ladder resolved it: per hop the pointer, the
        // named rung that produced it, its identity proof, and how many rungs
        // were tried/rejected. This replaces the four independent src tags.
        PlayerChain::State chain;

        // SDK reflection walker (Core/Reflection.hpp) — filled on class change.
        bool reflectOk = false;
        int reflectDepth = 0;      // UStruct chain length (SuperStruct walk)
        int reflectProps = 0;      // decoded FProperty count (PropertyLink walk)
        std::string reflectSample; // "Name=0xOFF, ..." for the first few properties

        // SDK FString self-check (Offsets::FStringVerificationRva).
        bool fstrOk = false;
        int fstrKey = -1;          // verified scramble index, -1 = none
        std::string fstrText;
    };
    EngineStateSnapshot GetStateSnapshot() const;

    /** The chain the last Update tick resolved (read under m_stateMutex). */
    PlayerChain::State m_chain;   // worldRaw / worldFailStep live on the World hop
    /**
     * Consecutive ticks the cached controller pair failed its world-position
     * gate. It is the pair rung's own state (the ladder reads it), and Update
     * clears the caches when it crosses kCacheFailClearAfter.
     */
    int m_pairCacheFailStreak = 0;
    bool m_pairCacheInvalidated = false;

    // SDK reflection walker + FString verification (written by Update, published
    // through EngineStateSnapshot).
    uintptr_t m_reflectPawn = 0;   // pawn whose class was last walked
    bool m_reflectOk = false;
    int m_reflectDepth = 0;
    int m_reflectProps = 0;
    std::string m_reflectSample;
    bool m_fstrChecked = false;    // one-shot per session (build constant)
    bool m_fstrOk = false;
    int m_fstrKey = -1;
    std::string m_fstrText;

public: // Local Cache
    uintptr_t PlayerController;
    /** Actor-scanned PioneerPlayerController — owns PlayerCameraManager (GI PC does not). */
    uintptr_t PioneerPlayerController;
    uintptr_t GWorld;
    uintptr_t m_lastWorldPtr = 0;
    uintptr_t m_lastPersistentLevel = 0;

    uintptr_t PersistentLevel;
    uintptr_t GameInstance;
    uintptr_t OwningGameInstance;
    uintptr_t localplayer;

    uintptr_t AcknowledgedPawn;
    uintptr_t RootComponent;
    uintptr_t PlayerState;
    uintptr_t Mesh;
    uintptr_t PlayerCameraManager;
    uintptr_t Actors;
    int ActorsCount = 0;

    uintptr_t AGameStateBase;
public: // PlayerCache
    struct PlayerCacheEntry {
        uintptr_t rootComponent = NULL;
        uintptr_t actorState = NULL;
        uintptr_t HealthPoint = NULL;
        uintptr_t actorMesh = NULL;
        uintptr_t APawn = NULL;

        Vector3 WorldPos;

        Vector3 head;
        Vector3 feet;

        Vector3 ScreenTop;
        Vector3 ScreenBottom;

        std::string ActorName;

        bool isVisible = false;
        bool Drawing = false;
        float Distance = 0.f;
        bool isAlly = false;
        uint8_t enemyTeamId = 0;
        uint8_t squadIdx = 0;
        float facingYaw = 0.f;

        float health = 0.0f;
        float maxhealth = 0.0f;

        float shield = 0.0f;
        float maxshield = 0.0f;

        bool bIsDead = false;

        BoneData boneData;
        uintptr_t boneArray = 0;
        uintptr_t boneMesh = 0;

        Vector3 cachedVelocity = { 0, 0, 0 };
        uint64_t lastVelocityUpdate = 0;
        Vector3 lastWorldPos = { 0, 0, 0 };
        // First-admission state is separate from Drawing: a new actor can be
        // in the cache before the budgeted refresh pass reaches it.
        bool positionInitialized = false;
        uint64_t positionSampleMs = 0;

        // Weapon system from InventoryComponent
        std::string weaponName;
        int weaponQuality = -1;
        int weaponClip = 0;
        std::string stowedWeapon0;
        int stowedQuality0 = -1;
        std::string stowedWeapon1;
        int stowedQuality1 = -1;
        float armorPlates = 0.f;
        float armorPerPlate = 0.f;
        // Armor tier 1-4 (I-IV) + resolved armor item name (loadout readout).
        int armorTier = -1;
        std::string armorName;
        // Full kit readout (phase 2): stowed tool, safe pouch item + rarity,
        // belt/backpack slot capacity. Empty/-1 = unresolved segment.
        std::string kitTool;
        std::string kitPouch;
        int kitPouchRarity = -1;
        int kitBeltSlots = -1;
        int kitPackSlots = -1;
        // PlayerState status bits (phase 2): 1=bot, 2=spectator, 4=finished
        // round (SquadRoster::StatusTag renders them).
        uint8_t statusFlags = 0;

        // DBNO / revive (feature #4): downed badge + countdown ring.
        bool isDbno = false;
        bool hasBrokenArmor = false;
        float reviveRemainS = -1.f;
        float reviveTotalS = -1.f;
        // Look direction (feature #5): where they AIM (ControlRotation yaw),
        // falling back to body facing when no controller resolves.
        float aimYawDeg = 0.f;
        bool hasAimYaw = false;
        // Identity (feature #6): permanent SteamID64 + real UEmbarkSquad*.
        uint64_t steamId64 = 0;
        uintptr_t squadPtr = 0;

        // Resolution state is captured on the worker that already performs
        // these reads. The menu only consumes these flags; it never reads DMA.
        bool healthResolved = false;
        bool armorResolved = false;
        bool steamIdResolved = false;
        bool squadResolved = false;
        bool inventoryResolved = false;
        bool dbnoResolved = false;

        PlayerCacheEntry() {};

        PlayerCacheEntry(std::string name, uint64_t root, uintptr_t Pawn, uintptr_t mesh)
            : ActorName(name), rootComponent(root), APawn(Pawn), actorMesh(mesh)
        {
        };
    };

    std::unordered_map<uintptr_t, PlayerCacheEntry> playerCache;

    struct PlayerEspDiagnostic {
        uintptr_t actorKey = 0;
        uintptr_t actorState = 0;
        std::string name;
        float distance = 0.f;
        bool drawing = false;
        bool isAlly = false;
        float health = 0.f;
        float maxHealth = 0.f;
        float armor = 0.f;
        float maxArmor = 0.f;
        uint64_t steamId64 = 0;
        uintptr_t squadPtr = 0;
        uint8_t squadIdx = 0;
        bool healthResolved = false;
        bool armorResolved = false;
        bool steamIdResolved = false;
        bool squadResolved = false;
        bool inventoryResolved = false;
        bool dbnoResolved = false;
        bool isDbno = false;
        bool hasBrokenArmor = false;
        float reviveRemainS = -1.f;
        float reviveTotalS = -1.f;
        std::string weaponName;
        int weaponQuality = -1;
        int weaponClip = 0;
        std::string stowedWeapon0;
        std::string stowedWeapon1;
        int armorTier = -1;
        float armorPlates = 0.f;
        std::string armorName;
        std::string kitTool;
        std::string kitPouch;
        int kitPouchRarity = -1;
        int kitBeltSlots = -1;
        int kitPackSlots = -1;
    };

    std::vector<PlayerEspDiagnostic> GetPlayerEspDiagnostics() const {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        std::vector<PlayerEspDiagnostic> out;
        out.reserve(playerCache.size());
        for (const auto& [key, entry] : playerCache) {
            PlayerEspDiagnostic row;
            row.actorKey = key;
            row.actorState = entry.actorState;
            row.name = entry.ActorName;
            row.distance = entry.Distance;
            row.drawing = entry.Drawing;
            row.isAlly = entry.isAlly;
            row.health = entry.health;
            row.maxHealth = entry.maxhealth;
            row.armor = entry.shield;
            row.maxArmor = entry.maxshield;
            row.steamId64 = entry.steamId64;
            row.squadPtr = entry.squadPtr;
            row.squadIdx = entry.squadIdx;
            row.healthResolved = entry.healthResolved;
            row.armorResolved = entry.armorResolved;
            row.steamIdResolved = entry.steamIdResolved;
            row.squadResolved = entry.squadResolved;
            row.inventoryResolved = entry.inventoryResolved;
            row.dbnoResolved = entry.dbnoResolved;
            row.isDbno = entry.isDbno;
            row.hasBrokenArmor = entry.hasBrokenArmor;
            row.reviveRemainS = entry.reviveRemainS;
            row.reviveTotalS = entry.reviveTotalS;
            row.weaponName = entry.weaponName;
            row.weaponQuality = entry.weaponQuality;
            row.weaponClip = entry.weaponClip;
            row.stowedWeapon0 = entry.stowedWeapon0;
            row.stowedWeapon1 = entry.stowedWeapon1;
            row.armorTier = entry.armorTier;
            row.armorPlates = entry.armorPlates;
            row.armorName = entry.armorName;
            row.kitTool = entry.kitTool;
            row.kitPouch = entry.kitPouch;
            row.kitPouchRarity = entry.kitPouchRarity;
            row.kitBeltSlots = entry.kitBeltSlots;
            row.kitPackSlots = entry.kitPackSlots;
            out.push_back(std::move(row));
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            if (a.distance != b.distance)
                return a.distance < b.distance;
            return a.actorKey < b.actorKey;
        });
        return out;
    }

    struct PlayerPipelineStats {
        int scanned = 0;
        int gsCandidates = 0;
        int gsPawnHit = 0;
        int gsPawnMiss = 0;
        int rootFail = 0;
        int meshFail = 0;
        int playerStateFail = 0;
        int botClassified = 0;
        int admitted = 0;
        int initialized = 0;
        int uninitialized = 0;
        int drawing = 0;
        int frameSelected = 0;
        int frameNotDrawing = 0;
        int frameNotInitialized = 0;
        int frameAlly = 0;
        int frameDistance = 0;
        int framePosition = 0;
    };

    mutable std::shared_mutex m_playerDiagMutex;
    PlayerPipelineStats m_playerDiag{};

    PlayerPipelineStats GetPlayerPipelineStats() const {
        std::shared_lock<std::shared_mutex> lock(m_playerDiagMutex);
        return m_playerDiag;
    }

    void SetPlayerFrameResult(size_t selected, int notDrawing, int notInitialized,
                              int ally, int distance, int position) {
        std::unique_lock<std::shared_mutex> lock(m_playerDiagMutex);
        m_playerDiag.frameSelected = static_cast<int>(selected);
        m_playerDiag.frameNotDrawing = notDrawing;
        m_playerDiag.frameNotInitialized = notInitialized;
        m_playerDiag.frameAlly = ally;
        m_playerDiag.frameDistance = distance;
        m_playerDiag.framePosition = position;
    }

public:
    size_t PlayerCacheCount() const {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        return playerCache.size();
    }
    size_t WorldCacheCount() const {
        return ContainerCacheCount() + ItemCacheCount();
    }
    size_t ContainerCacheCount() const {
        std::shared_lock<std::shared_mutex> lock(m_containerCacheMutex);
        return containerCache.size();
    }
    size_t ItemCacheCount() const {
        std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex);
        return itemCache.size();
    }
    size_t RobotCacheCount() const {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        return robotCache.size();
    }
    /** Low-rate, sampled NDJSON diagnostics for player/bot/loot cache health. */
    void LogEntityDiagnostics();
    bool IsCachedPlayer(uintptr_t actorKey) const {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        return playerCache.contains(actorKey);
    }
    // Paint-thread variant: never block on the player scanner's exclusive
    // DMA-backed cache write (same stall class as the radar lock). If the
    // scanner is mid-write, report "not a player" for this frame — the bot
    // box is drawn by the frame's own collect path, and the worker's
    // IsCachedPlayer (above) stays authoritative at collect time.
    bool IsCachedPlayerTry(uintptr_t actorKey) const {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex, std::defer_lock);
        if (!lock.try_lock())
            return false;
        return playerCache.contains(actorKey);
    }
    bool EntityListReady() const { return entityStarted.load(std::memory_order_acquire); }
    bool IsEntityStarted() const { return EntityListReady(); }
    bool IsInRaidRaw() const;
    bool IsEspRaidActive() const {
        return m_espRaidActive.load(std::memory_order_acquire);
    }
    /** True only after player + bot + world caches look healthy (or soft timeout). */
    bool IsEspDrawReady() const {
        return m_espDrawReady.load(std::memory_order_acquire);
    }
    bool IsInRaid() const { return IsEspRaidActive(); }

    std::atomic<bool> m_lastEspFrameValid{ false };

    size_t CountEspDrawablePlayers() const {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        size_t count = 0;
        for (const auto& [key, entry] : playerCache) {
            (void)key;
            if (!entry.Drawing)
                continue;
            if (entry.isAlly && var::hide_allies)
                continue;
            ++count;
        }
        return count;
    }

    void GetBones(PlayerCacheEntry& actor);
    // outBoneSpace receives true when the returned array holds bone-space
    // (local) transforms needing parent-chain accumulation before use;
    // false when the array is already component-space (read it directly).
    uintptr_t ResolveBoneArray(
        uintptr_t actor,
        uintptr_t primaryMesh,
        uintptr_t* outBoneMesh = nullptr,
        std::ptrdiff_t* outCtwOffset = nullptr,
        std::ptrdiff_t* outTransOff = nullptr,
        bool* outBoneSpace = nullptr);
    Vector3 GetBone(int boneIndex, uintptr_t boneArray, FTransform componentToWorld);

public:
    struct WorldCacheEntry {
        uintptr_t rootComponent = NULL;
        uintptr_t actorState = NULL;
        uintptr_t Mesh = NULL;
        uintptr_t APawn;

        Vector3 WorldPos;
        Vector3 cachedVelocity{};
        Vector3 lastWorldPos{};
        uint64_t lastVelocityUpdate = 0;
        uint64_t positionSampleMs = 0;
        // Newest sample written by the high-frequency live root sampler. Unlike
        // positionSampleMs, this is never advanced by the slow scanner fallback.
        // It is the proof that a scanner-disabled entry still has a live actor.
        uint64_t livePositionSampleMs = 0;
        // Last RobotList pass that saw this actor in the current actor array.
        // Prevents an old cache entry from being promoted by a stale plausible
        // root after the actor has left the current world list.
        uint64_t lastActorSeenMs = 0;
        // The sample before positionSampleMs. Paint interpolates between these
        // two (Core/BotMotion.hpp) instead of snapping to the newest one, which
        // is what removes the per-sample step in box motion.
        Vector3 prevSampleWorldPos{};
        uint64_t prevSampleMs = 0;

        // Discovery funnel (bot_admit / bot_appear telemetry): steady-ms when
        // the pending lane first saw this actor and when it was admitted to
        // the cache. Paint logs first-draw latency from these; 0 = unknown.
        uint64_t firstSeenMs = 0;
        uint64_t admittedMs = 0;
        static constexpr int kMaxBotParts = 32;
        Vector3 BotPartPos[kMaxBotParts]{};
        int BotPartCount = 0;
        Vector3 CenterWorldPos{};
        Vector3 BotHeadWorldPos{};
        bool hasBotHeadWorldPos = false;
        Vector3 ScreenPos;

        std::string ActorName;

        bool Drawing = false;
        bool isVisible = false;
        bool IsBreaked = false;
        // Admission proved this is a bot even if its display name has not
        // decrypted yet. Keeps real bots drawable without admitting props.
        bool botIdentityProven = false;
        int category = 0;
        float Distance = 0.f;

        float health = 0.0f;
        float maxhealth = 0.0f;

        std::string ItemDisplayName;
        std::string ItemType;
        std::string ClassFName;  // class fname for container keyword matching at draw time
        // Best-effort loadout (bots only): InventoryComponent resolves just on
        // bots that carry one; ARC constructables usually leave these empty.
        std::string weaponName;
        int weaponQuality = -1;
        int weaponClip = 0;
        int armorTier = -1;
        std::string armorName;
        // Bot vision cone (feature #7): AIStateService / AISenseConfigSight.
        // sightRadiusCm <= 0 = no cone data; alertness 0=Idle .. 3=Combat.
        float sightRadiusCm = 0.f;
        float sightHalfAngleDeg = 0.f;
        float facingYawDeg = 0.f;
        uint8_t alertness = 0;
        uint8_t combatPhase = 0;
        bool visionValid = false;
        // Per-part damage (feature #8): PART_HP_ARRAY fractions (-1 = unread),
        // index-aligned with BotPartPos on a best-effort basis.
        float partHp[kMaxBotParts]{};
        int partHpCount = 0;
        int destroyedParts = 0;

        int lootRarityTier = 0;
    int lootValue = 0;
    uint8_t worldCategory = 0;
    // Extraction hatch state (EExtractionState: 0=Dormant,1=Disabled,2=Broken,
    // 3=Closed,4=Closing,5=Requested,6=Arriving,7=Ready). -1 = not a hatch.
    int8_t extractState = -1;
    // Extraction countdown (FExtractionInfo @ ExtractionPoint_ExtractionInfo):
    // seconds until the extraction fires, -1 when no timer is running.
    // Stamp supports paint-side extrapolation between frame builds.
    double extractRemainS = -1.0;
    uint64_t extractRemainStampMs = 0;
    // Bots: spawn-cluster group # (Snitch summon waves / patrol spawns).
    // Drawn on ESP as "G#" under the skeleton. 0 = unassigned.
    int groupId = 0;
    // Cached opened-state for containers: avoids uncached DMA reads in
    // FinalizeWorldCacheMap.  -1 = not probed yet, 0 = closed, 1 = opened.
    int8_t cachedOpened = -1;
    uint64_t openedProbeMs = 0;
    /** Timestamp (ms) when the current cachedOpened value was first observed.
     *  Used to compute adaptive TTL: stable-closed containers get a longer
     *  TTL (5 s) since they rarely re-open mid-raid; recently-changed and
     *  stable-opened containers stay at 500 ms so closures are detected fast. */
    uint64_t openedStateMs = 0;
    // Crate contents preview (the "[...]" label summary). Filled at admission
    // by Engine::ReadCrateContents while var::show_crate_contents; fixed
    // capacity so per-frame entry copies never heap-allocate for this.
    CrateContents::Stack crateStacks[CrateContents::kMaxStacks]{};
    // Phase 2 container state: dispenser drop ports (0 = none/unresolved) +
    // socket-loot mesh proof, rendered via CrateContents::PortsTag.
    uint8_t dispenserPorts = 0;
    uint8_t socketLootMesh = 0;
    uint8_t crateStackCount = 0;
    // Ground-pickup stack size (hover AMOUNT, Pickup::VISIBLE_AMOUNT fallback);
    // 0 = unknown/unread. Drawn as " xN" while var::show_stack_counts.
    int stackAmount = 0;

    WorldCacheEntry() {};

        WorldCacheEntry(std::string name, uint64_t root, uintptr_t Pawn, uintptr_t MeshComponent)
            : ActorName(name), rootComponent(root), APawn(Pawn), Mesh(MeshComponent)
        {
        };
    };
    std::unordered_map<uintptr_t, WorldCacheEntry> containerCache;
    std::unordered_map<uintptr_t, WorldCacheEntry> itemCache;
    std::unordered_map<uintptr_t, WorldCacheEntry> robotCache;

    struct EspFramePlayer {
        uintptr_t actorKey = 0;
        PlayerCacheEntry entry{};
    };

    struct EspFrameWorld {
        uintptr_t actorKey = 0;
        WorldCacheEntry entry{};
    };

    struct EspRenderFrame {
        CameraCache camera{};
        std::vector<EspFramePlayer> players;
        std::vector<EspFrameWorld> world;
        std::vector<EspFrameWorld> robots;
        uint64_t frameSeq = 0;
        uint64_t worldGeneration = 0;
        // Steady ms when this frame was collected. Paint continues the
        // velocity lead from here so skeletons/boxes do not swim behind
        // during the collect→present gap.
        uint64_t collectStampMs = 0;
        bool valid = false;
    };

    enum class EspFrameResult : uint8_t {
        None = 0,
        Published,
        Inactive,
        ScatterInitFailed,
        ScatterExecuteFailed,
        CameraFailed,
        GenerationChanged,
    };

    struct EspFrameDiagnostics {
        std::atomic<uint64_t> lastCollectStartMs{ 0 };
        std::atomic<uint64_t> lastPublishMs{ 0 };
        std::atomic<uint64_t> lastFailureMs{ 0 };
        std::atomic<uint64_t> lastPublishedFrameSeq{ 0 };
        std::atomic<uint64_t> lastPublishedGeneration{ 0 };
        std::atomic<int> lastResult{ static_cast<int>(EspFrameResult::None) };
        std::atomic<int> consecutiveFailures{ 0 };
        std::atomic<int> lastPlayerCount{ 0 };
        std::atomic<int> lastRobotCount{ 0 };
        std::atomic<int> lastWorldCount{ 0 };
    };

    void RecordEspFrameFailure(EspFrameResult result) {
        const uint64_t nowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        m_espFrameDiagnostics.lastResult.store(
            static_cast<int>(result), std::memory_order_release);
        m_espFrameDiagnostics.lastFailureMs.store(
            nowMs, std::memory_order_release);
        m_espFrameDiagnostics.consecutiveFailures.fetch_add(
            1, std::memory_order_relaxed);
    }

    void RecordEspFramePublished(const EspRenderFrame& frame) {
        m_espFrameDiagnostics.lastResult.store(
            static_cast<int>(EspFrameResult::Published), std::memory_order_release);
        m_espFrameDiagnostics.lastPublishMs.store(
            SteadyEspNowMs(), std::memory_order_release);
        m_espFrameDiagnostics.lastPublishedFrameSeq.store(
            frame.frameSeq, std::memory_order_release);
        m_espFrameDiagnostics.lastPublishedGeneration.store(
            frame.worldGeneration, std::memory_order_release);
        m_espFrameDiagnostics.consecutiveFailures.store(
            0, std::memory_order_release);
        m_espFrameDiagnostics.lastPlayerCount.store(
            static_cast<int>(frame.players.size()), std::memory_order_relaxed);
        m_espFrameDiagnostics.lastRobotCount.store(
            static_cast<int>(frame.robots.size()), std::memory_order_relaxed);
        m_espFrameDiagnostics.lastWorldCount.store(
            static_cast<int>(frame.world.size()), std::memory_order_relaxed);
    }

    void ResetEspFrameDiagnostics() {
        m_espFrameDiagnostics.lastCollectStartMs.store(0, std::memory_order_release);
        m_espFrameDiagnostics.lastPublishMs.store(0, std::memory_order_release);
        m_espFrameDiagnostics.lastFailureMs.store(0, std::memory_order_release);
        m_espFrameDiagnostics.lastPublishedFrameSeq.store(0, std::memory_order_release);
        m_espFrameDiagnostics.lastPublishedGeneration.store(0, std::memory_order_release);
        m_espFrameDiagnostics.lastResult.store(
            static_cast<int>(EspFrameResult::None), std::memory_order_release);
        m_espFrameDiagnostics.consecutiveFailures.store(0, std::memory_order_release);
        m_espFrameDiagnostics.lastPlayerCount.store(0, std::memory_order_relaxed);
        m_espFrameDiagnostics.lastRobotCount.store(0, std::memory_order_relaxed);
        m_espFrameDiagnostics.lastWorldCount.store(0, std::memory_order_relaxed);
    }

    static uint64_t SteadyEspNowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    // Published as an immutable snapshot. Consumers copy the shared_ptr (no
    // deep copy); the 250Hz aim thread was previously memcpy'ing the whole
    // frame twice per tick (~100MB/s churn) plus one copy per paint.
    std::shared_ptr<const EspRenderFrame> m_espFrameShared;
    // Paint/aim retain the last accepted immutable snapshot without blocking
    // on the frame mutex. These atomics are reset with the world snapshot.
    std::atomic<std::shared_ptr<const EspRenderFrame>> m_espPaintFrame{nullptr};
    std::atomic<std::shared_ptr<const EspRenderFrame>> m_espAimFrame{nullptr};
    EspFrameDiagnostics m_espFrameDiagnostics;
    mutable std::shared_mutex m_espFrameMutex;
    std::atomic<uint64_t> m_espFrameSeq{ 0 };

public:


    static Vector3 ToVector3(const FVector3d& v)
    {
        return Vector3{
            static_cast<float>(v.x),
            static_cast<float>(v.y),
            static_cast<float>(v.z)
        };
    }

    /** ComponentToWorld translation is FVector3d @ +0x20 on this build (not float Vector3). */
    static Vector3 ReadSceneWorldPos(uintptr_t sceneComponent)
    {
        if (!sceneComponent || !Memory::IsValidPtrFast2(sceneComponent))
            return {};
        const FVector3d world = Memory::read<FVector3d>(
            sceneComponent + Offsets::WorldLocation);
        if (world.x != 0.0 || world.y != 0.0 || world.z != 0.0) {
            const Vector3 w = ToVector3(world);
            if (IsPlausibleWorldPos(w))
                return w;
        }
        const Vector3 rel = Memory::read<Vector3>(
            sceneComponent + Offsets::RelativeLocation);
        if (IsPlausibleWorldPos(rel))
            return rel;
        return {};
    }

    /**
     * NOCACHE WorldLocation read. allowRelativeFallback matches EntityList
     * trySceneNC; bots omit RelativeLocation (spawn-footprint freeze).
     */
    static Vector3 ReadWorldLocationNocache(uintptr_t sceneComponent, bool allowRelativeFallback)
    {
        if (!sceneComponent || !Memory::IsValidPtrFast2(sceneComponent))
            return {};
        // Offsets::WorldLocation is pinned to the ComponentToWorld (0x2D0) slot.
        // Bot roots do not use that slot on this build: the pos_refresh probe
        // reported bFail == bots, 39/39, for every pass while every other reader
        // went through the probe. Try the alternate slot before giving up, the
        // way ProbeComponentToWorldOffset does for the local pawn.
        const FVector3d world =
            Memory::read_nocache<FVector3d>(sceneComponent + Offsets::WorldLocation);
        const Vector3 w = ToVector3(world);
        if (IsPlausibleWorldPos(w))
            return w;
        const FVector3d worldAlt = Memory::read_nocache<FVector3d>(
            sceneComponent + Offsets::ComponentToWorld_Alt
                + Offsets::Transform_Translation);
        const Vector3 wAlt = ToVector3(worldAlt);
        if (IsPlausibleWorldPos(wAlt))
            return wAlt;
        if (!allowRelativeFallback)
            return {};
        const Vector3 rel =
            Memory::read_nocache<Vector3>(sceneComponent + Offsets::RelativeLocation);
        return IsPlausibleWorldPos(rel) ? rel : Vector3{};
    }

    /**
     * ComponentToWorld is not a UPROPERTY on this build, so no dump carries the slot.     * The working runtime profile uses 0x2D0; 0x310 stays as the alternate
     * candidate. Both use the dump's CoreUObject.Transform layout: Rotation
     * +0x00 (Quat), Translation +0x20, Scale3D +0x40, size 0x60. The same
     * world-position gate decides which candidate is usable at runtime.
     */
    static std::ptrdiff_t ProbeComponentToWorldOffset(uintptr_t sceneComponent)
    {
        if (!sceneComponent || !Memory::IsValidPtrFast2(sceneComponent))
            return Offsets::ComponentToWorld;

        auto plausible = [&](std::ptrdiff_t off) -> bool {
            const FVector3d t = Memory::read_nocache<FVector3d>(
                sceneComponent + off + Offsets::Transform_Translation);
            return IsPlausibleWorldPos(ToVector3(t));
        };
        if (plausible(Offsets::ComponentToWorld))
            return Offsets::ComponentToWorld;
        if (plausible(Offsets::ComponentToWorld_Alt))
            return Offsets::ComponentToWorld_Alt;
        return Offsets::ComponentToWorld;
    }

    /** FTransform.Translation is float; live world translation is FVector3d @ C2W+0x20. */
    static FTransform ReadComponentToWorld(uintptr_t sceneComponent)
    {
        FTransform ctw{};
        if (!sceneComponent || !Memory::IsValidPtrFast2(sceneComponent))
            return ctw;
        const std::ptrdiff_t ctwOff = ProbeComponentToWorldOffset(sceneComponent);
        ctw = Memory::read_nocache<FTransform>(sceneComponent + ctwOff);
        const FVector3d t = Memory::read_nocache<FVector3d>(
            sceneComponent + ctwOff + Offsets::Transform_Translation);
        ctw.Translation.x = static_cast<float>(t.x);
        ctw.Translation.y = static_cast<float>(t.y);
        ctw.Translation.z = static_cast<float>(t.z);
        return ctw;
    }

    /**
     * AActor::ReplicatedMovement (0x150) -> FRepMovement::Location (0x30): three
     * LWC doubles. This is the only world position on an actor that the 20260922
     * dump reflects (prop:Engine.RepMovement.Location), so it is the verified
     * cross-check for the ComponentToWorld path — ComponentToWorld is not a
     * UPROPERTY on this build (no dump source) and its offset is derived.
     */
    static Vector3 ReadActorReplicatedLocation(uintptr_t actor)
    {
        if (!actor || !Memory::IsValidPtrFast2(actor))
            return {};
        const FVector3d loc = Memory::read<FVector3d>(
            actor + Offsets::ReplicatedMovement + Offsets::RepMovement_Location);
        const Vector3 v = ToVector3(loc);
        return IsPlausibleWorldPos(v) ? v : Vector3{};
    }

    /** Scene-component world position first, replicated location second. */
    static Vector3 ReadActorWorldPos(uintptr_t actor)
    {
        if (const uintptr_t root = ResolveActorRoot(actor)) {
            const Vector3 pos = ReadSceneWorldPos(root);
            if (IsPlausibleWorldPos(pos))
                return pos;
        }
        return ReadActorReplicatedLocation(actor);
    }

    /** Distance anchor: frame camera first (matches projection); pawn only if camera bad. */
    static Vector3 ResolveDistanceReference(const CameraCache& cam, uintptr_t localPawn)
    {
        if (IsPlausibleWorldPos(cam.Location))
            return cam.Location;
        if (localPawn) {
            const uintptr_t pawnRoot = ResolveActorRoot(localPawn);
            if (pawnRoot) {
                const Vector3 pawnPos = ReadSceneWorldPos(pawnRoot);
                if (IsPlausibleWorldPos(pawnPos))
                    return pawnPos;
            }
        }
        return cam.Location;
    }

    static float EspDistanceMeters(
        const Vector3& worldPos,
        const CameraCache& cam,
        uintptr_t localPawn)
    {
        const Vector3 ref = ResolveDistanceReference(cam, localPawn);
        const double dx = static_cast<double>(worldPos.x - ref.x);
        const double dy = static_cast<double>(worldPos.y - ref.y);
        const double dz = static_cast<double>(worldPos.z - ref.z);
        return static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0);
    }

    static bool IsUsableObjectPtr(uintptr_t p)
    {
        return p != 0 && p != UINTPTR_MAX && Memory::IsValidPtrFast2(p);
    }

    /** Reject pointers that look like packed ASCII UTF-16 (common GWorld/GI garbage). */
    static bool LooksLikeUtf16Garbage(uintptr_t p)
    {
        if (!p)
            return true;
        int asciiPairs = 0;
        for (int i = 0; i < 4; ++i) {
            const unsigned char lo = static_cast<unsigned char>((p >> (i * 16)) & 0xFF);
            const unsigned char hi = static_cast<unsigned char>((p >> (i * 16 + 8)) & 0xFF);
            if (hi == 0 && lo >= 0x20 && lo < 0x7F)
                ++asciiPairs;
        }
        return asciiPairs >= 3;
    }

    /** Usable object ptr that also rejects UTF-16-looking garbage (Update GWorld path). */
    static bool IsPlausibleObjPtr(uintptr_t p)
    {
        return IsUsableObjectPtr(p) && !LooksLikeUtf16Garbage(p);
    }

 	/** RootComponent is often TObjectPtr — 0xFF..FF is invalid; try alt offsets + mesh. */
    static uintptr_t ResolveActorRoot(uintptr_t actor)
    {
        if (!IsUsableObjectPtr(actor))
            return 0;

        static const std::ptrdiff_t kRootOffs[] = {
            Offsets::RootComponent
        };
        for (std::ptrdiff_t off : kRootOffs) {
            const uintptr_t root = Memory::read_nocache<uintptr_t>(actor + off);
            if (!IsUsableObjectPtr(root))
                continue;
            const Vector3 pos = ReadSceneWorldPos(root);
            const float magSq = static_cast<float>(
                pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
            if (magSq > 100.f)
                return root;
        }

        static const std::ptrdiff_t kMeshOffs[] = {
            Offsets::USkeletalMeshComponent, Offsets::EmbarkMesh
        };
        for (std::ptrdiff_t off : kMeshOffs) {
            const uintptr_t mesh = Memory::read_nocache<uintptr_t>(actor + off);
            if (!IsUsableObjectPtr(mesh))
                continue;
            const Vector3 pos = ReadSceneWorldPos(mesh);
            const float magSq = static_cast<float>(
                pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
            if (magSq > 100.f)
                return mesh;
        }
        return 0;
    }

    /** Ground loot: Offsets::Pickup_RootCollider is often the real world anchor (dump: Pickup.RootCollider). */
    static uintptr_t ResolveLootActorRoot(uintptr_t actor, bool preferPickupCollider = false)
    {
        if (!IsUsableObjectPtr(actor))
            return 0;

        auto tryPickupCollider = [&]() -> uintptr_t {
            const uintptr_t collider =
                Memory::read_nocache<uintptr_t>(actor + Offsets::Pickup_RootCollider);
            if (!IsUsableObjectPtr(collider))
                return 0;
            const Vector3 pos = ReadSceneWorldPos(collider);
            const float magSq = static_cast<float>(
                pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
            return magSq > 100.f ? collider : 0;
        };

        if (preferPickupCollider) {
            if (const uintptr_t collider = tryPickupCollider())
                return collider;
            return ResolveActorRoot(actor);
        }

        if (const uintptr_t root = ResolveActorRoot(actor))
            return root;
        return tryPickupCollider();
    }

    /**
     * Read the possessed pawn pointer off a controller, trying every known slot:
     * 0x3F0 (SDK-verified AController.Pawn), 0x3D8 (StateName per SDK — heuristic),
     * 0x408 (PC::Character — heuristic). A wrong primary slot can no longer zero
     * PC resolution — callers still apply their own world-position/root gates.
     */
    static uintptr_t ReadAcknowledgedPawn(uintptr_t controller)
    {
        if (!IsUsableObjectPtr(controller))
            return 0;

        static const std::ptrdiff_t kPawnOffs[] = {
            Offsets::AcknowledgedPawn,
            Offsets::AcknowledgedPawn_Fallback,
            Offsets::Controller_Character,
        };
        for (std::ptrdiff_t off : kPawnOffs) {
            const uintptr_t pawn = Memory::read_nocache<uintptr_t>(controller + off);
            if (IsUsableObjectPtr(pawn))
                return pawn;
        }
        return 0;
    }

    static uintptr_t ResolveAcknowledgedPawn(uintptr_t pc)
    {
        if (!IsUsableObjectPtr(pc))
            return 0;

        const uintptr_t pawn = ReadAcknowledgedPawn(pc);
        if (pawn && ResolveActorRoot(pawn))
            return pawn;
        return 0;
    }

    CameraCache g_Camera;
    mutable std::shared_mutex m_cameraMutex;
public:
    // gameIndex -> UniBone lives in Core/BoneRoster.hpp: rebuilt at runtime
    // from the skeleton's own bone NAMES (patch-proof — a static index table
    // drifts and lands bones on the wrong body parts), with the static
    // CL-1233465 table as its fallback. Access via BoneRoster::GameBoneMap().

    std::vector<std::pair<UniBone, UniBone>> SkeletonLinksArcRaiders = {
        { UniBone::Pelvis, UniBone::Spine1 },
        { UniBone::Spine1, UniBone::Spine2 },
        { UniBone::Spine2, UniBone::Spine3 },
        { UniBone::Spine3, UniBone::Chest },
        { UniBone::Chest, UniBone::Neck },
        { UniBone::Neck, UniBone::Head },

        { UniBone::Chest,      UniBone::ClavicleL },
        { UniBone::ClavicleL,  UniBone::UpperArmL },
        { UniBone::UpperArmL,  UniBone::LowerArmL },
        { UniBone::LowerArmL,  UniBone::HandL },

        { UniBone::Chest,      UniBone::ClavicleR },
        { UniBone::ClavicleR,  UniBone::UpperArmR },
        { UniBone::UpperArmR,  UniBone::LowerArmR },
        { UniBone::LowerArmR,  UniBone::HandR },

        { UniBone::Pelvis, UniBone::ThighL },
        { UniBone::ThighL, UniBone::CalfL },
        { UniBone::CalfL,  UniBone::FootL },

        { UniBone::Pelvis, UniBone::ThighR },
        { UniBone::ThighR, UniBone::CalfR },
        { UniBone::CalfR,  UniBone::FootR },
    };
public:
    double DegToRad(double deg)
    {
        return deg * M_PI / 180.0;
    }

    std::string toLower(std::string str)
    {
        std::transform(str.begin(), str.end(), str.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return str;
    }

    D3DMATRIX MatrixMultiplication(D3DMATRIX pM1, D3DMATRIX pM2)
    {
        D3DMATRIX pOut;
        pOut._11 = pM1._11 * pM2._11 + pM1._12 * pM2._21 + pM1._13 * pM2._31 + pM1._14 * pM2._41;
        pOut._12 = pM1._11 * pM2._12 + pM1._12 * pM2._22 + pM1._13 * pM2._32 + pM1._14 * pM2._42;
        pOut._13 = pM1._11 * pM2._13 + pM1._12 * pM2._23 + pM1._13 * pM2._33 + pM1._14 * pM2._43;
        pOut._14 = pM1._11 * pM2._14 + pM1._12 * pM2._24 + pM1._13 * pM2._34 + pM1._14 * pM2._44;
        pOut._21 = pM1._21 * pM2._11 + pM1._22 * pM2._21 + pM1._23 * pM2._31 + pM1._24 * pM2._41;
        pOut._22 = pM1._21 * pM2._12 + pM1._22 * pM2._22 + pM1._23 * pM2._32 + pM1._24 * pM2._42;
        pOut._23 = pM1._21 * pM2._13 + pM1._22 * pM2._23 + pM1._23 * pM2._33 + pM1._24 * pM2._43;
        pOut._24 = pM1._21 * pM2._14 + pM1._22 * pM2._24 + pM1._23 * pM2._34 + pM1._24 * pM2._44;
        pOut._31 = pM1._31 * pM2._11 + pM1._32 * pM2._21 + pM1._33 * pM2._31 + pM1._34 * pM2._41;
        pOut._32 = pM1._31 * pM2._12 + pM1._32 * pM2._22 + pM1._33 * pM2._32 + pM1._34 * pM2._42;
        pOut._33 = pM1._31 * pM2._13 + pM1._32 * pM2._23 + pM1._33 * pM2._33 + pM1._34 * pM2._43;
        pOut._34 = pM1._31 * pM2._14 + pM1._32 * pM2._24 + pM1._33 * pM2._34 + pM1._34 * pM2._44;
        pOut._41 = pM1._41 * pM2._11 + pM1._42 * pM2._21 + pM1._43 * pM2._31 + pM1._44 * pM2._41;
        pOut._42 = pM1._41 * pM2._12 + pM1._42 * pM2._22 + pM1._43 * pM2._32 + pM1._44 * pM2._42;
        pOut._43 = pM1._41 * pM2._13 + pM1._42 * pM2._23 + pM1._43 * pM2._33 + pM1._44 * pM2._43;
        pOut._44 = pM1._41 * pM2._14 + pM1._42 * pM2._24 + pM1._43 * pM2._34 + pM1._44 * pM2._44;

        return pOut;
    }

    std::string GetPlayerName(uintptr_t playerStateAddr, uintptr_t pawnAddr = 0) {
        if (!playerStateAddr && !pawnAddr)
            return "";
        return steam_decrypt::ResolvePlayerDisplayName(pawnAddr, playerStateAddr);
    }

    static bool IsPlausibleUsermodePtr(uintptr_t ptr)
    {
        return ptr > 0x10000 && ptr < 0x00007FFFFFFFFFFF;
    }

    /** help/esp.txt: pawn+0xDC8 → HC, then HC+0x668 / HC+0x308 as double.
     *  Use NOCACHE — cached Memory::read left HP stuck at 100% (same class of
     *  bug as frozen remote positions before nocache). */
    static double ReadHealthComponentStat(uintptr_t actor, std::ptrdiff_t compOff)
    {
        if (!actor)
            return std::numeric_limits<double>::quiet_NaN();
        const uintptr_t health_comp =
            Memory::read_nocache<uintptr_t>(actor + Offsets::HealthComponent);
        if (!health_comp || !IsPlausibleUsermodePtr(health_comp))
            return std::numeric_limits<double>::quiet_NaN();
        const double value = Memory::read_nocache<double>(health_comp + compOff);
        if (!std::isfinite(value))
            return std::numeric_limits<double>::quiet_NaN();
        return value;
    }

    static double ReadPlayerStatWithHealthFallback(
        uintptr_t actor,
        std::ptrdiff_t psOff,
        std::ptrdiff_t compOff)
    {
        (void)psOff;
        if (!actor)
            return std::numeric_limits<double>::quiet_NaN();
        const double hcValue = ReadHealthComponentStat(actor, compOff);
        if (std::isfinite(hcValue) && hcValue >= 0.0 && hcValue < 100000.0)
            return hcValue;
        return std::numeric_limits<double>::quiet_NaN();
    }

    double get_health(uintptr_t actor)
    {
        return ReadPlayerStatWithHealthFallback(
            actor, Offsets::PlayerState_Health, Offsets::Health);
    }

    double get_maxhealth(uintptr_t actor)
    {
        return ReadPlayerStatWithHealthFallback(
            actor, Offsets::PlayerState_MaxHealth, Offsets::MaxHealth);
    }

    double get_armor(uintptr_t actor)
    {
        if (!actor)
            return 0.0;
        const double v = ReadHealthComponentStat(actor, Offsets::Shield);
        return std::isfinite(v) ? v : 0.0;
    }

    double get_maxarmor(uintptr_t actor)
    {
        if (!actor)
            return 0.0;
        const double v = ReadHealthComponentStat(actor, Offsets::ShieldMax);
        return std::isfinite(v) ? v : 0.0;
    }
public:
    // Read WeaponQuality (int8 tier 0..4) directly off a resolved BP_WeaponActor_*
    // instance. Works for remote players' weapons found via Instigator/Owner,
    // where the local InventoryComponent item array is empty.
    int GetWeaponQualityFromActor(uintptr_t weaponActor) {
        if (!weaponActor)
            return -1;
        return static_cast<int>(
            Memory::read<uint8_t>(weaponActor + Offsets::WeaponQuality));
    }
public:
    struct AimTarget {
        uint64_t entityKey = 0;
        Vector3 aimPos;       // Screen position
        Vector3 worldPos;     // World position (NOVO)
        float distToCenter = FLT_MAX;
        float score = -FLT_MAX;
        float distanceM = 0.f;
        float health = 100.f;
        bool isRobot = false;
        int32_t partID = -1;
        int32_t resistGroup = 0;
        uint8_t aimPosSrc = 0;
        float aimWorldAgeMs = 0.f;
    };

    Vector3 PredictPosition(
        const Vector3& targetPos,
        const Vector3& targetVelocity,
        const Vector3& myPos,
        float bulletSpeed,
        int iterations);

    Vector3 GetActorVelocity(uintptr_t actor);

    bool GetRobotAimPoint2D(
        const WorldCacheEntry& robot,
        float fovRadius,
        Vector3& outScreenPos,
        Vector3& outWorldPos,
        int32_t& outPartID,
        int32_t& outResistGroup);

    void AimAssistRobot(
        const Vector3& screenCenter,
        float fovRadius,
        float currentTime,
        std::vector<AimTarget>& targets);

    void AimAssistPlayer(
        const Vector3& screenCenter,
        float fovRadius,
        float bulletSpeed,
        float currentTime,
        std::vector<AimTarget>& targets);

public:
    std::unordered_set<std::string> robotsList = kRobotsList;
public:
    class FNameCache {
    private:
        std::unordered_map<int32_t, std::string> m_cache;
        mutable std::shared_mutex m_mutex;

        FNameCache() = default;

    public:
        static FNameCache& Instance() {
            static FNameCache instance;
            return instance;
        }

        void Clear() {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_cache.clear();
        }
    };
public:


public:
    bool g_fnameTablesReady = false;

    void HandleWorldLost();
    void CheckWorldTransition(uintptr_t newWorld, uintptr_t newPersistentLevel);
};

extern Engine engine;
