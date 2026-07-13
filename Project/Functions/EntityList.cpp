#include "../Core/Engine.h"
#include "../Core/ActorType.h"
#include "../Core/IntervalTimer.h"
#include "../Core/AssetNames.h"
#include "EspDraw.h"
#include "WorldScanCommon.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

// help FrostDumper 2026-07-12:
// Prefer root RelativeLocation 0x218, then scene CompToWorld, then net snapshots.
// EmbarkCharacterBase: StateInterpolator 0x7c0 → ReplicatedRootTransform 0x1f8
// AActor: ReplicatedMovement 0x150; FRepMovement::Location 0x30
// ACharacter: Mesh 0x428, CharacterMovement 0x430, Capsule 0x438
// CMC: LastUpdateLocation 0x3e0
constexpr std::ptrdiff_t kACharacterMesh = 0x428;
constexpr std::ptrdiff_t kACharacterMovement = 0x430;
constexpr std::ptrdiff_t kACharacterCapsule = 0x438;
constexpr std::ptrdiff_t kCmcLastUpdateLocation = 0x3e0;
constexpr std::ptrdiff_t kReplicatedMovement = 0x150; // help dump — Project Offsets.h still 0x148
constexpr std::ptrdiff_t kRepMovLocation = 0x30;
constexpr std::ptrdiff_t kStateInterpolator = 0x7c0;
constexpr std::ptrdiff_t kReplicatedRootTransform = 0x1f8; // Location at +0

// help FrostDumper / SDK: Pawn::PlayerState is 0x3c0 (Controller uses 0x3A8).
// Base PlayerState::PawnPrivate = 0x418; live probe also saw 0x410.
// PioneerPlayerState: PioneerCharacter 0x538, CurrentPawn 0x540 (ARC remotes).
// Keep Offsets::APlayerState (0x3A8) for world/bot — do not change globals.
constexpr std::ptrdiff_t kPawnPlayerState = 0x3c0;
constexpr std::ptrdiff_t kPsPawnPrivate = 0x418;
constexpr std::ptrdiff_t kPsPawnPrivateAlt = 0x410;
constexpr std::ptrdiff_t kPioneerCharacter = 0x538;
constexpr std::ptrdiff_t kPioneerCurrentPawn = 0x540;

bool PsBacklinksToPawn(uintptr_t ps, uintptr_t pawn)
{
    const std::ptrdiff_t offs[] = {
        kPioneerCurrentPawn,
        kPioneerCharacter,
        kPsPawnPrivate,
        kPsPawnPrivateAlt,
    };
    for (const std::ptrdiff_t off : offs) {
        const uintptr_t linked = Memory::read<uintptr_t>(ps + off);
        if (linked == pawn)
            return true;
    }
    return false;
}

bool TryResolvePawnPlayerState(uintptr_t pawn, uintptr_t& outPs)
{
    if (!pawn || !Memory::IsValidPtrFast2(pawn))
        return false;
    // Step 3: nocache on forward PS read — stale VMM page cache was suspected.
    const uintptr_t ps =
        Memory::read_nocache<uintptr_t>(pawn + kPawnPlayerState);
    if (!ps || !Memory::IsValidPtrFast2(ps))
        return false;
    if (!PsBacklinksToPawn(ps, pawn))
        return false;
    outPs = ps;
    return true;
}

bool IsActorTypePlayerPawn(uintptr_t actor)
{
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return false;
    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    return ArcActorType::IsPlayerClassId(masked);
}

/** Primary pawn+0x3C0 PS chase; fallback ActorType + 0x3C0 without backlink. */
bool TryResolvePlayerStateAny(uintptr_t pawn, uintptr_t& outPs, bool& outViaActorType)
{
    outViaActorType = false;
    if (TryResolvePawnPlayerState(pawn, outPs))
        return true;
    if (!IsActorTypePlayerPawn(pawn))
        return false;
    // Offsets::APlayerState (0x3A8) is Controller-only; on Pawn use 0x3C0.
    const uintptr_t ps =
        Memory::read_nocache<uintptr_t>(pawn + kPawnPlayerState);
    if (!ps || !Memory::IsValidPtrFast2(ps))
        return false;
    outPs = ps;
    outViaActorType = true;
    return true;
}

