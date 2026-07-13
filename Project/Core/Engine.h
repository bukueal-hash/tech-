#pragma once
#include "Vector.hpp"
#include "Cache.hpp"
#include "Offsets.h"
#include "SteamDecrypt.hpp"
#include "BotTypes.h"

#include <atomic>
#include <cmath>
#include <random>
#include <deque>
#include <chrono>
#include <d3d9.h>
#include <shared_mutex>
#include <unordered_set>
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

class Engine {
public:
    struct WorldCacheEntry;
private:
    std::atomic<bool> entityStarted{ false };

    std::atomic<bool> m_workerThreadsStarted{ false };
    std::unique_ptr<SyncedThread> m_worldThread;
    std::unique_ptr<SyncedThread> m_entityThread;
    std::unique_ptr<SyncedThread> m_worldEspThread;
    std::unique_ptr<SyncedThread> m_containerEspThread;
    std::unique_ptr<SyncedThread> m_robotEspThread;
    std::unique_ptr<SyncedThread> m_aimThread;
    std::unique_ptr<SyncedThread> m_positionThread;
    std::unique_ptr<SyncedThread> m_frameBuilderThread;

    // Thread synchronization
    mutable std::shared_mutex m_stateMutex;       // Protects state pointers (GWorld, PersistentLevel, etc.)
    mutable std::shared_mutex m_playerCacheMutex;  // Protects playerCache
    mutable std::shared_mutex m_containerCacheMutex; // Protects containerCache
    mutable std::shared_mutex m_itemCacheMutex;    // Protects itemCache
    mutable std::shared_mutex m_robotCacheMutex;   // Protects robotCache
    std::atomic<uint64_t> m_worldGeneration{ 0 };    // Incremented on world change

    std::atomic<bool> m_raidRaw{ false };
    std::atomic<bool> m_espRaidActive{ false };
    bool m_raidEnterPending{ false };
    std::chrono::steady_clock::time_point m_raidDebSince{};

    static constexpr std::chrono::milliseconds kRaidEnterDelayMs{ 1500 };
    static constexpr std::chrono::milliseconds kRaidRawFalseGraceMs{ 5000 };

    std::chrono::steady_clock::time_point m_raidFalseSince{};

    void TickRaidGate();
    void ClearEspCaches();
    /** PC cache, camera, FName/decrypt — same class of reset as process restart for raid transitions. */
    void ResetRaidTransitionState();

public:
    enum class EItemRarity : uint8_t
    {
        Common = 0,
        Uncommon = 1,
        Rare = 2,
        Epic = 3,
        Legendary = 4,
        MAX = 5,
        INVALID = 0xFF
    };

    struct CameraCache
    {
        Vector3 Location;
        Vector3 Rotation;
        float FOV;
    };

    struct FVector3d;
    struct CameraProbeSnapshot;

    BoneData boneData;

