#include "../Core/Engine.h"
#include "../Core/ActorType.h"
#include "../Core/IntervalTimer.h"
#include "../Core/AssetNames.h"
#include "EspDraw.h"
#include "WorldScanCommon.h"

#include <algorithm>
#include <iostream>
#include <chrono>
#include <fstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// Distance-edge Drawing hysteresis: require this many consecutive out-of-range
// scans before clearing Drawing (stops ESP blink at esp_distance boundary).
static std::unordered_map<uintptr_t, uint8_t> s_playerDistMisses;
static constexpr uint8_t kPlayerDistMissClearDrawing = 3;

static std::unordered_map<uintptr_t, uint8_t> s_playerRootMisses;
static std::unordered_map<uintptr_t, uint8_t> s_playerPosMisses;
static std::unordered_map<uintptr_t, uint8_t> s_playerHealthZeroMisses;
static constexpr uint8_t kPlayerGhostEvict = 10;

static void ClearPlayerGhostMisses(uintptr_t key)
{
    WorldScan::MissCounterClear(s_playerRootMisses, key);
    WorldScan::MissCounterClear(s_playerPosMisses, key);
    WorldScan::MissCounterClear(s_playerHealthZeroMisses, key);
}

// P7: negative memo for actors that failed both PS chase and pioneer-class
// backup. Staggered TTL so late-decrypting remotes still get re-checked.
static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
    s_playerScanNeg;

// P7: 4-slice rotating admission ring over secondary + U8 actor walks.
// GameState PlayerArray admission stays every-pass (cheap, primary path).
// Positions stay hot via PositionRefreshPass @16ms.
static constexpr size_t kPlayerAdmitSlices = 4;
static constexpr size_t kPlayerAdmitPrioNewMax = 64;
static size_t s_playerAdmitSliceCursor = 0;
static uint64_t s_playerAdmitRingGen = 0;
static uintptr_t s_playerAdmitRingActorsPtr = 0;
static size_t s_playerAdmitRingActorCount = 0;
static size_t s_playerAdmitRingEpoch = 0;
static uint64_t s_playerAdmitCoveredMask = 0;
static int s_playerAdmitRingResets = 0;
static int s_playerAdmitLastCycleMs = 0;
static std::chrono::steady_clock::time_point s_playerAdmitCycleStart{};
static std::unordered_set<uintptr_t> s_playerAdmitPrevActors;

static bool PlayerScanNegMemoHit(uintptr_t actor, int& outMemoSkip)
{
    const auto now = std::chrono::steady_clock::now();
    if (const auto it = s_playerScanNeg.find(actor); it != s_playerScanNeg.end()) {
        const auto ttl = std::chrono::seconds(8 + static_cast<int>((actor >> 4) & 7));
        if (now - it->second < ttl) {
            ++outMemoSkip;
            return true;
        }
        s_playerScanNeg.erase(it);
    }
    return false;
}

static void PlayerScanNegMemoize(uintptr_t actor)
{
    if (s_playerScanNeg.size() > 16384)
        s_playerScanNeg.clear();
    s_playerScanNeg[actor] = std::chrono::steady_clock::now();
}

// help/sdk.txt: prefer CompToWorld, then net snapshots.
// EmbarkCharacterBase: StateInterpolator 0x7c0 → ReplicatedRootTransform 0x1f8
// AActor: ReplicatedMovement 0x150; FRepMovement::Location 0x30
// ACharacter: Mesh 0x420, CharacterMovement 0x428, Capsule 0x430
// CMC: LastUpdateLocation 0x3e0
constexpr std::ptrdiff_t kACharacterMesh = 0x420;
constexpr std::ptrdiff_t kACharacterMovement = 0x428;
constexpr std::ptrdiff_t kACharacterCapsule = 0x430;
constexpr std::ptrdiff_t kCmcLastUpdateLocation = 0x3e0;
constexpr std::ptrdiff_t kReplicatedMovement = 0x150;
constexpr std::ptrdiff_t kRepMovLocation = 0x30;
constexpr std::ptrdiff_t kStateInterpolator = 0x7c0;
constexpr std::ptrdiff_t kReplicatedRootTransform = 0x1f8;

