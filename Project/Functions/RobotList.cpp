#include "../Core/Engine.h"
#include "../Interface/Utils/UpgradeOptions.hpp"
#include "../Core/AgentLog.h"
#include "../Core/ActorType.h"
#include "../Core/EntityDiagnostics.hpp"
#include "../Core/FeaturePolicy.hpp"
#include "../Core/AssetNames.h"
#include "../Core/BoneRoster.hpp"
#include "../Core/BotAdmitQueue.hpp"
#include "../Core/BotEspExpiry.hpp"
#include "../Core/BotPosSanity.hpp"
#include "../Core/BotTypes.h"
#include "../Core/IntervalTimer.h"
#include "../Core/ReadBudget.hpp"
#include "../Core/EmbreeVisCheck.hpp"
#include "../Core/WorldItemCategory.h"
#include "RobotList.h"
#include "WorldScanCommon.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

std::string ResolveBotTypeLabel(uintptr_t actor, const std::string& fname);

namespace {

bool IsWorldEspLabel(const std::string& name)
{
    return name == "Loot Item" || name == "World Item" || name == "Corpse"
        || name == "Raider stock" || name == "Arc Cargoship";
}

// Mid-raid pollution leaked into bot ESP (log: ActorName "GC Electrified",
// Camera). GameplayCue / Niagara must never become bots.
// Label variant: the Tick BOT's display label is literally "Tick"
// (C_TickBot -> "Tick" in en.json), so the engine-"Tick"-fname rule must only
// apply to raw fnames — applying it to labels erased every real Tick bot
// (debug-c190fb: "ticks not being picked up at all").
bool IsBotEspPollutionLabel(const std::string& s)
{
    if (s.empty())
        return false;
    std::string lower = s;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.rfind("gc_", 0) == 0 || lower.rfind("gc ", 0) == 0)
        return true;
    if (lower.rfind("ns_", 0) == 0)
        return true;
    return lower.find("camera") != std::string::npos
        || lower.find("spectator") != std::string::npos
        || lower.find("playercontroller") != std::string::npos
        || lower.find("cheatmanager") != std::string::npos
        || lower.find("debug") != std::string::npos
        || lower.find("widget") != std::string::npos
        || lower.find("gameplaycue") != std::string::npos
        || lower.find("niagara") != std::string::npos
        || lower.find("electrified") != std::string::npos;
}

bool IsBotEspPollutionName(const std::string& s)
{
    if (s.empty())
        return false;
    std::string lower = s;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Baseline name_trace_bot: engine "Tick" fname must never draw as a bot.
    // (Real Tick bots have fname C_TickBot_* — never the bare word.)
    if (lower == "tick")
        return true;
    // debug-c190fb R1 bot_nopos: EmbarkFastReplicatorTransformInterpolator was
    // admitted 69x via the Constructable struct path, and item/weapon actors
    // (BP_ItemActor_BasicMeleeWeapon_C) leaked into the bot cache. Raw-fname
    // tokens only — no real bot fname contains any of these.
    if (lower.find("replicator") != std::string::npos
        || lower.find("interpolator") != std::string::npos
        || lower.find("itemactor") != std::string::npos
        || lower.find("weaponactor") != std::string::npos
        || lower.find("pickupbase") != std::string::npos)
        return true;
    // debug-c190fb R4: "SpotAudioManager" was admitted as "Constructable" with
    // a garbage root (0x400000003), evicted at missCount=10 and re-admitted
    // 1.7s later, forever — 89 bot_nopos hits in one run. Audio infrastructure
    // actors are never bots.
    if (lower.find("audiomanager") != std::string::npos
        || lower.find("spotaudio") != std::string::npos)
        return true;
    // debug-c190fb flicker: "SM_World_02_BackdropLandscape_01" admitted as
    // "Bombardier" and "CollisionEffectNetworkSystem" admitted with a garbage
    // root — both churned admit→10-miss evict→re-admit, blinking the bot count
    // 11↔12 every second. SM_* static meshes and effect systems are never bots.
    // R4: a bare "Landscape" terrain actor (ALandscape) churned the same way in
    // TheDam_02_P — bot_nopos x443, labeled "Bombardier" via a garbage enemy-DA
    // slot read at 0x11B0. Terrain/landscape geometry is never a bot.
    if (lower.rfind("sm_", 0) == 0)
        return true;
    if (lower.find("collisioneffect") != std::string::npos
        || lower.find("networksystem") != std::string::npos
        || lower.find("backdrop") != std::string::npos
        || lower.find("landscape") != std::string::npos)
        return true;
    // Fix #11: "WorldItemEffectCue_Actor_Reusable1" sat in the bot pipeline as
    // "Constructable" with zero position, burning DMA every retain pass
    // (bot_nopos x26 per 2min). Effect cues are never bots.
    if (lower.find("effectcue") != std::string::npos)
        return true;
    // Fix #12: "PioneerWaterSystem" (label "Pioneerter System") admitted as a
    // bot with zero position — bot_nopos x102 in one Blue Gate raid, spared by
    // the G5 grace check forever. World water/plumbing systems are never bots.
    if (lower.find("watersystem") != std::string::npos)
        return true;
    return IsBotEspPollutionLabel(s);
}

bool IsMislabeledLootName(const std::string& label, const std::string& fname)
{
    if (label.empty())
        return false;
    if (IsWorldEspLabel(label))
        return true;
    if (IsAcceptedBotEspLabel(engine, label, fname))
        return false;
    int rarity = 0;
    int value = 0;
    return LookupItemMeta(label, rarity, value);
}

bool IsArcBotActor(uintptr_t actor, uintptr_t localPawn, const std::string& fnameHint);
uintptr_t ResolveBotSceneRoot(uintptr_t actor);

bool IsWorldHuskFName(const std::string& fname)
{
    if (fname.empty())
        return false;
    std::string lower = fname;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("husk") == std::string::npos)
        return false;
    // Husk is also used by placed world-object/container assets. A raw
    // "contains husk" match must never turn one of those into bot ESP.
    return lower.find("worldobject") != std::string::npos
        || lower.find("bp_worldobject") != std::string::npos
        || lower.find("lootcontainer") != std::string::npos
        || lower.find("container") != std::string::npos
        || lower.find("interactable") != std::string::npos
        || lower.find("pickup") != std::string::npos;
}

std::string ResolveHuskBotLabel(const std::string& fname)
{
    if (fname.empty() || IsWorldHuskFName(fname))
        return {};
    std::string lower = fname;
    for (char& c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("husk") == std::string::npos)
        return {};
    if (lower.find("small") != std::string::npos)
        return "Husk S";
    if (lower.find("medium") != std::string::npos)
        return "Husk M";
    if (lower.find("large") != std::string::npos)
        return "Husk L";
    return "Husk";
}

std::string FinalizeBotAdmissionLabel(
    uintptr_t actor,
    const std::string& fname,
    bool isBotEntry)
{
    std::string label = ResolveBotTypeLabel(actor, fname);

    if (!label.empty() && IsWorldEspLabel(label))
        label.clear();

    if (!label.empty() && IsMislabeledLootName(label, fname))
        label.clear();

    if (!label.empty() && IsWorldEspLabel(label))
        label.clear();

    if (!label.empty() && IsBotEspPollutionLabel(label))
        label.clear();

    // Always require accepted robot-list labels (even for struct bots).
    if (!label.empty() && !IsAcceptedBotEspLabel(engine, label, fname))
        return {};

    (void)isBotEntry;
    return label;
}

bool ActorHasKnownBotIdentity(uintptr_t actor, const std::string& fnameHint)
{
    return !ResolveBotTypeLabel(actor, fnameHint).empty();
}

bool HasVerifiedEnemyAsset(uintptr_t actor, const std::string& fname)
{
    if (!WorldScan::HasArcEnemyAssetPointer(actor))
        return false;
    if (ActorHasKnownBotIdentity(actor, fname))
        return true;
    if (!GetEnemyTypeDataAssetFName(actor).empty())
        return true;
    return !ResolveEnemyAssetBotLabel(actor).empty();
}

bool IsVerifiedCachedBot(
    uintptr_t actor,
    const std::string& fname,
    const std::string& actorName)
{
    if (ArcActorType::IsAnyBotActor(actor))
        return true;
    if (ActorHasKnownBotIdentity(actor, fname))
        return true;
    if (HasVerifiedEnemyAsset(actor, fname))
        return true;
    if (!actorName.empty()
        && actorName != "Constructable"
        && IsAcceptedBotEspLabel(engine, actorName, fname))
        return true;
    return false;
}

bool IsLikelyWorldItemActor(uintptr_t actor)
{
    if (!actor)
        return false;

    if (WorldScan::HasArcEnemyAssetPointer(actor))
        return false;

    std::string fname = engine.GetActorFNameStringCached(actor);
    if (fname.empty())
        fname = engine.GetActorFNameString(actor);
    if (ActorHasKnownBotIdentity(actor, fname))
        return false;

    const uint64_t itemDa =
        Memory::read<uint64_t>(actor + static_cast<uint64_t>(Offsets::ItemDataAsset));
    if (itemDa != 0 && Memory::IsValidPtrFast2(itemDa))
        return true;

    const uint64_t hover =
        Memory::read<uint64_t>(actor + static_cast<uint64_t>(Offsets::UIHoverData));
    if (hover != 0 && Memory::IsValidPtrFast2(hover))
        return true;

    const uintptr_t containerLoot =
        Memory::read<uintptr_t>(actor + Offsets::LootInteraction_Container);
    if (containerLoot && engine.IsValidPointer(containerLoot))
        return true;

    if (!fname.empty() && FnameLooksLikeWorldContainer(fname))
        return true;

    if (!fname.empty() && IsStrictWorldLootFname(fname))
        return true;

    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    if (ArcActorType::IsWorldItemClassIdMasked(masked))
        return true;

    return false;
}

bool QualifiesAsConstructableBot(uintptr_t actor, const std::string& fname)
{
    if (!actor || !engine.IsValidPointer(actor))
        return false;

    if (ActorHasKnownBotIdentity(actor, fname))
        return true;
    if (ArcActorType::IsAnyBotActor(actor))
        return true;
    if (HasVerifiedEnemyAsset(actor, fname))
        return true;

    if (WorldScan::LooksLikeContainerActor(actor, fname))
        return false;

    if (IsLikelyWorldItemActor(actor))
        return false;

    if (!fname.empty()) {
        if (!ResolveRobotTypeFromFName(engine, fname).empty())
            return true;
    }

    if (const uintptr_t mesh = engine.GetActorSkeletalMesh(actor); mesh) {
        std::string meshFname = engine.GetActorFNameStringCached(mesh);
        if (meshFname.empty())
            meshFname = engine.GetActorFNameString(mesh);
        if (!meshFname.empty()
            && !ResolveRobotTypeFromFName(engine, meshFname).empty())
            return true;
    }

    return false;
}

// Strict item/gun/pickup structure. Unlike IsLikelyWorldItemActor this has NO
// "known bot identity" escape hatch: a gun whose fname merely fuzzy-matches a
// bot token is still a gun if it carries item / hover / loot-container pointers.
bool HasWorldItemStructure(uintptr_t actor)
{
    if (!actor)
        return false;

    const uint64_t itemDa =
        Memory::read<uint64_t>(actor + static_cast<uint64_t>(Offsets::ItemDataAsset));
    if (itemDa != 0 && Memory::IsValidPtrFast2(itemDa))
        return true;

    const uint64_t hover =
        Memory::read<uint64_t>(actor + static_cast<uint64_t>(Offsets::UIHoverData));
    if (hover != 0 && Memory::IsValidPtrFast2(hover))
        return true;

    const uintptr_t containerLoot =
        Memory::read<uintptr_t>(actor + Offsets::LootInteraction_Container);
    if (containerLoot && engine.IsValidPointer(containerLoot))
        return true;

    return false;
}

// Validated ARC EnemyType data-asset name. This is the single bot-EXCLUSIVE
// signal: GetEnemyTypeDataAssetFName only returns non-empty when the actor
// carries a real "DA_EnemyType_" asset (or one that maps to a known bot). No
// gun, dropped item, container, world prop or player has this. It is the
// backbone of the check-and-balance system.
bool HasStrongEnemyDataAsset(uintptr_t actor)
{
    if (!actor)
        return false;
    if (!GetEnemyTypeDataAssetFName(actor).empty())
        return true;
    if (!ResolveEnemyAssetBotLabel(actor).empty())
        return true;
    return false;
}