    void Update();
    void EntityList();
    void RobotList();
    void ContainerList();
    void ItemList();

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
        uintptr_t localPawn,
        int& outDrawing);

    void StartWorkerThreads();
    void StopWorkerThreads();

    void RenderEsp();
    void RenderPlayerEspFromCache(const CameraCache& renderCam);
    void RenderFovCircle();
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

    uintptr_t ResolveBestGWorld(uint64_t moduleBase);
    uintptr_t GetGameInstance(uint64_t uworldAddr);
    /** Recover GameInstance from LocalPlayer Outer when UWorld+0x4D8 is encrypted. */
    uintptr_t ResolveGameInstanceFromLocalPlayer(uintptr_t localPlayer);
    /** CL-1315578: UWorld::LevelCollections → collection.GameState. */
    uintptr_t ResolveGameStateFromWorld(uintptr_t uworldAddr);
    bool ResolveLevelActors(uintptr_t persistentLevel, uintptr_t& outActorsData, int& outActorCount);
    bool ResolveLocalPlayerFromGameInstance(uintptr_t gameInstance, uintptr_t& outLocalPlayer,
        uintptr_t& outPlayerController);
    /** When PC is resolved via actor/PCM scan but GI TArray path failed. */
    uintptr_t ResolveLocalPlayerFromController(uintptr_t playerController);
    bool ResolveLocalPlayerChainFromActors(uintptr_t persistentLevel, uintptr_t actorsArray, int actorCount,
        uintptr_t gameInstance, uintptr_t& outController, uintptr_t& outPawn);
    uintptr_t GetCameraManagerFromActors();
    bool ControllerHasValidPcm(uintptr_t pc);
    bool ResolvePcFromLevelCameraManager(uintptr_t level, uintptr_t actorsData,
        uintptr_t& outPc, uintptr_t& outPawn, uintptr_t& outPcm);

    void UpdateCamera();
    void SetProjectionViewport(float width, float height);
    Vector3 GetProjectionScreenCenter() const;
    bool RefreshCameraFromViewTarget();

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
    bool ProjectWorldLocationToRadar(const Vector3& myWorldLocation, const Vector3& enemyWorldLocation, float myYaw, Vector3& outRadar);
    bool Visible(uintptr_t mesh) const;
    bool VisibleActor(uintptr_t actor) const;
    /** Bot-only mesh vis: embark-first + SDK recently-rendered fallback. */
    bool VisibleBotActor(uintptr_t actor) const;
    /** Collision KD-tree LOS when obstruction_check; else true. */
    bool HasLineOfSight(const Vector3& from, const Vector3& to) const;

    struct VisCheckDebugStats {
        int playersTotal = 0;
        int playersMeshVisible = 0;
        int botsTotal = 0;
        int botsMeshVisible = 0;
        bool collisionLosEnabled = false;
        int collisionTriCount = 0;
        int collisionSmcCount = 0;
        bool collisionRebuilding = false;
        float sampleSubmit = 0.f;
        float sampleRender = 0.f;
        float sampleRenderScr = 0.f;
        bool sampleOnScreen = false;
        bool sampleRecent = false;
        bool sampleVisible = false;
        /** Decrypted LastRenderTimeOnScreen (UC enc path) or plain onScr. */
        float sampleDecryptedLrtos = 0.f;
        float sampleWorldTimeSeconds = 0.f;
        /** "enc" | "plain" | "fail" */
        const char* samplePathUsed = "fail";
        bool hasSample = false;
    };
    VisCheckDebugStats CollectVisCheckDebugStats() const;
    void PrintVisCheckDebugConsole();

    uintptr_t GetActorSkeletalMesh(uintptr_t actor) const;

    std::uintptr_t GetBoneArrayDecrypt(std::uintptr_t Meh);

    bool IsValidPointer(uintptr_t ptr) const;
    bool IsUsermodePtr(uintptr_t ptr);
    std::string getEntityType(const std::string& actorName);
    bool getAllowType(const std::string& actorName, int category = 0) const;
    bool getAllowWorldEntry(const WorldCacheEntry& entry) const;
    bool Has(const std::string& s, const char* sub);

    int32_t GetActorFNameId(uint64_t actor_base);
    std::string GetActorFNameString(uint64_t actor_base);

    std::string GetActorFNameStringCached(uintptr_t actor_base);
    std::string GetActorClassFName(uintptr_t actor_base);

    void ClearFNameCache();

    std::string GetEnglishItemName(uint64_t actor);
    std::string GetWeaponName(const std::string& internal_name);

    struct EngineStateSnapshot {
        uintptr_t gWorld = 0;
        uintptr_t gWorldRaw = 0;       // direct read at base+UWorld (even if validation fails)
        int gWorldFailStep = 0;       // 0=ok, 1=null slot, 2=bad PL, 3=bad actors, 4=soft-accepted
        uintptr_t persistentLevel = 0;
        uintptr_t actors = 0;
        uintptr_t playerController = 0;
        uintptr_t acknowledgedPawn = 0;
        uintptr_t rootComponent = 0;
        uintptr_t playerCameraManager = 0;
        uintptr_t owningGameInstance = 0;
        uintptr_t localPlayer = 0;
    };
    EngineStateSnapshot GetStateSnapshot() const;

    /** Last GWorld probe (updated every Update tick, even on miss). */
    std::atomic<uintptr_t> m_gWorldRaw{0};
    std::atomic<int> m_gWorldFailStep{0};

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

    float PlayerPosition;

    uintptr_t AcknowledgedPawn;
    uintptr_t RootComponent;
    uintptr_t PlayerState;
    uintptr_t Mesh;
    uintptr_t PlayerCameraManager;
    uintptr_t Actors;

    int LocalPlayerTeam;

    uintptr_t CurrentGun;

    uintptr_t AGameStateBase;