// PioneerPlayerState: PioneerCharacter 0x528, CurrentPawn 0x530
// Controller::PlayerState 0x3A0; PlayerState::PawnPrivate 0x410
constexpr std::ptrdiff_t kPawnPlayerState = 0x3A0;
constexpr std::ptrdiff_t kPsPawnPrivate = 0x410;
constexpr std::ptrdiff_t kPsPawnPrivateAlt = 0x410;
constexpr std::ptrdiff_t kPioneerCharacter = 0x528;
constexpr std::ptrdiff_t kPioneerCurrentPawn = 0x530;

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
    // Offsets::APlayerState (0x3A0) on Controller; on Pawn use kPawnPlayerState.
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
        return Engine::ReadWorldLocationNocache(comp, /*allowRelativeFallback=*/true);
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

    // help/esp.txt: walk all UWorld::Levels — PersistentLevel alone misses
    // streamed remotes (gsPawnMiss). Same union bots/items already use.
    std::vector<uint64_t> currentActors;
    WorldScan::CollectLevelActors(sGWorld, sPersistentLevel, currentActors);
    if (currentActors.empty()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return;
    }

    int dbgScanned = static_cast<int>(currentActors.size());
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
        if (!currentActorSet.contains(it->first)) {
            WorldScan::MissCounterClear(s_playerDistMisses, it->first);
            it = localCache.erase(it);
        } else
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
            continue; // need pawn in all-Levels actor union for cache key / prune
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

    // P7: secondary PS chase + U8 pioneer-class backup, banded into a 4-slice
    // rotating ring. GameState PlayerArray above remains the every-pass primary.
    int dbgAdmitChecked = 0;
    int dbgAdmitMemoSkip = 0;
    int dbgAdmitPrioNew = 0;
    int dbgAdmitSliceActors = 0;
    int dbgAdmitScatterExecs = 0;
    int dbgAdmitFnameChecked = 0;
    size_t dbgAdmitSlice = 0;
    size_t dbgAdmitN = 0;
    bool admitRingAdvanceOk = false;

    // Prune negative memos for actors that left the world.
    for (auto it = s_playerScanNeg.begin(); it != s_playerScanNeg.end(); ) {
        if (!currentActorSet.contains(it->first))
            it = s_playerScanNeg.erase(it);
        else
            ++it;
    }

    // Cheap CPU index of valid non-local actors. DMA only hits the slice.
    std::vector<uintptr_t> admitIndex;
    admitIndex.reserve(currentActors.size());
    for (uint64_t actorU64 : currentActors) {
        const uintptr_t actor = static_cast<uintptr_t>(actorU64);
        if (!actor || !IsValidPointer(actor))
            continue;
        if (actor == sAcknowledgedPawn)
            continue;
        admitIndex.push_back(actor);
    }

    const size_t N = admitIndex.size();
    dbgAdmitN = N;

    bool ringReset = false;
    if (s_playerAdmitRingGen != gen) {
        ringReset = true;
    } else if (sActors != 0 && s_playerAdmitRingActorsPtr != 0
        && sActors != s_playerAdmitRingActorsPtr) {
        ringReset = true;
    } else if (s_playerAdmitRingActorCount != 0) {
        const size_t delta = (N > s_playerAdmitRingActorCount)
            ? (N - s_playerAdmitRingActorCount)
            : (s_playerAdmitRingActorCount - N);
        const size_t thresh = (std::max)(static_cast<size_t>(64), N / 8);
        if (delta > thresh)
            ringReset = true;
    }
    if (ringReset) {
        s_playerAdmitSliceCursor = 0;
        s_playerAdmitCoveredMask = 0;
        s_playerAdmitPrevActors.clear();
        s_playerAdmitCycleStart = std::chrono::steady_clock::now();
        s_playerAdmitLastCycleMs = 0;
        ++s_playerAdmitRingEpoch;
        ++s_playerAdmitRingResets;
        s_playerScanNeg.clear();
    }
    s_playerAdmitRingGen = gen;
    s_playerAdmitRingActorsPtr = sActors;
    s_playerAdmitRingActorCount = N;
    if (s_playerAdmitCycleStart.time_since_epoch().count() == 0)
        s_playerAdmitCycleStart = std::chrono::steady_clock::now();

    const size_t slice = s_playerAdmitSliceCursor % kPlayerAdmitSlices;
    const size_t sliceBase = (N * slice) / kPlayerAdmitSlices;
    const size_t sliceEnd = (N * (slice + 1)) / kPlayerAdmitSlices;
    dbgAdmitSlice = slice;

    std::unordered_set<uintptr_t> probeSet;
    probeSet.reserve((sliceEnd - sliceBase) + kPlayerAdmitPrioNewMax + 8);
    for (size_t i = sliceBase; i < sliceEnd; ++i) {
        const uintptr_t actor = admitIndex[i];
        if (localCache.contains(actor))
            continue;
        probeSet.insert(actor);
    }
    dbgAdmitSliceActors = static_cast<int>(probeSet.size());

    size_t prioAdded = 0;
    for (uintptr_t actor : admitIndex) {
        if (prioAdded >= kPlayerAdmitPrioNewMax)
            break;
        if (s_playerAdmitPrevActors.contains(actor))
            continue;
        if (localCache.contains(actor))
            continue;
        if (probeSet.insert(actor).second)
            ++prioAdded;
    }
    dbgAdmitPrioNew = static_cast<int>(prioAdded);

    // P7b: batched pre-gate (mirrors RobotList P6b). One scatter exec reads
    // PlayerState@0x3C0 + ActorTypeId for the whole band; serial PS-chase and
    // class-fname work only runs on actors that show a player-like signal.
    std::ptrdiff_t typeOff = ArcActorType::RuntimeActorTypeOffset();
    if (typeOff < 0) {
        for (size_t i = 0; i < admitIndex.size() && typeOff < 0; ++i) {
            (void)ArcActorType::ReadActorTypeId(admitIndex[i]);
            typeOff = ArcActorType::RuntimeActorTypeOffset();
        }
        if (typeOff < 0)
            typeOff = Offsets::ActorTypeId;
    }

    struct PlayerProbeRow {
        uintptr_t actor = 0;
        uint64_t ps = 0;
        uint32_t typeId = 0;
    };
    std::vector<PlayerProbeRow> probeRows;
    probeRows.reserve(probeSet.size());
    for (uintptr_t actor : probeSet) {
        if (localCache.contains(actor))
            continue;
        if (PlayerScanNegMemoHit(actor, dbgAdmitMemoSkip))
            continue;
        PlayerProbeRow row;
        row.actor = actor;
        probeRows.push_back(row);
    }

    constexpr size_t kPlayerAdmitChunk = 512;
    for (size_t base = 0; base < probeRows.size(); base += kPlayerAdmitChunk) {
        const size_t chunkEnd = (std::min)(base + kPlayerAdmitChunk, probeRows.size());
        ScatterSession scatter;
        if (!scatter.isValid())
            break;
        bool prepOk = true;
        for (size_t i = base; i < chunkEnd; ++i) {
            PlayerProbeRow& r = probeRows[i];
            prepOk = scatter.prepare(r.actor + kPawnPlayerState, r.ps) && prepOk;
            prepOk = scatter.prepare(r.actor + typeOff, r.typeId) && prepOk;
        }
        if (prepOk && scatter.execute())
            ++dbgAdmitScatterExecs;
    }

    for (const PlayerProbeRow& row : probeRows) {
        const uintptr_t actor = row.actor;
        const uint32_t masked = ArcActorType::MaskActorTypeId(row.typeId);
        const bool gate = (row.ps != 0
                              && Memory::IsValidPtrFast2(static_cast<uintptr_t>(row.ps)))
            || ArcActorType::IsPlayerClassId(masked);

        if (!gate) {
            // No PS pointer and not a player class id — pioneer check via fname
            // memo (instance fname embeds class for BP actors). Serial reads run
            // once per TTL window, then the memo holds.
            ++dbgAdmitFnameChecked;
            std::string fn = GetActorFNameStringCached(actor);
            if (fn.empty())
                fn = GetActorFNameString(actor);
            std::string classFn;
            if (fn.empty())
                classFn = GetActorClassFName(actor);

            auto containsPioneer = [](const std::string& s) -> bool {
                if (s.empty())
                    return false;
                std::string low = s;
                for (char& c : low)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                return low.find("pioneercharacter") != std::string::npos;
            };

            if (!containsPioneer(fn) && !containsPioneer(classFn)) {
                // Memoize only when a name decoded; undecrypted actors retry
                // next ring pass so late spawns are never permanently skipped.
                if (!fn.empty() || !classFn.empty())
                    PlayerScanNegMemoize(actor);
                continue;
            }
            // Pioneer-looking without gate signal — fall through to full check.
        }

        ++dbgAdmitChecked;

        // Secondary: local PS chase / ActorType for any GameState misses.
        uintptr_t playerState = 0;
        bool viaActorType = false;
        if (TryResolvePlayerStateAny(actor, playerState, viaActorType)) {
            if (viaActorType)
                ++dbgActorTypeAdmit;

            if (localPlayerState && playerState == localPlayerState) {
                ++dbgGhostEvict;
                continue;
            }

            if (PlayerStateIsBot(playerState)) {
                ++dbgGsBot;
                // Bots never become players — memoize so their PS chase does
                // not repeat serially every ring pass (TTL still re-checks).
                PlayerScanNegMemoize(actor);
                continue;
            }

            // Do NOT gate secondary admit on gsPlayerStates membership.
            // Stale/wrong PlayerArray allowlists were zeroing PlayerCache (gsEvict)
            // even when pawn→PS chase succeeded — keep gsArray diagnostic-only.
            if (!gsPlayerStates.empty() && !gsPlayerStates.contains(playerState))
                ++dbgGsEvict; // count only — still admit

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
                PlayerCacheEntry(
                    playerName.c_str(), root, actor, charMesh ? charMesh : mesh));
            if (inserted) {
                it->second.actorState = playerState;
                ++dbgAdmitted;
            }
            continue;
        }

        ++dbgPsSkip;

        // U8: BP_PioneerCharacter_C class FName backup when PS chase missed.
        const std::string classFname = GetActorClassFName(actor);
        if (classFname.empty())
            continue; // undecrypted — retry next ring pass, do not memoize

        std::string cl = classFname;
        for (char& c : cl)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (cl.find("pioneercharacter") == std::string::npos
            && cl.find("bp_pioneercharacter") == std::string::npos) {
            PlayerScanNegMemoize(actor);
            continue;
        }

        playerState = 0;
        viaActorType = false;
        TryResolvePlayerStateAny(actor, playerState, viaActorType);
        if (localPlayerState && playerState == localPlayerState)
            continue;
        if (playerState && PlayerStateIsBot(playerState)) {
            PlayerScanNegMemoize(actor);
            continue;
        }

        const uintptr_t root =
            Memory::read<uintptr_t>(actor + Offsets::RootComponent);
        if (!root)
            continue;
        const uintptr_t mesh =
            Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
        const uintptr_t charMesh = ResolvePlayerSkeletalMesh(actor);
        if (!charMesh && !mesh)
            continue;

        const std::string playerName =
            playerState ? GetPlayerName(playerState, actor) : std::string("Player");
        auto [it, inserted] = localCache.emplace(
            actor,
            PlayerCacheEntry(
                playerName.c_str(), root, actor, charMesh ? charMesh : mesh));
        if (inserted) {
            it->second.actorState = playerState;
            ++dbgAdmitted;
            ++dbgActorTypeAdmit;
        }
    }

    s_playerAdmitPrevActors.clear();
    s_playerAdmitPrevActors.insert(admitIndex.begin(), admitIndex.end());
    admitRingAdvanceOk = true;

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
            WorldScan::MissCounterClear(s_playerDistMisses, key);
            it = localCache.erase(it);
            continue;
        }

        const uintptr_t freshRoot =
            Memory::read<uintptr_t>(key + Offsets::RootComponent);
        if (!freshRoot || !IsValidPointer(freshRoot)) {
            ++dbgRootStale;
            if (WorldScan::MissCounterShouldEvict(
                    s_playerRootMisses, key, false, kPlayerGhostEvict)) {
                ClearPlayerGhostMisses(key);
                WorldScan::MissCounterClear(s_playerDistMisses, key);
                ++dbgGhostEvict;
                it = localCache.erase(it);
            } else {
                actor.Drawing = false;
                ++it;
            }
            continue;
        }
        WorldScan::MissCounterClear(s_playerRootMisses, key);
        actor.rootComponent = freshRoot;

        actor.facingYaw = static_cast<float>(
            Memory::read<double>(freshRoot + Offsets::RelativeRotation + 8));

        const uint8_t enemyTeamId = Memory::read<uint8_t>(key + Offsets::TeamID);
        actor.enemyTeamId = enemyTeamId;
        actor.isAlly = (myTeamId != 0 && myTeamId == enemyTeamId);
        if (actor.isAlly && var::hide_allies) {
            ++dbgTeamEvict;
            WorldScan::MissCounterClear(s_playerDistMisses, key);
            it = localCache.erase(it);
            continue;
        }

        uintptr_t playerState = actor.actorState;
        if (!playerState || !Memory::IsValidPtrFast2(playerState)) {
            bool viaActorType = false;
            if (!TryResolvePlayerStateAny(key, playerState, viaActorType)) {
                ++dbgPsEvict;
                WorldScan::MissCounterClear(s_playerDistMisses, key);
                it = localCache.erase(it);
                continue;
            }
            (void)viaActorType;
        }

        if (localPlayerState && playerState == localPlayerState) {
            ++dbgGhostEvict;
            WorldScan::MissCounterClear(s_playerDistMisses, key);
            it = localCache.erase(it);
            continue;
        }

        if (PlayerStateIsBot(playerState)) {
            ++dbgGsBot;
            WorldScan::MissCounterClear(s_playerDistMisses, key);
            it = localCache.erase(it);
            continue;
        }

        // Diagnostic only — do not prune on GS allowlist (see admit path).
        if (!gsPlayerStates.empty() && !gsPlayerStates.contains(playerState))
            ++dbgGsEvict;

        actor.actorState = playerState;

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
            if (WorldScan::MissCounterShouldEvict(
                    s_playerPosMisses, key, false, kPlayerGhostEvict)) {
                ClearPlayerGhostMisses(key);
                WorldScan::MissCounterClear(s_playerDistMisses, key);
                ++dbgGhostEvict;
                it = localCache.erase(it);
            } else {
                actor.Drawing = false;
                ++it;
            }
            continue;
        }
        WorldScan::MissCounterClear(s_playerPosMisses, key);

        const Vector3 delta = actor.WorldPos - cam.Location;
        const float distanceSq = static_cast<float>(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

        actor.Distance = sqrtf(distanceSq) / 100.0f;

        if (distanceSq > maxDistSq) {
            ++dbgDistSkip;
            // Soft flap: keep Drawing for a few out-of-range scans so boxes don't
            // blink at the distance edge. Hard erase paths (ghost/pos) unchanged.
            if (WorldScan::MissCounterShouldEvict(
                    s_playerDistMisses, key, false, kPlayerDistMissClearDrawing)) {
                actor.Drawing = false;
            } else {
                actor.Drawing = true;
            }
            ++it;
            continue;
        }
        WorldScan::MissCounterClear(s_playerDistMisses, key);

        actor.health = static_cast<float>(get_health(key));
        actor.maxhealth = static_cast<float>(get_maxhealth(key));
        actor.shield = static_cast<float>(get_armor(key));
        actor.maxshield = static_cast<float>(get_maxarmor(key));

        if (actor.health < 1.0f) {
            ++dbgHealthSkip;
            if (WorldScan::MissCounterShouldEvict(
                    s_playerHealthZeroMisses, key, false, kPlayerGhostEvict)) {
                ClearPlayerGhostMisses(key);
                WorldScan::MissCounterClear(s_playerDistMisses, key);
                ++dbgGhostEvict;
                it = localCache.erase(it);
                continue;
            }
        } else {
            WorldScan::MissCounterClear(s_playerHealthZeroMisses, key);
        }


        // Read weapon system from InventoryComponent (stowed slots + equipped + armor)
        std::string invWeapon, invStowed0, invStowed1;
        int invWq = -1, invSq0 = -1, invSq1 = -1, invClip = 0;
        float invArmorPlates = 0.f, invArmorPerPlate = 0.f;
        ReadPlayerInventory(key, invWeapon, invWq, invClip, invStowed0, invSq0, invStowed1, invSq1,
            invArmorPlates, invArmorPerPlate);
        // Only show Unarmed when there is no real gun in primary or stowed.
        if (!invWeapon.empty())
            actor.weaponName = invWeapon;
        else if (invStowed0.empty() && invStowed1.empty())
            actor.weaponName = "Unarmed";
        else
            actor.weaponName.clear();
        actor.weaponQuality = invWq;
        actor.weaponClip = invClip;
        actor.stowedWeapon0 = invStowed0;
        actor.stowedQuality0 = invSq0;
        actor.stowedWeapon1 = invStowed1;
        actor.stowedQuality1 = invSq1;
        actor.armorPlates = invArmorPlates;
        actor.armorPerPlate = invArmorPerPlate;

        // Skeleton refresh is owned by CollectEspRenderFrame (up to 16 nearest).
        // Calling GetBones here doubled NOCACHE scatter load and drove FPGA hitch storms.
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

        if (!haveScreenBox)
            ++dbgBoneMiss;

        if (haveScreenBox) {
            actor.ScreenTop = headScr;
            actor.ScreenBottom = footScr;
        }

        actor.isVisible = actor.actorMesh
            ? steam_decrypt::IsMeshVisible(static_cast<uint64_t>(actor.actorMesh))
            : true;

        actor.Drawing = actor.health >= 1.0f;
        ++it;
        entityStarted.store(true, std::memory_order_release);
    }

    for (const auto& [key, entry] : localCache) {
        if (entry.Drawing)
            ++dbgDrawing;
        (void)key;
    }

    // #region agent log
    {
        static std::unordered_set<uintptr_t> s_prevPlayerKeys;
        std::unordered_set<uintptr_t> curKeys;
        curKeys.reserve(localCache.size());
        for (const auto& [key, actor] : localCache) {
            curKeys.insert(key);
            WorldScan::FlickerCause cause = WorldScan::FlickerCause::Other;
            if (!actor.Drawing) {
                if (!IsPlausibleWorldPos(actor.WorldPos))
                    cause = WorldScan::FlickerCause::PosFail;
                else if (actor.Distance > var::esp_distance)
                    cause = WorldScan::FlickerCause::DistEdge;
                else if (actor.health < 1.0f)
                    cause = WorldScan::FlickerCause::Other;
                else
                    cause = WorldScan::FlickerCause::VisMiss;
            }
            WorldScan::NoteFlickerDrawing(
                WorldScan::FlickerChannel::Player, key, actor.Drawing, cause);
        }
        for (uintptr_t prev : s_prevPlayerKeys) {
            if (!curKeys.contains(prev))
                WorldScan::NoteFlickerGone(WorldScan::FlickerChannel::Player, prev);
        }
        s_prevPlayerKeys = std::move(curKeys);
        WorldScan::MaybeFlushFlickerScore();
    }
    // #endregion

    if (m_worldGeneration.load(std::memory_order_acquire) != gen)
        return;

    {
        std::unique_lock<std::shared_mutex> lock(m_playerCacheMutex);
        playerCache = std::move(localCache);
    }

    // Map unique enemy TeamIDs to squad indices (1-based, sorted ascending)
    {
        std::vector<uint8_t> uniqueTeams;
        for (const auto& [key, entry] : playerCache) {
            if (!entry.isAlly && entry.enemyTeamId != 0)
                uniqueTeams.push_back(entry.enemyTeamId);
        }
        std::sort(uniqueTeams.begin(), uniqueTeams.end());
        uniqueTeams.erase(std::unique(uniqueTeams.begin(), uniqueTeams.end()), uniqueTeams.end());
        for (auto& [key, entry] : playerCache) {
            if (!entry.isAlly && entry.enemyTeamId != 0) {
                auto it = std::find(uniqueTeams.begin(), uniqueTeams.end(), entry.enemyTeamId);
                if (it != uniqueTeams.end())
                    entry.squadIdx = static_cast<uint8_t>(std::distance(uniqueTeams.begin(), it) + 1);
            }
        }
    }

    // P7: advance ring only after successful gen-matched writeback so an
    // aborted pass never skips a slice band.
    if (admitRingAdvanceOk) {
        s_playerAdmitCoveredMask |= (1ull << (dbgAdmitSlice % kPlayerAdmitSlices));
        s_playerAdmitSliceCursor = (dbgAdmitSlice + 1) % kPlayerAdmitSlices;
        if (s_playerAdmitSliceCursor == 0) {
            const auto nowCycle = std::chrono::steady_clock::now();
            if (s_playerAdmitCycleStart.time_since_epoch().count() != 0) {
                s_playerAdmitLastCycleMs = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        nowCycle - s_playerAdmitCycleStart).count());
            }
            s_playerAdmitCycleStart = nowCycle;
            s_playerAdmitCoveredMask = 0;
        }
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
                << " slice=" << dbgAdmitSlice << "/" << kPlayerAdmitSlices
                << " sliceActors=" << dbgAdmitSliceActors
                << " prioNew=" << dbgAdmitPrioNew
                << " checked=" << dbgAdmitChecked
                << " fnameChecked=" << dbgAdmitFnameChecked
                << " scatterExecs=" << dbgAdmitScatterExecs
                << " memoSkip=" << dbgAdmitMemoSkip
                << " memoSize=" << s_playerScanNeg.size()
                << " cycleMs=" << s_playerAdmitLastCycleMs
                << " cover=0x" << std::hex << s_playerAdmitCoveredMask << std::dec
                << " ringResets=" << s_playerAdmitRingResets
                << std::endl;
        }
    }
}