// Runtime bot-type discovery (debug-c190fb: "new bots never get seen").
// A verified enemy actor whose name maps to no known robotsList type is a NEW
// bot type — derive its real name from its own enemy data asset and register
// it into engine.robotsList so every gate (getAllowType / IsAcceptedBotEspLabel
// / draw) accepts it uniformly. Strictly gated on bot-exclusive proof
// (DA_EnemyType_* / bot actor-type id) so loot/props/cues can never register.
std::string DiscoverNewBotType(uintptr_t actor, const std::string& fname)
{
    if (!actor)
        return {};
    if (!HasStrongEnemyDataAsset(actor) && !ArcActorType::IsAnyBotActor(actor))
        return {};

    // debug-c190fb bot_type_discovered: engine-infrastructure actors
    // (WorldItemEffectCue_Actor_Reusable1, EmbarkFastReplicatorTransform*,
    // SpotAudioManager) passed the structural gate and registered as bot
    // types, then churned admit/evict in bot ESP. Hard-block their tokens.
    {
        std::string fnLower = fname;
        for (char& c : fnLower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        static const char* kNeverBotFname[] = {
            "effectcue", "gameplaycue", "worlditem", "replicator",
            "interpolator", "blackboard", "audiomanager", "audiocomponent",
            "spotaudio", "manager", "spawner", "volume", "subsystem",
            "soundscape", "landscape",
        };
        for (const char* token : kNeverBotFname) {
            if (fnLower.find(token) != std::string::npos)
                return {};
        }
    }

    auto sanitize = [&](std::string label) -> std::string {
        // Strip enemy-asset prefixes humanize leaves behind.
        for (const char* prefix : { "Da Enemy Type ", "Enemy Type ", "Da Enemy " }) {
            const size_t n = strlen(prefix);
            if (label.size() > n && label.compare(0, n, prefix) == 0)
                label = label.substr(n);
        }
        if (label.size() < 3)
            return {};
        bool hasAlpha = false;
        for (unsigned char c : label) {
            if (std::isalpha(c)) {
                hasAlpha = true;
                break;
            }
        }
        if (!hasAlpha)
            return {};
        if (label == kBotStructAdmissionToken
            || label == "ARC" || label == "Bot" || label == "Oil")
            return {};
        if (IsBotEspPollutionLabel(label) || IsGenericWorldEspLabel(label)
            || IsJunkWorldEspLabel(label) || IsGarbledEspLabel(label))
            return {};
        // Same engine-junk tokens on the humanized label (label can come from
        // the class fname when the actor fname is empty).
        {
            std::string labelLower = label;
            for (char& c : labelLower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            static const char* kNeverBotLabel[] = {
                "effect cue", "gameplay cue", "replicator", "interpolator",
                "black board", "blackboard", "audio manager", "spot audio",
                "manager", "spawner", "volume",
            };
            for (const char* token : kNeverBotLabel) {
                if (labelLower.find(token) != std::string::npos)
                    return {};
            }
        }
        // A name that resolves as loot metadata is an item, not a bot type.
        int rarity = 0;
        int value = 0;
        if (LookupItemMeta(label, rarity, value))
            return {};
        return label;
    };

    std::string label;
    if (const std::string fromAsset = ResolveEnemyAssetBotLabel(actor); !fromAsset.empty())
        label = sanitize(NormalizeBotDisplayName(fromAsset));
    if (label.empty()) {
        if (const std::string da = GetEnemyTypeDataAssetFName(actor); !da.empty())
            label = sanitize(HumanizeActorFName(da));
    }
    if (label.empty() && !fname.empty() && fname != kBotStructAdmissionToken)
        label = sanitize(HumanizeActorFName(fname));
    if (label.empty()) {
        if (const std::string classFn = engine.GetActorClassFName(actor); !classFn.empty())
            label = sanitize(HumanizeActorFName(classFn));
    }
    if (label.empty())
        return {};

    if (!engine.robotsList.contains(label)) {
        engine.robotsList.insert(label);
        // #region agent log
        {
            static std::unordered_set<std::string> s_discoveredSeen;
            if (s_discoveredSeen.insert(label).second) {
                std::ostringstream f;
                char labelEsc[64]{}, fnameEsc[64]{};
                snprintf(labelEsc, sizeof(labelEsc), "%.48s", label.c_str());
                snprintf(fnameEsc, sizeof(fnameEsc), "%.48s", fname.c_str());
                const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                f << "{\"sessionId\":\"c190fb\",\"runId\":\"post-fix\",\"hypothesisId\":\"B3\","
                  << "\"location\":\"RobotList.cpp:DiscoverNewBotType\",\"message\":\"bot_type_discovered\","
                  << "\"data\":{\"label\":\"" << labelEsc
                  << "\",\"fname\":\"" << fnameEsc << "\"}"
                  << ",\"timestamp\":" << ts << "}\n";
                EntityDiagnostics::LogRaw(f.str());
            }
        }
        // #endregion
    }
    return label;
}

// -------------------------------------------------------------------------
// Bot check-and-balance verifier.
//
// This is THE single authoritative gate. Every admission pass and every
// retention pass runs an actor through it, so a bot must independently prove
// itself twice before it draws. Guarantee: only real ARC enemies survive �
// never the local player, other players, guns, dropped items, boxes or
// containers. Fuzzy fname/mesh name matches ALONE are never enough; a bot must
// present real structural proof (an ARC enemy data-asset, the ARC class tag,
// or a constructable enemy pointer) backed by a resolvable identity.
// -------------------------------------------------------------------------
// Skip VerifyBotActor on props/containers � full verify on every actor was taking
// tens of seconds per pass over the whole level (SyncedThread waits to finish).
bool QuickBotCandidate(uintptr_t actor)
{
    if (!actor || !engine.IsValidPointer(actor))
        return false;

    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    if (ArcActorType::IsPlayerClassId(masked))
        return false;

    // UE5 bots get PlayerStates from their AI controllers — a valid PS alone
    // no longer proves "human player". Snitch-summoned reinforcement bots
    // spawn AI-possessed with a PS and were invisible to ESP because of this
    // veto. Real players are already vetoed above by class id; this is a
    // cheap pre-filter, the authoritative gate is VerifyBotActor.
    if (ArcActorType::IsAnyBotActor(actor))
        return true;

    // Strong bot-exclusive DA signal (DA_EnemyType_* / mapped enemy asset).
    if (HasStrongEnemyDataAsset(actor))
        return true;

    if (WorldScan::HasArcEnemyAssetPointer(actor))
        return true;

    auto fnameLooksLikeBot = [](const std::string& fname) -> bool {
        if (fname.empty())
            return false;
        if (!ResolveRobotTypeFromFName(engine, fname).empty())
            return true;
        if (!LookupEnemyBotByFName(fname).empty())
            return true;
        return false;
    };

    // Cached fname first (cheap). Fresh snitch spawns often miss the cache —
    // decrypt once so QuickBot does not skip actors Verify would accept.
    const std::string fnameCached = engine.GetActorFNameStringCached(actor);
    if (fnameLooksLikeBot(fnameCached))
        return true;
    if (fnameCached.empty()) {
        const std::string fname = engine.GetActorFNameString(actor);
        if (fnameLooksLikeBot(fname))
            return true;
    }
    // Class FName (C_LightDrone / C_SnitchBot) — sky-drop spawns often have
    // class before instance fname decrypts.
    const std::string classFn = engine.GetActorClassFName(actor);
    if (fnameLooksLikeBot(classFn))
        return true;
    if (!LookupBotClassToken(classFn).empty())
        return true;

    // Item/hover/loot structure vetoes AFTER the name checks: constructable
    // bots (Turret, dispenser-style) can carry hover/interaction pointers, and
    // the old order rejected them before their "turret" fname was considered.
    return !HasWorldItemStructure(actor);
}

// P6: the admission scan re-ran fname/class-fname DMA reads on every static
// actor every pass (RobotList 600-900ms avg). Actors whose decoded name proved
// non-bot are memoized negative with a staggered TTL so late-decrypting spawns
// still get re-checked. Pruned against currentActorSet each pass.
static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
    s_quickFnameNeg;

// B1 (Fix #8): struct-probe candidates (garbage DA pointers) skipped the P6
// fname memo and re-ran full VerifyBotActor every pass — ~125 junk actors
// (AmbienceVolume, SplinePath, AudioComponentManager...) x several DMA reads
// per pass, a large slice of RobotList's 268ms avg hold. Memoize verify
// failures with a staggered TTL; decoded-name actors only. TTL keeps
// late-decrypting spawns re-checkable.
static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
    s_botVerifyNeg;

// P6b: 8-slice rotating admission ring. Full prune + retain still run every
// pass; only the expensive admission DMA probes are banded. Positions stay
// hot via PositionRefreshPass @16ms. 4 slices (was 8): the full admission sweep
// is what decides how late a newly-seen bot appears, and 8 slices put a full ring
// cycle at ~8.6s. 4 slices finishes the sweep in about half the time so late bots
// surface sooner. Cost: roughly double the reads per pass, so the shared DMA link
// has less headroom for the camera thread - revert to 8 if the camera/ESP stutters.
static constexpr size_t kAdmitSlices = 4;
// Streaming window: a transient-failing spawn (no root/mesh/name yet) keeps
// being retried every pass for this long before it is released. The old
// 8-try budget (~8 passes ≈ 8-16s) settled slow-streaming spawns, hid them
// behind the 8-slice band ring, and produced "no bot esp till a minute later"
// on large maps. After the window the band ring still re-checks the actor, so
// a late-finishing spawn is admitted on its next band pass.
static constexpr uint64_t kBotTransientWindowMs = 45000;
// C10: persistent pending lane (Core/BotAdmitQueue.hpp). Newly-seen actors
// stay queued until a definitive outcome instead of being "new" for one pass —
// the missed-window actors used to wait an up-to-19s ring sweep ("bot ESP takes
// a while before some bots show"). The priority cap lives on the queue.
static AdmitQueue::PendingQueue s_admitQueue;
static size_t s_admitSliceCursor = 0;
// RIVENTIDES resume frontier: with 16K actors the probe scatter cannot finish
// a 2048-row slice inside one 90ms budget. Rows are scattered AND processed
// in lockstep windows; the frontier persists across passes so every pass
// makes forward progress instead of restarting at row 0 forever (the
// "stuck slice, scanned:0, cache:0" deadlock).
static size_t s_admitResumeRow = 0;
// Priority-lane rows probed per pass. Uncapped, a 1000+ pending backlog filled
// the 90ms probe budget every pass from the same first rows, so the band slice
// was never scattered and the ring sat on slice 0 forever (admitted=0).
static constexpr size_t kAdmitPrioPerPass = 96;
// B5 (Stage 2): serial-verify backlog — candidates queued when the 60ms
// verify budget overruns are drained FIRST on later passes. Verify never
// gates ring advance, so a saturated bus defers the tail by one pass instead
// of freezing the ring (the Riventides 16K-actor deadlock).
static constexpr size_t kAdmitVerifyBacklogMax = 4096;
static std::vector<uintptr_t> s_admitVerifyBacklog;
static uint64_t s_admitRingGen = 0;
static uintptr_t s_admitRingActorsPtr = 0;
static size_t s_admitRingActorCount = 0;
static size_t s_admitRingEpoch = 0;
static uint64_t s_admitCoveredMask = 0;
static int s_admitRingResets = 0;
static int s_admitLastCycleMs = 0;
static std::chrono::steady_clock::time_point s_admitCycleStart{};
static std::unordered_set<uintptr_t> s_admitPrevActors;

// Fresh-definitive-reject lookups (TTLs shared with the memo writers below).
static bool HasFreshQuickFnameNeg(uintptr_t actor)
{
    const auto it = s_quickFnameNeg.find(actor);
    if (it == s_quickFnameNeg.end())
        return false;
    return std::chrono::steady_clock::now() - it->second
        < std::chrono::seconds(8 + static_cast<int>((actor >> 4) & 7));
}

static bool HasFreshBotVerifyNeg(uintptr_t actor)
{
    const auto it = s_botVerifyNeg.find(actor);
    if (it == s_botVerifyNeg.end())
        return false;
    return std::chrono::steady_clock::now() - it->second
        < std::chrono::seconds(10 + static_cast<int>((actor >> 5) & 7));
}

// ---------------------------------------------------------------------------
// Per-actor admission rejection trace.
//
// "The bot two metres away had no ESP" was unanswerable from the log: the
// funnel carried counters (verifyFail / failRoot / failMesh / failId) but never
// a NAME, and every stage-1 settle -- prop structure, cached player, undecrypted
// spawn -- settled the actor silently. This writes one NDJSON line per (actor,
// reason) per second, with the fname and the bot-positive evidence that was or
// was not present, so a drop can be judged instead of guessed at. Bounded by a
// per-pass budget so a 1,000-row band scan cannot flood the log, and gated on
// debug_ghost_bots like the other bot traces.
void LogBotAdmitReject(uintptr_t actor, const char* reason, const std::string& fnameRaw,
    int& budget, bool anyBot = false, bool strongDa = false, bool hasRoot = false,
    bool hasMesh = false, bool hasId = false, bool memoized = false,
    bool enemyDaPresent = false, bool aiTemplatePresent = false,
    double distM = -1.0)
{
    if (budget <= 0 || !var::debug_ghost_bots)
        return;

    // Throttle per (actor, reason): one line a second. A single slot per actor
    // would let two alternating reasons log every pass (the defect the position
    // logger already fixed).
    uint64_t reasonHash = 1469598103934665603ull;
    for (const char* p = reason; p && *p; ++p)
        reasonHash = (reasonHash ^ static_cast<uint64_t>(
            static_cast<unsigned char>(*p))) * 1099511628211ull;
    static std::unordered_map<uint64_t, uint64_t> last;
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t key =
        (static_cast<uint64_t>(actor) * 0x9E3779B97F4A7C15ull) ^ reasonHash;
    const auto it = last.find(key);
    if (it != last.end() && nowMs >= it->second && nowMs - it->second < 1000)
        return;
    last[key] = nowMs;
    if (last.size() > 8192)
        last.clear();
    --budget;

    // An actor's fname is untrusted bytes from the target: quote and control
    // characters would otherwise break the NDJSON line.
    std::string fname;
    fname.reserve(fnameRaw.size());
    for (char c : fnameRaw) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\')
            fname += '_';
        else if (uc < 0x20)
            fname += '?';
        else
            fname += c;
    }

    std::ostringstream f;
    f << "{\"message\":\"bot_admit_reject\",\"data\":{\"actor\":\"0x"
      << std::hex << actor << std::dec
      << "\",\"reason\":\"" << reason << "\",\"fname\":\"" << fname << "\""
      << ",\"anyBot\":" << (anyBot ? 1 : 0)
      << ",\"strongDa\":" << (strongDa ? 1 : 0)
      << ",\"hasRoot\":" << (hasRoot ? 1 : 0)
      << ",\"hasMesh\":" << (hasMesh ? 1 : 0)
      << ",\"hasId\":" << (hasId ? 1 : 0)
      << ",\"enemyDa\":" << (enemyDaPresent ? 1 : 0)
      << ",\"aiTemplate\":" << (aiTemplatePresent ? 1 : 0)
      << ",\"memoized\":" << (memoized ? 1 : 0)
      << ",\"distM\":" << static_cast<long long>(distM)
      << "},\"ts\":" << nowMs << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}

static bool QuickBotFnameCandidateMemo(
    uintptr_t actor, int& outChecked, int& outMemoSkip)
{
    const auto now = std::chrono::steady_clock::now();
    if (HasFreshQuickFnameNeg(actor)) {
        ++outMemoSkip;
        return false;
    }
    s_quickFnameNeg.erase(actor); // stale TTL — recheck
    ++outChecked;

    auto fnameLooksLikeBot = [](const std::string& fn) -> bool {
        if (fn.empty())
            return false;
        if (!ResolveRobotTypeFromFName(engine, fn).empty())
            return true;
        if (!LookupEnemyBotByFName(fn).empty())
            return true;
        return false;
    };

    std::string fname = engine.GetActorFNameStringCached(actor);
    if (fname.empty())
        fname = engine.GetActorFNameString(actor);
    if (fnameLooksLikeBot(fname))
        return true;
    const std::string classFn = engine.GetActorClassFName(actor);
    if (fnameLooksLikeBot(classFn))
        return true;
    if (!LookupBotClassToken(classFn).empty())
        return true;

    // Memoize only when a name decoded; undecrypted actors retry next pass so
    // sky-drop spawns are never permanently skipped.
    if (!fname.empty() || !classFn.empty()) {
        if (s_quickFnameNeg.size() > 16384)
            s_quickFnameNeg.clear();
        s_quickFnameNeg[actor] = now;
    }
    return false;
}

bool VerifyBotActor(uintptr_t actor, uintptr_t localPawn, const std::string& fname)
{
    if (!actor || !engine.IsValidPointer(actor))
        return false;
    if (localPawn && actor == localPawn)
        return false;

    // (1) Absolute player negatives � veto every positive signal below.
    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    if (ArcActorType::IsPlayerClassId(masked))
        return false;
    if (WorldScan::LooksLikePlayerPawn(actor, localPawn))
        return false;
    if (engine.IsCachedPlayer(actor))
        return false;
    // UE5 bots get PlayerStates from their AI controllers — only the local
    // player's own PS is a hard reject. AI-possessed reinforcement bots
    // (Snitch summons) spawn with a valid PS and must not be vetoed here.
    const uintptr_t localPs = localPawn
        ? Memory::read<uintptr_t>(localPawn + Offsets::APlayerState)
        : 0;
    const uintptr_t playerState =
        Memory::read<uintptr_t>(actor + Offsets::APlayerState);
    if (playerState && engine.IsValidPointer(playerState)
        && playerState == localPs)
        return false;

    // Arc entry / cargo / caches / socket containers are world ESP � never bots,
    // even when they carry constructable or enemy-type data assets.
    {
        std::string probe = fname;
        if (probe.empty())
            probe = engine.GetActorFNameStringCached(actor);
        if (probe.empty())
            probe = engine.GetActorFNameString(actor);
        const std::string classFname = engine.GetActorClassFName(actor);
        if (IsBotEspPollutionName(probe) || IsBotEspPollutionName(classFname))
            return false;
        // Extraction hatches are world props (Loot tab) — they can pass the
        // class-id bot positives below, so hard-veto on name before that.
        if (FnameLooksLikeExtractionHatch(probe)
            || FnameLooksLikeExtractionHatch(classFname))
            return false;
        if (WorldScan::LooksLikeContainerActor(actor, probe)
            || (!classFname.empty() && WorldScan::LooksLikeContainerActor(actor, classFname))
            || (!probe.empty() && FnameLooksLikeWorldContainer(probe))
            || (!classFname.empty() && FnameLooksLikeWorldContainer(classFname)))
            return false;
    }

    // Husk is a shared display token: placed world-object husks are containers,
    // not ARC pawns. Reject the ambiguous form before any stale enemy-data
    // pointer can promote it to bot ESP.
    {
        std::string probe = fname;
        if (probe.empty())
            probe = engine.GetActorFNameStringCached(actor);
        if (probe.empty())
            probe = engine.GetActorFNameString(actor);
        const std::string classFname = engine.GetActorClassFName(actor);
        if (IsWorldHuskFName(probe) || IsWorldHuskFName(classFname))
            return false;
    }

    // (2) DEFINITIVE proof: validated ARC EnemyType data-asset (tech- style).
    // Bot-exclusive � DA_EnemyType_* / ResolveEnemyAssetBotLabel. Bare
    // constructable pointers alone leaked Camera/ghost junk into bot ESP.
    // Fresh spawns often have DA before RootComponent settles � fall through
    // so mesh/root/class can still admit instead of hard-failing here.
    if (HasStrongEnemyDataAsset(actor) && ResolveBotSceneRoot(actor) != 0)
        return true;

    // (3) No definitive proof ? weak candidate. Container-name veto here;
    //     structure veto at (3b) below the bot-name checks. This ordering is
    //     what keeps guns, ground boxes and containers OUT of the bot cache
    //     while constructable bots still admit.
    if (WorldScan::LooksLikeContainerActor(actor, fname))
        return false;

    // (4) Scene root required for world pos. Mesh preferred but optional for
    // fresh class bots (Snitch/drone can spawn flying before skeletal mesh binds).
    if (ResolveBotSceneRoot(actor) == 0)
        return false;

    const uintptr_t mesh = engine.GetActorSkeletalMesh(actor);
    const bool meshOk = mesh && engine.IsValidPointer(mesh);

    // (5) Live ARC bot class / TARGET is enough for fresh Snitch/drone spawns.
    // fname/DA identity often decrypts 1-N ticks later. Mesh optional when class proves bot.
    if (ArcActorType::IsAnyBotActor(actor))
        return true;

    // (5b) Durable admit: known bot fname/class/mesh + root before mesh/DA settles.
    // QuickBotCandidate can pass on fname while skeletal mesh is still unbound —
    // the old !meshOk hard-fail dropped nearby Wasps/Snitches (c190fb H1).
    auto knownBotName = [&](const std::string& n) -> bool {
        if (n.empty())
            return false;
        if (!ResolveRobotTypeFromFName(engine, n).empty())
            return true;
        if (!LookupEnemyBotByFName(n).empty())
            return true;
        if (!LookupBotClassToken(n).empty())
            return true;
        return false;
    };
    if (knownBotName(fname))
        return true;
    {
        const std::string classFname = engine.GetActorClassFName(actor);
        if (knownBotName(classFname))
            return true;
    }
    if (meshOk) {
        std::string meshFname = engine.GetActorFNameStringCached(mesh);
        if (meshFname.empty())
            meshFname = engine.GetActorFNameString(mesh);
        if (knownBotName(meshFname))
            return true;
    }

    // (3b) Structure veto after names: a known-bot-named actor that carries
    // item/hover/loot pointers is still a bot (Turret-style constructs); a
    // gun/pickup never matches a bot table name, so this still keeps guns and
    // ground boxes OUT of the bot cache.
    if (HasWorldItemStructure(actor))
        return false;

    // No mesh yet: admit only after the SDK-owned constructable asset has
    // resolved to a real enemy asset name. A non-null pointer at +0x11B0 /
    // +0x11C0 is not sufficient — those slots belong to
    // APioneerConstructablePawn and contain arbitrary data on other actors.
    // That raw-pointer fallback admitted world actors as bots and is the
    // opposite of an SDK-backed identity check.
    if (!meshOk) {
        return HasStrongEnemyDataAsset(actor);
    }

    // Constructable enemy pointer still needs resolvable identity (no Camera leak).
    if (WorldScan::HasArcEnemyAssetPointer(actor) && ActorHasKnownBotIdentity(actor, fname))
        return true;

    return false;
}

// Internal cache label when struct detection succeeds but fname decrypt fails.
// Never shown on screen � draw uses ResolveBotDrawLabel for a real name.

std::string ResolveStructBotAdmissionLabel(uintptr_t actor, const std::string& fname)
{
    std::string label = FinalizeBotAdmissionLabel(actor, fname, true);
    if (!label.empty())
        return label;

    // Identity decrypt failed � still admit struct bots for boxes; draw resolves name.
    if (HasVerifiedEnemyAsset(actor, fname)
        || ActorHasKnownBotIdentity(actor, fname)
        || ArcActorType::IsAnyBotActor(actor)
        || IsArcBotActor(actor, 0, fname))
        return kBotStructAdmissionToken;

    return {};
}

uintptr_t ResolveBotSceneRoot(uintptr_t actor);
Vector3 ResolveBotWorldPos(uintptr_t actor, uintptr_t root, uintptr_t mesh);

bool ShouldSkipBotActor(
    uintptr_t actor,
    uintptr_t localPawn,
    const Vector3& localPos)
{
    (void)localPos;
    if (!actor)
        return true;
    if (localPawn && actor == localPawn)
        return true;
    if (WorldScan::LooksLikePlayerPawn(actor, localPawn))
        return true;

    if (engine.IsCachedPlayer(actor))
        return true;

    return false;
}

void UpdateBotVelocity(Engine::WorldCacheEntry& actor, const Vector3& worldPosRead)
{
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    if (actor.lastVelocityUpdate > 0.f) {
        const float dtSec =
            (nowMs - static_cast<uint64_t>(actor.lastVelocityUpdate)) * 0.001f;
        if (dtSec > 0.001f && dtSec < 0.5f) {
            const Vector3 delta{
                worldPosRead.x - actor.lastWorldPos.x,
                worldPosRead.y - actor.lastWorldPos.y,
                worldPosRead.z - actor.lastWorldPos.z,
            };
            Vector3 newVel{ delta.x / dtSec, delta.y / dtSec, delta.z / dtSec };
            WorldScan::BlendCachedVelocity(actor.cachedVelocity, newVel);
        }
    }

    actor.lastWorldPos = worldPosRead;
    actor.lastVelocityUpdate = nowMs;
    actor.positionSampleMs = nowMs;
}

Vector3 ReadComponentWorldPos(uintptr_t component)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Position);
    if (!component || !engine.IsValidPointer(component))
        return {};
    return Engine::ReadSceneWorldPos(component);
}

// Live bot tracking — mirror EntityList (NOCACHE WorldLocation, no RelativeLocation).
Vector3 ReadBotSceneWorldPosLive(uintptr_t component)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Position);
    return Engine::ReadWorldLocationNocache(component, /*allowRelativeFallback=*/false);
}

Vector3 ResolveBotWorldPos(uintptr_t actor, uintptr_t root, uintptr_t mesh)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Position);
    if (root) {
        const Vector3 fromRoot = ReadBotSceneWorldPosLive(root);
        if (IsPlausibleWorldPos(fromRoot))
            return fromRoot;
    }

    if (mesh) {
        const Vector3 fromMesh = ReadBotSceneWorldPosLive(mesh);
        if (IsPlausibleWorldPos(fromMesh))
            return fromMesh;
    }

    const uintptr_t embarkMesh =
        Memory::read_nocache<uintptr_t>(actor + Offsets::EmbarkMesh);
    if (embarkMesh && embarkMesh != mesh && engine.IsValidPointer(embarkMesh)) {
        const Vector3 fromEmbark = ReadBotSceneWorldPosLive(embarkMesh);
        if (IsPlausibleWorldPos(fromEmbark))
            return fromEmbark;
    }

    // debug-c190fb R1: RollBot (Fireball, C_RollBot_Flamethrower_C) has valid
    // root+mesh pointers but WorldLocation (CTW translation) reads 0 on both —
    // it never drew and churned evict/re-admit. A ROOT component has no parent,
    // so RelativeLocation IS its world position (players already use this
    // fallback). Meshes are skipped: their relative is a parent offset.
    if (root) {
        const Vector3 fromRel =
            Engine::ReadWorldLocationNocache(root, /*allowRelativeFallback=*/true);
        if (IsPlausibleWorldPos(fromRel)) {
            // #region agent log
            {
                static auto s_lastRelLog = std::chrono::steady_clock::time_point{};
                const auto nowRel = std::chrono::steady_clock::now();
                if (s_lastRelLog.time_since_epoch().count() == 0
                    || nowRel - s_lastRelLog >= std::chrono::seconds(3)) {
                    s_lastRelLog = nowRel;
                    std::ostringstream f;
                    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    f << "{\"sessionId\":\"c190fb\",\"runId\":\"roller\",\"hypothesisId\":\"R3\","
                      << "\"location\":\"RobotList.cpp:ResolveBotWorldPos\",\"message\":\"bot_pos_fallback\","
                      << "\"data\":{\"source\":\"root_relative\",\"key\":" << actor
                      << ",\"posX\":" << static_cast<long long>(fromRel.x)
                      << ",\"posY\":" << static_cast<long long>(fromRel.y)
                      << ",\"posZ\":" << static_cast<long long>(fromRel.z) << "}"
                      << ",\"timestamp\":" << ts << "}\n";
                    EntityDiagnostics::LogRaw(f.str());
                }
            }
            // #endregion
            return fromRel;
        }
    }

    return {};
}

struct TArrayU64Local {
    uintptr_t Data = 0;
    int32_t Num = 0;
    int32_t Max = 0;
};

void ReadChildComponentsLocal(
    uintptr_t sceneComponent,
    std::vector<uintptr_t>& out,
    int maxDepth,
    int depth = 0)
{
    if (!sceneComponent || depth > maxDepth)
        return;

    const TArrayU64Local children =
        Memory::read<TArrayU64Local>(sceneComponent + Offsets::AttachChildren);
    if (!children.Data || children.Num <= 0 || children.Num > 256)
        return;

    for (int32_t i = 0; i < children.Num; ++i) {
        const uintptr_t child = Memory::read<uintptr_t>(
            children.Data + static_cast<uintptr_t>(i) * sizeof(uintptr_t));
        if (!child || !engine.IsValidPointer(child))
            continue;
        out.push_back(child);
        ReadChildComponentsLocal(child, out, maxDepth, depth + 1);
    }
}

void PopulateBotPartCache(Engine::WorldCacheEntry& actor, uintptr_t key)
{
    // Ledger: mesh/child-component discovery is a pointer chase, and its position
    // reads re-tag themselves below.
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Link);
    actor.BotPartCount = 0;
    actor.CenterWorldPos = {};

    if (!actor.Mesh || !engine.IsValidPointer(actor.Mesh))
        actor.Mesh = engine.GetActorSkeletalMesh(key);

    std::vector<uintptr_t> comps;
    if (actor.Mesh) {
        comps.push_back(actor.Mesh);
        ReadChildComponentsLocal(actor.Mesh, comps, 3);
    }

    const uintptr_t embarkMesh = Memory::read<uintptr_t>(key + Offsets::EmbarkMesh);
    if (embarkMesh && embarkMesh != actor.Mesh && engine.IsValidPointer(embarkMesh)) {
        comps.push_back(embarkMesh);
        ReadChildComponentsLocal(embarkMesh, comps, 2);
    }

    std::unordered_set<uintptr_t> seen;
    Vector3 sum{};
    int valid = 0;

    for (uintptr_t comp : comps) {
        if (!comp || !seen.insert(comp).second)
            continue;

        const Vector3 wp = ReadComponentWorldPos(comp);
        if (!IsPlausibleWorldPos(wp))
            continue;

        if (actor.BotPartCount < Engine::WorldCacheEntry::kMaxBotParts)
            actor.BotPartPos[actor.BotPartCount++] = wp;

        sum.x += wp.x;
        sum.y += wp.y;
        sum.z += wp.z;
        ++valid;
    }

    if (valid > 0) {
        const Vector3 meshCenter = actor.Mesh ? ReadComponentWorldPos(actor.Mesh) : Vector3{};
        if (IsPlausibleWorldPos(meshCenter))
            actor.CenterWorldPos = meshCenter;
        else {
            const float inv = 1.f / static_cast<float>(valid);
            actor.CenterWorldPos = Vector3{ sum.x * inv, sum.y * inv, sum.z * inv };
        }
        // Do NOT write mesh CTW into WorldPos. Scene-root refresh + PositionRefresh
        // own live tracking; mesh often freezes at the initial footprint.
        if (!IsPlausibleWorldPos(actor.WorldPos))
            actor.WorldPos = actor.CenterWorldPos;
    } else {
        // A failed resolve returns zeros, and writing those over the last good
        // sample is what makes a bot permanently undrawable: ShouldDrawRobotEsp
        // needs a plausible WorldPos or CenterWorldPos before it will put the
        // entry in the render frame, so ONE bad read hid the bot until eviction
        // no matter how close it stood. Measured live: Tick and Surveyor sat in
        // the bot cache with [0,0,0] positions while still in the actor array,
        // and DrawBotEsp logged collect_gate / no_live_position / Tick's
        // position_implausible behind every one of those misses. Keep the last
        // good sample instead; the live sampler owns advancing it.
        const Vector3 resolved = ResolveBotWorldPos(key, actor.rootComponent, actor.Mesh);
        if (IsPlausibleWorldPos(resolved)) {
            actor.WorldPos = resolved;
            actor.CenterWorldPos = resolved;
            if (actor.BotPartCount < Engine::WorldCacheEntry::kMaxBotParts)
                actor.BotPartPos[actor.BotPartCount++] = resolved;
        }
    }

    actor.hasBotHeadWorldPos = false;
    if (actor.Mesh && engine.IsValidPointer(actor.Mesh)) {
        uintptr_t boneMesh = 0;
        std::ptrdiff_t botCtw = 0;
        std::ptrdiff_t botTrans = Offsets::Transform_Translation;
        const uintptr_t boneArray =
            engine.ResolveBoneArray(key, actor.Mesh, &boneMesh, &botCtw, &botTrans);
        if (boneArray && boneMesh && engine.IsValidPointer(boneMesh)) {
            const FTransform ctw = Engine::ReadComponentToWorld(boneMesh);
            for (const auto& [gameIndex, uniBone] : BoneRoster::GameBoneMap()) {
                if (uniBone != UniBone::Head)
                    continue;
                const Vector3 head = engine.GetBone(gameIndex, boneArray, ctw);
                if (IsPlausibleWorldPos(head)) {
                    actor.BotHeadWorldPos = head;
                    actor.hasBotHeadWorldPos = true;
                }
                break;
            }
        }
    }
}