public: // PlayerCache
    struct PlayerCacheEntry {
        uintptr_t rootComponent = NULL;
        uintptr_t actorState = NULL;
        uintptr_t HealthPoint = NULL;
        uintptr_t actorMesh = NULL;
        uintptr_t APawn = NULL;

        Vector3 WorldPos;
        Vector3 RadarPos;

        Vector3 head;
        Vector3 feet;

        Vector3 ScreenTop;
        Vector3 ScreenBottom;

        std::string ActorName;

        bool isVisible;
        bool Drawing = false;
        float Distance = 0.f;
        bool isAlly = false;

        float health;
        float maxhealth;

        float shield;
        float maxshield;
        float shieldLevel;

        bool bIsDeathVerge;
        bool bIsABot;

        bool bIsDead;

        BoneData boneData;
        uintptr_t boneArray = 0;
        uintptr_t boneMesh = 0;

        std::string weaponName;
        int weaponQuality = -1;

        Vector3 cachedVelocity = { 0, 0, 0 };
        float lastVelocityUpdate = 0.0f;
        Vector3 lastWorldPos = { 0, 0, 0 };

        uintptr_t lastWeaponPtr = 0;

        PlayerCacheEntry() {};

        PlayerCacheEntry(std::string name, uint64_t root, uintptr_t Pawn, uintptr_t mesh)
            : ActorName(name), rootComponent(root), APawn(Pawn), actorMesh(mesh)
        {
        };
    };

    std::unordered_map<uintptr_t, PlayerCacheEntry> playerCache;

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
    bool IsCachedPlayer(uintptr_t actorKey) const {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        return playerCache.contains(actorKey);
    }
    bool EntityListReady() const { return entityStarted.load(std::memory_order_acquire); }
    bool IsEntityStarted() const { return EntityListReady(); }
    bool IsInRaidRaw() const;
    bool IsEspRaidActive() const {
        return m_espRaidActive.load(std::memory_order_acquire);
    }
    bool IsInRaid() const { return IsEspRaidActive(); }

    size_t CountWorldDrawable() const {
        return CountContainerDrawable() + CountItemDrawable();
    }

    size_t CountContainerDrawable() const {
        std::shared_lock<std::shared_mutex> lock(m_containerCacheMutex);
        size_t count = 0;
        for (const auto& [key, entry] : containerCache) {
            (void)key;
            if (entry.Drawing)
                ++count;
        }
        return count;
    }

    size_t CountItemDrawable() const {
        std::shared_lock<std::shared_mutex> lock(m_itemCacheMutex);
        size_t count = 0;
        for (const auto& [key, entry] : itemCache) {
            (void)key;
            if (entry.Drawing)
                ++count;
        }
        return count;
    }

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

    size_t CountRobotDrawable() const {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        size_t count = 0;
        for (const auto& [key, entry] : robotCache) {
            (void)key;
            if (entry.Drawing)
                ++count;
        }
        return count;
    }

    void GetBones(PlayerCacheEntry& actor);
    uintptr_t ResolveBoneArray(
        uintptr_t actor,
        uintptr_t primaryMesh,
        uintptr_t* outBoneMesh = nullptr);
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
        float lastVelocityUpdate = 0.f;
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
        int category = 0;
        float Distance = 0.f;

        float health;
        float maxhealth;

        std::string ItemDisplayName;
        std::string ItemType;
        EItemRarity ItemRarity;

        int lootRarityTier = 0;
    int lootValue = 0;
    uint8_t worldCategory = 0;

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
        bool valid = false;
    };

    EspRenderFrame m_lastEspFrame{};
    mutable std::shared_mutex m_espFrameMutex;
    std::atomic<uint64_t> m_espFrameSeq{ 0 };

    struct ActorTypeProbeState {
        uint32_t localId78 = 0;
        uint32_t localId70 = 0;
        uint32_t localId18 = 0;
        int discoveredOff = -1;
        uint32_t localIdDisc = 0;
        bool classIdPath = false;
        uint32_t nearestId78 = 0;
        float nearestDistM = 99999.f;
    };

    mutable std::shared_mutex m_probeMutex;
    ActorTypeProbeState m_actorTypeProbe{};

    ActorTypeProbeState GetActorTypeProbe() const
    {
        std::shared_lock<std::shared_mutex> lock(m_probeMutex);
        return m_actorTypeProbe;
    }