constexpr std::ptrdiff_t kPlayerStateBIsABot = 0x3aa;
constexpr uint8_t kPlayerStateBIsABotMask = 0x8;

bool PlayerStateIsBot(uintptr_t ps)
{
    if (!ps || !Memory::IsValidPtrFast2(ps))
        return false;
    const uint8_t flags = Memory::read<uint8_t>(ps + kPlayerStateBIsABot);
    return (flags & kPlayerStateBIsABotMask) != 0;
}

uintptr_t ResolvePawnFromPlayerState(uintptr_t ps)
{
    if (!ps || !Memory::IsValidPtrFast2(ps))
        return 0;
    const std::ptrdiff_t offs[] = {
        kPioneerCurrentPawn,
        kPioneerCharacter,
        kPsPawnPrivate,
        kPsPawnPrivateAlt,
    };
    for (const std::ptrdiff_t off : offs) {
        const uintptr_t pawn = Memory::read<uintptr_t>(ps + off);
        if (pawn && Memory::IsValidPtrFast2(pawn))
            return pawn;
    }
    return 0;
}

/** PioneerPlayerState+0x530 is bIsInEncounter, not HealthInfo — use HealthComponent. */
float ReadPawnHealthForAdmit(uintptr_t pawn)
{
    return static_cast<float>(Engine::ReadHealthComponentStat(pawn, Offsets::Health));
}

Vector3 ResolvePlayerWorldPos(uintptr_t pawn, uintptr_t root, uintptr_t /*legacy*/)
{
    // Bypass VMM page cache — Memory::read was freezing remotes (posSame=N).
    auto trySceneNC = [](uintptr_t comp) -> Vector3 {
        if (!comp || !Memory::IsValidPtrFast2(comp))
            return {};
        const Engine::FVector3d world =
            Memory::read_nocache<Engine::FVector3d>(comp + Offsets::WorldLocation);
        const Vector3 w = Engine::ToVector3(world);
        if (IsPlausibleWorldPos(w))
            return w;
        const Vector3 rel =
            Memory::read_nocache<Vector3>(comp + Offsets::RelativeLocation);
        return IsPlausibleWorldPos(rel) ? rel : Vector3{};
    };
    auto tryFVector3dNC = [](uintptr_t addr) -> Vector3 {
        if (!addr || !Memory::IsValidPtrFast2(addr))
            return {};
        const Engine::FVector3d loc = Memory::read_nocache<Engine::FVector3d>(addr);
        const Vector3 p = Engine::ToVector3(loc);
        return IsPlausibleWorldPos(p) ? p : Vector3{};
    };

    if (pawn && Memory::IsValidPtrFast2(pawn)) {
        // Prefer live CompToWorld — RelativeLocation stays frozen on remotes (posSame).
        const uintptr_t comps[] = {
            Memory::read_nocache<uintptr_t>(pawn + Offsets::EmbarkMesh),
            Memory::read_nocache<uintptr_t>(pawn + kACharacterMesh),
            Memory::read_nocache<uintptr_t>(pawn + kACharacterCapsule),
        };
        for (uintptr_t c : comps) {
            if (const Vector3 p = trySceneNC(c); IsPlausibleWorldPos(p))
                return p;
        }
        if (const Vector3 p = trySceneNC(root); IsPlausibleWorldPos(p))
            return p;

        // Fallback: root RelativeLocation (local may update; remotes often stale).
        if (root && Memory::IsValidPtrFast2(root)) {
            const Vector3 rel =
                Memory::read_nocache<Vector3>(root + Offsets::RelativeLocation);
            if (IsPlausibleWorldPos(rel))
                return rel;
        }

        // StateInterpolator → ReplicatedRootTransform.Location
        {
            const uintptr_t interp =
                Memory::read_nocache<uintptr_t>(pawn + kStateInterpolator);
            if (interp && Memory::IsValidPtrFast2(interp)) {
                if (const Vector3 p = tryFVector3dNC(interp + kReplicatedRootTransform);
                    IsPlausibleWorldPos(p))
                    return p;
            }
        }

        // FRepMovement::Location @ Actor+0x150+0x30
        if (const Vector3 p = tryFVector3dNC(
                pawn + kReplicatedMovement + kRepMovLocation);
            IsPlausibleWorldPos(p))
            return p;

        // CMC LastUpdateLocation @ 0x3e0
        uintptr_t cmc =
            Memory::read_nocache<uintptr_t>(pawn + Offsets::PioneerCharacterMovement);
        if (!cmc || !Memory::IsValidPtrFast2(cmc))
            cmc = Memory::read_nocache<uintptr_t>(pawn + kACharacterMovement);
        if (cmc && Memory::IsValidPtrFast2(cmc)) {
            if (const Vector3 p = tryFVector3dNC(cmc + kCmcLastUpdateLocation);
                IsPlausibleWorldPos(p))
                return p;
        }
    }

    return trySceneNC(root);
}

