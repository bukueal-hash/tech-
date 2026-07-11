#include "../Core/Engine.h"
#include "../Core/ActorType.h"
#include "../Core/AssetNames.h"
#include "../Core/IntervalTimer.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

namespace {

#pragma pack(push, 1)
struct ActorOwnerInstigator {
    uintptr_t owner;
    uint8_t   _pad[Offsets::ActorInstigator - Offsets::ActorOwner - sizeof(uintptr_t)];
    uintptr_t instigator;
};
#pragma pack(pop)

bool ClassLooksLikeEquippedWeapon(const std::string& cls)
{
    if (cls.empty())
        return false;
    if (cls.find("Stowed") != std::string::npos)
        return false;
    return cls.find("BP_WeaponActor_") != std::string::npos
        || cls.find("BP_Weapon_") != std::string::npos;
}

std::string ResolveWeaponEspLabel(
    Engine& eng, const std::string& cls, uintptr_t weaponActor)
{
    std::string label = eng.GetWeaponName(cls);
    if (label.empty() || label == cls) {
        const std::string dataAsset = GetActorDataAssetFName(weaponActor);
        if (!dataAsset.empty()) {
            if (const std::string fromAsset = LookupByAssetName(dataAsset);
                !fromAsset.empty())
                label = fromAsset;
            else if (const std::string human = HumanizeActorFName(dataAsset);
                !human.empty())
                label = human;
        }
    }
    if (label.empty())
        label = eng.GetWeaponName(cls);
    if (label.empty())
        return {};
    return FormatEspDisplayLabel(label);
}

void CollectGameStatePlayerPawns(
    Engine& eng,
    uintptr_t gWorld,
    std::unordered_set<uint64_t>& outPawns,
    std::unordered_map<uintptr_t, uintptr_t>& outPawnToPlayerState)
{
    outPawns.clear();
    outPawnToPlayerState.clear();
    if (!gWorld)
        return;

    const uintptr_t gameState = eng.ResolveGameStateFromWorld(gWorld);
    if (!gameState || !Memory::IsValidPtrFast2(gameState))
        return;

    const uintptr_t arrData =
        Memory::read<uintptr_t>(gameState + Offsets::GameState_PlayerArray);
    const int32_t arrNum =
        Memory::read<int32_t>(gameState + Offsets::GameState_PlayerArray + 8);
    if (!arrData || !Memory::IsValidPtrFast2(arrData) || arrNum <= 0 || arrNum > 128)
        return;

    const int limit = (std::min)(arrNum, 128);
    for (int i = 0; i < limit; ++i) {
        const uintptr_t playerState = Memory::read<uintptr_t>(
            arrData + static_cast<size_t>(i) * sizeof(uintptr_t));
        if (!playerState || !Memory::IsValidPtrFast2(playerState))
            continue;

        const uintptr_t pawn = Memory::read<uintptr_t>(
            playerState + Offsets::PlayerState_PawnPrivate);
        if (!pawn || !Memory::IsValidPtrFast2(pawn))
            continue;

        outPawns.insert(pawn);
        outPawnToPlayerState[pawn] = playerState;
    }
}

bool TryAdmitOriginalPlayer(
    Engine& eng,
    uintptr_t actor,
    uintptr_t ackPawn,
    uintptr_t playerStateHint,
    bool fromGameState,
    std::unordered_map<uintptr_t, Engine::PlayerCacheEntry>& localCache)
{
    if (!actor || actor == ackPawn || localCache.contains(actor))
        return false;

    uintptr_t playerState = playerStateHint;
    if (!playerState || !eng.IsValidPointer(playerState))
        playerState = Memory::read<uintptr_t>(actor + Offsets::APlayerState);
    if (!playerState || !eng.IsValidPointer(playerState))
        return false;

    const Engine::PlayerHealthInfo healthInfo =
        Memory::read<Engine::PlayerHealthInfo>(playerState + Offsets::HealthInfo);
    const bool bIsDeathVerge = healthInfo.bIsDbno;

    float health = static_cast<float>(healthInfo.Health);
    if (health < 1.0f && !bIsDeathVerge)
        health = static_cast<float>(eng.get_health(actor));
    if (health < 1.0f && !bIsDeathVerge)
        return false;

    std::string playerName = eng.GetPlayerName(playerState, actor);
    if (playerName.empty())
        playerName = eng.GetPlayerNameFromActor(actor);

    const uintptr_t root =
        Memory::read<uintptr_t>(actor + Offsets::RootComponent);
    if (!root || !eng.IsValidPointer(root))
        return false;

    uintptr_t mesh =
        Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
    if (!mesh || !eng.IsValidPointer(mesh)) {
        if (fromGameState)
            mesh = eng.GetActorSkeletalMesh(actor);
    }
    if (!mesh || !eng.IsValidPointer(mesh))
        return false;

    Engine::PlayerCacheEntry entry(playerName.c_str(), root, actor, mesh);
    entry.actorState = playerState;
    entry.APawn = actor;
    localCache.emplace(actor, std::move(entry));
    return true;
}

} // namespace