uintptr_t ResolveBotSceneRoot(uintptr_t actor)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Link);
    if (!actor)
        return 0;

    const uintptr_t root = Memory::read<uintptr_t>(actor + Offsets::RootComponent);
    if (root && engine.IsValidPointer(root))
        return root;

    const uintptr_t mesh = engine.GetActorSkeletalMesh(actor);
    if (mesh && engine.IsValidPointer(mesh))
        return mesh;

    const uintptr_t embarkMesh = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
    if (embarkMesh && engine.IsValidPointer(embarkMesh))
        return embarkMesh;

    return 0;
}

// PioneerConstructablePawn: no PlayerState, enemy asset or resolvable bot mesh/class.
bool IsArcBotActor(uintptr_t actor, uintptr_t localPawn, const std::string& fnameHint)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Name);
    if (!actor || actor == localPawn)
        return false;

    // Bots get PlayerStates from AI controllers — only the local player's
    // own PS is a hard reject here (reinforcement bots spawn with a PS).
    const uintptr_t localPs = localPawn
        ? Memory::read<uintptr_t>(localPawn + Offsets::APlayerState)
        : 0;
    const uintptr_t playerState =
        Memory::read<uintptr_t>(actor + Offsets::APlayerState);
    if (playerState && engine.IsValidPointer(playerState)
        && playerState == localPs)
        return false;

    if (HasVerifiedEnemyAsset(actor, fnameHint))
        return ResolveBotSceneRoot(actor) != 0;

    if (ActorHasKnownBotIdentity(actor, fnameHint))
        return ResolveBotSceneRoot(actor) != 0;

    if (WorldScan::LooksLikeContainerActor(actor, fnameHint))
        return false;

    auto meshResolvesToBot = [&](uintptr_t comp) -> bool {
        if (!comp || !engine.IsValidPointer(comp))
            return false;
        std::string compFname = engine.GetActorFNameStringCached(comp);
        if (compFname.empty())
            compFname = engine.GetActorFNameString(comp);
        if (compFname.empty())
            return false;
        const std::string bot = ResolveRobotTypeFromFName(engine, compFname);
        return !bot.empty() && IsAcceptedBotEspLabel(engine, bot, compFname);
    };

    if (const uintptr_t mesh = engine.GetActorSkeletalMesh(actor); meshResolvesToBot(mesh))
        return ResolveBotSceneRoot(actor) != 0;

    const uintptr_t embarkMesh = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
    if (embarkMesh && meshResolvesToBot(embarkMesh))
        return ResolveBotSceneRoot(actor) != 0;

    return false;
}

bool StillLooksLikeBot(
    uintptr_t actor,
    uintptr_t localPawn,
    int category,
    const Vector3& localPos)
{
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Name);
    if (ShouldSkipBotActor(actor, localPawn, localPos))
        return false;

    if (category == 3) {
        std::string fname = engine.GetActorFNameStringCached(actor);
        if (fname.empty())
            fname = engine.GetActorFNameString(actor);
        return QualifiesAsConstructableBot(actor, fname);
    }

    if (!actor || !engine.IsValidPointer(actor))
        return false;

    const uint32_t masked =
        ArcActorType::MaskActorTypeId(ArcActorType::ReadActorTypeId(actor));
    if (ArcActorType::IsPlayerClassId(masked))
        return false;
    if (ArcActorType::IsBotClassId(masked))
        return true;
    if (ArcActorType::IsTargetBotActor(actor))
        return true;

    // Bots get PlayerStates from AI controllers — only the local player's
    // own PS is a hard reject here (reinforcement bots spawn with a PS).
    const uintptr_t localPs = localPawn
        ? Memory::read<uintptr_t>(localPawn + Offsets::APlayerState)
        : 0;
    const uintptr_t playerState =
        Memory::read<uintptr_t>(actor + Offsets::APlayerState);
    if (playerState && engine.IsValidPointer(playerState)
        && playerState == localPs)
        return false;

    std::string fname = engine.GetActorFNameStringCached(actor);
    if (fname.empty())
        fname = engine.GetActorFNameString(actor);
    if (!ResolveRobotTypeFromFName(engine, fname).empty())
        return true;

    return IsArcBotActor(actor, localPawn, fname);
}

bool ComponentHasPlausibleWorldPos(uintptr_t component)
{
    if (!component || !engine.IsValidPointer(component))
        return false;
    return IsPlausibleWorldPos(ReadComponentWorldPos(component));
}

// Visual liveness for bots MUST match ResolveBotWorldPos (NOCACHE WorldLocation).
// Cached Memory::read of WorldLocation often returns 0/stale for ARC meshes/roots
// while read_nocache still works — that produced cache>0 drawing=0 visSkip=cache.
bool ComponentHasLiveBotWorldPos(uintptr_t component)
{
    if (!component || !engine.IsValidPointer(component))
        return false;
    return IsPlausibleWorldPos(ReadBotSceneWorldPosLive(component));
}

// Transient DMA / mesh transform reads can fail for a few passes; don't evict
// bots from cache on the first miss or ESP randomly blinks off.
// debug-c190fb bot_grace: at threshold 5 a LIVE Wasp was evicted 128x in one
// raid (Leaper 36x, Turret 28x) and every single evict was followed by a full
// VerifyBotActor re-admit — the visual probe false-negatives under DMA load,
// not because bots died. Double the grace before evicting. Raised again 10 -> 20
// for the faster 4-slice sweep: more evaluations per second meant more misses
// reaching the threshold, and flicker_score showed evictReadmit climbing
// (8..84) - evict/re-admit churn, not deaths.
static std::unordered_map<uintptr_t, uint8_t> s_botVisualMisses;
static constexpr uint8_t kBotVisualMissEvict = 20;

// debug-c190fb G3: bots evicted after 5 visual misses were re-admitted on the
// very next scan (still in the actor array), producing evict/re-admit churn
// ("Constructable" x581). Once evicted for visual loss, require a live visual
// OR a short cooldown before re-admission. The cooldown cap exists because the
// post-fix run showed reEvict:2 held steadily — a permanently flaky visual
// probe must not turn a live attacking bot into a forever-missing one.
static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
    s_botVisualEvicted;
// debug-c190fb bot_admit_latency: avg re-admit latency was ~2.75s across ALL
// bot types — i.e. the 3s cooldown itself was the dark window (the flaky
// visual probe rarely passed early). Re-admission already re-runs the full
// VerifyBotActor gate, so a long cooldown only hides live bots. Cut to 1s.
static constexpr auto kBotReAdmitCooldown = std::chrono::seconds(1);

// debug-c190fb ghost flicker: a live Surveyor's root pointer is valid and equal
// to its live RootComponent, yet WorldLocation (root+0x350) intermittently
// reads 0,0,0 under DMA load (liveRootX/Z=0 while the bot stands 10m away).
// Each zero frame dropped the box, the next good frame drew it → on/off blink.
// Freeze the last plausible position for a short window so a single failed read
// no longer blanks the box. Bounded TTL so a truly-gone bot still clears.
struct BotLastGoodPos {
    Vector3 pos{};
    std::chrono::steady_clock::time_point when{};
};
static std::unordered_map<uintptr_t, BotLastGoodPos> s_botLastGoodPos;
static constexpr auto kBotPosFreezeTtl = std::chrono::milliseconds(750);

// Dead-ESP expiry (Core/BotEspExpiry.hpp policy + bot_expire tap): "when bots
// die, why does the ESP stay so long" — the retain loop kept Drawing=true on a
// dead bot and waited out a 10-miss eviction (5-25s of frozen box), and the
// dead flag compared a packed bitfield byte == 1. First broken observation
// stamps; persistence past BotEspExpiry::kDeadConfirmMs confirms death.
static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
    s_botDeadSince;
// Some live constructables do not update bIsDestroyed reliably until the
// actor is removed. Their HealthService part array still gives a independent
// death signal: a coherent all-zero array, held briefly, is enough to stop ESP.
static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
    s_botAllZeroHealthSince;
static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
    s_lastBotHealthProbe;

// #region agent log
// B2: measure admission latency — time between an actor first passing
// QuickBotCandidate and actually entering the bot cache.
static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
    s_botCandFirstSeen;
// #endregion

static bool BotVisualMissShouldEvict(uintptr_t key, bool visualOk)
{
    return WorldScan::MissCounterShouldEvict(
        s_botVisualMisses, key, visualOk, kBotVisualMissEvict);
}

static void ClearBotVisualMiss(uintptr_t key)
{
    WorldScan::MissCounterClear(s_botVisualMisses, key);
}

static void ClearRobotListStaticMaps()
{
    s_botVisualMisses.clear();
    s_quickFnameNeg.clear();
    s_botVerifyNeg.clear();
    s_admitQueue.Clear();
    s_botDeadSince.clear();
    s_botAllZeroHealthSince.clear();
    s_lastBotHealthProbe.clear();
    s_admitSliceCursor = 0;
    s_admitRingGen = 0;
    s_admitRingActorsPtr = 0;
    s_admitRingActorCount = 0;
    s_admitCoveredMask = 0;
    s_admitLastCycleMs = 0;
    s_admitCycleStart = {};
    s_admitPrevActors.clear();
    ++s_admitRingEpoch;
    ++s_admitRingResets;
}



bool HasLiveBotVisual(uintptr_t actor, uintptr_t mesh)
{
    // Batch NOCACHE WorldLocation probes (was serial read_nocache per component).
    // Miss grace (kBotVisualMissEvict) unchanged — this only cuts DMA flaps.
    // Do NOT use cached WorldLocation — it false-negatives live ARC bots.
    constexpr size_t kMaxVisProbeComps = 16;
    std::vector<uintptr_t> comps;
    comps.reserve(kMaxVisProbeComps);

    auto addComp = [&](uintptr_t c) {
        if (!c || !engine.IsValidPointer(c) || comps.size() >= kMaxVisProbeComps)
            return;
        for (uintptr_t existing : comps) {
            if (existing == c)
                return;
        }
        comps.push_back(c);
    };

    addComp(mesh);
    if (mesh && engine.IsValidPointer(mesh)) {
        std::vector<uintptr_t> childComps;
        ReadChildComponentsLocal(mesh, childComps, 3);
        for (uintptr_t child : childComps)
            addComp(child);
    }

    const uintptr_t embarkMesh = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
    if (embarkMesh && embarkMesh != mesh && engine.IsValidPointer(embarkMesh)) {
        addComp(embarkMesh);
        std::vector<uintptr_t> embarkChildren;
        ReadChildComponentsLocal(embarkMesh, embarkChildren, 2);
        for (uintptr_t child : embarkChildren)
            addComp(child);
    }

    const uintptr_t root = ResolveBotSceneRoot(actor);
    if (root && root != mesh && root != embarkMesh)
        addComp(root);

    if (comps.empty())
        return false;

    // debug-c190fb R1: RollBot (Fireball) reads WorldLocation=0 on EVERY
    // component while alive at point-blank — the visual probe evicted it in a
    // loop. Root RelativeLocation (== world pos for a parentless root) is the
    // last-resort liveness signal, only paid when all WorldLocation reads fail.
    auto rootRelativeAlive = [&]() -> bool {
        return root
            && IsPlausibleWorldPos(Engine::ReadWorldLocationNocache(
                   root, /*allowRelativeFallback=*/true));
    };

    ScatterSession session; // default NOCACHE
    if (session.isValid()) {
        std::vector<Engine::FVector3d> worlds(comps.size());
        for (size_t i = 0; i < comps.size(); ++i)
            session.prepare(comps[i] + Offsets::WorldLocation, worlds[i]);
        if (session.execute()) {
            for (const Engine::FVector3d& w : worlds) {
                if (IsPlausibleWorldPos(Engine::ToVector3(w)))
                    return true;
            }
            return rootRelativeAlive();
        }
    }

    for (uintptr_t c : comps) {
        if (ComponentHasLiveBotWorldPos(c))
            return true;
    }
    return rootRelativeAlive();
}

} // namespace

uint8_t ReadBotBrokenFlag(uintptr_t actor)
{
    // Ledger tag: this is a death-state probe, not a position read. Without it
    // the whole retain pass reports as "untagged" and can never be told apart
    // from the scan it competes with.
    ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Broken);
    // Soft-deprecate bIsBreaked@0x1220 (not in SDK). Prefer only
    // Constructable_bIsDestroyed (help/esp.txt + SDK). Do not treat
    // Health==0 as dead — spawn frames often read 0 HP and blocked admits.
    //
    // MASK, not == 1: the dump types this bool as "byte 0x1230 mask 0x1" —
    // sibling bools pack into the same byte, so a destroyed bot's byte is
    // rarely exactly 1 (3, 5, 9... all read "alive"). That comparison is a
    // big part of why dead bots kept their ESP up.
    const uint8_t byte =
        Memory::read<uint8_t>(actor + Offsets::Constructable_bIsDestroyed);
    constexpr uint8_t kDestroyedMask = 0x1;
    return (byte & kDestroyedMask) != 0 ? 1 : 0;
}

/** Steady-ms of a retain-pass time point, for the entry's field stamps. */
static uint64_t SteadyMsFrom(const std::chrono::steady_clock::time_point& tp)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            tp.time_since_epoch()).count());
}

static std::atomic<int> g_botDrawLabelMiss{ 0 };

// Bot spawn-cluster grouping: bots admitted near each other within a short
// window (Snitch summon waves, patrol spawns) share a group id shown on ESP
// as "G#" under the skeleton.
static int g_botGroupCounter = 0;
struct BotGroupSeed { float x, y, z; std::chrono::steady_clock::time_point tp; };
static std::unordered_map<int, BotGroupSeed> g_botGroupSeeds;
static std::unordered_map<uintptr_t, int> g_botKeyToGroup;

static int AssignBotGroup(uintptr_t key, const Vector3& pos)
{
    if (auto it = g_botKeyToGroup.find(key); it != g_botKeyToGroup.end())
        return it->second;
    const auto now = std::chrono::steady_clock::now();
    int best = 0;
    for (auto& [gid, seed] : g_botGroupSeeds) {
        if (now - seed.tp > std::chrono::seconds(4))
            continue;
        const float dx = static_cast<float>(pos.x - seed.x);
        const float dy = static_cast<float>(pos.y - seed.y);
        const float dz = static_cast<float>(pos.z - seed.z);
        if (dx * dx + dy * dy + dz * dz < 6.4e7f) {  // (80 m)^2 in cm^2
            best = gid;
            break;
        }
    }
    if (best == 0) {
        best = ++g_botGroupCounter;
        g_botGroupSeeds[best] = {
            static_cast<float>(pos.x), static_cast<float>(pos.y),
            static_cast<float>(pos.z), now };
        if (g_botGroupSeeds.size() > 256)
            g_botGroupSeeds.clear();
    } else {
        g_botGroupSeeds[best].tp = now;
    }
    g_botKeyToGroup[key] = best;
    if (g_botKeyToGroup.size() > 4096)
        g_botKeyToGroup.clear();
    return best;
}

void RecordBotDrawLabelMiss()
{
    g_botDrawLabelMiss.fetch_add(1, std::memory_order_relaxed);
}

namespace {

std::string FinalizeBotTypeLabel(const std::string& label, const std::string& fnameHint)
{
    if (label.empty())
        return {};
    if (!IsAcceptedBotEspLabel(engine, label, fnameHint))
        return {};
    return label;
}

} // namespace

std::string ResolveBotTypeLabel(uintptr_t actor, const std::string& fname)
{
    // Single ordered chain: fname ? class ? enemy DA ? mesh ? embark ? husk.
    auto tryName = [&](const std::string& name) -> std::string {
        if (name.empty())
            return {};
        if (const std::string fromType = ResolveRobotTypeFromFName(engine, name);
            !fromType.empty())
            return FinalizeBotTypeLabel(fromType, fname);
        if (const std::string husk = ResolveHuskBotLabel(name); !husk.empty())
            return FinalizeBotTypeLabel(husk, fname);
        return {};
    };

    if (const std::string hit = tryName(fname); !hit.empty())
        return hit;

    if (!actor)
        return {};

    if (const std::string hit = tryName(engine.GetActorClassFName(actor)); !hit.empty())
        return hit;

    if (const std::string fromEnemy = ResolveEnemyAssetBotLabel(actor); !fromEnemy.empty())
        return FinalizeBotTypeLabel(fromEnemy, fname);

    if (const std::string enemyAsset = GetEnemyTypeDataAssetFName(actor); !enemyAsset.empty()) {
        if (const std::string hit = tryName(enemyAsset); !hit.empty())
            return hit;
    }

    if (const uintptr_t mesh = engine.GetActorSkeletalMesh(actor); mesh) {
        std::string meshFname = engine.GetActorFNameStringCached(mesh);
        if (meshFname.empty())
            meshFname = engine.GetActorFNameString(mesh);
        if (const std::string hit = tryName(meshFname); !hit.empty())
            return hit;
    }

    const uintptr_t embarkMesh =
        Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
    if (embarkMesh && engine.IsValidPointer(embarkMesh)) {
        std::string emFname = engine.GetActorFNameStringCached(embarkMesh);
        if (emFname.empty())
            emFname = engine.GetActorFNameString(embarkMesh);
        if (const std::string hit = tryName(emFname); !hit.empty())
            return hit;
    }

    return {};
}

std::string ResolveBotDrawLabel(
    uintptr_t actor,
    const std::string& cachedLabel,
    const std::string& fnameHint,
    bool allowDma)
{
    auto acceptOrNormalize = [](const std::string& label, const std::string& hint) -> std::string {
        if (label.empty() || label == kBotStructAdmissionToken)
            return {};
        if (label == "ARC" || label == "Bot" || label == "Oil")
            return {};
        if (IsAcceptedBotEspLabel(engine, label, hint))
            return label;
        const std::string normalized = NormalizeBotDisplayName(label);
        if (IsRobotsListType(normalized))
            return normalized;
        if (const std::string mapped = LookupEnemyBotByFName(label); !mapped.empty())
            return mapped;
        if (const std::string mapped = LookupEnemyBotDisplayLabel(label); !mapped.empty()
            && IsRobotsListType(NormalizeBotDisplayName(mapped)))
            return NormalizeBotDisplayName(mapped);
        return {};
    };

    if (const std::string hit = acceptOrNormalize(cachedLabel, fnameHint); !hit.empty())
        return hit;

    // DMA fallbacks are for the robot worker (allowDma=true) only. The paint
    // thread must never run these: GetActorClassFName / ResolveEnemyAssetBotLabel
    // / GetEnemyTypeDataAssetFName / GetActorFNameString / Memory::read(EmbarkMesh)
    // are live DMA reads that stalled Present 100-440ms whenever a bot's fname
    // decrypt flaked (Constructable token) — the ghost-copy flash.
    if (!allowDma)
        return {};

    if (const std::string resolved = ResolveBotTypeLabel(actor, fnameHint); !resolved.empty()) {
        if (const std::string hit = acceptOrNormalize(resolved, fnameHint); !hit.empty())
            return hit;
    }

    // Bot-only fallbacks when FName decrypt flakes � do not use item/world naming.
    auto tryToken = [&](const std::string& name) -> std::string {
        if (name.empty())
            return {};
        if (const std::string tok = LookupBotClassToken(name); !tok.empty())
            return acceptOrNormalize(tok, fnameHint);
        if (const std::string fromPat = LookupEnemyBotByFName(name); !fromPat.empty())
            return acceptOrNormalize(fromPat, fnameHint);
        return {};
    };

    if (const std::string hit = tryToken(fnameHint); !hit.empty())
        return hit;

    if (actor) {
        if (const std::string hit = tryToken(engine.GetActorClassFName(actor)); !hit.empty())
            return hit;
        if (const std::string enemy = GetEnemyTypeDataAssetFName(actor); !enemy.empty()) {
            if (const std::string hit = tryToken(enemy); !hit.empty())
                return hit;
            if (const std::string fromEnemy = ResolveEnemyAssetBotLabel(actor); !fromEnemy.empty()) {
                if (const std::string hit = acceptOrNormalize(fromEnemy, fnameHint); !hit.empty())
                    return hit;
            }
        }
    }

    return {};
}