uintptr_t ResolvePlayerSkeletalMesh(uintptr_t pawn)
{
    if (!pawn || !Memory::IsValidPtrFast2(pawn))
        return 0;
    const uintptr_t embark = Memory::read<uintptr_t>(pawn + Offsets::EmbarkMesh);
    if (embark && Memory::IsValidPtrFast2(embark))
        return embark;
    const uintptr_t mesh = Memory::read<uintptr_t>(pawn + kACharacterMesh);
    if (mesh && Memory::IsValidPtrFast2(mesh))
        return mesh;
    return 0;
}

/** GameState PlayerArray → set of PlayerState pointers (membership only, not pawns). */
bool BuildGameStatePlayerStateAllowlist(
    Engine& eng,
    uintptr_t gWorld,
    std::unordered_set<uintptr_t>& outPs,
    int& outArraySize)
{
    outPs.clear();
    outArraySize = 0;

    uintptr_t bestGs = 0;
    int32_t bestArrNum = 0;

    auto considerGameState = [&](uintptr_t gs) {
        if (!gs || !Memory::IsValidPtrFast2(gs))
            return;
        const uintptr_t arrData =
            Memory::read<uintptr_t>(gs + Offsets::GameState_PlayerArray);
        const int32_t arrNum =
            Memory::read<int32_t>(gs + Offsets::GameState_PlayerArray + 8);
        if (!arrData || !Memory::IsValidPtrFast2(arrData) || arrNum <= 0 || arrNum > 128)
            return;
        if (arrNum > bestArrNum) {
            bestArrNum = arrNum;
            bestGs = gs;
        }
    };

    if (gWorld && Memory::IsValidPtrFast2(gWorld)) {
        const uintptr_t collectionsData =
            Memory::read<uintptr_t>(gWorld + Offsets::LevelCollections);
        const int32_t collectionsNum =
            Memory::read<int32_t>(gWorld + Offsets::LevelCollections + 8);
        if (collectionsData && Memory::IsValidPtrFast2(collectionsData)
            && collectionsNum > 0 && collectionsNum <= 16) {
            const int limit = (collectionsNum > 4) ? 4 : collectionsNum;
            for (int i = 0; i < limit; ++i) {
                const uintptr_t collection =
                    collectionsData + static_cast<uintptr_t>(i) * Offsets::LevelCollection_Stride;
                considerGameState(Memory::read<uintptr_t>(
                    collection + Offsets::LevelCollection_GameState));
            }
        }
    }

    if (const uint64_t base = Memory::getBaseAddress())
        considerGameState(Memory::read<uintptr_t>(base + Offsets::GameStateGlobalRva));

    (void)eng;
    if (!bestGs)
        return false;

    outArraySize = bestArrNum;
    const uintptr_t arrData =
        Memory::read<uintptr_t>(bestGs + Offsets::GameState_PlayerArray);

    outPs.reserve(static_cast<size_t>(bestArrNum));
    for (int32_t i = 0; i < bestArrNum; ++i) {
        const uintptr_t playerState = Memory::read<uintptr_t>(
            arrData + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
        if (playerState && Memory::IsValidPtrFast2(playerState))
            outPs.insert(playerState);
    }

    return !outPs.empty();
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

    const int actor_count =
        Memory::read<int>(sPersistentLevel + Offsets::ActorsCount);

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
    int dbgAdmitted = 0;
    int dbgPreAdmit = 0;
    int dbgDrawing = 0;
    int dbgTeamEvict = 0;
    int dbgPsEvict = 0;
    int dbgPosEvict = 0;
    int dbgDistSkip = 0;
    int dbgBoneMiss = 0;
    int dbgWorldBox = 0;
    int dbgGhostEvict = 0;
    int dbgRootStale = 0;
    int dbgRootSkip = 0;
    int dbgMeshSkip = 0;
    int dbgPsSkip = 0;
    int dbgHealthSkip = 0;
    int dbgGsArray = 0;
    int dbgGsEvict = 0;
    int dbgPosSame = 0;
    int dbgGsPawnHit = 0;
    int dbgGsPawnMiss = 0;
    int dbgGsPawnNull = 0;
    int dbgFwdMatch = 0;
    int dbgFwdMismatch = 0;
    int dbgActorTypeAdmit = 0;
    int dbgGsBot = 0;

    std::unordered_set<uint64_t> currentActorSet(
        currentActors.begin(),
        currentActors.end());

    CameraCache cam{};
    {
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        cam = g_Camera;
    }

    const float maxDistSq =
        static_cast<float>(var::esp_distance * var::esp_distance * 10000.0f);

    std::unordered_map<uintptr_t, PlayerCacheEntry> localCache;
    {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        localCache = playerCache;
    }

    for (auto it = localCache.begin(); it != localCache.end(); ) {
        if (!currentActorSet.contains(it->first))
            it = localCache.erase(it);
        else
            ++it;
    }

    uintptr_t localPlayerState = 0;
    if (!TryResolvePawnPlayerState(sAcknowledgedPawn, localPlayerState) && sPlayerController)
        localPlayerState = Memory::read<uintptr_t>(
            sPlayerController + Offsets::AController_PlayerState);

    // gsArray is diagnostic only — do not gate admission on GameState membership.
    // Wrong/stale GS PlayerArray pointers were zeroing PlayerCache (gsEvict).
    std::unordered_set<uintptr_t> gsPlayerStates;
    BuildGameStatePlayerStateAllowlist(*this, sGWorld, gsPlayerStates, dbgGsArray);

    // Primary admission: GameState PlayerArray → PS → pawn (proven by gsPawnHit).
    // Forward pawn→PS chase fails remotes; HealthInfo@PS+0x530 is wrong on PioneerPS.
    for (const uintptr_t ps : gsPlayerStates) {
        if (ps == localPlayerState)
            continue;
        if (PlayerStateIsBot(ps)) {
            ++dbgGsBot;
            continue;
        }

        const uintptr_t backPawn = ResolvePawnFromPlayerState(ps);
        if (!backPawn) {
            ++dbgGsPawnNull;
            continue;
        }
        if (backPawn == sAcknowledgedPawn)
            continue;

        if (currentActorSet.contains(backPawn)) {
            ++dbgGsPawnHit;
            const uintptr_t fwdPs =
                Memory::read_nocache<uintptr_t>(backPawn + kPawnPlayerState);
            if (fwdPs == ps)
                ++dbgFwdMatch;
            else
                ++dbgFwdMismatch;
        } else {
            ++dbgGsPawnMiss;
            continue; // need actor in this level's list for cache key / prune
        }

        if (localCache.contains(backPawn))
            continue;

        const float health = ReadPawnHealthForAdmit(backPawn);
        if (health < 1.0f) {
            // Soft: still admit GS humans; refresh can drop dead.
            // Count only — do not skip (HealthInfo@0x530 was false-rejecting everyone).
            ++dbgHealthSkip;
        }

        const uintptr_t root =
            Memory::read<uintptr_t>(backPawn + Offsets::RootComponent);
        if (!root) {
            ++dbgRootSkip;
            continue;
        }

        const uintptr_t mesh =
            Memory::read<uintptr_t>(backPawn + Offsets::USkeletalMeshComponent);
        const uintptr_t charMesh = ResolvePlayerSkeletalMesh(backPawn);
        if (!charMesh && !mesh) {
            ++dbgMeshSkip;
            continue;
        }

        const std::string playerName = GetPlayerName(ps, backPawn);
        auto [it, inserted] = localCache.emplace(
            backPawn,
            PlayerCacheEntry(
                playerName.c_str(),
                root,
                backPawn,
                charMesh ? charMesh : mesh));
        if (inserted) {
            it->second.actorState = ps;
            ++dbgAdmitted;
        }
    }

    // Secondary: level actor scan (local PS chase / ActorType) for any missed.
    for (uint64_t actor : currentActors)
    {
        if (!actor || actor == sAcknowledgedPawn || localCache.contains(actor))
            continue;

        uintptr_t playerState = 0;
        bool viaActorType = false;
        if (!TryResolvePlayerStateAny(actor, playerState, viaActorType)) {
            ++dbgPsSkip;
            continue;
        }
        if (viaActorType)
            ++dbgActorTypeAdmit;

        if (localPlayerState && playerState == localPlayerState) {
            ++dbgGhostEvict;
            continue;
        }

        if (PlayerStateIsBot(playerState)) {
            ++dbgGsBot;
            continue;
        }

        // When GameState PlayerArray is healthy, do not admit stray PS pawns —
        // that inflated preAdmit/cache (~15) vs ~6 remotes (ghost boxes).
        if (!gsPlayerStates.empty() && !gsPlayerStates.contains(playerState)) {
            ++dbgGsEvict;
            continue;
        }

        const float health = ReadPawnHealthForAdmit(actor);
        if (health < 1.0f) {
            ++dbgHealthSkip;
            // Soft: count only — GS path already admits; do not hard-skip (#28).
        }

        const uintptr_t root =
            Memory::read<uintptr_t>(actor + Offsets::RootComponent);
        if (!root) {
            ++dbgRootSkip;
            continue;
        }

        const uintptr_t mesh =
            Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
        const uintptr_t charMesh = ResolvePlayerSkeletalMesh(actor);
        if (!charMesh && !mesh) {
            ++dbgMeshSkip;
            continue;
        }

        const std::string playerName = GetPlayerName(playerState, actor);
        auto [it, inserted] = localCache.emplace(
            actor,
            PlayerCacheEntry(playerName.c_str(), root, actor, charMesh ? charMesh : mesh));
        if (inserted) {
            it->second.actorState = playerState;
            ++dbgAdmitted;
        }
    }

    dbgPreAdmit = static_cast<int>(localCache.size());

    const uint8_t myTeamId =
        Memory::read<uint8_t>(sAcknowledgedPawn + Offsets::TeamID);

    // Prefer mesh CompToWorld (bot parity); root fallback — root alone stays frozen on remotes.
    for (auto it = localCache.begin(); it != localCache.end(); ++it) {
        const uintptr_t key = it->first;
        if (key == sAcknowledgedPawn)
            continue;

        uintptr_t root = Memory::read<uintptr_t>(key + Offsets::RootComponent);
        if (!root || !IsValidPointer(root))
            continue;

        it->second.rootComponent = root;
        const uintptr_t charMesh = ResolvePlayerSkeletalMesh(key);
        if (charMesh)
            it->second.actorMesh = charMesh;

        const Vector3 pos = ResolvePlayerWorldPos(key, root, it->second.actorMesh);
        if (!IsPlausibleWorldPos(pos))
            continue;

        auto& entry = it->second;
        if (IsPlausibleWorldPos(entry.lastWorldPos)) {
            const double dx = static_cast<double>(pos.x - entry.lastWorldPos.x);
            const double dy = static_cast<double>(pos.y - entry.lastWorldPos.y);
            const double dz = static_cast<double>(pos.z - entry.lastWorldPos.z);
            if (dx * dx + dy * dy + dz * dz < 4.0)
                ++dbgPosSame;
        }
        entry.lastWorldPos = pos;
        entry.WorldPos = pos;
    }

    for (auto it = localCache.begin(); it != localCache.end(); )
    {
        auto& actor = it->second;
        const uintptr_t key = it->first;
        actor.APawn = key;

        if (key == sAcknowledgedPawn) {
            it = localCache.erase(it);
            continue;
        }

        const uintptr_t freshRoot =
            Memory::read<uintptr_t>(key + Offsets::RootComponent);
        if (!freshRoot || !IsValidPointer(freshRoot)) {
            ++dbgRootStale;
            it = localCache.erase(it);
            continue;
        }
        actor.rootComponent = freshRoot;

        const uint8_t enemyTeamId = Memory::read<uint8_t>(key + Offsets::TeamID);
        actor.isAlly = (myTeamId != 0 && myTeamId == enemyTeamId);
        if (actor.isAlly && var::hide_allies) {
            ++dbgTeamEvict;
            it = localCache.erase(it);
            continue;
        }

        uintptr_t playerState = actor.actorState;
        if (!playerState || !Memory::IsValidPtrFast2(playerState)) {
            bool viaActorType = false;
            if (!TryResolvePlayerStateAny(key, playerState, viaActorType)) {
                ++dbgPsEvict;
                it = localCache.erase(it);
                continue;
            }
            (void)viaActorType;
        }

        if (localPlayerState && playerState == localPlayerState) {
            ++dbgGhostEvict;
            it = localCache.erase(it);
            continue;
        }

        if (PlayerStateIsBot(playerState)) {
            ++dbgGsBot;
            it = localCache.erase(it);
            continue;
        }

        if (!gsPlayerStates.empty() && !gsPlayerStates.contains(playerState)) {
            ++dbgGsEvict;
            it = localCache.erase(it);
            continue;
        }

        actor.actorState = playerState;
        actor.bIsDeathVerge = false;

        const std::string liveName = GetPlayerName(playerState, key);
        if (!liveName.empty())
            actor.ActorName = liveName;

        const uintptr_t charMesh = ResolvePlayerSkeletalMesh(key);
        if (charMesh)
            actor.actorMesh = charMesh;

        actor.WorldPos = ResolvePlayerWorldPos(key, freshRoot, actor.actorMesh);

        if (!IsPlausibleWorldPos(actor.WorldPos))
        {
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

        actor.health = static_cast<float>(get_health(key));
        actor.maxhealth = static_cast<float>(get_maxhealth(key));
        actor.shield = static_cast<float>(get_armor(key));
        actor.maxshield = static_cast<float>(get_maxarmor(key));

        const uintptr_t currentHeld = WorldScan::ResolvePreferredHeldItemActor(key);
        std::string heldFName;
        if (currentHeld) {
            heldFName = GetActorFNameStringCached(currentHeld);
            if (heldFName.empty())
                heldFName = GetActorFNameString(currentHeld);
        }

        std::string heldLabel;
        if (currentHeld && !heldFName.empty())
            heldLabel = GetWeaponName(heldFName);
        if (heldLabel.empty() && !heldFName.empty())
            heldLabel = HumanizeActorFName(heldFName);
        actor.weaponName = heldLabel.empty() ? (currentHeld ? "Item" : "Unarmed") : heldLabel;

        // Firearm tier only — consumables/nades stay neutral-colored in ESP.
        int quality = -1;
        if (currentHeld) {
            std::string lower = heldFName;
            for (char& c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lower.find("weaponactor") != std::string::npos
                || lower.find("bp_weapon") != std::string::npos) {
                quality = GetWeaponQualityFromActor(currentHeld);
                if (quality < 0 || quality > 4)
                    quality = GetWeaponQuality(key);
            }
        }
        actor.weaponQuality = (quality >= 0 && quality <= 4) ? (quality + 1) : -1;

        GetBones(actor);

        Vector3 headScr{};
        Vector3 footScr{};
        bool haveScreenBox = false;

        if (actor.boneData.isVisible) {
            const Vector3 headBone =
                actor.boneData.bonesDouble[static_cast<size_t>(UniBone::Head)];
            Vector3 footBone =
                actor.boneData.bonesDouble[static_cast<size_t>(UniBone::FootL)];
            if (!actor.boneData.valid.test(static_cast<size_t>(UniBone::FootL)))
                footBone = actor.boneData.bonesDouble[static_cast<size_t>(UniBone::Pelvis)];

            if (headBone.x > 0.0 && headBone.y > 0.0 &&
                footBone.x > 0.0 && footBone.y > 0.0)
            {
                headScr = headBone;
                footScr = footBone;
                haveScreenBox = true;
            }
        }

        if (!haveScreenBox) {
            Vector3 headWorld{};
            Vector3 feetWorld{};
            if (EspDraw::ResolvePlayerHeadFeetWorld(actor, headWorld, feetWorld)) {
                ImVec2 head{};
                ImVec2 feet{};
                if (EspDraw::WorldToScreenBox(*this, cam, headWorld, feetWorld, head, feet)) {
                    headScr = Vector3{ head.x, head.y, 0.0 };
                    footScr = Vector3{ feet.x, feet.y, 0.0 };
                    haveScreenBox = true;
                    ++dbgWorldBox;
                }
            }
        }

        ProjectWorldLocationToRadar(
            cam.Location,
            actor.WorldPos,
            static_cast<float>(cam.Rotation.y),
            actor.RadarPos);

        if (!haveScreenBox)
            ++dbgBoneMiss;

        if (haveScreenBox) {
            actor.ScreenTop = headScr;
            actor.ScreenBottom = footScr;
        }

        if (var::visiblecheck)
            actor.isVisible = VisibleActor(key);
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
                << " admitted=" << dbgAdmitted
                << " preAdmit=" << dbgPreAdmit
                << " cache=" << playerCache.size()
                << " drawing=" << dbgDrawing
                << " teamEvict=" << dbgTeamEvict
                << " psEvict=" << dbgPsEvict
                << " posEvict=" << dbgPosEvict
                << " distSkip=" << dbgDistSkip
                << " boneMiss=" << dbgBoneMiss
                << " worldBox=" << dbgWorldBox
                << " ghostEvict=" << dbgGhostEvict
                << " rootStale=" << dbgRootStale
                << " rootSkip=" << dbgRootSkip
                << " meshSkip=" << dbgMeshSkip
                << " psSkip=" << dbgPsSkip
                << " healthSkip=" << dbgHealthSkip
                << " posSame=" << dbgPosSame
                << " gsArray=" << dbgGsArray
                << " gsEvict=" << dbgGsEvict
                << " gsPawnHit=" << dbgGsPawnHit
                << " gsPawnMiss=" << dbgGsPawnMiss
                << " gsPawnNull=" << dbgGsPawnNull
                << " fwdMatch=" << dbgFwdMatch
                << " fwdMismatch=" << dbgFwdMismatch
                << " actorTypeAdmit=" << dbgActorTypeAdmit
                << " gsBot=" << dbgGsBot
                << std::endl;
        }
    }
}
