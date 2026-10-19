#include "../Core/Engine.h"
#include "../Interface/Utils/UpgradeOptions.hpp"
#include "../Core/ActorType.h"
#include "../Core/IntervalTimer.h"
#include "../Core/AssetNames.h"
#include "../Core/AgentLog.h"
#include "../Core/BotAdmitQueue.hpp"
#include "../Core/EntityDiagnostics.hpp"
#include "../Core/EmbreeVisCheck.hpp"
#include "../Core/PlayerEspMissLog.hpp"
#include "../Core/ResolvedCache.hpp"
#include "EspDraw.h"
#include <atomic>
#include <cstdio>
#include "WorldScanCommon.h"

#include <algorithm>
#include <cctype>
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

// Actor-array snapshots can briefly omit a live pawn while the DMA ring is
// being rebuilt. Do not evict a player on one missing snapshot; the normal
// root/position ghost counters still remove genuinely departed actors.
static std::unordered_map<uintptr_t, uint8_t> s_playerActorSetMisses;
static constexpr uint8_t kPlayerActorSetGracePasses = 5;

static std::unordered_map<uintptr_t, uint8_t> s_playerRootMisses;
static std::unordered_map<uintptr_t, uint8_t> s_playerPosMisses;
static std::unordered_map<uintptr_t, uint8_t> s_playerHealthZeroMisses;
static constexpr uint8_t kPlayerGhostEvict = 10;

// Player death evidence: HealthComponent current health reads 0 continuously
// for this long. Drives actor.bIsDead only; liveness is still position-gated.
static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point> s_playerZeroHealthSince;
static constexpr auto kPlayerDeadZeroHealthMs = std::chrono::milliseconds(600);

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
// P10: persistent pending lane (Core/BotAdmitQueue.hpp, shared with the bot
// path). Newly-seen actors stay queued until a definitive outcome instead of
// being "new" for one pass — a player that missed its single priority window
// (64 cap / budget clip / undecrypted name) used to wait a full ring sweep
// before being probed again.
static AdmitQueue::PendingQueue s_playerAdmitQueue;
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

// All slots below come from the dumped SDK (Project/Core/Offsets.h) — nothing
// here is a hand-pinned literal any more, so a new dump moves them together.
// Prefer CompToWorld, then net snapshots.
// AActor: ReplicatedMovement; FRepMovement::Location @ +0x30.
constexpr std::ptrdiff_t kACharacterMesh = Offsets::USkeletalMeshComponent;
constexpr std::ptrdiff_t kACharacterMovement = Offsets::CharacterMovement;
constexpr std::ptrdiff_t kACharacterCapsule = Offsets::Character_CapsuleComponent;
constexpr std::ptrdiff_t kCmcLastUpdateLocation = Offsets::CMC_LastUpdateLocation;
constexpr std::ptrdiff_t kReplicatedMovement = Offsets::ReplicatedMovement;
constexpr std::ptrdiff_t kRepMovLocation = 0x30;   // FRepMovement::Location
constexpr std::ptrdiff_t kStateInterpolator = Offsets::StateInterpolator;
constexpr std::ptrdiff_t kReplicatedRootTransform = Offsets::ReplicatedRootTransform;

// APawn::PlayerState is 0x3E0 in the SDK. Do not use the controller's
// PlayerState slot (0x3D0) here; that pointer belongs to AController.
constexpr std::ptrdiff_t kPawnPlayerState = Offsets::APlayerState;

// Auto-learned link slots (Data\resolved.ini), defined lower in this file.
std::ptrdiff_t PinnedPsPawnSlot();
std::ptrdiff_t PawnStateOff();
constexpr std::ptrdiff_t kPsPawnPrivate = Offsets::PlayerState_PawnPrivate;
constexpr std::ptrdiff_t kPsPawnPrivateAlt = Offsets::PlayerState_PawnPrivate;
constexpr std::ptrdiff_t kPioneerCharacter = Offsets::PioneerPlayerState_PioneerCharacter;
constexpr std::ptrdiff_t kPioneerCurrentPawn = Offsets::PioneerPlayerState_CurrentPawn;

bool PsBacklinksToPawn(uintptr_t ps, uintptr_t pawn)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Link);
    // nocache: this runs on the REMOTE path. Through the VMM data cache the
    // back-link reads 0 while the probe (nocache) reads the live pawn at the
    // same slot in the same instant, so every remote PS was refused as
    // admit_pawn_missing and no remote player could be admitted.
    const std::ptrdiff_t pinned = PinnedPsPawnSlot();
    if (pinned > 0 && Memory::read_nocache<uintptr_t>(ps + pinned) == pawn)
        return true;
    const std::ptrdiff_t offs[] = {
        kPioneerCurrentPawn,
        kPioneerCharacter,
        kPsPawnPrivate,
        kPsPawnPrivateAlt,
        0x438,   // local-chain ground truth on the live build
    };
    for (const std::ptrdiff_t off : offs) {
        if (off == pinned)
            continue;
        if (Memory::read_nocache<uintptr_t>(ps + off) == pawn)
            return true;
    }
    return false;
}

bool TryResolvePawnPlayerState(uintptr_t pawn, uintptr_t& outPs)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Link);
    if (!pawn || !Memory::IsValidPtrFast2(pawn))
        return false;
    // Step 3: nocache on forward PS read — stale VMM page cache was suspected.
    const uintptr_t ps =
        Memory::read_nocache<uintptr_t>(pawn + PawnStateOff());
    if (!ps || !Memory::IsValidPtrFast2(ps))
        return false;
    if (!PsBacklinksToPawn(ps, pawn))
        return false;
    outPs = ps;
    return true;
}

bool IsActorTypePlayerPawn(uintptr_t actor)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Name);
    if (!actor || !Memory::IsValidPtrFast2(actor))
        return false;
    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    return ArcActorType::IsPlayerClassId(masked);
}

/** Primary pawn+0x3C0 PS chase; fallback ActorType + 0x3C0 without backlink. */
bool TryResolvePlayerStateAny(uintptr_t pawn, uintptr_t& outPs, bool& outViaActorType)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Link);
    outViaActorType = false;
    if (TryResolvePawnPlayerState(pawn, outPs))
        return true;
    if (!IsActorTypePlayerPawn(pawn))
        return false;
    // Offsets::APlayerState (0x3A0) on Controller; on Pawn the learned slot.
    const uintptr_t ps =
        Memory::read_nocache<uintptr_t>(pawn + PawnStateOff());
    if (!ps || !Memory::IsValidPtrFast2(ps))
        return false;
    outPs = ps;
    outViaActorType = true;
    return true;
}

// Use the SDK-backed PlayerState status byte already defined in Offsets.h.
// The old local 0x3AA probe could classify ordinary remote players as bots
// and silently remove them before they ever entered the ESP cache.
constexpr std::ptrdiff_t kPlayerStateBIsABot = Offsets::PS_BotStateByte;
constexpr uint8_t kPlayerStateBIsABotMask = Offsets::PS_BotStateBotMask;

bool PlayerStateIsBot(uintptr_t ps)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Name);
    if (!ps || !Memory::IsValidPtrFast2(ps))
        return false;
    const uint8_t flags = Memory::read<uint8_t>(ps + kPlayerStateBIsABot);
    return (flags & kPlayerStateBIsABotMask) != 0;
}

// Humans only: a real player's PlayerState class is BP_PioneerPlayerState_C.
// Empty class name (FName not decrypted yet) is treated as not-a-player.
bool PlayerStateIsPioneerClass(uintptr_t ps)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Name);
    if (!ps || !Memory::IsValidPtrFast2(ps))
        return false;
    std::string cls = steam_decrypt::GetActorClassFName(ps);
    for (char& c : cls)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return cls.find("pioneerplayerstate") != std::string::npos;
}

// PlayerState -> pawn slot, learned from the data instead of a dump. Every
// remote PlayerState holds its pawn at one fixed offset, so the offset whose
// value is a live player pawn for most sampled PlayerStates is the one. Pinned
// in Data\resolved.ini ("ps_pawn_slot") and tried first on later passes.
std::atomic<std::ptrdiff_t> s_psPawnSlotPinned{ -1 };

std::ptrdiff_t PinnedPsPawnSlot()
{
    std::ptrdiff_t v = s_psPawnSlotPinned.load(std::memory_order_acquire);
    if (v != -1)
        return v;
    v = 0;
    const std::string text = ResolvedCache::Get("ps_pawn_slot");
    if (!text.empty()) {
        try {
            v = static_cast<std::ptrdiff_t>(std::stoll(text, nullptr, 0));
        } catch (...) {
            v = 0;
        }
    }
    s_psPawnSlotPinned.store(v, std::memory_order_release);
    return v;
}

// pawn -> PlayerState slot (the forward hop remotes need). Same provenance as
// the reverse slot: learned from the local chain and pinned.
std::atomic<std::ptrdiff_t> s_pawnPsOffPinned{ -1 };