void Engine::EntityList()
{
    uintptr_t sGWorld, sPersistentLevel, sActors, sAcknowledgedPawn, sPlayerController;
    {
        std::shared_lock<std::shared_mutex> slock(m_stateMutex);
        sGWorld = GWorld;
        sPersistentLevel = PersistentLevel;
        sActors = Actors;
        sAcknowledgedPawn = AcknowledgedPawn;
        sPlayerController = PlayerController;
    }

    if (!sGWorld || !sPersistentLevel || !sAcknowledgedPawn || !sActors || !sPlayerController)
        return;

    const uint64_t gen = m_worldGeneration.load(std::memory_order_acquire);

    const int actor_count = Memory::read<int>(sPersistentLevel + Offsets::ActorsCount);
    if (sActors == 0 || actor_count <= 0 || actor_count > 10000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }

    std::vector<uint64_t> currentActors(static_cast<size_t>(actor_count));
    if (!Memory::ReadRaw(
            sActors,
            currentActors.data(),
            static_cast<size_t>(actor_count) * sizeof(uint64_t)))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }

    int dbgScanned = actor_count;
    int dbgPreAdmit = 0;
    int dbgDrawing = 0;
    int dbgGsPawns = 0;
    int dbgGsAdmit = 0;
    int dbgClassSkip = 0;

    std::unordered_set<uint64_t> currentActorSet(
        currentActors.begin(),
        currentActors.end());

    std::unordered_set<uint64_t> gameStatePawns;
    std::unordered_map<uintptr_t, uintptr_t> gameStatePawnToPs;
    CollectGameStatePlayerPawns(*this, sGWorld, gameStatePawns, gameStatePawnToPs);
    dbgGsPawns = static_cast<int>(gameStatePawns.size());

    UpdateCamera();

    CameraCache cam{};
    {
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        cam = g_Camera;
    }
    if (!IsUsableCameraFov(cam.FOV) || !IsPlausibleWorldPos(cam.Location)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }

    const float maxDistSq =
        static_cast<float>(var::esp_distance * var::esp_distance * 10000.0f);

    std::unordered_map<uintptr_t, PlayerCacheEntry> localCache;
    {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        localCache = playerCache;
    }

    for (auto it = localCache.begin(); it != localCache.end(); ) {
        const bool inLevel = currentActorSet.contains(it->first);
        const bool inGameState = gameStatePawns.contains(it->first);
        if (!inLevel && !inGameState)
            it = localCache.erase(it);
        else
            ++it;
    }

    for (uint64_t actor : currentActors)
    {
        if (!actor || actor == sAcknowledgedPawn || localCache.contains(actor))
            continue;

        const uint32_t masked =
            ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
        if (!ArcActorType::IsPlayerClassId(masked)) {
            ++dbgClassSkip;
            continue;
        }

        const uintptr_t quickPs =
            Memory::read<uintptr_t>(actor + Offsets::APlayerState);
        TryAdmitOriginalPlayer(
            *this, actor, sAcknowledgedPawn, quickPs, false, localCache);
    }

    for (uint64_t pawn : gameStatePawns)
    {
        if (!pawn || pawn == sAcknowledgedPawn || localCache.contains(pawn))
            continue;

        const auto psIt = gameStatePawnToPs.find(pawn);
        const uintptr_t psHint =
            psIt != gameStatePawnToPs.end() ? psIt->second : 0;
        if (TryAdmitOriginalPlayer(
                *this, pawn, sAcknowledgedPawn, psHint, true, localCache))
            ++dbgGsAdmit;
    }

    dbgPreAdmit = static_cast<int>(localCache.size());

    struct WeaponHit {
        uintptr_t actor = 0;
        std::string name;
        int quality = -1;
    };
    std::unordered_map<uintptr_t, WeaponHit> pawnToWeapon;

    static IntervalTimer weaponScanTimer(500);
    const bool doWeaponScan = weaponScanTimer.fire() && var::show_weapon && !localCache.empty();
    if (doWeaponScan) {
        for (uint64_t a : currentActors) {
            if (!a || a == sAcknowledgedPawn)
                continue;

            const ActorOwnerInstigator oi =
                Memory::read<ActorOwnerInstigator>(a + Offsets::ActorOwner);
            uintptr_t holder = 0;
            if (oi.instigator && localCache.contains(oi.instigator))
                holder = oi.instigator;
            else if (oi.owner && localCache.contains(oi.owner))
                holder = oi.owner;
            if (!holder || pawnToWeapon.contains(holder))
                continue;

            const std::string cls = GetActorClassFName(a);
            if (!ClassLooksLikeEquippedWeapon(cls)) {
                const std::string dataAsset = GetActorDataAssetFName(a);
                if (dataAsset.find("DA_Item_") == std::string::npos
                    && dataAsset.find("Weapon") == std::string::npos)
                    continue;
            }

            WeaponHit hit;
            hit.actor = a;
            hit.quality = GetWeaponQualityFromActor(a);
            hit.name = ResolveWeaponEspLabel(*this, cls, a);
            if (hit.name.empty())
                hit.name = GetWeaponName(cls);
            pawnToWeapon.emplace(holder, std::move(hit));
        }
    }

    const uint8_t myTeamId = Memory::read<uint8_t>(sAcknowledgedPawn + Offsets::TeamID);

    int dbgTeamEvict = 0;
    int dbgPsEvict = 0;
    int dbgPosEvict = 0;
    int dbgDistSkip = 0;
    int dbgBoneMiss = 0;

    for (auto it = localCache.begin(); it != localCache.end(); )
    {
        auto& actor = it->second;
        const uintptr_t key = it->first;
        actor.APawn = key;

        if (key == sAcknowledgedPawn) {
            it = localCache.erase(it);
            continue;
        }

        const uint8_t enemyTeamId = Memory::read<uint8_t>(key + Offsets::TeamID);
        actor.isAlly = (myTeamId != 0 && myTeamId == enemyTeamId);
        if (actor.isAlly && var::hide_allies) {
            ++dbgTeamEvict;
            it = localCache.erase(it);
            continue;
        }

        const uintptr_t playerState =
            Memory::read<uintptr_t>(key + Offsets::APlayerState);
        if (!playerState || !IsValidPointer(playerState)) {
            ++dbgPsEvict;
            it = localCache.erase(it);
            continue;
        }

        const PlayerHealthInfo healthInfo =
            Memory::read<PlayerHealthInfo>(playerState + Offsets::HealthInfo);
        actor.bIsDeathVerge = healthInfo.bIsDbno;
        actor.actorState = playerState;

        const uintptr_t freshRoot =
            Memory::read<uintptr_t>(key + Offsets::RootComponent);
        if (freshRoot && IsValidPointer(freshRoot))
            actor.rootComponent = freshRoot;

        const uintptr_t freshMesh = GetActorSkeletalMesh(key);
        if (freshMesh && IsValidPointer(freshMesh))
            actor.actorMesh = freshMesh;

        actor.WorldPos = ReadSceneWorldPos(actor.rootComponent);
        if (!IsPlausibleWorldPos(actor.WorldPos)) {
            ++dbgPosEvict;
            it = localCache.erase(it);
            continue;
        }

        const Vector3 delta = actor.WorldPos - cam.Location;
        const float distanceSq = static_cast<float>(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

        actor.Distance = sqrtf(distanceSq) / 100.0f;

        if (distanceSq > maxDistSq) {
            ++dbgDistSkip;
            actor.Drawing = false;
            ++it;
            continue;
        }

        actor.health = static_cast<float>(healthInfo.Health);
        actor.maxhealth = static_cast<float>(healthInfo.MaxHealth);
        actor.shield = static_cast<float>(healthInfo.Armor);
        actor.maxshield = static_cast<float>(healthInfo.MaxArmor);
        if (actor.maxhealth < 1.0f) {
            actor.health = static_cast<float>(get_health(key));
            actor.maxhealth = static_cast<float>(get_maxhealth(key));
            actor.shield = static_cast<float>(get_armor(key));
            actor.maxshield = static_cast<float>(get_maxarmor(key));
        }
        if (actor.maxshield <= 0.f) {
            actor.shield = 0.f;
            actor.maxshield = 0.f;
        }

        if (var::show_weapon) {
            if (doWeaponScan) {
                if (auto wIt = pawnToWeapon.find(key); wIt != pawnToWeapon.end()) {
                    actor.lastWeaponPtr = wIt->second.actor;
                    actor.weaponName = wIt->second.name.empty() ? "Armed" : wIt->second.name;
                    actor.weaponQuality = wIt->second.quality + 1;
                } else {
                    actor.lastWeaponPtr = 0;
                    actor.weaponName = "Unarmed";
                    actor.weaponQuality = -1;
                }
            }
        }

        if (!actor.ActorName.empty()) {
            std::string freshName = GetPlayerName(playerState, key);
            if (!freshName.empty())
                actor.ActorName = freshName;
        } else {
            const std::string freshName = GetPlayerName(playerState, key);
            if (!freshName.empty())
                actor.ActorName = freshName;
        }

        GetBones(actor);

        const Vector3 headBone = actor.boneData.bonesDouble[static_cast<size_t>(UniBone::Head)];
        Vector3 footBone = actor.boneData.bonesDouble[static_cast<size_t>(UniBone::FootL)];
        if (!actor.boneData.valid.test(static_cast<size_t>(UniBone::FootL)))
            footBone = actor.boneData.bonesDouble[static_cast<size_t>(UniBone::Pelvis)];

        if (!actor.boneData.isVisible) {
            actor.Drawing = false;
            ++dbgBoneMiss;
            ++it;
            continue;
        }

        ProjectWorldLocationToRadar(
            cam.Location,
            actor.WorldPos,
            static_cast<float>(cam.Rotation.y),
            actor.RadarPos);

        if (headBone.x <= 0 || headBone.y <= 0 ||
            footBone.x <= 0 || footBone.y <= 0)
        {
            actor.Drawing = false;
            ++it;
            continue;
        }

        actor.ScreenTop = headBone;
        actor.ScreenBottom = footBone;

        if (var::visiblecheck)
            actor.isVisible = Visible(key);
        else
            actor.isVisible = true;

        actor.Drawing = true;
        ++it;
        entityStarted.store(true, std::memory_order_release);
    }

    for (const auto& [key, entry] : localCache) {
        if (entry.Drawing)
            ++dbgDrawing;
        (void)key;
    }

    if (m_worldGeneration.load(std::memory_order_acquire) != gen)
        return;

    {
        std::unique_lock<std::shared_mutex> lock(m_playerCacheMutex);
        playerCache = std::move(localCache);
    }

    if (var::show_debug_overlay) {
        static IntervalTimer playerDebugTimer(500);
        if (playerDebugTimer.fire()) {
            std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
            std::cout << "[debugPlayer] scanned=" << dbgScanned
                << " gsPawns=" << dbgGsPawns
                << " gsAdmit=" << dbgGsAdmit
                << " preAdmit=" << dbgPreAdmit
                << " cache=" << playerCache.size()
                << " drawing=" << dbgDrawing
                << " teamEvict=" << dbgTeamEvict
                << " psEvict=" << dbgPsEvict
                << " posEvict=" << dbgPosEvict
                << " classSkip=" << dbgClassSkip
                << " distSkip=" << dbgDistSkip
                << " boneMiss=" << dbgBoneMiss
                << std::endl;
        }
    }
}