// Stage trace for the robot pass (gated on debug_ghost_bots). Shows whether
// RobotList is entered, whether the world context was gathered, and whether
// admission starts: a silent pass otherwise looks identical to a no-bots world.
static void TraceRobotPass(int stage, size_t actors)
{
    if (!var::debug_ghost_bots)
        return;
    static const char* const kNames[3] = { "enter", "ctx_ok", "admit_start" };
    static uint64_t s_lastMs[3] = { 0, 0, 0 };
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    if (s_lastMs[stage] != 0 && nowMs - s_lastMs[stage] < 1000)
        return;
    s_lastMs[stage] = nowMs;
    std::ostringstream f;
    f << "{\"message\":\"robot_pass\",\"data\":{\"stage\":\"" << kNames[stage]
      << "\",\"actors\":" << actors << "},\"ts\":" << nowMs << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}

void Engine::RobotList()
{
    // Pre-size so runtime bot-type discovery inserts never rehash while the
    // render thread reads robotsList concurrently.
    static bool s_robotsListReserved = false;
    if (!s_robotsListReserved) {
        s_robotsListReserved = true;
        robotsList.reserve(256);
    }

    const bool wantRobots = var::showRobots || var::show_radar;
    if (!wantRobots) {
        std::unique_lock<std::shared_mutex> lock(m_robotCacheMutex);
        robotCache.clear();
        return;
    }

    TraceRobotPass(0, 0);
    WorldScanContext ctx;
    if (!GatherWorldScanContext(ctx))
        return;

    TraceRobotPass(1, ctx.currentActors.size());
    const uintptr_t sGWorld = ctx.gWorld;
    const uintptr_t sAcknowledgedPawn = ctx.acknowledgedPawn;
    if (!sGWorld || !ctx.persistentLevel)
        return;

    const uint64_t genAtStart =
        m_worldGeneration.load(std::memory_order_acquire);

    const std::vector<uint64_t>& currentActors = ctx.currentActors;
    if (currentActors.empty())
        return;

    std::unordered_set<uint64_t> currentActorSet(
        currentActors.begin(),
        currentActors.end()
    );

    CameraCache cam{};
    {
        std::shared_lock<std::shared_mutex> lock(m_cameraMutex);
        cam = g_Camera;
    }

    Vector3 localPos = cam.Location;
    {
        const uintptr_t localRoot = Memory::read<uintptr_t>(
            sAcknowledgedPawn + Offsets::RootComponent);
        if (localRoot && IsValidPointer(localRoot)) {
            const Vector3 rootPos = ReadSceneWorldPos(localRoot);
            if (IsPlausibleWorldPos(rootPos))
                localPos = rootPos;
        }
    }
    // LOS mesh rebuild is owned by Update � calling it every RobotList tick

    const float visualMaxM = var::bot_esp_distance > 0.f ? var::bot_esp_distance : var::kMaxDistanceSliderM;
    const float maxDistM = visualMaxM;
    const float maxDistSq = maxDistM * maxDistM * 10000.0f;
    // Distance anchor: localPos (pawn-root preferred, camera fallback) instead
    // of g_Location directly. A transiently reset/unreadable camera made every
    // bot measure its distance from the origin — the WHOLE cache failed the
    // range gate in one pass ("all filtered"), then all came back the next:
    // the bot ESP blink. Bad anchor this pass = hold previous draw state.
    const bool distRefOk = IsPlausibleWorldPos(localPos);
    // Dist-edge hysteresis (Schmitt trigger): a bot already drawing stays on up
    // to 15% past the slider cutoff, so a bot hovering on the boundary can't
    // blink on/off. New bots still only admit at the true cutoff.
    constexpr float kDistOffFactor = 1.15f;
    const float maxDistOffSq = maxDistSq * kDistOffFactor * kDistOffFactor;

    std::unordered_map<uintptr_t, WorldCacheEntry> localCache;
    {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        localCache = robotCache;
    }
    for (auto it = localCache.begin(); it != localCache.end(); )
    {
        if (!currentActorSet.contains(it->first)) {
            // Dead-bot fix #1 (grey corpse on vanish): a bot that leaves the
            // actor array while it was DRAWING is almost always a kill. With
            // dead-bot ESP on, keep it as a grey corpse for the bounded hold
            // instead of a silent erase — the corpse path (DeadDisposition)
            // greys it, then expires it after kCorpseHoldMs. The death stamp
            // also makes the broken flag sticky below, so freed memory reading
            // "alive" cannot resurrect the box as a live bot.
            if (var::show_dead_bots && it->second.Drawing) {
                it->second.IsBreaked = true;
                it->second.broken = true;
                s_botDeadSince.try_emplace(
                    it->first, std::chrono::steady_clock::now());
                ++it;
                continue;
            }
            ClearBotVisualMiss(it->first);
            s_botVisualEvicted.erase(it->first);
            it = localCache.erase(it);
        } else
            ++it;
    }
    // Drop sticky-evict marks for actors that left the world (address reuse).
    for (auto it = s_botVisualEvicted.begin(); it != s_botVisualEvicted.end(); ) {
        if (!currentActorSet.contains(it->first))
            it = s_botVisualEvicted.erase(it);
        else
            ++it;
    }
    // #region agent log
    for (auto it = s_botCandFirstSeen.begin(); it != s_botCandFirstSeen.end(); ) {
        if (!currentActorSet.contains(it->first))
            it = s_botCandFirstSeen.erase(it);
        else
            ++it;
    }
    // #endregion

    // C11: this pass's GATE budget. The scanner fleet shares one DMA bus and
    // a turn longer than the 200ms cadence delays every other scanner (and
    // RobotList's own next turn — SyncedThread paces interval after the turn
    // ends). Admission's stage budgets total 190ms; this cap mainly truncates
    // the retain tail. Retain runs nearest-first, so the bots actually on
    // screen keep their Drawing flags first.
    TraceRobotPass(2, currentActors.size());
    WorldScan::ScanBudget turnBudget(std::chrono::milliseconds(300));

    const bool doAdmission = true;

    int dbgScanned = 0;
    int dbgAdmitted = 0;
    int dbgZeroPos = 0;
    int dbgDrawing = 0;
    int dbgVisSkip = 0;
    int dbgDistSkip = 0;
    int dbgStructHit = 0;
    int dbgQuickPass = 0;
    int dbgVerifyFail = 0;
    int dbgReEvict = 0;
    int dbgBrokenSkip = 0;
    int dbgAdmitAnyBot = 0;
    int dbgFailNoRoot = 0;
    int dbgFailNoMesh = 0;
    int dbgFailNoId = 0;
    int dbgFailOther = 0;

    // Why an actor left the admission lane without entering the cache. The
    // funnel previously reported only verifyFail plus the fail* sub-gates, so
    // every stage-1 settle (props, cached players, undecrypted spawns) was
    // invisible in the totals and entirely invisible per actor -- which is why
    // "75 enemies, 22 cached" could not be attributed to anything.
    int dbgRejPlayerClass = 0;
    int dbgRejLocalPs = 0;
    int dbgRejCachedPlayer = 0;
    int dbgRejWorldItem = 0;
    int dbgRejFnameNeg = 0;
    int dbgRejUndecrypted = 0;
    int dbgRejVerifyMemo = 0;
    int dbgRejVerifyTransient = 0;
    int dbgRejVerifyExpired = 0;
    // Per-pass per-actor trace budget. Counters above are unaffected by it.
    int rejectLogBudget = 24;

    // P6/P6b (Bukupex admission batching + 4-slice ring): batch the struct
    // fields both gates need, but only for one rotating band of the actor
    // array per pass (~1/4 DMA) plus a bounded newly-seen priority set.
    // Full prune + retain still cover the whole world every pass.
    struct AdmitProbeRow {
        uintptr_t actor = 0;
        uint32_t typeId = 0;
        uint64_t playerState = 0;
        uint64_t enemyDa = 0;
        uint64_t aiTemplate = 0;
        uint64_t targetFlag = 0;
        uint64_t itemDa = 0;
        uint64_t hover = 0;
        uint64_t containerLoot = 0;
        uintptr_t root = 0;
        double distSq = 0.0;
        bool hasDist = false;
    };

    std::vector<uintptr_t> admitCandidates;
    int dbgAdmitScatterExecs = 0;
    int dbgAdmitStructCand = 0;
    int dbgAdmitFnameChecked = 0;
    int dbgAdmitFnameMemoSkip = 0;
    int dbgAdmitPrioNew = 0;
    // The priority lane by class (Core/BotAdmitQueue.hpp): prioSight is
    // never-probed work, prioOverdue is never-probed work past the bound (it
    // preempts every retry), prioRetry is actors that already had a probe.
    int dbgAdmitPrioOverdue = 0;
    int dbgAdmitPrioFirstSight = 0;
    int dbgAdmitPrioRetry = 0;
    int dbgAdmitSliceActors = 0;
    size_t dbgAdmitSlice = 0;
    size_t dbgAdmitSliceBase = 0;
    size_t dbgAdmitSliceEnd = 0;
    size_t dbgAdmitN = 0;
    bool admitRingAdvanceOk = false;

    // LAG1: cap this pass's DMA. Camera @8ms, position @16ms and the
    // frame-builder bone reads @12ms share the one DMA link; a scanner that
    // hogs it starves them all and bones lag behind moving targets.
    WorldScan::ScanBudget scanBudget(std::chrono::milliseconds(90));
    bool slicePartial = false;

    // Retain must never be starved by admission: the retain loop is what
    // sets Drawing=true for cached bots (the ESP itself), while admission is
    // background discovery. Retain gets its OWN budget created FRESH right
    // before the retain loop (debug-c190fb: drawing:0, visSkip:0, distSkip:0
    // while sceneOk:46 = retain never ran). It must NOT be created here at
    // the top: admission's scatters + serial loop burn 90-150ms, so a budget
    // created before them is already expired when the retain loop checks it
    // (drawing stays 0 even with a full cache).

    if (doAdmission) {
        // Resolve the runtime actor-type offset once (serial) so the batch
        // reads the same field ReadActorTypeId uses.
        std::ptrdiff_t typeOff = ArcActorType::RuntimeActorTypeOffset();
        if (typeOff < 0) {
            for (size_t i = 0; i < currentActors.size() && typeOff < 0; ++i) {
                if (currentActors[i])
                    (void)ArcActorType::ReadActorTypeId(
                        static_cast<uintptr_t>(currentActors[i]));
                typeOff = ArcActorType::RuntimeActorTypeOffset();
            }
            if (typeOff < 0)
                typeOff = Offsets::ActorTypeId;
        }

        // Prune fname-negative memos for actors that left the world.
        for (auto it = s_quickFnameNeg.begin(); it != s_quickFnameNeg.end(); ) {
            if (!currentActorSet.contains(it->first))
                it = s_quickFnameNeg.erase(it);
            else
                ++it;
        }
        // B1 (Fix #8): same pruning for verify-fail memos so a reused actor
        // address never suppresses a real bot spawn.
        for (auto it = s_botVerifyNeg.begin(); it != s_botVerifyNeg.end(); ) {
            if (!currentActorSet.contains(it->first))
                it = s_botVerifyNeg.erase(it);
            else
                ++it;
        }
        s_admitQueue.Prune(
            [&](uint64_t a) { return currentActorSet.contains(a); });

        // Cheap CPU index of valid non-local actors. DMA only hits the slice.
        std::vector<uintptr_t> admitIndex;
        admitIndex.reserve(currentActors.size());
        for (uint64_t actorU64 : currentActors) {
            const uintptr_t actor = static_cast<uintptr_t>(actorU64);
            if (!actor || !engine.IsValidPointer(actor))
                continue;
            if (sAcknowledgedPawn && actor == sAcknowledgedPawn)
                continue;
            admitIndex.push_back(actor);
        }

        const size_t N = admitIndex.size();
        dbgAdmitN = N;

        // Reset ring on world gen / actor-array identity / large N jumps.
        bool ringReset = false;
        if (s_admitRingGen != genAtStart) {
            ringReset = true;
        } else if (ctx.actors != 0 && s_admitRingActorsPtr != 0
            && ctx.actors != s_admitRingActorsPtr) {
            ringReset = true;
        } else if (s_admitRingActorCount != 0) {
            const size_t delta = (N > s_admitRingActorCount)
                ? (N - s_admitRingActorCount)
                : (s_admitRingActorCount - N);
            const size_t thresh = (std::max)(static_cast<size_t>(64), N / 8);
            if (delta > thresh)
                ringReset = true;
        }
        if (ringReset) {
            s_admitSliceCursor = 0;
            s_admitResumeRow = 0;
            s_admitVerifyBacklog.clear();
            s_admitCoveredMask = 0;
            s_admitPrevActors.clear();
            // A world-generation change invalidates every pointer; a plain
            // ring reset (N jump / array identity) must KEEP the pending lane
            // — its actors are still unclassified and would be lost again.
            if (s_admitRingGen != genAtStart)
                s_admitQueue.Clear();
            s_admitCycleStart = std::chrono::steady_clock::now();
            s_admitLastCycleMs = 0;
            ++s_admitRingEpoch;
            ++s_admitRingResets;
        }
        s_admitRingGen = genAtStart;
        s_admitRingActorsPtr = ctx.actors;
        s_admitRingActorCount = N;
        if (s_admitCycleStart.time_since_epoch().count() == 0)
            s_admitCycleStart = std::chrono::steady_clock::now();

        // Everything in this pass is stamped from one clock so the lane can age
        // its rows against the same instant the probes are measured against.
        const uint64_t admitNowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        // C10 pending lane: every newly-seen actor joins the queue and STAYS
        // until a definitive outcome (admitted / proven non-bot / try budget
        // spent / left the world). The one-shot diff lost actors that missed
        // their single priority window to an up-to-19s ring sweep.
        for (uintptr_t actor : admitIndex) {
            if (s_admitQueue.Contains(actor))
                continue;
            if (localCache.contains(actor) || s_admitPrevActors.contains(actor))
                continue;
            // Fresh definitive rejects don't re-queue — their TTL memo owns
            // the retry policy.
            if (HasFreshQuickFnameNeg(actor) || HasFreshBotVerifyNeg(actor))
                continue;
            // True first sight — the discovery funnel clock starts here
            // (bot_admit / bot_appear telemetry), and the lane's own first-probe
            // clock with it.
            s_botCandFirstSeen.try_emplace(actor, std::chrono::steady_clock::now());
            s_admitQueue.NoteNew(actor, admitNowMs);
        }

        const size_t slice = s_admitSliceCursor % kAdmitSlices;
        const size_t sliceBase = (N * slice) / kAdmitSlices;
        const size_t sliceEnd = (N * (slice + 1)) / kAdmitSlices;
        dbgAdmitSlice = slice;
        dbgAdmitSliceBase = sliceBase;
        dbgAdmitSliceEnd = sliceEnd;

        // C10: priority rows come from the PENDING QUEUE (not a one-shot
        // diff), and they are collected FIRST so the band below can tell which
        // of its own rows the lane already took this pass. The lane orders them
        // by AGE (Core/BotAdmitQueue.hpp): a never-probed actor past
        // kFirstProbeBoundMs leads, never-probed work keeps a reserved share of
        // the pass, and retries take the rest. The old retry-first collection
        // let a retry storm (a spawn that fails transiently for the whole 45s
        // streaming window) consume the pass, so a brand-new actor could not be
        // probed until the retries settled.
        std::vector<AdmitProbeRow> prioRows;
        std::unordered_set<uintptr_t> laneSet;
        {
            AdmitQueue::PendingQueue::PassPlan plan;
            plan.maxRows = (std::min)(s_admitQueue.Cap(), kAdmitPrioPerPass);
            std::vector<uint64_t> prioActors;
            prioActors.reserve(plan.maxRows);
            s_admitQueue.CollectPrio(
                admitIndex.begin(), admitIndex.end(), admitNowMs, plan, prioActors);
            prioRows.reserve(prioActors.size());
            laneSet.reserve(prioActors.size());
            for (uint64_t actorU64 : prioActors) {
                const uintptr_t actor = static_cast<uintptr_t>(actorU64);
                switch (s_admitQueue.Classify(actor, admitNowMs)) {
                case AdmitQueue::PendingQueue::Lane::OverdueFirst:
                    ++dbgAdmitPrioOverdue;
                    break;
                case AdmitQueue::PendingQueue::Lane::FirstSight:
                    ++dbgAdmitPrioFirstSight;
                    break;
                case AdmitQueue::PendingQueue::Lane::Retry:
                    ++dbgAdmitPrioRetry;
                    break;
                }
                laneSet.insert(actor);
                AdmitProbeRow row{};
                row.actor = actor;
                prioRows.push_back(row);
            }
        }
        dbgAdmitPrioNew = static_cast<int>(prioRows.size());

        // ── Proximity-first window ───────────────────────────────────────────
        // The pending lane and the band both select by AGE and RING POSITION, so
        // a bot that just streamed in beside the player (running into a new
        // area) waits for a lane slot or a full ring rotation regardless of how
        // close it is. This reads a small rotating window of unadmitted actors,
        // orders it nearest-first, and lets those rows lead the pass — so the
        // closest new-area bot is screened first instead of last. Bounded: 96
        // rows, two 48-row scatters.
        std::vector<uintptr_t> proxActors;
        if (distRefOk && admitIndex.size() > 4) {
            constexpr size_t kProxWindow = 96;
            constexpr size_t kProxKeep = 32;
            constexpr size_t kProxChunk = 48;
            // Own budget: the proximity window must not spend the probe budget
            // the band and the lane need to finish their own sweep.
            WorldScan::ScanBudget proxBudget(std::chrono::milliseconds(25));
            static size_t s_proxCursor = 0;
            const size_t n = admitIndex.size();
            std::vector<uintptr_t> win;
            win.reserve(kProxWindow);
            for (size_t i = 0; i < n && win.size() < kProxWindow; ++i) {
                const uintptr_t actor = admitIndex[(s_proxCursor + i) % n];
                if (!actor || localCache.contains(actor))
                    continue;
                if (HasFreshQuickFnameNeg(actor) || HasFreshBotVerifyNeg(actor))
                    continue;
                win.push_back(actor);
            }
            s_proxCursor = (s_proxCursor + kProxWindow) % n;
            if (!win.empty()) {
                std::vector<uintptr_t> roots(win.size(), 0);
                // Identity reads travel with the proximity scatter: without them
                // the nearest rows would just be nearby scenery, and props would
                // spend the pass that a close bot needs.
                std::vector<uint32_t> typeIds(win.size(), 0);
                std::vector<uint64_t> itemDas(win.size(), 0);
                std::vector<uint64_t> hovers(win.size(), 0);
                for (size_t b = 0; b < win.size(); b += kProxChunk) {
                    if (proxBudget.expired())
                        break;
                    const size_t e = (std::min)(b + kProxChunk, win.size());
                    ScatterSession rs;
                    if (!rs.isValid())
                        break;
                    bool ok = true;
                    for (size_t i = b; i < e; ++i) {
                        ok = rs.prepare(win[i] + Offsets::RootComponent, roots[i]) && ok;
                        ok = rs.prepare(win[i] + typeOff, typeIds[i]) && ok;
                        ok = rs.prepare(win[i] + static_cast<uint64_t>(Offsets::ItemDataAsset),
                                 itemDas[i]) && ok;
                        ok = rs.prepare(win[i] + Offsets::UIHoverData, hovers[i]) && ok;
                    }
                    if (!(ok && rs.execute()))
                        for (size_t i = b; i < e; ++i)
                            roots[i] = 0;
                    std::this_thread::yield();
                }
                std::vector<Engine::FVector3d> locs(win.size());
                std::vector<char> locOk(win.size(), 0);
                for (size_t b = 0; b < win.size(); b += kProxChunk) {
                    if (proxBudget.expired())
                        break;
                    const size_t e = (std::min)(b + kProxChunk, win.size());
                    ScatterSession ls;
                    if (!ls.isValid())
                        break;
                    bool ok = true;
                    for (size_t i = b; i < e; ++i) {
                        if (!roots[i])
                            continue;
                        ok = ls.prepare(roots[i] + Offsets::WorldLocation, locs[i]) && ok;
                    }
                    if ((ok && ls.execute()))
                        for (size_t i = b; i < e; ++i)
                            if (roots[i])
                                locOk[i] = 1;
                    std::this_thread::yield();
                }
                std::vector<std::pair<double, uintptr_t>> ranked;
                ranked.reserve(win.size());
                for (size_t i = 0; i < win.size(); ++i) {
                    if (!roots[i] || !locOk[i])
                        continue;
                    // Definite non-bots never take a proximity slot: players,
                    // chests, ground loot, and anything carrying world-item
                    // structure (item data asset / UI hover).
                    const uint32_t masked = ArcActorType::MaskActorTypeId(typeIds[i]);
                    if (ArcActorType::IsPlayerClassId(masked)
                        || ArcActorType::IsChestClassId(masked)
                        || ArcActorType::IsGroundLootClassId(masked))
                        continue;
                    if (itemDas[i] != 0 || hovers[i] != 0)
                        continue;
                    const Vector3 p = Engine::ToVector3(locs[i]);
                    if (!IsPlausibleWorldPos(p))
                        continue;
                    const double dx = p.x - localPos.x;
                    const double dy = p.y - localPos.y;
                    const double dz = p.z - localPos.z;
                    ranked.emplace_back(dx * dx + dy * dy + dz * dz, win[i]);
                }
                std::sort(ranked.begin(), ranked.end(),
                    [](const std::pair<double, uintptr_t>& a,
                       const std::pair<double, uintptr_t>& b) {
                        return a.first < b.first;
                    });
                const size_t keep = (std::min)(kProxKeep, ranked.size());
                proxActors.reserve(keep);
                for (size_t i = 0; i < keep; ++i)
                    proxActors.push_back(ranked[i].second);
            }
        }
        // Lead the pass with the nearest new-area actors. Rows the lane already
        // took stay where the lane put them; prox actors join laneSet so the
        // band below cannot double-probe them.
        if (!proxActors.empty()) {
            std::vector<AdmitProbeRow> proxRows;
            proxRows.reserve(proxActors.size());
            for (const uintptr_t a : proxActors) {
                if (laneSet.count(a) != 0)
                    continue;
                AdmitProbeRow row{};
                row.actor = a;
                proxRows.push_back(row);
                laneSet.insert(a);
            }
            prioRows.insert(prioRows.begin(), proxRows.begin(), proxRows.end());
        }

        // B4 (RIVENTIDES resume-band): stable deterministic row order
        // (admitIndex order) so the cross-pass resume frontier points at the
        // same rows — unordered_set iteration reorders each pass.
        //
        // The pending lane owns unclassified actors (probed first, every pass,
        // until they settle) — never let the band double-probe them. Letting the
        // band also carry queued actors the lane did not take was tried on a cold
        // load and measured at 178 first probes/s against 193/s without it —
        // inside this lane's run-to-run spread, so the measurement alone does not
        // condemn it. The MECHANISM does: band rows are scattered after the
        // lane's, and the 40ms process budget is already spent by the time it
        // reaches them, so a queued actor carried by the band costs scatter time
        // and is then dropped unscreened. That is DMA taken from the lane to no
        // end, so the band keeps its old rule.
        std::unordered_set<uintptr_t> bandSet;
        bandSet.reserve(sliceEnd - sliceBase);
        std::vector<AdmitProbeRow> bandRows;
        bandRows.reserve(sliceEnd - sliceBase);
        for (size_t i = sliceBase; i < sliceEnd; ++i) {
            const uintptr_t actor = admitIndex[i];
            // Cached bots are owned by retain + PositionRefreshPass.
            if (localCache.contains(actor))
                continue;
            // The lane took this one this pass (see the measurement above: the
            // band must not spend rows on queued actors).
            if (laneSet.count(actor) != 0)
                continue;
            if (s_admitQueue.Contains(actor))
                continue;
            if (!bandSet.insert(actor).second)
                continue;
            AdmitProbeRow row{};
            row.actor = actor;
            bandRows.push_back(row);
        }
        dbgAdmitSliceActors = static_cast<int>(bandRows.size());

        // Frontier: continue the band where the last pass stopped instead of
        // restarting at row 0 every pass (stuck slice / cache:0 deadlock on
        // 16K-actor maps). Prio rows lead so brand-new spawns are still probed
        // every pass.
        const size_t resume = (std::min)(s_admitResumeRow, bandRows.size());
        std::vector<AdmitProbeRow> probeRows;
        probeRows.reserve(prioRows.size() + (bandRows.size() - resume));
        probeRows.insert(probeRows.end(), prioRows.begin(), prioRows.end());
        if (resume < bandRows.size()) {
            probeRows.insert(
                probeRows.end(),
                bandRows.begin() + static_cast<std::ptrdiff_t>(resume),
                bandRows.end());
        }

        // RIVENTIDES: 48 rows x 9 preps = ~430 preps per exec. With the DMA
        // bus saturated by the 16K-actor world scanners, a 128-row chunk's
        // single execute blew the whole 90ms budget, the process loop then
        // broke instantly (slicePartial) and the ring stayed on one slice
        // forever -> cache:0, zero bots. Small chunks keep every execute
        // short even on a loaded bus.
        constexpr size_t kAdmitChunk = 48;
        // Own probe budget: the prep above (index build, actor-type reads) must
        // not consume the probe scatter's time. Same fix as the items walk.
        WorldScan::ScanBudget probeBudget(std::chrono::milliseconds(90));
        size_t base = 0;
        for (; base < probeRows.size(); base += kAdmitChunk) {
            if (probeBudget.expired()) { slicePartial = true; break; }
            const size_t end = (std::min)(base + kAdmitChunk, probeRows.size());
            ScatterSession scatter;
            if (!scatter.isValid())
                break;
            bool prepOk = true;
            for (size_t i = base; i < end; ++i) {
                AdmitProbeRow& r = probeRows[i];
                prepOk = scatter.prepare(r.actor + typeOff, r.typeId) && prepOk;
                prepOk = scatter.prepare(r.actor + Offsets::APlayerState, r.playerState) && prepOk;
                prepOk = scatter.prepare(
                             r.actor + Offsets::Constructable_EnemyTypeDataAsset, r.enemyDa)
                    && prepOk;
                prepOk = scatter.prepare(
                             r.actor + Offsets::Constructable_AITemplateData, r.aiTemplate)
                    && prepOk;
                prepOk = scatter.prepare(r.actor + 0x1D0, r.targetFlag) && prepOk;
                prepOk = scatter.prepare(
                             r.actor + static_cast<uint64_t>(Offsets::ItemDataAsset), r.itemDa)
                    && prepOk;
                prepOk = scatter.prepare(
                             r.actor + static_cast<uint64_t>(Offsets::UIHoverData), r.hover)
                    && prepOk;
                prepOk = scatter.prepare(
                             r.actor + Offsets::LootInteraction_Container, r.containerLoot)
                    && prepOk;
                prepOk = scatter.prepare(r.actor + Offsets::RootComponent, r.root) && prepOk;
            }
            if (prepOk && scatter.execute())
                ++dbgAdmitScatterExecs;
            // Short batches + yield so the 8ms camera thread keeps cadence.
            std::this_thread::yield();
        }

        // B5: only rows in [0, scatteredEnd) were scattered THIS pass and may
        // be screened; rows beyond still hold zero buffers and must never hit
        // the fname memo (mass negative-memoization = permanent blindness).
        // The resume frontier itself is computed AFTER screening from
        // processEnd so a screening break cannot rewind the band (Stage 1).
        const size_t scatteredEnd = (std::min)(base, probeRows.size());

        // Distance-priority admission (same as EntityList player path): scatter
        // each row's root WorldLocation and sort the slice nearest-first so a
        // budget-clipped pass still admits the bots closest to the player. Without
        // it, admit order is actor-array order and nearby bots can wait multiple
        // ring sweeps.
        // Only a fully-scattered sweep may distance-sort: a partial pass must
        // keep [0, scatteredEnd) order aligned with the resume frontier.
        if (scatteredEnd >= probeRows.size() && !probeRows.empty()) {
            const uintptr_t lroot = sAcknowledgedPawn
                ? Memory::read<uintptr_t>(sAcknowledgedPawn + Offsets::RootComponent)
                : 0;
            Vector3 localPos{};
            if (lroot && engine.IsValidPointer(lroot))
                localPos = Engine::ReadWorldLocationNocache(lroot, /*allowRelativeFallback=*/true);
            if (IsPlausibleWorldPos(localPos)) {
                std::vector<Engine::FVector3d> locBuf(probeRows.size());
                for (size_t distBase = 0; distBase < probeRows.size(); distBase += kAdmitChunk) {
                    if (scanBudget.expired())
                        break;
                    const size_t distEnd = (std::min)(distBase + kAdmitChunk, probeRows.size());
                    ScatterSession distScatter;
                    if (!distScatter.isValid())
                        break;
                    bool distPrepOk = true;
                    for (size_t i = distBase; i < distEnd; ++i) {
                        AdmitProbeRow& r = probeRows[i];
                        if (!r.root) {
                            distPrepOk = false;
                            continue;
                        }
                        distPrepOk = distScatter.prepare(r.root + Offsets::WorldLocation, locBuf[i])
                            && distPrepOk;
                    }
                    if (distPrepOk && distScatter.execute())
                        ++dbgAdmitScatterExecs;
                    std::this_thread::yield();
                }
                for (size_t i = 0; i < probeRows.size(); ++i) {
                    AdmitProbeRow& r = probeRows[i];
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
                    [](const AdmitProbeRow& a, const AdmitProbeRow& b) {
                        if (a.hasDist != b.hasDist)
                            return a.hasDist > b.hasDist;
                        return a.distSq < b.distSq;
                    });
            }
        }

        // Bots get PlayerStates from AI controllers — only the local player's
        // own PS is a hard skip here; players are otherwise caught by class
        // id and IsCachedPlayer below. Reinforcement bots (Snitch summons)
        // spawn with a valid PS and must not be skipped.
        const uintptr_t localPs = sAcknowledgedPawn
            ? Memory::read<uintptr_t>(sAcknowledgedPawn + Offsets::APlayerState)
            : 0;
        admitCandidates.reserve(64);
        // Fresh micro-budget: the probe scatter may have consumed the pass
        // budget under DMA load, but rows that WERE scattered must still be
        // processed this pass or admission starves (stuck ring, cache:0).
        // Only freshly-scattered rows [0, scatteredEnd) are processed.
        WorldScan::ScanBudget procBudget(std::chrono::milliseconds(40));
        size_t processEnd = 0;
        for (size_t ri = 0; ri < scatteredEnd; ++ri) {
            const AdmitProbeRow& r = probeRows[ri];
            if (procBudget.expired()) { slicePartial = true; break; }
            processEnd = ri + 1;
            // First-probe stopwatch: a row that reached screening has been
            // probed. Only queued actors can be in the lane (band rows skip
            // them), and NoteProbed records the wait once per actor — this is
            // the measurement the first-probe bound is judged by.
            if (s_admitQueue.Contains(r.actor))
                s_admitQueue.NoteProbed(r.actor, admitNowMs);
            const uint32_t masked = ArcActorType::MaskActorTypeId(r.typeId);
            // Counted, not logged: a player is never the actor this funnel is
            // asked about, and the band scans thousands of them per pass.
            if (ArcActorType::IsPlayerClassId(masked)) {
                ++dbgRejPlayerClass;
                s_admitQueue.Settle(r.actor);
                continue;
            }
            if (r.playerState != 0 && Memory::IsValidPtrFast2(r.playerState)
                && r.playerState == localPs) {
                ++dbgRejLocalPs;
                s_admitQueue.Settle(r.actor);
                continue;
            }
            if (engine.IsCachedPlayer(r.actor)) {
                ++dbgRejCachedPlayer;
                s_admitQueue.Settle(r.actor);
                continue;
            }

            const bool structCand = ArcActorType::IsBotClassId(masked)
                || (masked == static_cast<uint32_t>(ArcActorType::EActorType::EACTOR_TARGET)
                    && r.targetFlag == 1)
                || (r.enemyDa != 0 && Memory::IsValidPtrFast2(r.enemyDa))
                || (r.aiTemplate != 0 && Memory::IsValidPtrFast2(r.aiTemplate));

            if (structCand) {
                ++dbgAdmitStructCand;
            } else {
                // World-item structure (batched HasWorldItemStructure): never a bot.
                if ((r.itemDa != 0 && Memory::IsValidPtrFast2(r.itemDa))
                    || (r.hover != 0 && Memory::IsValidPtrFast2(r.hover))
                    || (r.containerLoot != 0
                        && engine.IsValidPointer(static_cast<uintptr_t>(r.containerLoot)))) {
                    ++dbgRejWorldItem;
                    // Prop-shaped, but the same row carries an enemy data asset
                    // or an AI template pointer: that is the one shape where this
                    // screen can drop a real bot, so it gets a named line.
                    if (r.enemyDa != 0 || r.aiTemplate != 0)
                        LogBotAdmitReject(r.actor, "stage1_world_item_structure",
                            engine.GetActorFNameStringCached(r.actor), rejectLogBudget,
                            false, false, r.root != 0, false, false, false,
                            r.enemyDa != 0, r.aiTemplate != 0,
                            r.hasDist ? std::sqrt(r.distSq) / 100.0 : -1.0);
                    s_admitQueue.Settle(r.actor);
                    continue;
                }
                if (!QuickBotFnameCandidateMemo(
                        r.actor, dbgAdmitFnameChecked, dbgAdmitFnameMemoSkip)) {
                    // Settled negatives leave the lane; undecrypted spawns
                    // (no memo written) stay queued and retry next pass.
                    if (HasFreshQuickFnameNeg(r.actor)) {
                        ++dbgRejFnameNeg;
                        // The name is why it was dropped, so print it: this is
                        // the screen that decides "not a bot" from a string, and
                        // a wrong verdict here looks exactly like a missing bot.
                        LogBotAdmitReject(r.actor, "stage1_fname_not_botlike",
                            engine.GetActorFNameStringCached(r.actor), rejectLogBudget,
                            false, false, r.root != 0, false, false, false,
                            r.enemyDa != 0, r.aiTemplate != 0,
                            r.hasDist ? std::sqrt(r.distSq) / 100.0 : -1.0);
                        s_admitQueue.Settle(r.actor);
                        continue;
                    }
                    // Undecrypted name with no memo: the lane has no try budget
                    // here, so a never-decrypting actor sat in the priority lane
                    // forever and displaced new bots (the band ring skips queued
                    // actors). Release it once the streaming window has passed.
                    if (const auto fsIt = s_botCandFirstSeen.find(r.actor);
                        fsIt != s_botCandFirstSeen.end()
                        && std::chrono::steady_clock::now() - fsIt->second
                            >= std::chrono::milliseconds(kBotTransientWindowMs)) {
                        ++dbgRejUndecrypted;
                        LogBotAdmitReject(r.actor, "stage1_name_never_decrypted",
                            engine.GetActorFNameStringCached(r.actor), rejectLogBudget,
                            false, false, r.root != 0, false, false, false,
                            r.enemyDa != 0, r.aiTemplate != 0,
                            r.hasDist ? std::sqrt(r.distSq) / 100.0 : -1.0);
                        s_admitQueue.Settle(r.actor);
                    }
                    continue;
                }
            }
            admitCandidates.push_back(r.actor);
        }

        // B5 (Stage 1): monotonic resume frontier — computed AFTER screening so
        // a screening budget break persists its position instead of rewinding
        // the band. consumedAll (full scatter + full screen) resets the resume
        // and lets the ring advance; a sorted sweep that didn't finish replays
        // in distance order; anything else resumes from the screened prefix.
        {
            const size_t prioCount = prioRows.size();
            const bool sortedSweep = scatteredEnd >= probeRows.size();
            if (sortedSweep && processEnd == scatteredEnd) {
                s_admitResumeRow = 0;
            } else if (sortedSweep) {
                // probeRows were distance-sorted this pass; a broken sweep must
                // not map processEnd back onto the unsorted band order.
                s_admitResumeRow = 0;
                slicePartial = true;
            } else {
                const size_t bandProcessed =
                    (processEnd > prioCount) ? (processEnd - prioCount) : 0;
                s_admitResumeRow =
                    resume + (std::min)(bandProcessed, bandRows.size() - resume);
                slicePartial = true;
            }
        }

        // Snapshot current actors for next-pass new-actor diff. Cursor advances
        // only after gen-matched writeback so an aborted pass never skips a band.
        s_admitPrevActors.clear();
        s_admitPrevActors.insert(admitIndex.begin(), admitIndex.end());
        // A budgeted partial pass must not skip a ring band — the slice is
        // retried in full next pass instead (see the advance gate below).
        admitRingAdvanceOk = !slicePartial;
    }

    if (doAdmission) {
    // LAG1 (c190fb): the admission loop is SERIAL — each candidate does
    // fname + VerifyBotActor DMA. With ~1,000-1,500 candidates per slice
    // band it blows the 90ms shared budget before the retain loop runs,
    // so NO cached bot ever gets Drawing=true (drawing:0, visSkip:0,
    // distSkip:0 while sceneOk:46). Cap admission and let retain breathe.
    // Fresh budget HERE, not the shared scanBudget: the probe + distance
    // scatters above already consume the whole 90ms, so checking
    // scanBudget.expired() here broke on the first candidate every pass
    // (quickPass:0, cache drained to 0, zero bot ESP).
    // Raid-start mode: deep backlog + near-empty cache means there is nothing
    // to retain yet, so spend more bus time admitting — 60ms paced only
    // ~35-50 verifies/pass and took 62-78s to drain a 1000-actor backlog.
    // Tapers to 60ms as soon as the cache fills (LAG1 guard: retain must
    // keep Drawing flags fresh once bots are cached).
    const bool raidStartMode =
        s_admitQueue.Size() > 256 && localCache.size() < 8;
    WorldScan::ScanBudget admitBudget(
        std::chrono::milliseconds(raidStartMode ? 150 : 60));
    // B5 (Stage 2): verify is best-effort and never gates ring advance. On a
    // budget overrun the tail re-enters via s_admitVerifyBacklog next pass;
    // new spawns are unaffected (they are re-listed by the prio diff).
    std::vector<uintptr_t> pending;
    pending.reserve(s_admitVerifyBacklog.size() + admitCandidates.size());
    pending.insert(pending.end(), admitCandidates.begin(), admitCandidates.end());
    pending.insert(pending.end(), s_admitVerifyBacklog.begin(), s_admitVerifyBacklog.end());
    s_admitVerifyBacklog.clear();
    size_t vi = 0;
    for (; vi < pending.size(); ++vi)
    {
        if (admitBudget.expired()) {
            const size_t rest = pending.size() - vi;
            if (rest <= kAdmitVerifyBacklogMax)
                s_admitVerifyBacklog.assign(
                    pending.begin() + static_cast<std::ptrdiff_t>(vi), pending.end());
            else
                s_admitVerifyBacklog.assign(
                    pending.end() - static_cast<std::ptrdiff_t>(kAdmitVerifyBacklogMax),
                    pending.end());
            break;  // NO slicePartial here — verify must not stall the ring
        }
        const uintptr_t actor = pending[vi];
        ++dbgQuickPass;

        // #region agent log
        if (!localCache.contains(actor))
            s_botCandFirstSeen.try_emplace(actor, std::chrono::steady_clock::now());
        // #endregion

        std::string fname = engine.GetActorFNameStringCached(actor);
        if (fname.empty())
            fname = engine.GetActorFNameString(actor);

        // R4: polluted fnames (SpotAudioManager et al.) passed the struct
        // candidate probe via garbage data-asset pointers — block before verify.
        if (IsBotEspPollutionName(fname)) {
            s_admitQueue.Settle(actor);
            continue;
        }

        // Extraction hatches belong in the container (Loot) cache, not bots.
        // ContainerList admits them with WorldItemCategory::Hatch; admitting
        // them here too put a bot box + bot label on every extraction point.
        if (FnameLooksLikeExtractionHatch(fname)) {
            s_admitQueue.Settle(actor);
            continue;
        }

        // B1 (Fix #8): skip actors whose verify already failed recently.
        if (!localCache.contains(actor)) {
            if (HasFreshBotVerifyNeg(actor)) {
                // Fresh definitive reject — its TTL memo owns the retry. This is
                // the longest fixed invisibility in admission (10-17s), so it is
                // counted and named rather than left to the TTL to explain.
                ++dbgRejVerifyMemo;
                LogBotAdmitReject(actor, "verify_memo_fresh", fname, rejectLogBudget);
                s_admitQueue.Settle(actor);
                continue;
            }
            s_botVerifyNeg.erase(actor); // stale — recheck
        }

        // CHECK #1 � authoritative verification at admission. Only actors that
        // prove they are ARC bots (and not players/guns/items/containers) pass.
        if (!VerifyBotActor(actor, sAcknowledgedPawn, fname)) {
            ++dbgStructHit;
            if (!localCache.contains(actor)) {
                ++dbgVerifyFail;
                const bool anyBot = ArcActorType::IsAnyBotActor(actor);
                const bool strongDa = HasStrongEnemyDataAsset(actor);
                const bool hasRoot = ResolveBotSceneRoot(actor) != 0;
                const uintptr_t meshProbe = engine.GetActorSkeletalMesh(actor);
                const bool hasMesh = meshProbe && engine.IsValidPointer(meshProbe);
                const bool hasId = ActorHasKnownBotIdentity(actor, fname);
                if (!hasRoot)
                    ++dbgFailNoRoot;
                else if (!hasMesh && !anyBot)
                    ++dbgFailNoMesh;
                else if (!hasId && !anyBot && !strongDa)
                    ++dbgFailNoId;
                else
                    ++dbgFailOther;

                // Name the sub-gate. Which of the four it was is the whole
                // question: no_root / no_mesh are spawn-shape problems (worth a
                // retry), no_identity is the pollution guard doing its job, and
                // contradicted means component evidence says bot while verify
                // said no -- the shape that must never be memoized silently.
                LogBotAdmitReject(actor,
                    !hasRoot ? "verify_no_root"
                    : (!hasMesh && !anyBot) ? "verify_no_mesh"
                    : (!hasId && !anyBot && !strongDa) ? "verify_no_identity"
                    : "verify_contradicted",
                    fname, rejectLogBudget, anyBot, strongDa, hasRoot, hasMesh,
                    hasId, false);

                // Transient shape (spawn still streaming in — no root / no
                // mesh yet, or the name has not decrypted): keep it queued and
                // retrying for the whole STREAMING WINDOW instead of the old
                // 8-try budget that settled it as gone after ~8 passes.
                const bool transient = fname.empty() || !hasRoot || !hasMesh;
                if (transient) {
                    const auto fsIt = s_botCandFirstSeen.find(actor);
                    const uint64_t ageMs = (fsIt != s_botCandFirstSeen.end())
                        ? static_cast<uint64_t>(
                              std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now()
                                      - fsIt->second).count())
                        : 0ULL;
                    if (ageMs < kBotTransientWindowMs) {
                        ++dbgRejVerifyTransient;
                        LogBotAdmitReject(actor, "verify_transient_retry", fname,
                            rejectLogBudget, anyBot, strongDa, hasRoot, hasMesh,
                            hasId, false);
                        s_admitQueue.NoteTry(actor); // keeps retry-first ordering
                        continue; // still streaming — retried next pass
                    }
                    // Window expired without root+mesh+name ever appearing:
                    // the actor is gone. No memo — it was never proven a
                    // non-bot, so the band ring may re-check it.
                    ++dbgRejVerifyExpired;
                    LogBotAdmitReject(actor, "verify_window_expired", fname,
                        rejectLogBudget, anyBot, strongDa, hasRoot, hasMesh,
                        hasId, false);
                    s_admitQueue.Settle(actor);
                    continue;
                }
                // Definitive reject: fname+root+mesh all present but verify
                // still failed — memoize so the ring does not re-probe serially
                // every pass (TTL still re-checks).
                LogBotAdmitReject(actor, "verify_memoized_reject", fname,
                    rejectLogBudget, anyBot, strongDa, hasRoot, hasMesh, hasId,
                    true);
                s_admitQueue.Settle(actor);
                if (!fname.empty()) {
                    if (s_botVerifyNeg.size() > 16384)
                        s_botVerifyNeg.clear();
                    s_botVerifyNeg[actor] = std::chrono::steady_clock::now();
                }
            }
            continue;
        }
        s_botVerifyNeg.erase(actor);

        if (localCache.contains(actor)) {
            s_admitQueue.Settle(actor);
            continue;
        }

        // Evicted for visual loss: re-admit on live visual OR after the
        // cooldown. Without the cooldown a live bot with a flaky visual probe
        // stayed blocked forever (post-fix run: reEvict:2 pinned, user had
        // bots attacking at 2m with no ESP).
        if (const auto evIt = s_botVisualEvicted.find(actor);
            evIt != s_botVisualEvicted.end()) {
            const bool cooldownOver =
                std::chrono::steady_clock::now() - evIt->second >= kBotReAdmitCooldown;
            if (!cooldownOver
                && !HasLiveBotVisual(actor, engine.GetActorSkeletalMesh(actor))) {
                ++dbgReEvict;
                continue; // stays queued: retried until cooldown / live visual
            }
            s_botVisualEvicted.erase(evIt);
        }

        // Verified bot past every gate — the cache path owns it now.
        s_admitQueue.Settle(actor);
        ++dbgScanned;

        // Prefer a real bot name before the Constructable token — Esp skips draw
        // when ResolveBotDrawLabel stays empty for that token (c190fb H3).
        std::string itemName = ResolveStructBotAdmissionLabel(actor, fname);
        if (itemName.empty() || itemName == kBotStructAdmissionToken) {
            if (const std::string drawn = ResolveBotDrawLabel(actor, itemName, fname);
                !drawn.empty())
                itemName = drawn;
        }
        if (itemName.empty() || itemName == kBotStructAdmissionToken) {
            if (const std::string fromF = ResolveRobotTypeFromFName(engine, fname);
                !fromF.empty())
                itemName = fromF;
        }
        if (itemName.empty() || itemName == kBotStructAdmissionToken) {
            const std::string classFn = engine.GetActorClassFName(actor);
            if (const std::string fromClass = ResolveRobotTypeFromFName(engine, classFn);
                !fromClass.empty())
                itemName = fromClass;
            else if (const std::string tok = LookupBotClassToken(classFn); !tok.empty())
                itemName = tok;
        }
        if (itemName.empty() || itemName == kBotStructAdmissionToken) {
            if (const std::string fromEnemy = ResolveEnemyAssetBotLabel(actor);
                !fromEnemy.empty())
                itemName = fromEnemy;
        }
        // NEW bot types absent from every token map: derive a real name from
        // the actor's own enemy data asset and register it at runtime so this
        // bot draws (debug-c190fb: new bots were admitted then never drawn).
        if (itemName.empty() || itemName == kBotStructAdmissionToken
            || !IsAcceptedBotEspLabel(engine, itemName, fname)) {
            if (const std::string discovered = DiscoverNewBotType(actor, fname);
                !discovered.empty())
                itemName = discovered;
        }
        if (itemName.empty())
            itemName = kBotStructAdmissionToken;

        const uint8_t broken = ReadBotBrokenFlag(actor);
        if (broken != 0 && var::DeadBotsHidden()) {
            continue;
        }

        const int botCategory = 3;
        if (!getAllowType(itemName, botCategory))
            continue;

        const uintptr_t root = ResolveBotSceneRoot(actor);
        if (!root) {
            ++dbgZeroPos;
            continue;
        }

        const uintptr_t mesh = GetActorSkeletalMesh(actor);

        auto& entry = localCache.emplace(
            actor,
            WorldCacheEntry(itemName, root, actor, mesh)
        ).first->second;

        entry.ItemType = itemName;
        entry.ActorName = itemName;
        entry.IsBreaked = broken != 0;
        entry.botIdentityProven = true;
        entry.category = 3;
        entry.lastActorSeenMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (var::show_bot_loadout) {
            // Best-effort loadout readout (feature: loadout): bots that carry an
            // InventoryComponent resolve a weapon/armor line like players do;
            // plain ARC constructables leave every field empty.
            std::string wName, s0, s1, aName;
            int wq = -1, sq0 = -1, sq1 = -1, clip = 0, aTier = -1;
            float plates = 0.f, perPlate = 0.f;
            ReadPlayerInventory(actor, wName, wq, clip, s0, sq0, s1, sq1,
                plates, perPlate, aTier, aName);
            entry.weaponName = wName;
            entry.weaponQuality = wq;
            entry.weaponClip = clip;
            entry.armorTier = aTier;
            entry.armorName = aName;
        }
        if (var::show_bot_vision) {
            entry.visionValid = ReadBotVision(actor,
                entry.sightRadiusCm, entry.sightHalfAngleDeg,
                entry.alertness, entry.combatPhase);
            entry.facingYawDeg = static_cast<float>(
                Memory::read_nocache<double>(
                    root + Offsets::RelativeRotation + 0x8));
        }
        if (var::show_bot_parts) {
            entry.partHpCount = ReadBotPartHp(actor, entry.partHp,
                WorldCacheEntry::kMaxBotParts, entry.destroyedParts);
        } else {
            entry.partHpCount = 0;
            entry.destroyedParts = 0;
        }
        {
            // Group # for ESP: bots spawning near each other within seconds
            // (Snitch summons, patrols) get the same id.
            const Vector3 groupPos = ReadBotSceneWorldPosLive(root);
            entry.groupId = AssignBotGroup(actor, groupPos);
        }
        ++dbgAdmitted;

        // #region agent log
        {
            // bot_admit (verify log): the seen -> cached stage of the discovery
            // funnel. The old bot_admit_latency wrote to kArcDebugLogPath (NUL
            // — invisible) and only above 1500ms; this logs EVERY admission so
            // the funnel can be measured end to end with bot_appear.
            const auto admittedNow = std::chrono::steady_clock::now();
            const uint64_t admittedMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    admittedNow.time_since_epoch()).count());
            const auto seenIt = s_botCandFirstSeen.find(actor);
            uint64_t seenMs = 0;
            if (seenIt != s_botCandFirstSeen.end()) {
                seenMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        seenIt->second.time_since_epoch()).count());
                s_botCandFirstSeen.erase(seenIt);
            }
            entry.admittedMs = admittedMs;
            entry.firstSeenMs = seenMs ? seenMs : admittedMs;
            s_botDeadSince.erase(actor); // fresh admit = fresh life
            const long long admitMs =
                seenMs ? static_cast<long long>(admittedMs - seenMs) : -1;
            {
                char nameEsc[48]{};
                snprintf(nameEsc, sizeof(nameEsc), "%.40s", itemName.c_str());
                const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                std::ostringstream oss;
                oss << "{\"sessionId\":\"c190fb\",\"runId\":\"appear\","
                  << "\"location\":\"RobotList.cpp:RobotList\",\"message\":\"bot_admit\","
                  << "\"data\":{\"key\":" << actor
                  << ",\"name\":\"" << nameEsc
                  << "\",\"admitMs\":" << admitMs << "}"
                  << ",\"timestamp\":" << ts << "}\n";
                EntityDiagnostics::LogRaw(oss.str());
            }
        }
        // #endregion
        if (ArcActorType::IsAnyBotActor(actor))
            ++dbgAdmitAnyBot;

    }
    }

    // Bukupex-style P1: one scatter for all roots, one for WorldLocation+Relative
    // across the whole cache — replaces per-bot serial ResolveBotSceneRoot /
    // ReadBotSceneWorldPosLive that was starving the DMA bus.
    int dbgScenePosOk = 0;
    int dbgScenePosFail = 0;
    int dbgScenePosJump = 0;
    int dbgBatchRootHits = 0;
    int dbgBatchPosHits = 0;
    int dbgBatchPosAlt = 0;
    int dbgBatchVisFast = 0;
    int dbgBatchVisFallback = 0;
    int dbgBatchScatterExecs = 0;
    int dbgVerifyRan = 0;
    int dbgVerifySkipped = 0;
    int dbgPartCacheRan = 0;
    int dbgPartCacheSkipped = 0;
    std::unordered_map<uintptr_t, bool> batchLiveFast;
    batchLiveFast.reserve(localCache.size() * 2 + 8);

    struct BotRetainBatchRow {
        uintptr_t key = 0;
        uintptr_t root = 0;
        Engine::FVector3d world{};
        // Second ComponentToWorld candidate (0x310 + Transform::Translation).
        // Offsets::WorldLocation (0x2F0) is implausible on every bot root, so
        // without this the batch paid a serial NOCACHE ResolveBotWorldPos
        // fallback per bot — which is a large part of the 2.4s pass time.
        Engine::FVector3d worldAlt{};
        Vector3 relative{};
    };
    std::vector<BotRetainBatchRow> batchRows;
    batchRows.reserve(localCache.size());
    for (const auto& kv : localCache) {
        BotRetainBatchRow row{};
        row.key = kv.first;
        batchRows.push_back(row);
    }

    if (!batchRows.empty()) {
        {
            ScatterSession rootScatter;
            if (rootScatter.isValid()) {
                bool prepOk = true;
                for (auto& row : batchRows)
                    prepOk = rootScatter.prepare(
                                 row.key + Offsets::RootComponent, row.root)
                        && prepOk;
                if (prepOk && rootScatter.execute())
                    ++dbgBatchScatterExecs;
            }
        }
        for (auto& row : batchRows) {
            if (row.root && engine.IsValidPointer(row.root)) {
                ++dbgBatchRootHits;
            } else {
                row.root = ResolveBotSceneRoot(row.key);
                if (row.root && engine.IsValidPointer(row.root))
                    ++dbgBatchRootHits;
                else
                    row.root = 0;
            }
        }

        {
            ScatterSession posScatter;
            if (posScatter.isValid()) {
                bool prepOk = true;
                int prepared = 0;
                for (auto& row : batchRows) {
                    if (!row.root)
                        continue;
                    prepOk = posScatter.prepare(
                                 row.root + Offsets::WorldLocation, row.world)
                        && prepOk;
                    prepOk = posScatter.prepare(
                                 row.root + Offsets::ComponentToWorld_Alt
                                     + Offsets::Transform_Translation,
                                 row.worldAlt)
                        && prepOk;
                    prepOk = posScatter.prepare(
                                 row.root + Offsets::RelativeLocation, row.relative)
                        && prepOk;
                    ++prepared;
                }
                if (prepared > 0 && prepOk && posScatter.execute())
                    ++dbgBatchScatterExecs;
            }
        }

        for (const auto& row : batchRows) {
            auto it = localCache.find(row.key);
            if (it == localCache.end())
                continue;
            if (!row.root) {
                ++dbgScenePosFail;
                continue;
            }
            it->second.rootComponent = row.root;
            Vector3 scene = Engine::ToVector3(row.world);
            if (!IsPlausibleWorldPos(scene)) {
                const Vector3 alt = Engine::ToVector3(row.worldAlt);
                if (IsPlausibleWorldPos(alt)) {
                    scene = alt;
                    ++dbgBatchPosAlt;
                }
            }
            if (!IsPlausibleWorldPos(scene) && IsPlausibleWorldPos(row.relative))
                scene = row.relative;
            if (!IsPlausibleWorldPos(scene)) {
                scene = ResolveBotWorldPos(
                    row.key, row.root, it->second.Mesh);
            }
            if (IsPlausibleWorldPos(scene)) {
                // Same consistency gate the 16 ms sampler applies, for the same
                // reason: this is the bot sampler whenever PositionRefreshPass is
                // blocked, and a plausible-but-wrong vector committed here puts a
                // box kilometres away from the bot that owns it. Refusal keeps the
                // previous position and does not advance the stamps, so paint and
                // the expiry policy see "no new sample" rather than a bad one.
                {
                    const BotPosSanity::Sample newestNow{
                        it->second.WorldPos,
                        it->second.positionSampleMs,
                        IsPlausibleWorldPos(it->second.WorldPos)};
                    const BotPosSanity::Sample olderNow{
                        it->second.prevSampleWorldPos,
                        it->second.prevSampleMs,
                        it->second.prevSampleMs != 0
                            && IsPlausibleWorldPos(it->second.prevSampleWorldPos)};
                    const uint64_t judgeMs = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
                    const BotPosSanity::Verdict verdict = BotPosSanity::Judge(
                        newestNow, olderNow, scene, judgeMs, it->second.posJumpRejects);
                    if (!verdict.accept) {
                        if (it->second.posJumpRejects < 255)
                            ++it->second.posJumpRejects;
                        ++dbgScenePosJump;
                        continue;
                    }
                    it->second.posJumpRejects = 0;
                }
                // This is the bot sampler whenever PositionRefreshPass is
                // blocked, so it must maintain the sample pair paint interpolates
                // across (Core/BotMotion.hpp).
                if (it->second.positionSampleMs != 0) {
                    it->second.prevSampleWorldPos = it->second.WorldPos;
                    it->second.prevSampleMs = it->second.positionSampleMs;
                }
                it->second.WorldPos = scene;
                it->second.positionSampleMs = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
                ++dbgScenePosOk;
                ++dbgBatchPosHits;
                batchLiveFast[row.key] = true;
            } else {
                ++dbgScenePosFail;
            }
        }
    }

    // Retain loop budget: fresh HERE (after admission + P1 batch), so a heavy
    // admission pass can never starve the code that sets Drawing=true.
    // Raised from 60→200ms: with 50+ cached bots at ~3ms each, 60ms only
    // covered ~20 bots — the rest never got Drawing=true (drawing:11 with
    // cache:53, sceneOk:51, distSkip:0, visSkip:0).
    WorldScan::ScanBudget retainBudget(std::chrono::milliseconds(500));
    // Retain nearest-first: the budget caps how many bots the loop can
    // process per pass, and unordered_map iteration order is arbitrary but
    // deterministic — so with a fixed hash order the SAME far-away bots were
    // processed every pass (all distSkip) while nearby bots never got
    // Drawing=true (drawing:0 with a full cache). Sort by distance so the
    // bots actually on screen are processed first.
    // Liveness stamp for EVERY cached bot, not just the ones the budgeted loop
    // below reaches. That loop breaks at its 500ms budget, so bots past the cut
    // kept a stale lastActorSeenMs and the paint gate hid them as
    // actor_not_current (3s limit) even while still in the actor array. A set
    // lookup is CPU-only; no DMA.
    {
        const uint64_t seenNowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        for (auto& kv : localCache) {
            if (currentActorSet.contains(static_cast<uint64_t>(kv.first)))
                kv.second.lastActorSeenMs = seenNowMs;
        }
    }
    std::vector<uintptr_t> retainOrder;
    retainOrder.reserve(localCache.size());
    for (const auto& kv : localCache)
        retainOrder.push_back(kv.first);
    std::sort(retainOrder.begin(), retainOrder.end(),
        [&localCache, &cam](uintptr_t a, uintptr_t b) {
            const auto ia = localCache.find(a);
            const auto ib = localCache.find(b);
            const bool pa = ia != localCache.end()
                && IsPlausibleWorldPos(ia->second.WorldPos);
            const bool pb = ib != localCache.end()
                && IsPlausibleWorldPos(ib->second.WorldPos);
            if (pa != pb)
                return pa > pb;
            if (!pa)
                return false;
            const Vector3 da = ia->second.WorldPos - cam.Location;
            const Vector3 db = ib->second.WorldPos - cam.Location;
            return (da.x * da.x + da.y * da.y + da.z * da.z)
                < (db.x * db.x + db.y * db.y + db.z * db.z);
        });
    for (uintptr_t key : retainOrder)
    {
        auto it = localCache.find(key);
        if (it == localCache.end())
            continue;
        auto& actor = it->second;
        if (currentActorSet.contains(static_cast<uint64_t>(key))) {
            actor.lastActorSeenMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
        } else {
            // Do not refresh liveness for a cache entry that disappeared from
            // the current actor array. The frame gate will reject it once this
            // bounded grace expires instead of treating a stale root as live.
            // Keep the last sighting: the frame gate (kMaxActorSeenAgeMs) expires it after
            // the bounded grace. Zeroing here hid the bot on any single pass whose
            // actor-array read came back short — the on/off blink.
        }
        if (retainBudget.expired() || turnBudget.expired())
            break;

        std::string fname = engine.GetActorFNameStringCached(key);
        if (fname.empty())
            fname = engine.GetActorFNameString(key);

        if (IsBotEspPollutionName(fname) || IsBotEspPollutionLabel(actor.ActorName)) {
            ClearBotVisualMiss(key);
            it = localCache.erase(it);
            continue;
        }

        // Evict hatches that slipped into the bot cache before the admit-side
        // exclusion existed (or via any other path) — they are Loot-tab props.
        if (fname.find("Hatch") != std::string::npos
            || fname.find("hatch") != std::string::npos) {
            ClearBotVisualMiss(key);
            it = localCache.erase(it);
            continue;
        }

        if (actor.ActorName.empty()
            || actor.ActorName == kBotStructAdmissionToken
            || !IsAcceptedBotEspLabel(engine, actor.ActorName, fname)) {
            if (const std::string resolved =
                    ResolveBotDrawLabel(key, actor.ActorName, fname);
                !resolved.empty())
                actor.ActorName = resolved;
            else if (const std::string discovered = DiscoverNewBotType(key, fname);
                !discovered.empty())
                actor.ActorName = discovered;
        }

        // Constructable is only a temporary internal token. Do not retain it
        // as a renderable bot: this is the boundary that prevents props such as
        // Constructable actors from becoming ghost bots in the overlay.
        if (actor.ActorName.empty()
            || actor.ActorName == kBotStructAdmissionToken
            || !IsAcceptedBotEspLabel(engine, actor.ActorName, fname)
            || IsBotEspPollutionLabel(actor.ActorName)) {
            // Keep a positively classified bot with a temporary label. The
            // paint path will draw geometry as "Bot" but never Constructable;
            // unclassified props never reach this cache because admission and
            // VerifyBotActor are the proof boundary.
            if (!actor.botIdentityProven) {
                ClearBotVisualMiss(key);
                it = localCache.erase(it);
                continue;
            }
            actor.ActorName.clear();
        }

        // Live bot intel (features #7/#8): alertness + facing move fast, so the
        // retain loop refreshes the cheap vision bytes only while the feature is on.
        if (var::show_bot_vision) {
            uint8_t alertness = 0, combatPhase = 0;
            float radiusCm = 0.f, halfDeg = 0.f;
            if (ReadBotVision(key, radiusCm, halfDeg, alertness, combatPhase)) {
                actor.sightRadiusCm = radiusCm;
                actor.sightHalfAngleDeg = halfDeg;
                actor.alertness = alertness;
                actor.combatPhase = combatPhase;
                actor.visionValid = true;
            } else {
                actor.visionValid = false;
            }
            if (actor.rootComponent) {
                actor.facingYawDeg = static_cast<float>(
                    Memory::read_nocache<double>(
                        actor.rootComponent + Offsets::RelativeRotation + 0x8));
            }
        } else {
            actor.visionValid = false;
        }

        // CHECK #2 — Bukupex identity-once (P2): re-verify at most every 500ms
        // per bot. Between ticks trust cache membership + batched pos/liveness.
        static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
            s_lastBotVerify;
        static std::unordered_map<uintptr_t, std::chrono::steady_clock::time_point>
            s_lastBotPartCache;
        static std::unordered_map<uintptr_t, uintptr_t> s_lastBotPartMesh;
        // P2b: pass length is 600-900ms, so a 500ms cooldown never engaged
        // (verifySkipped stayed 0). Raise to 3s so identity re-verify is truly
        // deferred; position/liveness still refresh every pass via the P1 batch.
        constexpr auto kBotVerifyCooldown = std::chrono::milliseconds(3000);
        constexpr auto kBotPartCacheCooldown = std::chrono::milliseconds(3000);

        const auto nowRetain = std::chrono::steady_clock::now();
        bool needVerify = true;
        {
            const auto vit = s_lastBotVerify.find(key);
            if (vit != s_lastBotVerify.end()
                && nowRetain - vit->second < kBotVerifyCooldown)
                needVerify = false;
        }
        if (needVerify) {
            ++dbgVerifyRan;
            if (!VerifyBotActor(key, sAcknowledgedPawn, fname)) {
                ++dbgReEvict;
                // GRACE: transient DMA flaps false-negative the verify; an
                // instant erase blinks the bot for one frame then re-admits it
                // (evictReadmit flicker). Keep it cached with its last Drawing,
                // throttle re-verify, erase only after kBotVisualMissEvict
                // consecutive failures.
                if (!BotVisualMissShouldEvict(key, false)) {
                    s_lastBotVerify[key] = nowRetain;
                    ++it;
                    continue;
                }
                ClearBotVisualMiss(key);
                s_lastBotVerify.erase(key);
                s_lastBotPartCache.erase(key);
                s_lastBotPartMesh.erase(key);
                s_botVisualEvicted[key] = nowRetain;
                it = localCache.erase(it);
                continue;
            }
            s_lastBotVerify[key] = nowRetain;
            if (s_lastBotVerify.size() > 4096)
                s_lastBotVerify.clear();
        } else {
            ++dbgVerifySkipped;
        }

        if (!BotEspExpiry::ShouldRetainCachedBot(
                actor.botIdentityProven, getAllowType(actor.ActorName, 3))) {
            ClearBotVisualMiss(key);
            it = localCache.erase(it);
            continue;
        }

        // Prefer root filled by the P1 batch; cold-path resolve only if missing.
        if (!actor.rootComponent || !engine.IsValidPointer(actor.rootComponent))
            actor.rootComponent = ResolveBotSceneRoot(key);
        if (!actor.rootComponent) {
            // GRACE: root reads flap under DMA load; keep the bot cached
            // instead of blink-evicting it (evictReadmit flicker).
            if (!BotVisualMissShouldEvict(key, false)) {
                ++it;
                continue;
            }
            ClearBotVisualMiss(key);
            s_botVisualEvicted[key] = nowRetain;
            s_botLastGoodPos.erase(key);
            s_botDeadSince.erase(key);
            it = localCache.erase(it);
            continue;
        }

        // Broken flag read BEFORE the Drawing reset: a garbage/DMA-flapped read
        // must not blank the box for one frame then re-draw it (flicker).
        uint8_t broken = ReadBotBrokenFlag(key);
        // Dead-bot fix #2 (sticky death): once a corpse is stamped, it stays
        // dead until the corpse path expires it. A despawned actor's freed
        // memory can read "alive" on the next pass and would otherwise flip the
        // grey corpse back to a live-coloured box.
        if (s_botDeadSince.find(key) != s_botDeadSince.end())
            broken = 1;
        actor.broken = broken != 0; // published for DMA-free consumers (aim/paint)
        actor.brokenSampleMs = SteadyMsFrom(nowRetain);
        // Secondary SDK-backed death evidence. Probe at the HealthMs tolerance
        // rather than in the 16ms position pass; a single zero/partially-read
        // frame is never enough to remove a live flying bot. The interval and
        // the consumers' tolerance are the same number (FeaturePolicy), so a
        // reader that finds the value older than that knows the probe is behind
        // rather than silently trusting a stale health bar.
        {
            const auto probeIt = s_lastBotHealthProbe.find(key);
            if (probeIt == s_lastBotHealthProbe.end()
                || nowRetain - probeIt->second >=
                    std::chrono::milliseconds(FeaturePolicy::Staleness::HealthMs)) {
                s_lastBotHealthProbe[key] = nowRetain;
                ReadBudget::PurposeScope purpose(ReadBudget::Purpose::Health);
                float partHp[Engine::WorldCacheEntry::kMaxBotParts]{};
                int destroyedParts = 0;
                const int count = ReadBotPartHp(
                    key, partHp, Engine::WorldCacheEntry::kMaxBotParts,
                    destroyedParts);
                int valid = 0;
                bool allZero = count > 0;
                for (int i = 0; i < count; ++i) {
                    if (!std::isfinite(partHp[i]) || partHp[i] < 0.f)
                        continue;
                    ++valid;
                    if (partHp[i] > 0.01f)
                        allZero = false;
                }
                allZero = allZero && valid == count && valid > 0;
                if (valid > 0) {
                    double healthSum = 0.0;
                    for (int i = 0; i < count; ++i) {
                        if (std::isfinite(partHp[i]) && partHp[i] >= 0.f)
                            healthSum += partHp[i];
                    }
                    actor.health = static_cast<float>(healthSum / valid * 100.0);
                    actor.maxhealth = 100.f;
                    // Stamp what the reader is about to trust.
                    actor.healthSampleMs = SteadyMsFrom(nowRetain);
                    actor.partHpSampleMs = actor.healthSampleMs;
                }
                if (allZero) {
                    const auto zeroIt = s_botAllZeroHealthSince.try_emplace(
                        key, nowRetain).first;
                    if (nowRetain - zeroIt->second >= std::chrono::milliseconds(600))
                        broken = 1;
                } else {
                    s_botAllZeroHealthSince.erase(key);
                }
            }
        }
        actor.IsBreaked = broken != 0;
        // Publish the FINAL decision, not the raw probe taken before the health
        // evidence above. Consumers (aim, paint) read entry.broken through the
        // frame; publishing only the pre-health value meant a bot whose parts
        // all read zero for 600 ms never appeared broken to them at all.
        actor.broken = broken != 0;
        actor.brokenSampleMs = SteadyMsFrom(nowRetain);
        if (broken != 0) {
            // Dead-ESP expiry (BotEspExpiry): the old grace kept Drawing=true
            // and waited out kBotVisualMissEvict — 5-25s of frozen box after
            // death. Now: hold through the flap debounce, then drop the box
            // and expire — bounded even for the show_dead_bots corpse.
            ++dbgBrokenSkip;
            const auto deadIt = s_botDeadSince.try_emplace(key, nowRetain).first;
            const auto deadForMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    nowRetain - deadIt->second).count());
            const auto act = BotEspExpiry::DeadDisposition(
                deadForMs, var::show_dead_bots);
            if (act == BotEspExpiry::DeadAction::Debounce) {
                ++it;
                continue;
            }
            if (act == BotEspExpiry::DeadAction::Expire) {
                actor.Drawing = false;
                // #region agent log
                // bot_expire (verify log): death -> erase window. heldMs is
                // the number this fix exists to shrink.
                {
                    std::ostringstream f;
                    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    f << "{\"sessionId\":\"c190fb\",\"runId\":\"verify\","
                      << "\"location\":\"RobotList.cpp:retain\",\"message\":\"bot_expire\","
                      << "\"data\":{\"key\":" << key
                      << ",\"heldMs\":" << deadForMs
                      << ",\"corpse\":" << (var::show_dead_bots ? 1 : 0)
                      << "},\"timestamp\":" << ts << "}\n";
                    EntityDiagnostics::LogRaw(f.str());
                }
                // #endregion
                ClearBotVisualMiss(key);
                s_botVisualEvicted[key] = nowRetain;
                s_botLastGoodPos.erase(key);
                s_botDeadSince.erase(key);
                it = localCache.erase(it);
                continue;
            }
            // KeepCorpse: fall through — the corpse refreshes normally and
            // paints in the dead color; the bounded hold expires it above.
        } else {
            s_botDeadSince.erase(key);
        }

        actor.category = 3;
        // Capture the previous draw state BEFORE the reset below: the distance
        // gate's Schmitt hysteresis ("a bot already drawing stays on up to 15%
        // past the cutoff") must read THIS. Reading actor.Drawing after the
        // reset made the hold dead code, so bots at the range boundary blinked
        // on every scan.
        const bool wasDrawing = actor.Drawing;
        actor.Drawing = false;

        // Health is refreshed in the same SDK-backed probe above. Preserve the
        // last good sample when that probe cannot read the array; a failed DMA
        // read must not make the bar disappear.

        if (!actor.Mesh || !IsValidPointer(actor.Mesh))
            actor.Mesh = GetActorSkeletalMesh(key);

        // Part positions are needed by the pips feature and by the flying-bot
        // projection fallback. Do not rebuild the component/bone cache while
        // both consumers are off.
        const bool wantBotParts = var::show_bot_parts || var::showRobots;
        bool needPartCache = wantBotParts && actor.BotPartCount <= 0;
        if (wantBotParts && !needPartCache) {
            const auto meshIt = s_lastBotPartMesh.find(key);
            if (meshIt == s_lastBotPartMesh.end() || meshIt->second != actor.Mesh)
                needPartCache = true;
        }
        if (wantBotParts && !needPartCache) {
            const auto pit = s_lastBotPartCache.find(key);
            if (pit == s_lastBotPartCache.end()
                || nowRetain - pit->second >= kBotPartCacheCooldown)
                needPartCache = true;
        }
        if (needPartCache) {
            ++dbgPartCacheRan;
            PopulateBotPartCache(actor, key);
            s_lastBotPartCache[key] = nowRetain;
            s_lastBotPartMesh[key] = actor.Mesh;
            if (s_lastBotPartCache.size() > 4096) {
                s_lastBotPartCache.clear();
                s_lastBotPartMesh.clear();
            }
        } else {
            ++dbgPartCacheSkipped;
        }

        // Destroyed bots can linger with stale roots. Visual proof uses NOCACHE
        // WorldLocation (HasLiveBotVisual). During miss grace keep Drawing only
        // when within bot ESP distance — do not bypass the distance gate.
        // P1: batched WorldLocation/Relative already proved live for most bots.
        if (!broken) {
            bool visualOk = false;
            if (batchLiveFast.count(key) && batchLiveFast[key]
                && IsPlausibleWorldPos(actor.WorldPos)) {
                visualOk = true;
                ++dbgBatchVisFast;
            } else {
                visualOk = HasLiveBotVisual(key, actor.Mesh);
                ++dbgBatchVisFallback;
            }
            if (!visualOk) {
                ++dbgVisSkip;
                // G5: live Surveyor/Leaper/Hornet/Rocketeer were evicted every
                // ~4.5s on flaky zero WorldLocation reads, blinking their ESP.
                // Only evict when the actor no longer identifies as a bot —
                // despawned actors read garbage identity, live ones still decode.
                bool wantEvict = BotVisualMissShouldEvict(key, false);
                if (wantEvict) {
                    bool identityGone = true;
                    if (ArcActorType::IsAnyBotActor(key)) {
                        identityGone = false;
                    } else {
                        std::string liveFn = engine.GetActorFNameStringCached(key);
                        if (liveFn.empty())
                            liveFn = engine.GetActorFNameString(key);
                        if (!liveFn.empty()
                            && (!ResolveRobotTypeFromFName(engine, liveFn).empty()
                                || !LookupEnemyBotByFName(liveFn).empty()
                                || !LookupBotClassToken(liveFn).empty()))
                            identityGone = false;
                    }
                    if (!identityGone) {
                        // Alive but unreadable this window: restart the grace.
                        wantEvict = false;
                        ClearBotVisualMiss(key);
                    }
                }
                if (wantEvict) {
                    ClearBotVisualMiss(key);
                    s_botVisualEvicted[key] = std::chrono::steady_clock::now();
                    s_botLastGoodPos.erase(key);
                    it = localCache.erase(it);
                } else if (IsPlausibleWorldPos(actor.WorldPos)) {
                    s_botLastGoodPos[key] = { actor.WorldPos, nowRetain };
                    Vector3 delta = actor.WorldPos - localPos;
                    const float distanceSq = static_cast<float>(
                        delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                    actor.Distance = sqrtf(distanceSq) / 100.0f;
                    // Distance hysteresis: require 3 consecutive out-of-range scans
                    // before clearing Drawing, same as the player path. Stops bot ESP
                    // from flickering at the distance boundary (41% of all flickers).
                    static std::unordered_map<uintptr_t, uint8_t> s_botDistMisses;
                    constexpr uint8_t kBotDistMissClear = 3;
                    // distHeld is the 1.15x Schmitt band and must read
                    // wasDrawing (state BEFORE this pass's reset), and a bad
                    // anchor holds the previous state instead of culling on
                    // garbage distance.
                    // FAIL-OPEN: a bad anchor must not hide every bot — an
                    // unmeasurable range is treated as in-range.
                    const bool distInside = distRefOk ? distanceSq <= maxDistSq : true;
                    const bool distHeld = wasDrawing && distanceSq <= maxDistOffSq;
                    if (distInside || distHeld) {
                        actor.Drawing = true;
                        s_botDistMisses.erase(key);
                        ++dbgDrawing;
                    } else {
                        uint8_t& misses = s_botDistMisses[key];
                        if (misses < 255) ++misses;
                        if (misses >= kBotDistMissClear) {
                            actor.Drawing = false;
                            s_botDistMisses.erase(key);
                        } else {
                            actor.Drawing = true;  // keep drawing during grace
                            ++dbgDrawing;
                        }
                    }
                    ++it;
                } else {
                    // Ghost-flicker fix: reuse the last plausible position for a
                    // short window instead of blanking the box on one failed read.
                    bool froze = false;
                    if (const auto fit = s_botLastGoodPos.find(key);
                        fit != s_botLastGoodPos.end()
                        && nowRetain - fit->second.when <= kBotPosFreezeTtl) {
                        // A freeze substitutes the position without a fresh
                        // sample, so carry the sample pair over explicitly; a
                        // half-updated pair makes paint interpolate from a
                        // position that never existed.
                        if (actor.positionSampleMs != 0) {
                            actor.prevSampleWorldPos = actor.WorldPos;
                            actor.prevSampleMs = actor.positionSampleMs;
                        }
                        actor.WorldPos = fit->second.pos;
                        Vector3 delta = actor.WorldPos - localPos;
                        const float distanceSq = static_cast<float>(
                            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                        actor.Distance = sqrtf(distanceSq) / 100.0f;
                        if ((distRefOk ? distanceSq <= maxDistSq : true)
                            || (wasDrawing && distanceSq <= maxDistOffSq)) {
                            actor.Drawing = true;
                            ++dbgDrawing;
                        } else {
                            actor.Drawing = false;
                        }
                        froze = true;
                        // #region agent log
                        {
                            static auto s_lastFreezeLog = std::chrono::steady_clock::time_point{};
                            if (s_lastFreezeLog.time_since_epoch().count() == 0
                                || nowRetain - s_lastFreezeLog >= std::chrono::seconds(3)) {
                                s_lastFreezeLog = nowRetain;
                                std::ostringstream f;
                                char fnEsc[64]{};
                                snprintf(fnEsc, sizeof(fnEsc), "%.48s",
                                    engine.GetActorFNameStringCached(key).c_str());
                                const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::system_clock::now().time_since_epoch()).count();
                                f << "{\"sessionId\":\"c190fb\",\"runId\":\"post-fix\",\"hypothesisId\":\"F1\","
                                  << "\"location\":\"RobotList.cpp:RobotList\",\"message\":\"bot_pos_freeze\","
                                  << "\"data\":{\"fname\":\"" << fnEsc
                                  << "\",\"key\":" << key << "}"
                                  << ",\"timestamp\":" << ts << "}\n";
                                EntityDiagnostics::LogRaw(f.str());
                            }
                        }
                        // #endregion
                        ++it;
                        continue;
                    }
                    (void)froze;
                    actor.Drawing = false;
                    // #region agent log
                    // R1: bot in visual grace with IMPLAUSIBLE WorldPos never
                    // draws and never logs (roller invisible at point blank).
                    {
                        static auto s_lastNoPos = std::chrono::steady_clock::time_point{};
                        const auto nowNoPos = std::chrono::steady_clock::now();
                        if (s_lastNoPos.time_since_epoch().count() == 0
                            || nowNoPos - s_lastNoPos >= std::chrono::seconds(2)) {
                            s_lastNoPos = nowNoPos;
                            std::ostringstream f;
                            char nameEsc[64]{}, fnEsc[64]{};
                            snprintf(nameEsc, sizeof(nameEsc), "%.48s", actor.ActorName.c_str());
                            snprintf(fnEsc, sizeof(fnEsc), "%.48s",
                                engine.GetActorFNameStringCached(key).c_str());
                            const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            // R4: zero-pos drones (Rocketeer/Firefly) — dump every
                            // alternative position source to find the live one.
                            Vector3 childPos{};
                            int childIdx = -1;
                            int childN = 0;
                            if (actor.rootComponent) {
                                std::vector<uintptr_t> kids;
                                ReadChildComponentsLocal(actor.rootComponent, kids, 1);
                                childN = static_cast<int>(kids.size());
                                for (size_t ci = 0; ci < kids.size() && ci < 8; ++ci) {
                                    const Vector3 cp =
                                        Engine::ReadWorldLocationNocache(kids[ci], false);
                                    if (IsPlausibleWorldPos(cp)) {
                                        childPos = cp;
                                        childIdx = static_cast<int>(ci);
                                        break;
                                    }
                                }
                            }
                            const uintptr_t embarkM = Memory::read_nocache<uintptr_t>(
                                key + Offsets::EmbarkMesh);
                            Vector3 embarkPos{};
                            if (embarkM && engine.IsValidPointer(embarkM))
                                embarkPos = Engine::ReadWorldLocationNocache(embarkM, false);
                            // R6: cached root may be stale — reread live.
                            const uintptr_t liveRoot = Memory::read_nocache<uintptr_t>(
                                key + Offsets::RootComponent);
                            Vector3 liveRootPos{};
                            if (liveRoot && engine.IsValidPointer(liveRoot))
                                liveRootPos = Engine::ReadWorldLocationNocache(liveRoot, true);
                            f << "{\"sessionId\":\"c190fb\",\"runId\":\"roller\",\"hypothesisId\":\"R4\","
                              << "\"location\":\"RobotList.cpp:RobotList\",\"message\":\"bot_nopos\","
                              << "\"data\":{\"label\":\"" << nameEsc
                              << "\",\"fname\":\"" << fnEsc
                              << "\",\"root\":" << actor.rootComponent
                              << ",\"mesh\":" << actor.Mesh
                              << ",\"posZ\":" << static_cast<long long>(actor.WorldPos.z)
                              << ",\"headOk\":" << (actor.hasBotHeadWorldPos ? 1 : 0)
                              << ",\"headX\":" << static_cast<long long>(actor.BotHeadWorldPos.x)
                              << ",\"headZ\":" << static_cast<long long>(actor.BotHeadWorldPos.z)
                              << ",\"parts\":" << actor.BotPartCount
                              << ",\"part0X\":" << static_cast<long long>(
                                     actor.BotPartCount > 0 ? actor.BotPartPos[0].x : 0.f)
                              << ",\"childN\":" << childN
                              << ",\"childIdx\":" << childIdx
                              << ",\"childX\":" << static_cast<long long>(childPos.x)
                              << ",\"childZ\":" << static_cast<long long>(childPos.z)
                              << ",\"embark\":" << embarkM
                              << ",\"embarkX\":" << static_cast<long long>(embarkPos.x)
                              << ",\"liveRoot\":" << liveRoot
                              << ",\"liveRootX\":" << static_cast<long long>(liveRootPos.x)
                              << ",\"liveRootZ\":" << static_cast<long long>(liveRootPos.z)
                              << ",\"key\":" << key << "}"
                              << ",\"timestamp\":" << ts << "}\n";
                            EntityDiagnostics::LogRaw(f.str());
                        }
                    }
                    // #endregion
                    ++it;
                }
                continue;
            }
            BotVisualMissShouldEvict(key, true);
        }

        if (!IsPlausibleWorldPos(actor.WorldPos)) {
            // Ghost-flicker fix: a transient zero read must not erase the bot;
            // hold the last plausible position within the freeze TTL.
            //
            // Hidden trap behind this path: the position it restores comes from
            // the module-side last-good table, which is written from whatever was
            // last committed (below). It is a plausible vector by construction and
            // carries no history of its own, so it is deliberately NOT run through
            // Core/BotPosSanity.hpp -- re-anchoring to it would let one poisoned read
            // become the reference every later good sample is judged against. The
            // table is stamped with nowRetain and TTL-bounded for exactly that reason.
            if (const auto fit = s_botLastGoodPos.find(key);
                fit != s_botLastGoodPos.end()
                && nowRetain - fit->second.when <= kBotPosFreezeTtl) {
                actor.WorldPos = fit->second.pos;
            } else {
                ++dbgZeroPos;
                ClearBotVisualMiss(key);
                s_botLastGoodPos.erase(key);
                it = localCache.erase(it);
                continue;
            }
        } else {
            s_botLastGoodPos[key] = { actor.WorldPos, nowRetain };
            if (s_botLastGoodPos.size() > 4096)
                s_botLastGoodPos.clear();
        }

        // PositionRefreshPass is the single live bot sampler. Do not update
        // velocity here as well: RobotList runs at a different cadence and the
        // two writers produced sawtooth velocity estimates and choppy lead.

        // Visible = clear line of sight from the camera to the bot's body (about 90 cm up).
        const bool botRaysOn = UpgradeOptions::visCheckPerBone
            && EmbreeVis::g_visibility_meshes_ready.load(std::memory_order_acquire);
        const bool rawBotVisible = !botRaysOn || EmbreeVis::CheckVisibility(
            EmbreeVis::Vec3{ cam.Location.x, cam.Location.y, cam.Location.z },
            EmbreeVis::Vec3{ actor.WorldPos.x, actor.WorldPos.y, actor.WorldPos.z + 90.0 },
            key, true);
        actor.isVisible = EmbreeVis::ApplyVisHold(
            actor.visShown, actor.visPendingSinceMs, rawBotVisible, key);

        Vector3 delta = actor.WorldPos - localPos;
        const float distanceSq = static_cast<float>(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

        actor.Distance = sqrtf(distanceSq) / 100.0f;
        // Bad anchor (camera reset AND pawn root unreadable): the distance is
        // meaningless — culling on it blanks the whole cache for a pass (the
        // "all bots blink off together" bug). FAIL-OPEN per "Bots always draw":
        // skip the range gate instead of hiding the bot.
        if (!distRefOk) {
            actor.Drawing = true;
            ++dbgDrawing;
            ++it;
            continue;
        }
        if (distanceSq > maxDistSq
            && !(wasDrawing && distanceSq <= maxDistOffSq)) {
            ++dbgDistSkip;
            actor.Drawing = false;
            ++it;
            continue;
        }

        actor.Drawing = true;
        ++dbgDrawing;
        ++it;
    }

    // #region agent log
    {
        static std::unordered_set<uintptr_t> s_prevBotKeys;
        std::unordered_set<uintptr_t> curKeys;
        curKeys.reserve(localCache.size());
        for (const auto& [key, actor] : localCache) {
            curKeys.insert(key);
            WorldScan::FlickerCause cause = WorldScan::FlickerCause::Other;
            if (!actor.Drawing) {
                if (!IsPlausibleWorldPos(actor.WorldPos))
                    cause = WorldScan::FlickerCause::PosFail;
                else if (actor.Distance > maxDistM)
                    cause = WorldScan::FlickerCause::DistEdge;
                else
                    cause = WorldScan::FlickerCause::VisMiss;
            }
            WorldScan::NoteFlickerDrawing(
                WorldScan::FlickerChannel::Bot, key, actor.Drawing, cause);
        }
        for (uintptr_t prev : s_prevBotKeys) {
            if (!curKeys.contains(prev))
                WorldScan::NoteFlickerGone(WorldScan::FlickerChannel::Bot, prev);
        }
        s_prevBotKeys = std::move(curKeys);
        WorldScan::MaybeFlushFlickerScore();
    }
    // #endregion

    if (m_worldGeneration.load(std::memory_order_acquire) != genAtStart)
        return;

    // MERGE, do not clobber. localCache was copied at the START of this pass,
    // which can run 200-900ms; PositionRefreshPass (16ms) wrote fresh bot
    // positions into robotCache the whole time. Assigning localCache wholesale
    // rolled every bot back to a pass-start position, so the box advanced then
    // snapped backwards — the reported choppy/off-target bot motion.
    // Carry forward the newest live sample group whenever it is newer.
    {
        std::unique_lock<std::shared_mutex> lock(m_robotCacheMutex);
        for (const auto& [key, live] : robotCache) {
            auto it = localCache.find(key);
            if (it == localCache.end())
                continue;   // not in this pass; PositionRefreshPass owns it, leave it alone
            if (live.positionSampleMs <= it->second.positionSampleMs)
                continue;   // our own work this pass is the newer sample
            // Position sample, velocity and every derived offset were moved as
            // one consistent group by PositionRefreshPass — take them together.
            it->second.WorldPos = live.WorldPos;
            // Consecutive-jump memory travels with the sample group: the live
            // sampler is the newest judge of whether this entry's positions agree
            // with each other, so its count is the current one.
            it->second.posJumpRejects = live.posJumpRejects;
            it->second.lastWorldPos = live.lastWorldPos;
            it->second.lastVelocityUpdate = live.lastVelocityUpdate;
            it->second.cachedVelocity = live.cachedVelocity;
            it->second.positionSampleMs = live.positionSampleMs;
            it->second.livePositionSampleMs = live.livePositionSampleMs;
            it->second.CenterWorldPos = live.CenterWorldPos;
            it->second.BotHeadWorldPos = live.BotHeadWorldPos;
            it->second.hasBotHeadWorldPos = live.hasBotHeadWorldPos;
            it->second.BotPartCount = live.BotPartCount;
            for (int i = 0; i < live.BotPartCount
                && i < Engine::WorldCacheEntry::kMaxBotParts; ++i)
                it->second.BotPartPos[i] = live.BotPartPos[i];
        }
        robotCache = std::move(localCache);
    }

    // P6b: advance ring only after successful gen-matched writeback so an
    // aborted pass never skips a slice band.
    if (admitRingAdvanceOk) {
        s_admitCoveredMask |= (1ull << (dbgAdmitSlice % kAdmitSlices));
        s_admitSliceCursor = (dbgAdmitSlice + 1) % kAdmitSlices;
        if (s_admitSliceCursor == 0) {
            const auto nowCycle = std::chrono::steady_clock::now();
            if (s_admitCycleStart.time_since_epoch().count() != 0) {
                s_admitLastCycleMs = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        nowCycle - s_admitCycleStart).count());
            }
            s_admitCycleStart = nowCycle;
            s_admitCoveredMask = 0;
        }
    }

    int dbgEnemyCount = 0;
    const int dbgFnameHit = g_botDrawLabelMiss.exchange(0, std::memory_order_relaxed);
    {
        uintptr_t gameState = 0;
        {
            std::shared_lock<std::shared_mutex> slock(m_stateMutex);
            gameState = AGameStateBase;
        }
        if (gameState && IsValidPointer(gameState))
            dbgEnemyCount = Memory::read<int32_t>(
                gameState + Offsets::GameState_EnemyCount);
    }

    // Always-on file trace of every bot admission/draw stage so "turret never
    // shows" / "not all bots picked up" reports are traceable from the log —
    // the console [debugRobot] print needs the debug overlay on and never
    // reaches disk. kArcDebugLogPath is NUL by design; this tap uses the real
    // verify log. Throttled 2s.
    {
        static IntervalTimer botFileTimer(2000);
        if (botFileTimer.fire()) {
            size_t botCacheSz = 0;
            {
                std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
                botCacheSz = robotCache.size();
            }
            // 1664, not 1280: the rej{} block below names every way an actor can
            // leave the lane, and each size so far has been within ~100 bytes of
            // truncating (760 -> 1280 -> here, for the first-probe fields). A
            // truncated stats line would silently lose the tail.
            char bbuf[1792]{};
            // First-probe results: fp* is cumulative since the last world-gen
            // clear (a cold load's flood lives there), fw* is the last
            // kProbeWindowSamples probes and is the one that answers "is a new
            // actor verified within a second NOW". A large cumulative fpMaxMs
            // with a small fwMaxMs is the shape to expect after a raid load; a
            // large fwMaxMs means the lane's per-pass capacity is the limit, not
            // the ordering.
            const AdmitQueue::PendingQueue::FirstProbeStats fpStats =
                s_admitQueue.ProbeStats();
            snprintf(bbuf, sizeof(bbuf),
                "{\"scanned\":%d,\"admitted\":%d,\"cache\":%zu,\"drawing\":%d,"
                "\"structHit\":%d,\"quickPass\":%d,\"verifyFail\":%d,\"reEvict\":%d,"
                "\"admitAny\":%d,\"failRoot\":%d,\"failMesh\":%d,\"failId\":%d,"
                "\"failOther\":%d,\"fnameMiss\":%d,\"visSkip\":%d,\"distSkip\":%d,"
                "\"zeroPos\":%d,\"sceneOk\":%d,\"sceneFail\":%d,\"enemyCount\":%d,"
                "\"posAlt\":%d,\"posJump\":%d,"
                "\"slice\":%zu,\"prioNew\":%d,\"prioOverdue\":%d,"
                "\"prioSight\":%d,\"prioRetry\":%d,\"pend\":%zu,\"tries\":%zu,"
                "\"cycleMs\":%d,\"brokenSkip\":%d,"
                "\"fpN\":%llu,\"fpOver\":%llu,\"fpMeanMs\":%llu,\"fpMaxMs\":%llu,"
                "\"fwN\":%llu,\"fwOver\":%llu,\"fwMeanMs\":%llu,\"fwMaxMs\":%llu,"
                "\"rej\":{\"playerClass\":%d,\"localPs\":%d,\"cachedPlayer\":%d,"
                "\"worldItem\":%d,\"fnameNeg\":%d,\"undecrypted\":%d,"
                "\"verifyMemo\":%d,\"verifyTransient\":%d,\"verifyExpired\":%d}}",
                dbgScanned, dbgAdmitted, botCacheSz, dbgDrawing,
                dbgStructHit, dbgQuickPass, dbgVerifyFail, dbgReEvict,
                dbgAdmitAnyBot, dbgFailNoRoot, dbgFailNoMesh, dbgFailNoId,
                dbgFailOther, dbgFnameHit, dbgVisSkip, dbgDistSkip,
                dbgZeroPos, dbgScenePosOk, dbgScenePosFail, dbgEnemyCount,
                dbgBatchPosAlt, dbgScenePosJump,
                dbgAdmitSlice, dbgAdmitPrioNew, dbgAdmitPrioOverdue,
                dbgAdmitPrioFirstSight, dbgAdmitPrioRetry,
                s_admitQueue.Size(),
                s_admitQueue.TrySize(), s_admitLastCycleMs, dbgBrokenSkip,
                static_cast<unsigned long long>(fpStats.n),
                static_cast<unsigned long long>(fpStats.overBound),
                static_cast<unsigned long long>(fpStats.n ? fpStats.sumMs / fpStats.n : 0),
                static_cast<unsigned long long>(fpStats.maxMs),
                static_cast<unsigned long long>(fpStats.winN),
                static_cast<unsigned long long>(fpStats.winOverBound),
                static_cast<unsigned long long>(
                    fpStats.winN ? fpStats.winSumMs / fpStats.winN : 0),
                static_cast<unsigned long long>(fpStats.winMaxMs),
                dbgRejPlayerClass, dbgRejLocalPs, dbgRejCachedPlayer,
                dbgRejWorldItem, dbgRejFnameNeg, dbgRejUndecrypted,
                dbgRejVerifyMemo, dbgRejVerifyTransient, dbgRejVerifyExpired);
            EntityDiagnostics::LogScan("bots", bbuf);
            std::ostringstream f;
            const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            f << "{\"sessionId\":\"c190fb\",\"runId\":\"bot-admit\","
              << "\"hypothesisId\":\"B10\",\"location\":\"RobotList.cpp:RobotList\","
              << "\"message\":\"bot_admit_stats\",\"data\":" << bbuf
              << ",\"timestamp\":" << ts << "}\n";
            EntityDiagnostics::LogRaw(f.str());
        }
    }

    // ── Native enemy-container probe ─────────────────────────────────────────
    // Players resolve from GameState.PlayerArray in one read; bots have no such
    // array and are brute-forced from the ~2k-actor level list, so coverage is
    // partial ("some bots never show"). This looks for a native TArray<actor*>
    // in PioneerGameState (EnemyCount lives at 0xA40) and scores each candidate
    // by how many of its elements are already-admitted bots — a machine-derived
    // way to find the real enemy list from live data (the dump has no members).
    // Throttled to the same 2s cadence as the stats line; always written to
    // entity_diagnostics.ndjson.
    {
        static IntervalTimer probeTimer(2000);
        uintptr_t gs = 0;
        {
            std::shared_lock<std::shared_mutex> slock(m_stateMutex);
            gs = AGameStateBase;
        }
        std::unordered_set<uintptr_t> botKeys;
        {
            std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
            botKeys.reserve(robotCache.size());
            for (const auto& kv : robotCache)
                botKeys.insert(kv.first);
        }
        if (probeTimer.fire() && gs && IsValidPointer(gs) && !botKeys.empty()) {
            // Single ReadRaw site: the whole GameState window is read as one
            // block, then each candidate array's element block. Keeps this
            // diagnostic to +1 direct-read site (ratcheted in the baseline).
            auto rd = [](uintptr_t addr, void* buf, size_t n) {
                return Memory::ReadRaw(addr, buf, n);
            };
            constexpr std::ptrdiff_t kWinBase = 0x4D0;
            constexpr size_t kWinSize = static_cast<size_t>(0xD10 - kWinBase);
            std::vector<uint8_t> win(kWinSize, 0);
            int candidates = 0, bestBotHits = 0;
            std::ptrdiff_t bestOff = 0;
            if (rd(gs + kWinBase, win.data(), win.size())) {
                for (std::ptrdiff_t off = kWinBase; off <= 0xD00; off += 0x8) {
                    const size_t b = static_cast<size_t>(off - kWinBase);
                    uintptr_t data = 0;
                    int32_t n = 0, mx = 0;
                    std::memcpy(&data, win.data() + b, sizeof(data));
                    std::memcpy(&n, win.data() + b + 8, sizeof(n));
                    std::memcpy(&mx, win.data() + b + 12, sizeof(mx));
                    if (!data || !IsUsermodePtr(data))
                        continue;
                    if (n <= 0 || n > 512 || mx < n)
                        continue;
                    // Read the WHOLE array (capped at the 512 element filter) and
                    // score every element, not just a sample: a true enemy list
                    // should contain most admitted bots, so full coverage makes
                    // bestBotHits conclusive instead of a first-8 coincidence.
                    const int probeN = (n < 512) ? n : 512;
                    std::vector<uint64_t> elems(static_cast<size_t>(probeN), 0);
                    if (!rd(data, elems.data(), elems.size() * sizeof(uint64_t)))
                        continue;
                    int valid = 0, botHits = 0;
                    uintptr_t first = 0;
                    for (int i = 0; i < probeN; ++i) {
                        const uintptr_t e = static_cast<uintptr_t>(elems[i]);
                        if (e && IsValidPointer(e)) {
                            ++valid;
                            if (first == 0) first = e;
                            if (botKeys.contains(e)) ++botHits;
                        }
                    }
                    if (valid == 0 && botHits == 0)
                        continue;
                    ++candidates;
                    if (botHits > bestBotHits) { bestBotHits = botHits; bestOff = off; }
                    std::string firstName, firstClass;
                    if (valid >= 4 && first) {
                        firstName = engine.GetActorFNameStringCached(first);
                        if (firstName.empty()) firstName = engine.GetActorFNameString(first);
                        if (botHits > 0) firstClass = engine.GetActorClassFName(first);
                    }
                    std::ostringstream p;
                    p << "{\"ev\":\"enemy_array_probe\",\"off\":" << off
                      << ",\"gameState\":" << gs
                      << ",\"data\":" << data << ",\"n\":" << n << ",\"max\":" << mx
                      << ",\"probeN\":" << probeN << ",\"valid\":" << valid
                      << ",\"botHits\":" << botHits
                      << ",\"first\":" << first
                      << ",\"firstName\":\"" << firstName << "\""
                      << ",\"firstClass\":\"" << firstClass << "\""
                      << ",\"botCache\":" << botKeys.size() << "}\n";
                    EntityDiagnostics::LogRaw(p.str());
                }
            }
            std::ostringstream s;
            s << "{\"ev\":\"enemy_array_probe_scan\",\"gameState\":" << gs
              << ",\"botCache\":" << botKeys.size()
              << ",\"candidates\":" << candidates
              << ",\"bestOff\":" << bestOff
              << ",\"bestBotHits\":" << bestBotHits << "}\n";
            EntityDiagnostics::LogRaw(s.str());
        }
    }

    if (var::show_debug_overlay) {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        // Same first-probe numbers as the 2s file tap, so the overlay and the
        // log cannot disagree about whether the bound held.
        const AdmitQueue::PendingQueue::FirstProbeStats fpStats =
            s_admitQueue.ProbeStats();
        std::cout << "[debugRobot] scanned=" << dbgScanned
            << " admitted=" << dbgAdmitted
            << " drawing=" << dbgDrawing
            << " cache=" << robotCache.size()
            << " structHit=" << dbgStructHit
            << " quickPass=" << dbgQuickPass
            << " verifyFail=" << dbgVerifyFail
            << " reEvict=" << dbgReEvict
            << " admitAny=" << dbgAdmitAnyBot
            << " failRoot=" << dbgFailNoRoot
            << " failMesh=" << dbgFailNoMesh
            << " failId=" << dbgFailNoId
            << " failOther=" << dbgFailOther
            << " fnameHit=" << dbgFnameHit
            << " visSkip=" << dbgVisSkip
            << " distSkip=" << dbgDistSkip
            << " maxDist=" << static_cast<int>(maxDistM)
            << " zeroPos=" << dbgZeroPos
            << " sceneOk=" << dbgScenePosOk
            << " sceneFail=" << dbgScenePosFail
            << " enemyCount=" << dbgEnemyCount
            << " slice=" << dbgAdmitSlice << "/" << kAdmitSlices
            << " n=" << dbgAdmitN
            << " band=" << dbgAdmitSliceBase << "-" << dbgAdmitSliceEnd
            << " sliceActors=" << dbgAdmitSliceActors
            << " prioNew=" << dbgAdmitPrioNew
            << " prioOverdue=" << dbgAdmitPrioOverdue
            << " prioSight=" << dbgAdmitPrioFirstSight
            << " prioRetry=" << dbgAdmitPrioRetry
            << " fpN=" << fpStats.n
            << " fpOver=" << fpStats.overBound
            << " fpMeanMs=" << (fpStats.n ? fpStats.sumMs / fpStats.n : 0)
            << " fpMaxMs=" << fpStats.maxMs
            << " fwN=" << fpStats.winN
            << " fwOver=" << fpStats.winOverBound
            << " fwMeanMs=" << (fpStats.winN ? fpStats.winSumMs / fpStats.winN : 0)
            << " fwMaxMs=" << fpStats.winMaxMs
            << " pend=" << s_admitQueue.Size()
            << " cover=0x" << std::hex << s_admitCoveredMask << std::dec
            << " cycleMs=" << s_admitLastCycleMs
            << " ringResets=" << s_admitRingResets
            << std::endl;
    }
}