std::ptrdiff_t PinnedPawnPsOff()
{
    std::ptrdiff_t v = s_pawnPsOffPinned.load(std::memory_order_acquire);
    if (v != -1)
        return v;
    v = 0;
    const std::string text = ResolvedCache::Get("pawn_ps_off");
    if (!text.empty()) {
        try {
            v = static_cast<std::ptrdiff_t>(std::stoll(text, nullptr, 0));
        } catch (...) {
            v = 0;
        }
    }
    s_pawnPsOffPinned.store(v, std::memory_order_release);
    return v;
}

/** The pawn->PlayerState slot in force: learned when pinned, else the SDK value. */
std::ptrdiff_t PawnStateOff()
{
    const std::ptrdiff_t p = PinnedPawnPsOff();
    return p > 0 ? p : kPawnPlayerState;
}

// Learn both link slots from the LOCAL chain, which is ground truth: the local
// pawn and its PlayerState are both known and already trusted (the local player
// panel reads them). One scan of the pawn finds the slot holding that exact
// PlayerState, and one scan of the PlayerState finds the slot holding that exact
// pawn. This replaces guessing: the compiled PioneerPlayerState offsets do not
// hold on the live build (PlayerState+0x438 carries the pawn, not +0x560).
void LearnLocalPairOffsets(uintptr_t pawn, uintptr_t ps)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Link);
    if (!pawn || !ps || !Memory::IsValidPtrFast2(pawn) || !Memory::IsValidPtrFast2(ps))
        return;
    if (PinnedPawnPsOff() <= 0) {
        for (std::ptrdiff_t off = 0x300; off <= 0x600; off += 8) {
            if (Memory::read_nocache<uintptr_t>(pawn + off) != ps)
                continue;
            s_pawnPsOffPinned.store(off, std::memory_order_release);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(off));
            ResolvedCache::Put("pawn_ps_off", buf);
            break;
        }
    }
    if (PinnedPsPawnSlot() <= 0) {
        for (std::ptrdiff_t off = 0x200; off <= 0x800; off += 8) {
            if (Memory::read_nocache<uintptr_t>(ps + off) != pawn)
                continue;
            s_psPawnSlotPinned.store(off, std::memory_order_release);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(off));
            ResolvedCache::Put("ps_pawn_slot", buf);
            break;
        }
    }
}

uintptr_t ResolvePawnFromPlayerState(uintptr_t ps)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Link);
    if (!ps || !Memory::IsValidPtrFast2(ps))
        return 0;
    // Learned slot first (one read); the named offsets below are the fallback.
    const std::ptrdiff_t learned = PinnedPsPawnSlot();
    if (learned > 0) {
        const uintptr_t pawn = Memory::read_nocache<uintptr_t>(ps + learned);
        if (pawn && Memory::IsValidPtrFast2(pawn))
            return pawn;
    }
    const std::ptrdiff_t offs[] = {
        kPioneerCurrentPawn,
        kPioneerCharacter,
        kPsPawnPrivate,
        kPsPawnPrivateAlt,
    };
    for (const std::ptrdiff_t off : offs) {
        // nocache: remotes are the whole point of this helper ("Primary
        // admission: GameState PlayerArray -> PS -> pawn"). The cached read
        // returned 0 for slots the nocache probe read as a live pawn, which is
        // what starved remote players out with admit_pawn_missing while robots,
        // which never take this hop, kept working.
        const uintptr_t pawn = Memory::read_nocache<uintptr_t>(ps + off);
        if (pawn && Memory::IsValidPtrFast2(pawn))
            return pawn;
    }
    return 0;
}

/** PioneerPlayerState has no health block — 0x560 is bIsInEncounter; use the pawn's HealthComponent. */
float ReadPawnHealthForAdmit(uintptr_t pawn)
{
    // The scope covers the nested HealthComponent chase and stat read too.
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Health);
    return static_cast<float>(
        Engine::ReadHealthSlot(pawn, Offsets::Health, Offsets::HealthLegacy));
}

