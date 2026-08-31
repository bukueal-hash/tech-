#include "../Core/Engine.h"
#include "../Core/ActorType.h"
#include "../Core/IntervalTimer.h"
#include "../Core/AssetNames.h"
#include "../Core/AgentLog.h"
#include "EspDraw.h"
#include "WorldScanCommon.h"
#include "LrtsVisibility.h"
#include "CollisionMirror.h"

#include <algorithm>
#include <cstdio>
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

// LRTS per-mesh visibility state (persisted across frames)
static std::unordered_map<uintptr_t, LrtsVis::MeshState> s_lrtsMeshStates;
// Per-actor verdict smoothing: keyed on cache key (NOT mesh) so MeshState
// Resets during scan retries cannot wipe verdict history and flicker the box.
static std::unordered_map<uintptr_t, LrtsVis::VerdictSmoother> s_playerVisSmooth;
// One Scan feed per pass while the session key is unverified (Scan's Phase 2
// costs a handful of reads per candidate; one rendered mesh is enough to lock
// the session-wide offset+key).
static bool s_playerScanFedThisPass = false;

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

// P7: 8-slice rotating admission ring over secondary + U8 actor walks.
// GameState PlayerArray admission stays every-pass (cheap, primary path).
// Positions stay hot via PositionRefreshPass @16ms. 8 slices (was 4) halves
// reads per pass so each EntityList burst is shorter and the shared DMA link
// gives the camera/position threads more air — less rot-lead skip, no player
// box jumps.
static constexpr size_t kPlayerAdmitSlices = 8;
static constexpr size_t kPlayerAdmitPrioNewMax = 64;
static size_t s_playerAdmitSliceCursor = 0;
// B5 (Riventides mirror): the player ring used the pre-B4 pattern (unordered
// probeSet, no resume frontier) and froze on slice 0 on 16K-actor maps,
// surviving only on cached players. Deterministic band + resume frontier now
// match the bot path; the serial PS-chase IS this list's verify stage, so
// the processEnd frontier covers partial passes (RobotList's separate verify
// backlog is not needed here).
static size_t s_playerAdmitResumeRow = 0;
static size_t s_playerAdmitLastDelta = 0;
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
constexpr std::ptrdiff_t kPsPawnPrivate = 0x428;
constexpr std::ptrdiff_t kPsPawnPrivateAlt = 0x428;
constexpr std::ptrdiff_t kPioneerCharacter = 0x548;
constexpr std::ptrdiff_t kPioneerCurrentPawn = 0x550;

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

    // Read UWorld::TimeSeconds (double in UE5) for LRTS visibility check
    const float worldTime = var::LrtsVisActive()
        ? static_cast<float>(Memory::read_nocache<double>(sGWorld + Offsets::UWorld_TimeSeconds))
        : 0.f;

    // Surface the raw read even when the per-actor gate never opens, so the
    // LRTS tab distinguishes "bad TimeSeconds offset" from "no meshes reached".
    if (var::LrtsVisActive()) {
        // RealTimeSeconds sits 0x10 past TimeSeconds and runs ahead by the
        // level load time. Render stamps may be based on either, so both are
        // surfaced to compare against what the decrypt actually produces.
        const float realTime = static_cast<float>(Memory::read_nocache<double>(
            sGWorld + Offsets::UWorld_TimeSeconds + 0x10));
        std::lock_guard<std::mutex> lk(LrtsVis::g_session.mu);
        LrtsVis::g_session.lastWorldTime = worldTime;
        LrtsVis::g_session.lastRealTime = realTime;
    }

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
    // P9: cache entries erased because the actor left currentActorSet — the
    // mass-eviction signature (flaky actor-array read = all players gone).
    int dbgListEvict = 0;
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
            ++dbgListEvict;
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
        // Agent log (throttled): resolved player-name diagnostic.
        {
            static std::chrono::steady_clock::time_point s_lastNameLog{};
            const auto nNow = std::chrono::steady_clock::now();
            if (nNow - s_lastNameLog > std::chrono::seconds(30)) {
                s_lastNameLog = nNow;
                const auto nMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                std::ofstream nf(kArcVerifyPath, std::ios::app);
                if (nf) {
                    // Raw FString fields at PS+0x448 for the name diagnosis:
                    // textData + count + first 16 bytes as hex (scramble check).
                    uint64_t fsPtr = 0; int32_t fsCount = 0;
                    uint64_t fsHex[2] = { 0, 0 };
                    if (Engine::IsUsermodePtr(ps)) {
                        fsPtr = Memory::read<uint64_t>(ps + Offsets::PlayerNamePrivate);
                        fsCount = Memory::read<int32_t>(ps + Offsets::PlayerNamePrivate + 8);
                        const uintptr_t src = Engine::IsUsermodePtr(fsPtr)
                            ? static_cast<uintptr_t>(fsPtr) : ps + Offsets::PlayerNamePrivate;
                        Memory::ReadRaw(src, fsHex, sizeof(fsHex));
                    }
                    nf << "{\"sessionId\":\"c190fb\",\"runId\":\"names\",\"hypothesisId\":\"N1\","
                       << "\"location\":\"EntityList.cpp\",\"message\":\"player_name\",\"data\":{\"ps\":"
                       << ps << ",\"pawn\":" << backPawn << ",\"name\":\"" << playerName
                       << "\",\"fs\":" << fsPtr << ",\"fc\":" << fsCount
                       << ",\"h0\":" << fsHex[0] << ",\"h1\":" << fsHex[1]
                       << "},\"timestamp\":" << nMs << "}\n";
                }
            }
        }
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
        s_playerAdmitLastDelta = 0;
    } else if (sActors != 0 && s_playerAdmitRingActorsPtr != 0
        && sActors != s_playerAdmitRingActorsPtr) {
        ringReset = true;
        s_playerAdmitLastDelta = 0;
    } else if (s_playerAdmitRingActorCount != 0) {
        const size_t delta = (N > s_playerAdmitRingActorCount)
            ? (N - s_playerAdmitRingActorCount)
            : (s_playerAdmitRingActorCount - N);
        const size_t thresh = (std::max)(static_cast<size_t>(64), N / 8);
        if (delta > thresh) {
            ringReset = true;
            s_playerAdmitLastDelta = delta;
        }
    }
    if (ringReset) {
        s_playerAdmitSliceCursor = 0;
        s_playerAdmitResumeRow = 0;
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

    // B5 (Riventides mirror): deterministic band order + bounded prio rows,
    // exactly like the bot path. The old unordered probeSet reordered every
    // pass, so no resume frontier could point at stable rows — the ring froze
    // on slice 0 (cache survived, new players never admitted).
    struct PlayerProbeRow {
        uintptr_t actor = 0;
        uint64_t ps = 0;
        uint32_t typeId = 0;
        uintptr_t root = 0;
        double distSq = 0.0;
        bool hasDist = false;
    };
    std::vector<PlayerProbeRow> bandRows;
    bandRows.reserve(sliceEnd - sliceBase);
    for (size_t i = sliceBase; i < sliceEnd; ++i) {
        const uintptr_t actor = admitIndex[i];
        if (localCache.contains(actor))
            continue;
        PlayerProbeRow row;
        row.actor = actor;
        bandRows.push_back(row);
    }
    dbgAdmitSliceActors = static_cast<int>(bandRows.size());

    std::vector<PlayerProbeRow> prioRows;
    prioRows.reserve(kPlayerAdmitPrioNewMax);
    for (uintptr_t actor : admitIndex) {
        if (prioRows.size() >= kPlayerAdmitPrioNewMax)
            break;
        if (s_playerAdmitPrevActors.contains(actor))
            continue;
        if (localCache.contains(actor))
            continue;
        PlayerProbeRow row;
        row.actor = actor;
        prioRows.push_back(row);
    }
    dbgAdmitPrioNew = static_cast<int>(prioRows.size());

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

    // Frontier: continue the band where the last pass stopped. Prio rows lead
    // so brand-new spawns are still probed every pass. Neg-memo filtering
    // moved into the screening loop so probeRows stays a 1:1 projection of
    // prio+band — that keeps the resume frontier positionally exact.
    const size_t bandResume = (std::min)(s_playerAdmitResumeRow, bandRows.size());
    std::vector<PlayerProbeRow> probeRows;
    probeRows.reserve(prioRows.size() + (bandRows.size() - bandResume));
    probeRows.insert(probeRows.end(), prioRows.begin(), prioRows.end());
    if (bandResume < bandRows.size()) {
        probeRows.insert(
            probeRows.end(),
            bandRows.begin() + static_cast<std::ptrdiff_t>(bandResume),
            bandRows.end());
    }

    // LAG1: cap this pass's DMA. Camera @8ms, position @16ms and the
    // frame-builder bone reads @12ms share the one DMA link; a scanner that
    // hogs it starves them all and bones/boxes lag behind moving targets.
    WorldScan::ScanBudget scanBudget(std::chrono::milliseconds(90));
    bool slicePartial = false;

    // Small scatter chunks + yield after each exec so the HIGHEST-priority
    // camera thread (8ms) can slip onto the DMA link between batches; a
    // single huge batch starves it and boxes jump when rot-lead skips.
    // RIVENTIDES: 48-row chunks keep every execute short on a saturated bus
    // (16K actors) so the process loop still runs this pass.
    constexpr size_t kPlayerAdmitChunk = 48;
    size_t base = 0;
    for (; base < probeRows.size(); base += kPlayerAdmitChunk) {
        if (scanBudget.expired()) { slicePartial = true; break; }
        const size_t chunkEnd = (std::min)(base + kPlayerAdmitChunk, probeRows.size());
        ScatterSession scatter;
        if (!scatter.isValid())
            break;
        bool prepOk = true;
        for (size_t i = base; i < chunkEnd; ++i) {
            PlayerProbeRow& r = probeRows[i];
            prepOk = scatter.prepare(r.actor + kPawnPlayerState, r.ps) && prepOk;
            prepOk = scatter.prepare(r.actor + typeOff, r.typeId) && prepOk;
            prepOk = scatter.prepare(r.actor + Offsets::RootComponent, r.root) && prepOk;
        }
        if (prepOk && scatter.execute())
            ++dbgAdmitScatterExecs;
        std::this_thread::yield();
    }

    // B5: only rows in [0, scatteredEnd) were scattered this pass; rows beyond
    // still hold zero buffers and must never be screened (mass negative-memo
    // blindness). The resume frontier is computed after screening (processEnd).
    const size_t scatteredEnd = (std::min)(base, probeRows.size());

    // Distance-priority admission: scatter each row's root WorldLocation and
    // sort the slice nearest-first. Without this, admission order is the actor
    // array order, so a player 300m away hides in a low-priority slice while
    // far actors of earlier slices get admitted first — new ESP can take a full
    // ring sweep to appear. With the sort, a budget-clipped pass still admits
    // the nearest actors of this slice first.
    // Only a fully-scattered sweep may distance-sort: a partial pass must keep
    // [0, scatteredEnd) order aligned with the band resume frontier.
    if (scatteredEnd >= probeRows.size() && !probeRows.empty()) {
        const uintptr_t lroot = Memory::read<uintptr_t>(
            sAcknowledgedPawn + Offsets::RootComponent);
        Vector3 localPos{};
        if (lroot && IsValidPointer(lroot))
            localPos = Engine::ReadWorldLocationNocache(lroot, /*allowRelativeFallback=*/true);
        if (IsPlausibleWorldPos(localPos)) {
            std::vector<Engine::FVector3d> locBuf(probeRows.size());
            for (size_t distBase = 0; distBase < probeRows.size(); distBase += kPlayerAdmitChunk) {
                if (scanBudget.expired())
                    break;
                const size_t distEnd = (std::min)(distBase + kPlayerAdmitChunk, probeRows.size());
                ScatterSession distScatter;
                if (!distScatter.isValid())
                    break;
                bool distPrepOk = true;
                for (size_t i = distBase; i < distEnd; ++i) {
                    PlayerProbeRow& r = probeRows[i];
                    if (!r.root) {
                        distPrepOk = false;
                        continue;
                    }
                    distPrepOk = distScatter.prepare(r.root + Offsets::WorldLocation, locBuf[i]) && distPrepOk;
                }
                if (distPrepOk && distScatter.execute())
                    ++dbgAdmitScatterExecs;
                std::this_thread::yield();
            }
            for (size_t i = 0; i < probeRows.size(); ++i) {
                PlayerProbeRow& r = probeRows[i];
                const Vector3 p = Engine::ToVector3(locBuf[i]);
                if (!r.root || !IsPlausibleWorldPos(p))
                    continue;
                const double dx = p.x - localPos.x;
                const double dy = p.y - localPos.y;
                const double dz = p.z - localPos.z;
                r.distSq = dx * dx + dy * dy + dz * dz;
                r.hasDist = true;
            }
            std::stable_sort(
                probeRows.begin(),
                probeRows.end(),
                [](const PlayerProbeRow& a, const PlayerProbeRow& b) {
                    if (a.hasDist != b.hasDist)
                        return a.hasDist > b.hasDist;
                    return a.distSq < b.distSq;
                });
        }
    }

    // Fresh micro-budget (same as the bot path): the probe scatter may have
    // consumed the pass budget under DMA load, but scattered rows must still
    // be processed or the ring sticks and admission starves.
    WorldScan::ScanBudget procBudget(std::chrono::milliseconds(40));
    size_t processEnd = 0;
    for (size_t ri = 0; ri < scatteredEnd; ++ri) {
        const PlayerProbeRow& row = probeRows[ri];
        if (procBudget.expired()) { slicePartial = true; break; }
        processEnd = ri + 1;
        if (PlayerScanNegMemoHit(row.actor, dbgAdmitMemoSkip))
            continue;
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

    // B5 (mirror): monotonic resume frontier from the screening frontier. A
    // budget break anywhere persists progress; only a full scatter+screen
    // sweep resets the resume and lets the ring advance.
    {
        const size_t prioCount = prioRows.size();
        const bool sortedSweep = scatteredEnd >= probeRows.size();
        if (sortedSweep && processEnd == scatteredEnd) {
            s_playerAdmitResumeRow = 0;
        } else if (sortedSweep) {
            // probeRows were distance-sorted this pass; a broken sweep must
            // not map processEnd back onto the unsorted band order.
            s_playerAdmitResumeRow = 0;
            slicePartial = true;
        } else {
            const size_t bandProcessed =
                (processEnd > prioCount) ? (processEnd - prioCount) : 0;
            s_playerAdmitResumeRow =
                bandResume + (std::min)(bandProcessed, bandRows.size() - bandResume);
            slicePartial = true;
        }
    }

    s_playerAdmitPrevActors.clear();
    s_playerAdmitPrevActors.insert(admitIndex.begin(), admitIndex.end());
    // A budgeted partial pass must not skip a ring band — the slice is
    // retried in full next pass instead (see the advance gate below).
    admitRingAdvanceOk = !slicePartial;

    dbgPreAdmit = static_cast<int>(localCache.size());

    const uint8_t myTeamId =
        Memory::read<uint8_t>(sAcknowledgedPawn + Offsets::TeamID);
    // EEmbarkTeamId (SDK Enum.cpp): Team1=0, Team2=1, Team3=2, NoTeam=255.
    // Audit #4: isAlly used myTeamId != 0 as a validity check — Team1 (=0)
    // never matched, so hide_allies silently failed for Team1. Failed DMA
    // reads return 0 (= Team1), so BOTH sides must read a valid team (0..2)
    // before anyone is classified an ally — no wrong answers from garbage.
    // TeamID values are 4-10 (NOT the SDK enum 0-2). Reject NoTeam (255)
    // and zero (DMA garbage returns 0, which is NOT a valid team).
    const bool myTeamValid = (myTeamId != 255 && myTeamId != 0);

    // Prefer mesh CompToWorld (bot parity); root fallback — root alone stays frozen on remotes.
    s_playerScanFedThisPass = false;
    for (auto it = localCache.begin(); it != localCache.end(); ++it) {
        const uintptr_t key = it->first;
        if (key == sAcknowledgedPawn)
            continue;
        if (scanBudget.expired())
            break;

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
        if (scanBudget.expired())
            break;

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
        actor.isAlly =
            (myTeamValid && enemyTeamId != 255 && enemyTeamId != 0 && myTeamId == enemyTeamId);
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

        // LRTS visibility: raw fast-path first, encrypted scan/key fallback.
        // Every read here must bypass the VMM cache — render timestamps change
        // per frame and the cached path serves the same bytes twice, so the
        // scan would never see a value move and could never lock a key.
        if (var::LrtsVisActive() && actor.actorMesh && worldTime > 10.f) {
            // Fast-switch precedence: the per-frame decrypted render stamp
            // (CheckDirect) reflects the last time the renderer drew the mesh,
            // so it flips within a frame of occlusion. The bRecentlyRendered
            // flag (CheckRendered) is render-side sticky and holds set for a
            // few seconds — keep it only as fallback for when decrypt can't
            // resolve, never as the primary verdict.
            const char* stage = "unknown";
            float directVal = -1.f;
            // Prefer the session-discovered offset/key pair — hardcoded pairs
            // go stale every game build (the current one reads 0x4C4 as zero).
            // Until Scan lands a discovery, fall back to the hardcoded constant.
            uint32_t lrtsOff =
                static_cast<uint32_t>(Offsets::Mesh_LastRenderTimeOnScreenEnc);
            uint32_t lrtsKey = Offsets::Mesh_LastRenderTimeOnScreenKey;
            {
                // Read the shared session pair under its mutex: Scan publishes
                // them asynchronously from the same worker, so an unlocked read
                // could tear a half-published offset/key pair.
                std::lock_guard<std::mutex> sk(LrtsVis::g_session.mu);
                if (LrtsVis::g_session.lrtsOffset != 0
                    && LrtsVis::g_session.keyCount > 0) {
                    lrtsOff = static_cast<uint32_t>(LrtsVis::g_session.lrtsOffset);
                    lrtsKey = LrtsVis::g_session.keys[0];
                }
            }
            auto vis = LrtsVis::CheckDirect(
                [](uint64_t a) { return Memory::read_nocache<uint32_t>(a); },
                actor.actorMesh, LrtsVis::g_session, worldTime,
                lrtsOff, lrtsKey);
            if (vis != LrtsVis::Result::Unknown) {
                stage = "direct";
                std::lock_guard<std::mutex> dl(LrtsVis::g_session.mu);
                directVal = LrtsVis::g_session.lastDirectValue;
            } else {
                // Feed Scan while unverified: a rendered mesh (flag byte set)
                // is the one thing Scan needs to rediscover the current
                // build's offset+key. One mesh per pass — once verified,
                // CheckDirect above is primary and this costs nothing.
                if (!LrtsVis::g_session.verified
                    && !s_playerScanFedThisPass) {
                    const uint8_t brrFed = Memory::read_nocache<uint8_t>(
                        actor.actorMesh + LrtsVis::BrrOffset);
                    if (brrFed & LrtsVis::BrrMask) {
                        auto& ms = s_lrtsMeshStates[actor.actorMesh];
                        if (!ms.meshComp) ms.meshComp = actor.actorMesh;
                        LrtsVis::Scan(ms, LrtsVis::g_session,
                            [](uint64_t a) { return Memory::read_nocache<uint8_t>(a); },
                            [](uint64_t a) { return Memory::read_nocache<uint32_t>(a); },
                            [](uint64_t a, void* b, uint32_t s) {
                                return PCIMemory::ReadVirtualMemoryNoCache(a, b, s);
                            },
                            worldTime, sGWorld);
                        s_playerScanFedThisPass = true;
                    }
                }
                vis = LrtsVis::CheckRendered(
                    [](uint64_t a) { return Memory::read_nocache<uint8_t>(a); },
                    actor.actorMesh, LrtsVis::g_session,
                    LrtsVis::BrrOffset, LrtsVis::BrrMask);
                if (vis != LrtsVis::Result::Unknown)
                    stage = "flag";
            }
            if (vis == LrtsVis::Result::Unknown) {
                vis = LrtsVis::CheckRaw(
                    [](uint64_t a) { return Memory::read_nocache<uint32_t>(a); },
                    actor.actorMesh, LrtsVis::g_session, worldTime);
                if (vis != LrtsVis::Result::Unknown)
                    stage = "raw";
            }
            if (vis == LrtsVis::Result::Unknown) {
                auto& ms = s_lrtsMeshStates[actor.actorMesh];
                if (!ms.meshComp) ms.meshComp = actor.actorMesh;
                LrtsVis::Scan(ms, LrtsVis::g_session,
                    [](uint64_t a) { return Memory::read_nocache<uint8_t>(a); },
                    [](uint64_t a) { return Memory::read_nocache<uint32_t>(a); },
                    [](uint64_t a, void* b, uint32_t s) {
                        return PCIMemory::ReadVirtualMemoryNoCache(a, b, s);
                    },
                    worldTime, sGWorld);
                vis = LrtsVis::Check(ms, LrtsVis::g_session,
                    [](uint64_t a) { return Memory::read_nocache<uint8_t>(a); },
                    [](uint64_t a) { return Memory::read_nocache<uint32_t>(a); },
                    worldTime, sGWorld);
                if (vis != LrtsVis::Result::Unknown)
                    stage = "scan";
            }
            // Hysteresis: require 2 confirmed Occluded checks before hiding
            // (kills flag-byte flicker + transient 0x00 reads); Unknown keeps
            // the last verdict so read failures never pop boxes through walls.
            const uint64_t nowMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            // BRR-aware hysteresis: fast-hide ONLY on a successfully-read byte
            // (non-zero) with the rendered bit clear. brr == 0x00 means the DMA
            // read failed — falling back to the 2-check hysteresis keeps a
            // transient flap from blinking the box (ghost-flicker regression).
            const uint8_t brrSmooth = Memory::read_nocache<uint8_t>(
                actor.actorMesh + LrtsVis::BrrOffset);
            const bool confirmedNotRendered =
                brrSmooth != 0 && (brrSmooth & LrtsVis::BrrMask) == 0;

            // Stage 3: LRTS + collision combined voting. LRTS is primary;
            // collision is the tie-breaker for stale stamps (Visible+blocked
            // → Occluded) and the fallback for Unknown. Occluded wins outright
            // (renderer truth). Fail-open when the tree isn't ready.
            if (var::collision_vis_enabled && CollisionMirror::IsReady()
                && (actor.WorldPos.x != 0.0 || actor.WorldPos.y != 0.0 || actor.WorldPos.z != 0.0)) {
                const bool blocked = !CollisionMirror::QueryVisible(
                    cam.Location, actor.WorldPos);
                switch (vis) {
                case LrtsVis::Result::Occluded:
                    break;  // renderer truth wins
                case LrtsVis::Result::Visible:
                    if (blocked)
                        vis = LrtsVis::Result::Occluded;  // stale stamp behind wall
                    break;
                case LrtsVis::Result::Unknown:
                default:
                    vis = blocked
                        ? LrtsVis::Result::Occluded
                        : LrtsVis::Result::Visible;  // collision fills LRTS gaps
                    break;
                }
            }
            actor.isVisible = s_playerVisSmooth[key].Update(
                vis, nowMs, confirmedNotRendered);

            // Per-actor trace, 1 Hz. Answers two things the aggregate counters
            // cannot: whether distinct actors get distinct flag bytes, and how
            // long the byte takes to follow a line-of-sight change.
            {
                static std::chrono::steady_clock::time_point sLastVisTrace{};
                static int sVisTraceThisPass = 0;
                const auto nowTrace = std::chrono::steady_clock::now();
                if (nowTrace - sLastVisTrace > std::chrono::seconds(1)) {
                    sLastVisTrace = nowTrace;
                    sVisTraceThisPass = 0;
                }
                if (sVisTraceThisPass < 8) {
                    ++sVisTraceThisPass;
                    const uint8_t brrTrace = Memory::read_nocache<uint8_t>(
                        actor.actorMesh + LrtsVis::BrrOffset);
                    // Raw encrypted render-stamp dwords + worldTime so the
                    // correct XOR key can be derived offline (CheckDirect's
                    // key is stale for this build — direct stays -1).
                    const uint32_t raw4c4 = Memory::read_nocache<uint32_t>(
                        actor.actorMesh + 0x4C4);
                    const uint32_t raw4cc = Memory::read_nocache<uint32_t>(
                        actor.actorMesh + 0x4CC);
                    std::ofstream vf(kArcVerifyPath, std::ios::app);
                    if (vf) {
                        const auto vts = std::chrono::duration_cast<
                            std::chrono::milliseconds>(
                                std::chrono::system_clock::now()
                                    .time_since_epoch()).count();
                        vf << "{\"location\":\"EntityList.cpp\","
                           << "\"message\":\"vis_trace\","
                           << "\"timestamp\":" << vts
                           << ",\"mesh\":\"0x" << std::hex << actor.actorMesh
                           << "\",\"brr\":\"0x" << static_cast<unsigned>(brrTrace)
                           << std::dec << "\""
                           << ",\"vis\":" << static_cast<int>(vis)
                           << ",\"stage\":\"" << stage << "\""
                           << ",\"direct\":" << directVal
                           << ",\"raw4c4\":\"0x" << std::hex << raw4c4
                           << "\",\"raw4cc\":\"0x" << raw4cc << std::dec << "\""
                           << ",\"worldTime\":" << worldTime
                           << "}\n";
                    }
                }
            }
        } else {
            actor.isVisible = true;

            // The gate skipped this actor, so nothing above ran. Record why.
            static std::chrono::steady_clock::time_point sLastGateTrace{};
            const auto nowGate = std::chrono::steady_clock::now();
            if (nowGate - sLastGateTrace > std::chrono::seconds(1)) {
                sLastGateTrace = nowGate;
                std::ofstream gf(kArcVerifyPath, std::ios::app);
                if (gf) {
                    const auto gts = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            std::chrono::system_clock::now()
                                .time_since_epoch()).count();
                    gf << "{\"location\":\"EntityList.cpp\","
                       << "\"message\":\"vis_gate\","
                       << "\"timestamp\":" << gts
                       << ",\"visEnabled\":" << (var::vis_enabled ? 1 : 0)
                       << ",\"mesh\":\"0x" << std::hex << actor.actorMesh << std::dec
                       << "\",\"worldTime\":" << worldTime
                       << "}\n";
                }
            }
        }

        // Audit #3: Drawing must not hinge on health. get_health() returns NaN on
        // a failed read and NaN >= 1.0f is false, so one bad health read erased a
        // live player from ESP. Pos/distance were already validated above; dead
        // players are evicted by the health-zero ghost counter (kPlayerGhostEvict).
        actor.Drawing = true;
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
            if (!entry.isAlly && entry.enemyTeamId != 255 && entry.enemyTeamId != 0)
                uniqueTeams.push_back(entry.enemyTeamId);
        }
        std::sort(uniqueTeams.begin(), uniqueTeams.end());
        uniqueTeams.erase(std::unique(uniqueTeams.begin(), uniqueTeams.end()), uniqueTeams.end());
        for (auto& [key, entry] : playerCache) {
            if (!entry.isAlly && entry.enemyTeamId != 255 && entry.enemyTeamId != 0) {
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

    // P9: always-on file trace of every admission/draw stage so a "player ran
    // up, no ESP" report is traceable from the debug log alone — the console
    // [debugPlayer] print needs the debug overlay on and never reaches disk.
    {
        static IntervalTimer playerFileTimer(2000);
        if (playerFileTimer.fire()) {
            char pbuf[768]{};
            snprintf(pbuf, sizeof(pbuf),
                "{\"scanned\":%d,\"admitted\":%d,\"cache\":%zu,\"listEvict\":%d,"
                "\"teamEvict\":%d,\"psEvict\":%d,\"posEvict\":%d,\"distSkip\":%d,"
                "\"ghostEvict\":%d,\"rootSkip\":%d,\"meshSkip\":%d,\"psSkip\":%d,"
                "\"healthSkip\":%d,\"gsBot\":%d,\"gsPawnNull\":%d,\"gsEvict\":%d,"
                "\"actorTypeAdmit\":%d,\"prioNew\":%d,\"checked\":%d,"
                "\"memoSkip\":%d,\"slice\":%zu,\"cycleMs\":%d,\"ringResets\":%d,"
                "\"admitN\":%zu,\"lastDelta\":%zu}",
                dbgScanned, dbgAdmitted, playerCache.size(), dbgListEvict,
                dbgTeamEvict, dbgPsEvict, dbgPosEvict, dbgDistSkip,
                dbgGhostEvict, dbgRootSkip, dbgMeshSkip, dbgPsSkip,
                dbgHealthSkip, dbgGsBot, dbgGsPawnNull, dbgGsEvict,
                dbgActorTypeAdmit, dbgAdmitPrioNew, dbgAdmitChecked,
                dbgAdmitMemoSkip, dbgAdmitSlice, s_playerAdmitLastCycleMs,
                s_playerAdmitRingResets, dbgAdmitN, s_playerAdmitLastDelta);
            // player_admit_stats is a throttled (2s) verification tap — it must
            // reach the real log (kArcDebugLogPath is NUL by design).
            std::ofstream f(kArcVerifyPath, std::ios::app);
            if (f) {
                const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"c190fb\",\"runId\":\"post-fix\","
                  << "\"hypothesisId\":\"P9\",\"location\":\"EntityList.cpp:EntityList\","
                  << "\"message\":\"player_admit_stats\",\"data\":" << pbuf
                  << ",\"timestamp\":" << ts << "}\n";
            }
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