namespace WorldScan {

// ── First-probe ordering: executed, not just documented ─────────────────────
// The lane's ordering is the admission latency (a queued actor is probed only
// from the lane — the band skips it), and the tests project cannot build here
// (no compile targets in Project.Tests.vcxproj, and scripts/build.ps1 -Test
// refuses by policy), so the decision table runs inside the shipping exe on the
// overlay thread, in microseconds, and logs its own record. Same shape as
// CollisionDebug::RunVerdictSelfTest.
void EnsureFirstProbeSelfTestRan()
{
    static bool s_ran = false;
    if (s_ran)
        return;
    s_ran = true;

    using Queue = AdmitQueue::PendingQueue;
    constexpr uint64_t now = 100000;
    const auto plan = [](size_t rows) {
        Queue::PassPlan p;
        p.maxRows = rows;
        return p;
    };
    const auto laneOf = [](Queue& q, size_t retries, size_t firstSight, uint64_t nowMs,
                            std::vector<uint64_t>& order) {
        for (size_t i = 0; i < retries; ++i) {
            const uint64_t a = i;
            q.NoteNew(a, nowMs);
            q.NoteProbed(a, nowMs);
            order.push_back(a);
        }
        for (size_t i = 0; i < firstSight; ++i) {
            const uint64_t a = 1000 + i;
            q.NoteNew(a, nowMs);
            order.push_back(a);
        }
    };

    // 1. A fresh actor is collected on the pass that noticed it — no rotation,
    //    no waiting for the ring band that skips it anyway.
    bool freshOk = false;
    {
        Queue q;
        q.NoteNew(5150, now);
        const std::vector<uint64_t> order{ 5150 };
        std::vector<uint64_t> rows;
        q.CollectPrio(order.begin(), order.end(), now, plan(96), rows);
        freshOk = rows.size() == 1 && rows[0] == 5150
            && q.Classify(5150, now) == Queue::Lane::FirstSight;
    }

    // 2. A retry storm cannot take the reserved share from never-probed work.
    bool shareOk = false;
    {
        Queue q;
        std::vector<uint64_t> order;
        laneOf(q, 300, 40, now, order);
        std::vector<uint64_t> rows;
        q.CollectPrio(order.begin(), order.end(), now, plan(96), rows);
        size_t sight = 0;
        for (uint64_t a : rows) {
            if (q.Classify(a, now) == Queue::Lane::FirstSight)
                ++sight;
        }
        shareOk = rows.size() == 96 && sight == Queue::kFirstSightRowsPerPass;
    }

    // 3. Past the bound, a never-probed actor leads the lane over every retry.
    bool preemptOk = false;
    {
        Queue q;
        std::vector<uint64_t> order;
        laneOf(q, 300, 0, now, order);
        const uint64_t laggard = 9999;
        q.NoteNew(laggard, now - (Queue::kFirstProbeBoundMs + 250));
        order.push_back(laggard);
        std::vector<uint64_t> rows;
        q.CollectPrio(order.begin(), order.end(), now, plan(96), rows);
        preemptOk = !rows.empty() && rows[0] == laggard;
    }

    // 4. A set larger than the budget drains oldest first, not in caller order.
    bool oldestOk = false;
    {
        Queue q;
        const uint64_t beyond = Queue::kFirstProbeBoundMs + 100;
        q.NoteNew(33, now - beyond);
        q.NoteNew(11, now - (beyond + 200));
        q.NoteNew(22, now - (beyond + 100));
        const std::vector<uint64_t> order{ 33, 22, 11 };   // hostile order
        std::vector<uint64_t> rows;
        q.CollectPrio(order.begin(), order.end(), now, plan(2), rows);
        oldestOk = rows.size() == 2 && rows[0] == 11 && rows[1] == 22;
    }

    // 5. A re-note (ring reset, N jump) must not restart the latency clock, or a
    //    reset could erase a wait that had already passed the bound.
    bool renoteOk = false;
    {
        Queue q;
        q.NoteNew(5, now);
        q.NoteNew(5, now + 5000);
        renoteOk =
            q.Classify(5, now + Queue::kFirstProbeBoundMs) == Queue::Lane::OverdueFirst;
    }

    // 6. The wait is recorded once per actor, and the over-bound ones counted.
    bool statsOk = false;
    {
        Queue q;
        q.NoteNew(77, now);
        q.NoteProbed(77, now + 120);
        q.NoteProbed(77, now + 9000);   // not a first probe
        q.NoteNew(88, now);
        q.NoteProbed(88, now + 1100);
        const Queue::FirstProbeStats s = q.ProbeStats();
        statsOk = s.n == 2 && s.overBound == 1 && s.sumMs == 1220 && s.maxMs == 1100;
    }

    // 7. The window reports the STEADY state, not the cold load: a 5000ms spike
    //    must leave the window once it scrolls out, or the number can never
    //    answer "is a new actor verified within a second now".
    bool windowOk = false;
    {
        Queue q;
        q.NoteNew(1, 0);
        q.NoteProbed(1, 5000);
        const bool spikeSeen = q.ProbeStats().winMaxMs == 5000;
        for (size_t i = 0; i < Queue::kProbeWindowSamples; ++i) {
            const uint64_t actor = 100 + i;
            q.NoteNew(actor, 0);
            q.NoteProbed(actor, 40);
        }
        const Queue::FirstProbeStats s = q.ProbeStats();
        windowOk = spikeSeen && s.winN == Queue::kProbeWindowSamples
            && s.winMaxMs == 40 && s.winOverBound == 0
            && s.n == Queue::kProbeWindowSamples + 1 && s.maxMs == 5000;
    }

    const bool cases[7] = {
        freshOk, shareOk, preemptOk, oldestOk, renoteOk, statsOk, windowOk
    };
    int failed = 0;
    for (const bool ok : cases) {
        if (!ok)
            ++failed;
    }

    std::ostringstream detail;
    detail << "fresh_probed_now=" << (freshOk ? "pass" : "fail") << " ";
    detail << "reserved_share=" << (shareOk ? "pass" : "fail") << " ";
    detail << "overdue_preempts=" << (preemptOk ? "pass" : "fail") << " ";
    detail << "oldest_first=" << (oldestOk ? "pass" : "fail") << " ";
    detail << "renote_keeps_clock=" << (renoteOk ? "pass" : "fail") << " ";
    detail << "stats_once=" << (statsOk ? "pass" : "fail") << " ";
    detail << "window_is_steady_state=" << (windowOk ? "pass" : "fail") << " ";

    std::ostringstream f;
    f << "{\"location\":\"RobotList.cpp\",\"message\":\"bot_admit_first_probe_selftest\","
      << "\"timestamp\":" << EntityDiagnostics::NowMs()
      << ",\"passed\":" << (failed == 0 ? "true" : "false")
      << ",\"cases\":7,\"failed\":" << failed
      << ",\"boundMs\":" << Queue::kFirstProbeBoundMs
      << ",\"reservedRows\":" << Queue::kFirstSightRowsPerPass
      << ",\"detail\":\"" << EntityDiagnostics::Sanitize(detail.str(), 200) << "\"}\n";
    EntityDiagnostics::LogRaw(f.str());
}

void ClearRobotScannerStaticState()
{
    ClearRobotListStaticMaps();
}

} // namespace WorldScan