Vector3 ResolvePlayerWorldPos(uintptr_t pawn, uintptr_t root, uintptr_t /*legacy*/)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Position);
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
        // Root component RelativeLocation (0x280) is the reflected, live world
        // position: a root component has no parent, so its relative transform IS
        // its world transform. Child components (the mesh) only carry a frozen
        // offset to the root, and the unreflected ComponentToWorld slot the
        // readers used to trust (0x330) reads plausible rotation junk on this
        // build, so root rel must win before any 0x330 read.
        if (root && Memory::IsValidPtrFast2(root)) {
            const Vector3 rel =
                Memory::read_nocache<Vector3>(root + Offsets::RelativeLocation);
            if (IsPlausibleWorldPos(rel))
                return rel;
        }

        // Fallback: live CompToWorld on the mesh/capsule components.
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

        // Fallback: root RelativeLocation again (kept for the mesh-less case).

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
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Link);
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
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::World);
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
    // hide_allies evidence. Ally identity has one silent failure mode: a team
    // byte that cannot be read (or reads 0/255) makes NOTHING an ally, so every
    // teammate is drawn as an enemy with no error and no counter to prove it.
    // These split the verdict by the signal that produced it.
    int dbgAllyByTeam = 0;
    int dbgAllyBySquad = 0;
    int dbgAllyMyTeamInvalid = 0;
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
    // PlayerStates dropped by the pioneer-class gate in the GameState pass —
    // a missing player whose class isn't BP_PioneerPlayerState_C shows up here.
    int dbgNotPioneer = 0;
    // Enemies inside ESP range (2m..collect range) that an admission gate
    // rejected this pass and that never reached the player cache.
    int dbgInRangeNotAdmitted = 0;

    std::unordered_set<uint64_t> currentActorSet(
        currentActors.begin(),
        currentActors.end());

    CameraCache cam{};
    {
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        cam = g_Camera;
    }

    // Must match the frame cull (Esp.cpp PlayerCollectMaxM). Previously this
    // ignored var::enableesp, so with ESP off it admitted at esp_distance while
    // the frame cull used the smaller radius and dropped them every frame.
    const float collectRange = PlayerCollectMaxM();
    const float maxDistSq = collectRange * collectRange * 10000.0f;
    // Keep the scanner's Drawing state hysteretic. Without this, a player near
    // the configured edge flips Drawing every other EntityList pass and the
    // frame builder reports a readmit even though the target never left.
    const float maxDistOffSq = maxDistSq * 1.1025f; // 5% hold band

    // A newly admitted actor must get a first position before it is published.
    // Otherwise Drawing remains false, PositionRefreshPass skips it, and the
    // frame collector can never select it (the PlayerCache:3/EspDraw:0 bug).
    auto initializePlayerEntry = [&](PlayerCacheEntry& entry,
                                     uintptr_t actor,
                                     uintptr_t root,
                                     uintptr_t mesh) {
        entry.APawn = actor;
        entry.rootComponent = root;
        entry.actorMesh = mesh;
        const Vector3 pos = ResolvePlayerWorldPos(actor, root, mesh);
        if (!IsPlausibleWorldPos(pos)) {
            entry.positionInitialized = false;
            entry.Drawing = false;
            PlayerEspMiss::Global().Note(actor, entry.ActorName, -1.f,
                PlayerEspMiss::Reason::AdmitPosInvalid);
            return;
        }
        entry.WorldPos = pos;
        entry.lastWorldPos = pos;
        entry.positionInitialized = true;
        entry.positionSampleMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        const Vector3 delta = pos - cam.Location;
        entry.Distance = static_cast<float>(std::sqrt(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) / 100.0);
        entry.Drawing = entry.Distance >= 2.f && entry.Distance <= collectRange;
    };

    // Debug counter: resolves the reject candidate's world position (reject
    // paths only) and counts it when it sits inside the same range the frame
    // cull uses. Admitted players never reach this.
    auto countInRangeNotAdmitted = [&](uintptr_t actor, uintptr_t root) {
        const Vector3 pos = ResolvePlayerWorldPos(actor, root, 0);
        if (!IsPlausibleWorldPos(pos))
            return;
        const Vector3 delta = pos - cam.Location;
        const float dist = static_cast<float>(std::sqrt(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) / 100.0);
        if (dist >= 2.f && dist <= collectRange)
            ++dbgInRangeNotAdmitted;
    };

    std::unordered_map<uintptr_t, PlayerCacheEntry> localCache;
    {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        localCache = playerCache;
    }

    for (auto it = localCache.begin(); it != localCache.end(); )
    {
        const uintptr_t key = it->first;
        if (currentActorSet.contains(key)) {
            s_playerActorSetMisses.erase(key);
            ++it;
            continue;
        }

        if (currentActorSet.empty()) {
            ++it;
            continue;
        }

        auto& misses = s_playerActorSetMisses[key];
        if (++misses < kPlayerActorSetGracePasses) {
            ++it;
            continue;
        }

        ++dbgListEvict;
        PlayerEspMiss::Global().Note(key, it->second.ActorName,
            it->second.Distance, PlayerEspMiss::Reason::EvictListGone);
        s_playerActorSetMisses.erase(key);
        WorldScan::MissCounterClear(s_playerDistMisses, key);
        it = localCache.erase(it);
    }

    uintptr_t localPlayerState = 0;
    if (!TryResolvePawnPlayerState(sAcknowledgedPawn, localPlayerState) && sPlayerController)
        localPlayerState = Memory::read<uintptr_t>(
            sPlayerController + Offsets::AController_PlayerState);

    // DIAGNOSTIC (temporary): derive the PS->pawn link offset from the LOCAL
    // chain (golden reference). localPawn is known, so scanning its PlayerState
    // for the slot == localPawn yields the true back-link offset for remotes.
    {
        static auto s_lastChain = std::chrono::steady_clock::time_point{};
        const auto chainNow = std::chrono::steady_clock::now();
        if (chainNow - s_lastChain >= std::chrono::seconds(2)) {
            s_lastChain = chainNow;
            const uintptr_t pawn = sAcknowledgedPawn;
            const uintptr_t psFwd = pawn
                ? Memory::read_nocache<uintptr_t>(pawn + kPawnPlayerState) : 0;
            std::ptrdiff_t foundOff = -1;
            if (psFwd && Memory::IsValidPtrFast2(psFwd)) {
                for (std::ptrdiff_t off = 0x3F0; off <= 0x620; off += 8) {
                    if (Memory::read_nocache<uintptr_t>(psFwd + off) == pawn) {
                        foundOff = off; break;
                    }
                }
            }
            const uintptr_t pcPs = sPlayerController
                ? Memory::read_nocache<uintptr_t>(
                      sPlayerController + Offsets::AController_PlayerState) : 0;
            std::ptrdiff_t foundOffCtl = -1;
            if (pcPs && Memory::IsValidPtrFast2(pcPs)) {
                for (std::ptrdiff_t off = 0x3F0; off <= 0x620; off += 8) {
                    if (Memory::read_nocache<uintptr_t>(pcPs + off) == pawn) {
                        foundOffCtl = off; break;
                    }
                }
            }
            std::ostringstream oss;
            oss << "{\"ev\":\"local_chain\",\"pawn\":\"" << std::hex << pawn
               << "\",\"psFwd\":\"" << psFwd
               << "\",\"psCtl\":\"" << pcPs
               << "\",\"foundOff\":\"" << foundOff
               << "\",\"foundOffCtl\":\"" << foundOffCtl
               << "\",\"localPs\":\"" << localPlayerState
               << std::dec << "\"}\n";
            EntityDiagnostics::LogRaw(oss.str());
        }
    }

    // gsArray is diagnostic only — do not gate admission on GameState membership.
    // Wrong/stale GS PlayerArray pointers were zeroing PlayerCache (gsEvict).
    std::unordered_set<uintptr_t> gsPlayerStates;
    BuildGameStatePlayerStateAllowlist(*this, sGWorld, gsPlayerStates, dbgGsArray);

    // Primary admission: GameState PlayerArray → PS → pawn (proven by gsPawnHit).
    // Forward pawn→PS chase fails remotes; HealthInfo@PS+0x530 is wrong on PioneerPS.
    // DIAGNOSTIC (temporary): once per 2s dump every PS->pawn candidate slot so a
    // missing remote pawn is one log line away from its correct offset.
    const bool logPsProbe = [] {
        static auto s_last = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last >= std::chrono::seconds(2)) { s_last = now; return true; }
        return false;
    }();
    // Learn the pawn<->PlayerState slots from the local chain (ground truth)
    // while either one is unpinned. Cheap: one slot scan per direction.
    if (PinnedPawnPsOff() <= 0 || PinnedPsPawnSlot() <= 0) {
        LearnLocalPairOffsets(sAcknowledgedPawn, localPlayerState);
    }
    for (const uintptr_t ps : gsPlayerStates) {
        if (ps == localPlayerState)
            continue;
        if (!PlayerStateIsPioneerClass(ps)) {
            ++dbgNotPioneer;
            continue;
        }
        if (PlayerStateIsBot(ps)) {
            ++dbgGsBot;
            // A real player misclassified as a bot is exactly the "missing
            // ESP" failure mode — surface it with a lazy name read.
            PlayerEspMiss::Global().NoteLazy(ps, -1.f,
                PlayerEspMiss::Reason::AdmitBotClassified,
                [this, ps] { return GetPlayerName(ps); });
            continue;
        }

        const uintptr_t backPawn = ResolvePawnFromPlayerState(ps);
        if (!backPawn) {
            ++dbgGsPawnNull;
            if (logPsProbe) {
                const uintptr_t cur570 = Memory::read_nocache<uintptr_t>(ps + 0x570);
                const uintptr_t char568 = Memory::read_nocache<uintptr_t>(ps + 0x568);
                const uintptr_t cur560 = Memory::read_nocache<uintptr_t>(ps + 0x560);
                const uintptr_t char558 = Memory::read_nocache<uintptr_t>(ps + 0x558);
                const uintptr_t priv438 = Memory::read_nocache<uintptr_t>(ps + kPsPawnPrivate);
                std::ostringstream oss;
                oss << "{\"ev\":\"ps_probe\",\"ps\":\"" << std::hex << ps
                  << "\",\"cur570\":\"" << cur570
                  << "\",\"char568\":\"" << char568
                  << "\",\"cur560\":\"" << cur560
                  << "\",\"char558\":\"" << char558
                  << "\",\"priv438\":\"" << priv438
                  << std::dec << "\",\"name\":\"" << GetPlayerName(ps) << "\"}\n";
                EntityDiagnostics::LogRaw(oss.str());
            }
            PlayerEspMiss::Global().NoteLazy(ps, -1.f,
                PlayerEspMiss::Reason::AdmitPawnMissing,
                [this, ps] { return GetPlayerName(ps); });
            continue;
        }
        if (backPawn == sAcknowledgedPawn)
            continue;

        if (currentActorSet.contains(backPawn)) {
            ++dbgGsPawnHit;
            const uintptr_t fwdPs =
                Memory::read_nocache<uintptr_t>(backPawn + PawnStateOff());
            if (fwdPs == ps)
                ++dbgFwdMatch;
            else
                ++dbgFwdMismatch;
        } else {
            ++dbgGsPawnMiss;
            countInRangeNotAdmitted(backPawn, 0);
            PlayerEspMiss::Global().NoteLazy(backPawn, -1.f,
                PlayerEspMiss::Reason::AdmitPawnNotInLevels,
                [this, ps, backPawn] { return GetPlayerName(ps, backPawn); });
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
            countInRangeNotAdmitted(backPawn, 0);
            PlayerEspMiss::Global().NoteLazy(backPawn, -1.f,
                PlayerEspMiss::Reason::AdmitRootMissing,
                [this, ps, backPawn] { return GetPlayerName(ps, backPawn); });
            continue;
        }

        const uintptr_t mesh =
            Memory::read<uintptr_t>(backPawn + Offsets::USkeletalMeshComponent);
        const uintptr_t charMesh = ResolvePlayerSkeletalMesh(backPawn);
        if (!charMesh && !mesh) {
            ++dbgMeshSkip;
            countInRangeNotAdmitted(backPawn, root);
            PlayerEspMiss::Global().NoteLazy(backPawn, -1.f,
                PlayerEspMiss::Reason::AdmitMeshMissing,
                [this, ps, backPawn] { return GetPlayerName(ps, backPawn); });
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
                std::ostringstream nf;
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
                EntityDiagnostics::LogRaw(nf.str());
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
            initializePlayerEntry(it->second, backPawn, root,
                charMesh ? charMesh : mesh);
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
    s_playerAdmitQueue.Prune(
        [&](uint64_t a) { return currentActorSet.contains(a); });

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

    // DIAGNOSTIC (temporary): sample the admit band to see whether ANY actor
    // classifies as a player pawn (typeId mask) or owns a valid +0x3F0 PS.
    {
        static auto s_lastGate = std::chrono::steady_clock::time_point{};
        const auto gateNow = std::chrono::steady_clock::now();
        if (gateNow - s_lastGate >= std::chrono::seconds(2)) {
            s_lastGate = gateNow;
            int nSampled = 0, nValidPs = 0, nPlayer = 0;
            std::string samples;
            size_t i = 0;
            for (; i < admitIndex.size() && nSampled < 512; ++i) {
                const uintptr_t a = admitIndex[i];
                const uintptr_t ps = Memory::read_nocache<uintptr_t>(a + kPawnPlayerState);
                const uint32_t tid = ArcActorType::ReadActorTypeId(a);
                const uint32_t m = ArcActorType::MaskActorTypeId(tid);
                ++nSampled;
                if (ps && Memory::IsValidPtrFast2(ps)) ++nValidPs;
                if (ArcActorType::IsPlayerClassId(m)) ++nPlayer;
                if (nSampled <= 8) {
                    char buf[64];
            std::snprintf(buf, sizeof(buf), "(%08X,%s) ", m,
                (ps && Memory::IsValidPtrFast2(ps)) ? "ps" : "--");
            samples += buf;
        }
    }
    std::ostringstream gf;
    gf << "{\"ev\":\"gate_probe\",\"sampled\":" << nSampled
       << ",\"validPs\":" << nValidPs
       << ",\"player\":" << nPlayer
       << ",\"actors\":" << admitIndex.size()
       << ",\"typeOff\":" << ArcActorType::RuntimeActorTypeOffset()
       << ",\"samples\":\"" << samples << "\"}\n";
    EntityDiagnostics::LogRaw(gf.str());
}
}

    // DIAGNOSTIC (temporary): old-vs-new offset migration probe for the
    // dump-backed correctives (A1 character cluster, A2 PS misc, A4 inventory,
    // B6 SquadmateInfo). Logs BOTH slot values per chain so a regression is
    // immediately attributable: when the NEW slot resolves while the OLD one is
    // null/garbage, the migration did its job; when NEW is worse, revert that
    // constant and re-verify the rest.
    {
        static auto s_lastMig = std::chrono::steady_clock::time_point{};
        const auto migNow = std::chrono::steady_clock::now();
        if (migNow - s_lastMig >= std::chrono::seconds(2)) {
            s_lastMig = migNow;
            std::ostringstream mf;
            for (size_t i = 0; i < admitIndex.size() && i < 8; ++i) {
                const uintptr_t a = admitIndex[i];
                mf << "{\"ev\":\"migration_probe\",\"i\":" << i
                   << ",\"actor\":\"" << std::hex << a << "\"";
                const uintptr_t hcN = Memory::read_nocache<uintptr_t>(a + Offsets::HealthComponent);
                const uintptr_t hcO = Memory::read_nocache<uintptr_t>(a + 0xDC0);
                const uintptr_t invN = Memory::read_nocache<uintptr_t>(a + Offsets::InventoryComponent);
                const uintptr_t invO = Memory::read_nocache<uintptr_t>(a + 0xC80);
                const uintptr_t meshN = Memory::read_nocache<uintptr_t>(a + Offsets::EmbarkMesh);
                const uintptr_t meshO = Memory::read_nocache<uintptr_t>(a + 0x7B8);
                const uint8_t teamN = Memory::read_nocache<uint8_t>(a + Offsets::TeamID);
                const uint8_t teamO = Memory::read_nocache<uint8_t>(a + 0x7F2);
                int32_t cN = -1, cO = -1;
                if (invN && Memory::IsValidPtrFast2(invN)) {
                    cN = Memory::read_nocache<int32_t>(invN + Offsets::CurrentItemActors + 0x8);
                    cN = (cN >= 0 && cN <= 256) ? cN : -1;
                }
                if (invO && Memory::IsValidPtrFast2(invO)) {
                    cO = Memory::read_nocache<int32_t>(invO + 0x520 + 0x8);
                    cO = (cO >= 0 && cO <= 256) ? cO : -1;
                }
                mf << ",\"hc\":[\"" << hcO << "\",\"" << hcN << "\"]"
                   << ",\"inv\":[\"" << invO << "\",\"" << invN << "\"]"
                   << ",\"mesh\":[\"" << meshO << "\",\"" << meshN << "\"]"
                   << ",\"team\":[" << static_cast<int>(teamO) << ',' << static_cast<int>(teamN) << ']'
                   << ",\"currItems\":[" << cO << ',' << cN << ']'
                   << ",\"smInfo\":[\"" << std::hex
                   << Memory::read_nocache<uint64_t>(a + Offsets::SquadmateInfo0) << "\",\""
                   << Memory::read_nocache<uint64_t>(a + Offsets::SquadmateInfo1)
                   << "\"]";
                const uintptr_t ps = Memory::read_nocache<uintptr_t>(a + kPawnPlayerState);
                if (ps && Memory::IsValidPtrFast2(ps)) {
                    const uint8_t finN = Memory::read_nocache<uint8_t>(ps + 0x5B8);
                    const uint8_t finO = Memory::read_nocache<uint8_t>(ps + 0x5A8);
                    const uintptr_t achN = Memory::read_nocache<uintptr_t>(ps + 0x5C0);
                    const uintptr_t achO = Memory::read_nocache<uintptr_t>(ps + 0x5B0);
                    mf << ",\"ps\":\"" << std::hex << ps << "\""
                       << ",\"fin\":[" << static_cast<int>(finO & 1U) << ',' << static_cast<int>(finN & 1U) << ']'
                       << ",\"ach\":[\"" << achO << "\",\"" << achN << "\"]";
                }
                mf << std::dec << "}\n";
            }
            EntityDiagnostics::LogRaw(mf.str());
        }
    }

    // ---- PLAYER-lane streaming window (mirror of the bot-lane fix) ----------
    // A transient failure (no root / no mesh / undecrypted name yet) used to
    // spend the queue's try budget (~8 passes), get settled, then wait for the
    // 8-slice band ring — a spawn whose mesh streams slowly stayed invisible
    // for well over a minute. Instead: keep the actor queued and retried every
    // pass until it has been observed for kPlayerTransientWindowMs; after
    // that the band ring still re-checks it at ring rate.
    static constexpr uint64_t kPlayerTransientWindowMs = 45000;
    static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
        s_playerCandFirstSeen;
    auto playerTransientRetryOrSettle = [&](uintptr_t actor) {
        const auto fsIt = s_playerCandFirstSeen.find(actor);
        const uint64_t ageMs = (fsIt != s_playerCandFirstSeen.end())
            ? static_cast<uint64_t>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - fsIt->second)
                      .count())
            : 0ULL;
        if (ageMs < kPlayerTransientWindowMs) {
            s_playerAdmitQueue.NoteTry(actor); // keeps retry-first ordering
            return; // still streaming — stays queued, retried next pass
        }
        // Window expired without root+mesh+name ever appearing: the actor is
        // gone. Release it WITHOUT a memo (never proven a non-player), so the
        // band ring may re-check it at ring rate.
        s_playerAdmitQueue.Settle(actor);
        s_playerCandFirstSeen.erase(fsIt);
    };

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
        // A world-generation change invalidates every pointer; a plain ring
        // reset (N jump / array identity) must KEEP the pending lane — its
        // actors are still unclassified and would be lost again.
        if (s_playerAdmitRingGen != gen) {
            s_playerAdmitQueue.Clear();
            // World-gen change invalidates every actor pointer — drop the
            // first-seen stamps too, or a re-used address would inherit a
            // stale age and a fresh spawn would skip its streaming window.
            s_playerCandFirstSeen.clear();
        }
    }
    s_playerAdmitRingGen = gen;
    s_playerAdmitRingActorsPtr = sActors;
    s_playerAdmitRingActorCount = N;
    if (s_playerAdmitCycleStart.time_since_epoch().count() == 0)
        s_playerAdmitCycleStart = std::chrono::steady_clock::now();

    // One clock for the pass: the lane ages its rows against the same instant
    // its first probes are measured against (Core/BotAdmitQueue.hpp).
    const uint64_t playerAdmitNowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // P10 pending lane: every newly-seen actor joins the queue and STAYS until
    // a definitive outcome (admitted / proven non-player / memoized reject /
    // try budget spent / left the world). The one-shot diff lost actors that
    // missed their single priority window to a full ring sweep.
    for (uintptr_t actor : admitIndex) {
        if (s_playerAdmitQueue.Contains(actor))
            continue;
        if (localCache.contains(actor) || s_playerAdmitPrevActors.contains(actor))
            continue;
        int memoSink = 0;
        if (PlayerScanNegMemoHit(actor, memoSink))
            continue; // fresh definitive reject — the TTL memo owns the retry
        if (s_playerCandFirstSeen.size() > 16384)
            s_playerCandFirstSeen.clear();
        s_playerCandFirstSeen.try_emplace(
            actor, std::chrono::steady_clock::now());
        s_playerAdmitQueue.NoteNew(actor, playerAdmitNowMs);
    }

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
    // P10: priority rows come from the PENDING QUEUE (not a one-shot diff), and
    // they are collected FIRST so the band below knows which of its own rows the
    // lane already took this pass. The lane orders them by AGE, not by class or
    // array position (mirrors the bot lane, Core/BotAdmitQueue.hpp): a
    // never-probed actor past the first probe bound leads, never-probed work
    // keeps a reserved share of the pass, and retries take the rest.
    std::vector<PlayerProbeRow> prioRows;
    std::unordered_set<uintptr_t> laneSet;
    {
        AdmitQueue::PendingQueue::PassPlan plan;
        plan.maxRows = s_playerAdmitQueue.Cap();
        std::vector<uint64_t> prioActors;
        prioActors.reserve(plan.maxRows);
        s_playerAdmitQueue.CollectPrio(
            admitIndex.begin(), admitIndex.end(), playerAdmitNowMs, plan, prioActors);
        prioRows.reserve(prioActors.size());
        laneSet.reserve(prioActors.size());
        for (uint64_t actorU64 : prioActors) {
            const uintptr_t actor = static_cast<uintptr_t>(actorU64);
            laneSet.insert(actor);
            PlayerProbeRow row;
            row.actor = actor;
            prioRows.push_back(row);
        }
    }
    dbgAdmitPrioNew = static_cast<int>(prioRows.size());

    // The band keeps its old rule (same as the bot path, where the alternative
    // was measured): queued actors belong to the lane, so a band row is only for
    // actors the lane did not take AND is not holding. Rows the lane took this
    // pass are excluded, so nothing is probed twice in one pass.
    std::vector<PlayerProbeRow> bandRows;
    bandRows.reserve(sliceEnd - sliceBase);
    for (size_t i = sliceBase; i < sliceEnd; ++i) {
        const uintptr_t actor = admitIndex[i];
        if (localCache.contains(actor))
            continue;
        if (laneSet.count(actor) != 0)
            continue;
        if (s_playerAdmitQueue.Contains(actor))
            continue;
        PlayerProbeRow row;
        row.actor = actor;
        bandRows.push_back(row);
    }
    dbgAdmitSliceActors = static_cast<int>(bandRows.size());

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
    //
    // 2.2: one ceiling for the whole pass. The probe scatter / distance sort
    // and the process+admit loop used to run on two independent clocks (90 ms
    // probe + 40/100 ms process) so a single turn could hold the shared DMA
    // link for ~190 ms against a 90 ms ScanBudget. Everything now measures
    // against one start; the probe phase reserves the tail (70 ms) so the
    // admission ring still advances under load, and the hard ceiling is 110 ms.
    const auto passStart = std::chrono::steady_clock::now();
    constexpr auto kEntityPassCeiling = std::chrono::milliseconds(110);
    constexpr auto kEntityProbeCap = std::chrono::milliseconds(70);
    const auto elapsedMs = [&passStart]() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - passStart);
    };
    const auto passExpired = [&]() {
        return elapsedMs() >= kEntityPassCeiling;
    };
    const auto probeExpired = [&]() {
        return elapsedMs() >= kEntityProbeCap;
    };
    bool slicePartial = false;

    // Small scatter chunks + yield after each exec so the HIGHEST-priority
    // camera thread (8ms) can slip onto the DMA link between batches; a
    // single huge batch starves it and boxes jump when rot-lead skips.
    // RIVENTIDES: 48-row chunks keep every execute short on a saturated bus
    // (16K actors) so the process loop still runs this pass.
    constexpr size_t kPlayerAdmitChunk = 48;
    size_t base = 0;
    for (; base < probeRows.size(); base += kPlayerAdmitChunk) {
        if (probeExpired()) { slicePartial = true; break; }
        const size_t chunkEnd = (std::min)(base + kPlayerAdmitChunk, probeRows.size());
        ScatterSession scatter;
        if (!scatter.isValid())
            break;
        bool prepOk = true;
        for (size_t i = base; i < chunkEnd; ++i) {
            PlayerProbeRow& r = probeRows[i];
            prepOk = scatter.prepare(r.actor + PawnStateOff(), r.ps) && prepOk;
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
                if (probeExpired())
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

    // 2.2: the process+admit loop shares the single pass ceiling instead of a
    // second, independent micro-budget (the two used to stack to ~190 ms). The
    // probe phase reserves the tail, so scattered rows still get processed and
    // the admission ring keeps advancing under DMA load.
    size_t processEnd = 0;
    for (size_t ri = 0; ri < scatteredEnd; ++ri) {
        const PlayerProbeRow& row = probeRows[ri];
        if (passExpired()) { slicePartial = true; break; }
        processEnd = ri + 1;
        // First-probe stopwatch, same rule as the bot lane: a row that reached
        // screening was probed, and only queued actors are ever screened from
        // the lane (band rows skip them).
        if (s_playerAdmitQueue.Contains(row.actor))
            s_playerAdmitQueue.NoteProbed(row.actor, playerAdmitNowMs);
        if (PlayerScanNegMemoHit(row.actor, dbgAdmitMemoSkip)) {
            // Fresh definitive reject — its TTL memo owns the retry.
            s_playerAdmitQueue.Settle(row.actor);
            continue;
        }
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
                // next pass via the pending lane (bounded tries) so late
                // spawns are never permanently skipped.
                if (!fn.empty() || !classFn.empty()) {
                    PlayerScanNegMemoize(actor);
                    s_playerAdmitQueue.Settle(actor);
                } else {
                    playerTransientRetryOrSettle(actor); // streaming window, not try-budget
                }
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
                s_playerAdmitQueue.Settle(actor);
                continue;
            }

            if (PlayerStateIsBot(playerState)) {
                ++dbgGsBot;
                PlayerEspMiss::Global().NoteLazy(actor, -1.f,
                    PlayerEspMiss::Reason::AdmitBotClassified,
                    [this, playerState, actor] {
                        return GetPlayerName(playerState, actor);
                    });
                // Bots never become players — memoize so their PS chase does
                // not repeat serially every ring pass (TTL still re-checks).
                PlayerScanNegMemoize(actor);
                s_playerAdmitQueue.Settle(actor);
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
                countInRangeNotAdmitted(actor, 0);
                PlayerEspMiss::Global().NoteLazy(actor, -1.f,
                    PlayerEspMiss::Reason::AdmitRootMissing,
                    [this, playerState, actor] {
                        return GetPlayerName(playerState, actor);
                    });
                playerTransientRetryOrSettle(actor); // streaming window, not try-budget // spawn not ready
                continue;
            }

            const uintptr_t mesh =
                Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
            const uintptr_t charMesh = ResolvePlayerSkeletalMesh(actor);
            if (!charMesh && !mesh) {
                ++dbgMeshSkip;
                countInRangeNotAdmitted(actor, root);
                PlayerEspMiss::Global().NoteLazy(actor, -1.f,
                    PlayerEspMiss::Reason::AdmitMeshMissing,
                    [this, playerState, actor] {
                        return GetPlayerName(playerState, actor);
                    });
                playerTransientRetryOrSettle(actor); // streaming window, not try-budget // mesh not ready
                continue;
            }

            const std::string playerName = GetPlayerName(playerState, actor);
            auto [it, inserted] = localCache.emplace(
                actor,
                PlayerCacheEntry(
                    playerName.c_str(), root, actor, charMesh ? charMesh : mesh));
            if (inserted) {
                it->second.actorState = playerState;
                initializePlayerEntry(it->second, actor, root,
                    charMesh ? charMesh : mesh);
                ++dbgAdmitted;
            }
            s_playerAdmitQueue.Settle(actor); // in the cache — retain owns it now
            continue;
        }

        ++dbgPsSkip;

        // U8: BP_PioneerCharacter_C class FName backup when PS chase missed.
        const std::string classFname = GetActorClassFName(actor);
        if (classFname.empty()) {
            // Undecrypted — retry via the pending lane (bounded tries), no
            // memoize.
            playerTransientRetryOrSettle(actor); // streaming window, not try-budget
            continue;
        }

        std::string cl = classFname;
        for (char& c : cl)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (cl.find("pioneercharacter") == std::string::npos
            && cl.find("bp_pioneercharacter") == std::string::npos) {
            PlayerScanNegMemoize(actor);
            s_playerAdmitQueue.Settle(actor);
            continue;
        }

        playerState = 0;
        viaActorType = false;
        TryResolvePlayerStateAny(actor, playerState, viaActorType);
        if (localPlayerState && playerState == localPlayerState) {
            s_playerAdmitQueue.Settle(actor);
            continue;
        }
        if (!playerState || !PlayerStateIsPioneerClass(playerState) || PlayerStateIsBot(playerState)) {
            PlayerScanNegMemoize(actor);
            s_playerAdmitQueue.Settle(actor);
            continue;
        }

        const uintptr_t root =
            Memory::read<uintptr_t>(actor + Offsets::RootComponent);
        if (!root) {
            countInRangeNotAdmitted(actor, 0);
            PlayerEspMiss::Global().Note(actor, std::string(), -1.f,
                PlayerEspMiss::Reason::AdmitRootMissing);
            playerTransientRetryOrSettle(actor); // streaming window, not try-budget // spawn not ready
            continue;
        }
        const uintptr_t mesh =
            Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
        const uintptr_t charMesh = ResolvePlayerSkeletalMesh(actor);
        if (!charMesh && !mesh) {
            countInRangeNotAdmitted(actor, root);
            PlayerEspMiss::Global().Note(actor, std::string(), -1.f,
                PlayerEspMiss::Reason::AdmitMeshMissing);
            playerTransientRetryOrSettle(actor); // streaming window, not try-budget // mesh not ready
            continue;
        }

        const std::string playerName =
            playerState ? GetPlayerName(playerState, actor) : std::string("Player");
        auto [it, inserted] = localCache.emplace(
            actor,
            PlayerCacheEntry(
                playerName.c_str(), root, actor, charMesh ? charMesh : mesh));
        if (inserted) {
            it->second.actorState = playerState;
            initializePlayerEntry(it->second, actor, root,
                charMesh ? charMesh : mesh);
            ++dbgAdmitted;
            ++dbgActorTypeAdmit;
        }
        s_playerAdmitQueue.Settle(actor); // in the cache — retain owns it now
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

    // The evidence that survives a bad byte read: real UEmbarkSquad membership.
    // This is the same pointer the squad-tag pass reads for enemies, from this
    // pawn's own player state, and it costs one read per pass. Only read when
    // hide_allies actually asks a question, so the feature is free when off.
    uintptr_t mySquadPtr = 0;
    if (var::hide_allies && localPlayerState
        && Memory::IsValidPtrFast2(localPlayerState)) {
        const uintptr_t mySquad = Memory::read_nocache<uintptr_t>(
            localPlayerState + Offsets::PS_Squad);
        if (IsValidPointer(mySquad))
            mySquadPtr = mySquad;
    }

    // Distinct team bytes seen on the candidates this pass (bounded, players
    // only), so the probe can show whether any teammate even shares this pawn's
    // team value — the difference between "the byte is wrong" and "the byte is
    // right and something else is drawing them".
    struct TeamSeen { uint8_t id; int n; };
    TeamSeen teamsSeen[8]{};
    int teamsSeenCount = 0;

    // Prefer mesh CompToWorld (bot parity); root fallback — root alone stays frozen on remotes.
    for (auto it = localCache.begin(); it != localCache.end(); ++it) {
        const uintptr_t key = it->first;
        if (key == sAcknowledgedPawn)
            continue;
        if (passExpired())
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
        if (passExpired())
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
                PlayerEspMiss::Global().Note(key, actor.ActorName,
                    actor.Distance, PlayerEspMiss::Reason::EvictRootStale);
                it = localCache.erase(it);
            } else {
                actor.Drawing = false;
                PlayerEspMiss::Global().Note(key, actor.ActorName,
                    actor.Distance, PlayerEspMiss::Reason::EvictRootStale);
                ++it;
            }
            continue;
        }
        WorldScan::MissCounterClear(s_playerRootMisses, key);
        actor.rootComponent = freshRoot;

        const bool wantFacingYaw =
            var::show_look_arrows || (var::show_radar && var::radar_ally_arrows);
        actor.facingYaw = wantFacingYaw
            ? static_cast<float>(Memory::read<double>(
                freshRoot + Offsets::RelativeRotation + 8))
            : 0.f;

        // Look direction (feature #5): where they AIM is their controller's
        // ControlRotation yaw; body facing stays the fallback. The camera
        // manager's view clamps gate free-cam garbage rotations. No toggle means
        // no controller/PCM reads on the player worker.
        actor.hasAimYaw = false;
        actor.hasAimPitch = false;
        if (var::show_look_arrows) {
            const uintptr_t ctrl =
                Memory::read<uintptr_t>(key + Offsets::Pawn_Controller);
            if (ctrl && IsValidPointer(ctrl)) {
                const double aimPitch =
                    Memory::read_nocache<double>(ctrl + Offsets::ControlRotation);
                const double aimYaw = Memory::read_nocache<double>(
                    ctrl + Offsets::ControlRotation + 0x8);
                double clampMin = 0.0, clampMax = 0.0;
                if (PlayerCameraManager && IsValidPointer(PlayerCameraManager)) {
                    clampMin = static_cast<double>(Memory::read_nocache<float>(
                        PlayerCameraManager + Offsets::PCM_ViewPitchMin));
                    clampMax = static_cast<double>(Memory::read_nocache<float>(
                        PlayerCameraManager + Offsets::PCM_ViewPitchMax));
                }
                if (std::isfinite(aimYaw)
                    && LookArrow::AimPitchValid(aimPitch, clampMin, clampMax)) {
                    actor.aimYawDeg = static_cast<float>(aimYaw);
                    actor.hasAimYaw = true;
                    actor.aimPitchDeg = static_cast<float>(aimPitch);
                    actor.hasAimPitch = true;
                }
            }
            // B3: when the controller path fails, the server-replicated
            // RemoteViewPitch (uint8 on the Pawn, 0..255 → -180..180) is the
            // remote pawn's actual aim pitch. Store it as the aim signal; the
            // current 2D arrow renders yaw only, so it tightens the aim
            // validity (via hasAimPitch) rather than the arrow geometry.
            if (!actor.hasAimPitch) {
                const uint8_t rvp =
                    Memory::read_nocache<uint8_t>(key + Offsets::Pawn_RemoteViewPitch);
                if (rvp > 16 && rvp < 240) {
                    double pitch = static_cast<double>(rvp) * (360.0 / 255.0);
                    if (pitch > 180.0)
                        pitch -= 360.0;
                    actor.aimPitchDeg = static_cast<float>(pitch);
                    actor.hasAimPitch = true;
                }
            }
            if (!actor.hasAimYaw && std::isfinite(actor.facingYaw)) {
                actor.aimYawDeg = actor.facingYaw;
                actor.hasAimYaw = true;
            }
        }

        const uint8_t enemyTeamId = Memory::read<uint8_t>(key + Offsets::TeamID);
        actor.enemyTeamId = enemyTeamId;
        {
            int seen = -1;
            for (int i = 0; i < teamsSeenCount; ++i) {
                if (teamsSeen[i].id == enemyTeamId) {
                    seen = i;
                    break;
                }
            }
            if (seen >= 0)
                ++teamsSeen[seen].n;
            else if (teamsSeenCount < 8) {
                teamsSeen[teamsSeenCount].id = enemyTeamId;
                teamsSeen[teamsSeenCount].n = 1;
                ++teamsSeenCount;
            }
        }

        // Two signals, in priority order. TeamID equality is the cheap one, but
        // it fails closed: an unreadable or garbage byte makes every teammate
        // look like an enemy. The real squad pointer is consulted whenever the
        // byte comparison did not already settle the question, and the branch
        // that decided is counted so the probe can name it.
        const bool teamAlly =
            (myTeamValid && enemyTeamId != 255 && enemyTeamId != 0 && myTeamId == enemyTeamId);
        bool squadAlly = false;
        if (!teamAlly && mySquadPtr != 0) {
            uintptr_t allyPs = actor.actorState;
            bool allyViaActorType = false;
            if (!allyPs || !Memory::IsValidPtrFast2(allyPs))
                TryResolvePlayerStateAny(key, allyPs, allyViaActorType);
            if (allyPs && Memory::IsValidPtrFast2(allyPs)) {
                const uintptr_t squad = Memory::read_nocache<uintptr_t>(
                    allyPs + Offsets::PS_Squad);
                squadAlly = IsValidPointer(squad) && squad == mySquadPtr;
            }
        }
        actor.isAlly = teamAlly || squadAlly;
        if (teamAlly)
            ++dbgAllyByTeam;
        else if (squadAlly)
            ++dbgAllyBySquad;
        if (var::hide_allies && !myTeamValid)
            ++dbgAllyMyTeamInvalid;
        if (actor.isAlly && var::hide_allies) {
            ++dbgTeamEvict;
            PlayerEspMiss::Global().Note(key, actor.ActorName,
                actor.Distance, PlayerEspMiss::Reason::EvictAllyHidden);
            WorldScan::MissCounterClear(s_playerDistMisses, key);
            it = localCache.erase(it);
            continue;
        }

        uintptr_t playerState = actor.actorState;
        if (!playerState || !Memory::IsValidPtrFast2(playerState)) {
            bool viaActorType = false;
            if (!TryResolvePlayerStateAny(key, playerState, viaActorType)) {
                ++dbgPsEvict;
                PlayerEspMiss::Global().Note(key, actor.ActorName,
                    actor.Distance, PlayerEspMiss::Reason::EvictPsLost);
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
            PlayerEspMiss::Global().Note(key, actor.ActorName,
                actor.Distance, PlayerEspMiss::Reason::EvictBotReclassified);
            WorldScan::MissCounterClear(s_playerDistMisses, key);
            it = localCache.erase(it);
            continue;
        }

        // Diagnostic only — do not prune on GS allowlist (see admit path).
        if (!gsPlayerStates.empty() && !gsPlayerStates.contains(playerState))
            ++dbgGsEvict;

        actor.actorState = playerState;

        // Identity (feature #6): permanent SteamID64 + real UEmbarkSquad*
        // membership (the TeamID grouping stays the fallback).
        bool steamPathResolved = false;
        actor.steamId64 = (var::show_steam_ids || UpgradeOptions::platformBadges)
            ? ReadPlayerSteamId(playerState, &steamPathResolved) : 0;
        actor.steamIdResolved = (var::show_steam_ids || UpgradeOptions::platformBadges) && steamPathResolved;
        // Status tags ([Bot]/[Spec]/[Done]) ride the identity toggle so the
        // PioneerPlayerState block reads cost zero DMA while it is off.
        actor.statusFlags =
            var::show_steam_ids ? ReadPlayerStatusTags(playerState) : 0;
        actor.squadPtr = var::show_squad_idx
            ? Memory::read_nocache<uintptr_t>(playerState + Offsets::PS_Squad)
            : 0;
        if (!IsValidPointer(actor.squadPtr))
            actor.squadPtr = 0;
        actor.squadResolved = var::show_squad_idx && actor.squadPtr != 0;
        // B5: real squad slot (1..8) when the squad resolves; the pointer-rank
        // numbering remains the grouping fallback in the squad-tag pass.
        actor.squadSlotIdx = 0;
        if (actor.squadResolved) {
            const int32_t slot = Memory::read_nocache<int32_t>(
                playerState + Offsets::EPS_SquadSlotIndex);
            if (slot >= 1 && slot <= 8)
                actor.squadSlotIdx = static_cast<uint8_t>(slot);
        }

        const std::string liveName = GetPlayerName(playerState, key);
        if (!liveName.empty())
            actor.ActorName = liveName;

        const uintptr_t charMesh = ResolvePlayerSkeletalMesh(key);
        if (charMesh)
            actor.actorMesh = charMesh;

        actor.WorldPos = ResolvePlayerWorldPos(key, freshRoot, actor.actorMesh);
        if (IsPlausibleWorldPos(actor.WorldPos)) {
            actor.positionInitialized = true;
            actor.positionSampleMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        if (!IsPlausibleWorldPos(actor.WorldPos))
        {
            ++dbgPosEvict;
            PlayerEspMiss::Global().Note(key, actor.ActorName,
                actor.Distance, PlayerEspMiss::Reason::EvictPosInvalid);
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

        if (distanceSq > maxDistSq
            && !(actor.Drawing && distanceSq <= maxDistOffSq)) {
            ++dbgDistSkip;
            PlayerEspMiss::Global().Note(key, actor.ActorName,
                actor.Distance, PlayerEspMiss::Reason::DrawClearedDistance);
            // Keep a valid entry alive through the 5% edge band. Only a
            // sustained move beyond that band clears Drawing.
            actor.Drawing = false;
            WorldScan::MissCounterClear(s_playerDistMisses, key);
            ++it;
            continue;
        }
        WorldScan::MissCounterClear(s_playerDistMisses, key);

        const double liveHealth = get_health(key);
        const double liveMaxHealth = get_maxhealth(key);
        // A valid bar requires both values. Do not leave a stale/zero max in
        // the frame when the current-health read succeeds alone.
        const bool healthPairOk =
            std::isfinite(liveHealth) && std::isfinite(liveMaxHealth)
            && liveHealth >= 0.0 && liveHealth < 100000.0
            && liveMaxHealth > 0.0 && liveMaxHealth < 100000.0
            && liveHealth <= liveMaxHealth + 250.0;
        actor.healthResolved = healthPairOk;
        if (healthPairOk) {
            actor.health = static_cast<float>(liveHealth);
            actor.maxhealth = static_cast<float>(liveMaxHealth);
        }
        {
            const auto nowHp = std::chrono::steady_clock::now();
            if (healthPairOk && liveHealth <= 0.0) {
                const auto zeroIt = s_playerZeroHealthSince.try_emplace(key, nowHp).first;
                actor.bIsDead = nowHp - zeroIt->second >= kPlayerDeadZeroHealthMs;
            } else {
                s_playerZeroHealthSince.erase(key);
                actor.bIsDead = false;
            }
        }
        const bool wantArmor = var::health || var::show_armor_line;
        const double liveArmor = wantArmor ? get_armor(key) : 0.0;
        const double liveMaxArmor = wantArmor ? get_maxarmor(key) : 0.0;
        // Unknown health is not evidence that a live player should be removed.
        // Keep the cache entry alive; the custom bar simply receives the last
        // known pair until a later scan resolves a fresh one.
        const bool armorPairOk =
            std::isfinite(liveArmor) && std::isfinite(liveMaxArmor)
            && liveArmor >= 0.0 && liveArmor < 100000.0
            && liveMaxArmor > 0.0 && liveMaxArmor < 100000.0
            && liveArmor <= liveMaxArmor + 250.0;
        actor.armorResolved = armorPairOk;
        if (std::isfinite(liveArmor) && liveArmor >= 0.0 && liveArmor < 100000.0)
            actor.shield = static_cast<float>(liveArmor);
        if (std::isfinite(liveMaxArmor) && liveMaxArmor > 0.0 && liveMaxArmor < 100000.0)
            actor.maxshield = static_cast<float>(liveMaxArmor);

        // DBNO badge + revive countdown (feature #4) + activity feed (#10).
        // When every consumer is off, preserve no stale DBNO presentation state
        // and skip the PlayerState/HealthComponent/timer DMA chain.
        const bool wasDbno = actor.isDbno;
        if (var::show_dbno_badge || var::show_activity_feed || UpgradeOptions::playerStates) {
            bool dbnoPathResolved = false;
            actor.isDbno = ReadPlayerDbnoState(
                playerState, actor.hasBrokenArmor, &dbnoPathResolved);
            actor.dbnoResolved = dbnoPathResolved;
            if (actor.isDbno != wasDbno) {
                char feedText[80];
                snprintf(feedText, sizeof(feedText), "%.40s %s",
                    actor.ActorName.empty() ? "A raider" : actor.ActorName.c_str(),
                    actor.isDbno ? "is DOWN" : "is back up");
                PushActivity(key, feedText);
            }
        } else {
            actor.isDbno = false;
            actor.hasBrokenArmor = false;
            actor.dbnoResolved = false;
        }
        if (var::show_dbno_badge) {
            float timerElapsed = 0.f, timerTotal = 0.f;
            if (actor.isDbno && ReadReviveTimer(key, timerElapsed, timerTotal)) {
                actor.reviveTotalS = timerTotal;
                actor.reviveRemainS =
                    ReviveBadge::RemainSeconds(timerElapsed, timerTotal);
            } else {
                actor.reviveRemainS = -1.f;
                actor.reviveTotalS = -1.f;
            }
        } else {
            actor.reviveRemainS = -1.f;
            actor.reviveTotalS = -1.f;
        }

        // Do not evict or suppress a live player because one health read is
        // zero/NaN. Position and distance are the ESP liveness gates; health
        // only controls the custom bar's fill.
        WorldScan::MissCounterClear(s_playerHealthZeroMisses, key);


        // Read weapon system from InventoryComponent only when a presentation
        // feature consumes it.
        std::string invWeapon, invStowed0, invStowed1, invArmorName;
        int invWq = -1, invSq0 = -1, invSq1 = -1, invClip = 0;
        int invArmorTier = -1;
        float invArmorPlates = 0.f, invArmorPerPlate = 0.f;
        bool inventoryResolved = false;
        const bool wantInventory = var::show_weapon || var::show_armor_line
            || var::show_player_kit;
        if (wantInventory) {
            ReadPlayerInventory(key, invWeapon, invWq, invClip, invStowed0, invSq0, invStowed1, invSq1,
                invArmorPlates, invArmorPerPlate, invArmorTier, invArmorName,
                &inventoryResolved);
        }
        actor.inventoryResolved = inventoryResolved;
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
        actor.armorTier = invArmorTier;
        actor.armorName = invArmorName;

        // Full kit readout (phase 2) - zero DMA while its toggle is off.
        if (var::show_player_kit) {
            LoadoutFormat::KitParts kit;
            ReadPlayerKit(key, kit);
            actor.kitTool = std::move(kit.tool);
            actor.kitPouch = std::move(kit.pouch);
            actor.kitPouchRarity = kit.pouchRarity;
            actor.kitBeltSlots = kit.beltSlots;
            actor.kitPackSlots = kit.packSlots;
        } else {
            actor.kitTool.clear();
            actor.kitPouch.clear();
            actor.kitPouchRarity = -1;
            actor.kitBeltSlots = -1;
            actor.kitPackSlots = -1;
        }

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

        actor.isVisible = EmbreeVis::PeekVisHold(
            actor.APawn, actor.visShown, actor.visPendingSinceMs, true);
        if (actor.isVisible) {
            actor.visibleStampMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
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
        PlayerPipelineStats diag{};
        diag.scanned = dbgScanned;
        diag.gsCandidates = static_cast<int>(gsPlayerStates.size());
        diag.gsPawnHit = dbgGsPawnHit;
        diag.gsPawnMiss = dbgGsPawnMiss;
        diag.rootFail = dbgRootSkip + dbgRootStale;
        diag.meshFail = dbgMeshSkip;
        diag.playerStateFail = dbgPsSkip + dbgPsEvict;
        diag.botClassified = dbgGsBot;
        diag.admitted = dbgAdmitted;
        for (const auto& [key, entry] : localCache) {
            (void)key;
            if (entry.positionInitialized)
                ++diag.initialized;
            else
                ++diag.uninitialized;
            if (entry.Drawing)
                ++diag.drawing;
        }
        std::unique_lock<std::shared_mutex> lock(m_playerDiagMutex);
        m_playerDiag = diag;
    }

    {
        std::unique_lock<std::shared_mutex> lock(m_playerCacheMutex);
        playerCache = std::move(localCache);
    }

    // Squad indices (1-based, sorted ascending so they are stable per session).
    // Real UEmbarkSquad* membership wins (feature #6: real squad rosters); the
    // old TeamID mapping stays as the fallback when no squad pointer resolved.
    {
        std::vector<uintptr_t> uniqueSquads;
        std::vector<uint8_t> uniqueTeams;
        for (const auto& [key, entry] : playerCache) {
            if (entry.isAlly)
                continue;
            if (entry.squadPtr)
                uniqueSquads.push_back(entry.squadPtr);
            else if (entry.enemyTeamId != 255 && entry.enemyTeamId != 0)
                uniqueTeams.push_back(entry.enemyTeamId);
        }
        std::sort(uniqueSquads.begin(), uniqueSquads.end());
        uniqueSquads.erase(
            std::unique(uniqueSquads.begin(), uniqueSquads.end()), uniqueSquads.end());
        std::sort(uniqueTeams.begin(), uniqueTeams.end());
        uniqueTeams.erase(
            std::unique(uniqueTeams.begin(), uniqueTeams.end()), uniqueTeams.end());
        for (auto& [key, entry] : playerCache) {
            if (entry.isAlly)
                continue;
            if (entry.squadPtr) {
                if (entry.squadSlotIdx >= 1 && entry.squadSlotIdx <= 8) {
                    // B5: the real slot number IS the squad label (all members
                    // of a squad share their slot's label).
                    entry.squadIdx = entry.squadSlotIdx;
                } else {
                    auto squadIt = std::lower_bound(
                        uniqueSquads.begin(), uniqueSquads.end(), entry.squadPtr);
                    entry.squadIdx = static_cast<uint8_t>(
                        std::distance(uniqueSquads.begin(), squadIt) + 1);
                }
            } else if (entry.enemyTeamId != 255 && entry.enemyTeamId != 0) {
                auto it = std::find(
                    uniqueTeams.begin(), uniqueTeams.end(), entry.enemyTeamId);
                if (it != uniqueTeams.end())
                    entry.squadIdx = static_cast<uint8_t>(
                        std::distance(uniqueTeams.begin(), it) + 1);
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
            // fp* is cumulative since the last world-gen clear (a cold load's
            // flood lives there); fw* is the last kProbeWindowSamples probes and
            // is the one that says whether a new actor is looked at inside the
            // bound NOW.
            const AdmitQueue::PendingQueue::FirstProbeStats fpStats =
                s_playerAdmitQueue.ProbeStats();
            char pbuf[1152]{};
            snprintf(pbuf, sizeof(pbuf),
                "{\"scanned\":%d,\"admitted\":%d,\"cache\":%zu,\"listEvict\":%d,"
                "\"teamEvict\":%d,\"psEvict\":%d,\"posEvict\":%d,\"distSkip\":%d,"
                "\"ghostEvict\":%d,\"rootSkip\":%d,\"meshSkip\":%d,\"psSkip\":%d,"
                "\"healthSkip\":%d,\"gsBot\":%d,\"gsPawnNull\":%d,\"gsEvict\":%d,"
                "\"actorTypeAdmit\":%d,\"prioNew\":%d,\"checked\":%d,"
                "\"memoSkip\":%d,\"slice\":%zu,\"cycleMs\":%d,\"ringResets\":%d,"
                "\"admitN\":%zu,\"lastDelta\":%zu,"
                "\"fpN\":%llu,\"fpOver\":%llu,\"fpMeanMs\":%llu,\"fpMaxMs\":%llu,"
                "\"fwN\":%llu,\"fwOver\":%llu,\"fwMeanMs\":%llu,\"fwMaxMs\":%llu,"
                "\"pend\":%zu,\"tries\":%zu,\"inRangeMiss\":%d}",
                dbgScanned, dbgAdmitted, playerCache.size(), dbgListEvict,
                dbgTeamEvict, dbgPsEvict, dbgPosEvict, dbgDistSkip,
                dbgGhostEvict, dbgRootSkip, dbgMeshSkip, dbgPsSkip,
                dbgHealthSkip, dbgGsBot, dbgGsPawnNull, dbgGsEvict,
                dbgActorTypeAdmit, dbgAdmitPrioNew, dbgAdmitChecked,
                dbgAdmitMemoSkip, dbgAdmitSlice, s_playerAdmitLastCycleMs,
                s_playerAdmitRingResets, dbgAdmitN, s_playerAdmitLastDelta,
                static_cast<unsigned long long>(fpStats.n),
                static_cast<unsigned long long>(fpStats.overBound),
                static_cast<unsigned long long>(fpStats.n ? fpStats.sumMs / fpStats.n : 0),
                static_cast<unsigned long long>(fpStats.maxMs),
                static_cast<unsigned long long>(fpStats.winN),
                static_cast<unsigned long long>(fpStats.winOverBound),
                static_cast<unsigned long long>(
                    fpStats.winN ? fpStats.winSumMs / fpStats.winN : 0),
                static_cast<unsigned long long>(fpStats.winMaxMs),
                s_playerAdmitQueue.Size(), s_playerAdmitQueue.TrySize(),
                dbgInRangeNotAdmitted);
            EntityDiagnostics::LogScan("players", pbuf);
            // player_miss_summary: WHO is missing and WHICH gate dropped them
            // (PlayerEspMissLog) — the per-actor answer to "player ran up, no
            // ESP" that the aggregate player_admit_stats cannot give.
            std::ostringstream mf;
            mf << PlayerEspMiss::Global().FormatSummaryLine(
                PlayerEspMiss::NowMs(), 16)
               << "\n";
            EntityDiagnostics::LogRaw(mf.str());

            // player_admit_stats is a throttled (2s) verification tap — it must
            // reach the real log (kArcDebugLogPath is NUL by design).
            std::ostringstream f;
            const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            f << "{\"sessionId\":\"c190fb\",\"runId\":\"post-fix\","
              << "\"hypothesisId\":\"P9\",\"location\":\"EntityList.cpp:EntityList\","
              << "\"message\":\"player_admit_stats\",\"data\":" << pbuf
              << ",\"timestamp\":" << ts << "}\n";
            EntityDiagnostics::LogRaw(f.str());

            // ally_probe: the hide_allies decision itself. A teammate drawn as
            // an enemy has no symptom other than the box, and the field that
            // decides it is one byte read from one location — so it is reported
            // with its validity, the branch that classified each ally, and the
            // team bytes actually seen on candidates.
            {
                std::ostringstream ap;
                const auto allyTs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                ap << "{\"sessionId\":\"c190fb\",\"runId\":\"ally\","
                   << "\"hypothesisId\":\"A1\",\"location\":\"EntityList.cpp:EntityList\","
                   << "\"message\":\"ally_probe\",\"data\":{\"hideAllies\":";
                ap << (var::hide_allies ? 1 : 0)
                   << ",\"myTeamId\":" << static_cast<int>(myTeamId)
                   << ",\"myTeamValid\":" << (myTeamValid ? 1 : 0)
                   << ",\"mySquadResolved\":" << (mySquadPtr != 0 ? 1 : 0)
                   << ",\"candidates\":" << dbgPreAdmit
                   << ",\"teamEvict\":" << dbgTeamEvict
                   << ",\"allyByTeam\":" << dbgAllyByTeam
                   << ",\"allyBySquad\":" << dbgAllyBySquad
                   << ",\"myTeamInvalid\":" << dbgAllyMyTeamInvalid
                   << ",\"teamsSeen\":[";
                for (int i = 0; i < teamsSeenCount; ++i) {
                    if (i)
                        ap << ',';
                    ap << "{\"id\":" << static_cast<int>(teamsSeen[i].id)
                       << ",\"n\":" << teamsSeen[i].n << '}';
                }
                ap << "]},\"timestamp\":" << allyTs << "}\n";
                EntityDiagnostics::LogRaw(ap.str());
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
                << " notPioneer=" << dbgNotPioneer
                << " inRangeMiss=" << dbgInRangeNotAdmitted
                << " slice=" << dbgAdmitSlice << "/" << kPlayerAdmitSlices
                << " sliceActors=" << dbgAdmitSliceActors
                << " prioNew=" << dbgAdmitPrioNew
                << " pend=" << s_playerAdmitQueue.Size()
                << " checked=" << dbgAdmitChecked
                << " fnameChecked=" << dbgAdmitFnameChecked
                << " scatterExecs=" << dbgAdmitScatterExecs
                << " memoSkip=" << dbgAdmitMemoSkip
                << " memoSize=" << s_playerScanNeg.size()
                << " cycleMs=" << s_playerAdmitLastCycleMs
                << " cover=0x" << std::hex << s_playerAdmitCoveredMask << std::dec
                << " ringResets=" << s_playerAdmitRingResets
                << std::endl;
            std::cout << "[debugPlayerMiss] "
                << PlayerEspMiss::Global().FormatConsoleSummary(
                    PlayerEspMiss::NowMs(), 8)
                << std::endl;
        }
    }
}