public:
    struct FVector3d
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
    };

    // UC CL-1315578 (qwe900 #3687): POV Relative Location+0x00, Rotation+0x28, FOV+0x50
    struct FMinimalViewInfo
    {
        FVector3d Location;         // 0x00 (UC: POV+0x00)
        char pad_to_rotation[0x10]; // 0x18–0x27
        FVector3d Rotation;         // 0x28 (UC: POV+0x28)
        char pad_to_fov[0x10];      // 0x40–0x4F
        float FOV;                  // 0x50 (UC: POV+0x50)
        // DesiredFOV / AspectRatio / PostProcessSettings omitted (not read)
    };

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

    /** FTransform.Translation is float; live world translation is FVector3d @ C2W+0x20. */
    static FTransform ReadComponentToWorld(uintptr_t sceneComponent)
    {
        FTransform ctw{};
        if (!sceneComponent || !Memory::IsValidPtrFast2(sceneComponent))
            return ctw;
        ctw = Memory::read_nocache<FTransform>(
            sceneComponent + Offsets::ComponentToWorld);
        const FVector3d t = Memory::read_nocache<FVector3d>(
            sceneComponent + Offsets::WorldLocation);
        ctw.Translation.x = static_cast<float>(t.x);
        ctw.Translation.y = static_cast<float>(t.y);
        ctw.Translation.z = static_cast<float>(t.z);
        return ctw;
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

    /** Ground loot: Pickup_RootCollider @0x460 is often the real world anchor (help CL-1315578). */
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

    static uintptr_t ResolveAcknowledgedPawn(uintptr_t pc)
    {
        if (!IsUsableObjectPtr(pc))
            return 0;

        static const std::ptrdiff_t kPawnOffs[] = {
            Offsets::AcknowledgedPawn
        };
        for (std::ptrdiff_t off : kPawnOffs) {
            const uintptr_t pawn = Memory::read_nocache<uintptr_t>(pc + off);
            if (!IsUsableObjectPtr(pawn))
                continue;
            if (ResolveActorRoot(pawn))
                return pawn;
        }
        return 0;
    }

    CameraCache g_Camera;
    mutable std::shared_mutex m_cameraMutex;
public:
    // Arc Raiders CL-1233465 skeleton indices (see fremework::game::bones)
    static inline const std::vector<std::pair<int, UniBone>> GameBoneMapArcRaiders = {
        { BoneID::Root, UniBone::Root },
        { BoneID::Pelvis, UniBone::Pelvis },
        { BoneID::Spine01, UniBone::Spine1 },
        { BoneID::Spine02, UniBone::Spine2 },
        { BoneID::Spine03, UniBone::Spine3 },
        { BoneID::Chest, UniBone::Chest },
        { BoneID::Neck, UniBone::Neck },
        { BoneID::Head, UniBone::Head },
        { BoneID::L_Clavicle, UniBone::ClavicleL },
        { BoneID::L_UpperArm, UniBone::UpperArmL },
        { BoneID::L_Forearm, UniBone::LowerArmL },
        { BoneID::L_Hand, UniBone::HandL },
        { BoneID::R_Clavicle, UniBone::ClavicleR },
        { BoneID::R_UpperArm, UniBone::UpperArmR },
        { BoneID::R_Forearm, UniBone::LowerArmR },
        { BoneID::R_Hand, UniBone::HandR },
        { BoneID::L_Thigh, UniBone::ThighL },
        { BoneID::L_Calf, UniBone::CalfL },
        { BoneID::L_Foot, UniBone::FootL },
        { BoneID::R_Thigh, UniBone::ThighR },
        { BoneID::R_Calf, UniBone::CalfR },
        { BoneID::R_Foot, UniBone::FootR },
    };

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
    double RadToDeg(double radians)
    {
        return radians * 180 / M_PI;
    }

    double DegToRad(double deg)
    {
        return deg * M_PI / 180.0;
    }

    uint32_t ROL4(uint32_t x, uint32_t n)
    {
        return (x << n) | (x >> (32 - n));
    }

    uint64_t ROL8(uint64_t x, uint32_t n)
    {
        return (x << n) | (x >> (64 - n));
    }

    uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    uint64_t rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

    uint64_t u64_lo(__m128i v) {
        alignas(16) uint64_t arr[2];
        _mm_store_si128(reinterpret_cast<__m128i*>(arr), v);
        return arr[0];
    }

    std::string toLower(std::string str)
    {
        std::transform(str.begin(), str.end(), str.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return str;
    }

    D3DMATRIX to_matrix(Vector3 rot, Vector3 origin = Vector3(0, 0, 0))
    {
        const float radpitch = static_cast<float>(rot.x * M_PI / 180.0);
        const float radyaw = static_cast<float>(rot.y * M_PI / 180.0);
        const float radroll = static_cast<float>(rot.z * M_PI / 180.0);
        float sp = sinf(radpitch);
        float cp = cosf(radpitch);
        float sy = sinf(radyaw);
        float cy = cosf(radyaw);
        float sr = sinf(radroll);
        float cr = cosf(radroll);
        D3DMATRIX matrix{};
        matrix.m[0][0] = cp * cy;
        matrix.m[0][1] = cp * sy;
        matrix.m[0][2] = sp;
        matrix.m[0][3] = 0.f;
        matrix.m[1][0] = sr * sp * cy - cr * sy;
        matrix.m[1][1] = sr * sp * sy + cr * cy;
        matrix.m[1][2] = -sr * cp;
        matrix.m[1][3] = 0.f;
        matrix.m[2][0] = -(cr * sp * cy + sr * sy);
        matrix.m[2][1] = cy * sr - cr * sp * sy;

        matrix.m[2][2] = cr * cp;
        matrix.m[2][3] = 0.f;
        matrix.m[3][0] = static_cast<float>(origin.x);
        matrix.m[3][1] = static_cast<float>(origin.y);
        matrix.m[3][2] = static_cast<float>(origin.z);
        matrix.m[3][3] = 1.f;
        return matrix;
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
public:
    uint32_t rol32(uint32_t v, int s) {
        return (v << s) | (v >> (32 - s));
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

    static double ReadHealthComponentStat(uintptr_t actor, std::ptrdiff_t compOff)
    {
        const uintptr_t health_comp =
            Memory::read<uintptr_t>(actor + Offsets::HealthComponent);
        if (!health_comp || !IsPlausibleUsermodePtr(health_comp))
            return 0.0;
        const double value = Memory::read<double>(health_comp + compOff);
        return std::isfinite(value) ? value : 0.0;
    }

    static double ReadPlayerStatWithHealthFallback(
        uintptr_t actor,
        std::ptrdiff_t psOff,
        std::ptrdiff_t compOff)
    {
        if (!actor)
            return 0.0;

        const uintptr_t ps = Memory::read<uintptr_t>(actor + Offsets::APlayerState);
        if (ps && IsPlausibleUsermodePtr(ps)) {
            const double psValue = Memory::read<double>(ps + psOff);
            if (std::isfinite(psValue) && psValue > 0.0)
                return psValue;
        }

        return ReadHealthComponentStat(actor, compOff);
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
        return ReadPlayerStatWithHealthFallback(
            actor, Offsets::PlayerState_Armor, Offsets::Shield);
    }

    double get_maxarmor(uintptr_t actor)
    {
        if (!actor)
            return 0.0;

        const uintptr_t ps = Memory::read<uintptr_t>(actor + Offsets::APlayerState);
        if (ps && IsPlausibleUsermodePtr(ps)) {
            const double psValue =
                Memory::read<double>(ps + Offsets::PlayerState_MaxArmor);
            if (std::isfinite(psValue) && psValue > 0.0)
                return psValue;
        }

        return ReadHealthComponentStat(actor, Offsets::Shield + 0x8);
    }

    struct PlayerHealthInfo {
        double Health;
        double MaxHealth;
        double Armor;
        double MaxArmor;
        bool bHasBrokenArmor;
        bool bIsDbno;
        char _pad[6];
    };
public:
    struct FTArrayRaw {
        uint64_t Data;
        uint32_t Count;
        uint32_t Max;
    };
    uintptr_t GetCurrentWeaponActor(uintptr_t actor) {
        if (!actor) return 0;
        auto inventoryComponent = Memory::read<uintptr_t>(actor + Offsets::InventoryComponent);
        if (!inventoryComponent)
            return 0;

        FTArrayRaw LocalCurrentItemActors = Memory::read<FTArrayRaw>(inventoryComponent + Offsets::LocalCurrentItemActors);

        if (!LocalCurrentItemActors.Data || LocalCurrentItemActors.Count <= 0)
            return 0;

        return Memory::read<uintptr_t>(LocalCurrentItemActors.Data);
    }

    int GetWeaponQuality(uintptr_t actor) {
        if (!actor) return -1;
        auto inventoryComponent = Memory::read<uintptr_t>(actor + Offsets::InventoryComponent);
        if (!inventoryComponent)
            return -1;

        FTArrayRaw itemArray = Memory::read<FTArrayRaw>(inventoryComponent + Offsets::LocalCurrentItemActors);

        if (!itemArray.Data || itemArray.Count <= 0 || itemArray.Count > 100)
            return -1;

        uintptr_t currentWeapon = Memory::read<uintptr_t>(itemArray.Data);
        if (!currentWeapon)
            return -1;

        uint8_t WeaponQuality = Memory::read<uint8_t>(currentWeapon + Offsets::WeaponQuality);

        return static_cast<int>(WeaponQuality);
    }

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
    std::unordered_map<std::string, ImColor> itemImColors = {
        // Laranja
        {"loot item",              ImColor(255, 165, 0, 255)},

        // Vermelho
        {"ai robot",             ImColor(227, 101, 101, 255)},
        {"turret",             ImColor(227, 101, 101, 255)},
        {"leaper",             ImColor(227, 101, 101, 255)},
        {"roll bot",             ImColor(227, 101, 101, 255)},
        {"fireball",             ImColor(227, 101, 101, 255)},
        {"pop",             ImColor(227, 101, 101, 255)},
        {"surveyor",             ImColor(227, 101, 101, 255)},
        {"hornet",             ImColor(227, 101, 101, 255)},
        {"rocketeer",             ImColor(227, 101, 101, 255)},
        {"the queen",             ImColor(227, 101, 101, 255)},
        {"matriarch",             ImColor(227, 101, 101, 255)},
        {"pinger robot",             ImColor(227, 101, 101, 255)},
        {"bastion",             ImColor(227, 101, 101, 255)},
        {"bombardier",             ImColor(227, 101, 101, 255)},
        {"sentinel",             ImColor(227, 101, 101, 255)},
        {"shredder",             ImColor(227, 101, 101, 255)},
        {"snitch",             ImColor(227, 101, 101, 255)},
        {"wasp",             ImColor(227, 101, 101, 255)},
        {"spotter",             ImColor(227, 101, 101, 255)},
        {"comet",             ImColor(227, 101, 101, 255)},
        {"firefly",             ImColor(227, 101, 101, 255)},

        // Azul
        {"arc cargoship",             ImColor(0, 255, 255, 255)},

        // Magenta
        {"raider stock",              ImColor(255, 0, 255, 255)},

        // Verde claro
        {"corpse",             ImColor(144, 238, 144, 255)},
    };
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

        bool TryGet(int32_t fnameId, std::string& outName) const {
            if (fnameId == 0)
                return false;

            std::shared_lock<std::shared_mutex> lock(m_mutex);
            auto it = m_cache.find(fnameId);
            if (it != m_cache.end()) {
                outName = it->second;
                return true;
            }
            return false;
        }

        void Add(int32_t fnameId, const std::string& name) {
            if (fnameId == 0 || name.empty())
                return;

            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_cache[fnameId] = name;
        }

        bool Contains(int32_t fnameId) const {
            std::shared_lock<std::shared_mutex> lock(m_mutex);
            return m_cache.find(fnameId) != m_cache.end();
        }

        void Clear() {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_cache.clear();
        }

        size_t Size() const {
            std::shared_lock<std::shared_mutex> lock(m_mutex);
            return m_cache.size();
        }
    };
public:


public:
    bool g_fnameTablesReady = false;

    void HandleWorldLost();
    void CheckWorldTransition(uintptr_t newWorld, uintptr_t newPersistentLevel);
};

extern Engine engine;
