#include "../Core/Engine.h"
#include "../Core/ActorType.h"
#include "../Core/AssetNames.h"
#include "../Core/BotTypes.h"
#include "../Core/IntervalTimer.h"
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

bool IsZeroWorldPos(const Vector3& pos)
{
    return pos.x == 0.0 && pos.y == 0.0 && pos.z == 0.0;
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
    if (lower.rfind("sm_", 0) == 0)
        return true;
    if (lower.find("collisioneffect") != std::string::npos
        || lower.find("networksystem") != std::string::npos
        || lower.find("backdrop") != std::string::npos)
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

std::string ResolveHuskBotLabel(const std::string& fname)
{
    if (fname.empty())
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
            "soundscape",
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
                std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                if (f) {
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
                }
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

    const uintptr_t playerState =
        Memory::read<uintptr_t>(actor + Offsets::APlayerState);
    if (playerState && engine.IsValidPointer(playerState))
        return false;

    if (ArcActorType::IsAnyBotActor(actor))
        return true;

    // Strong bot-exclusive DA signal (DA_EnemyType_* / mapped enemy asset).
    if (HasStrongEnemyDataAsset(actor))
        return true;

    if (WorldScan::HasArcEnemyAssetPointer(actor))
        return true;

    if (HasWorldItemStructure(actor))
        return false;

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

    return false;
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

// P6b: 4-slice rotating admission ring. Full prune + retain still run every
// pass; only the expensive admission DMA probes are banded. Positions stay
// hot via PositionRefreshPass @16ms.
static constexpr size_t kAdmitSlices = 4;
static constexpr size_t kAdmitPrioNewMax = 64;
static size_t s_admitSliceCursor = 0;
static uint64_t s_admitRingGen = 0;
static uintptr_t s_admitRingActorsPtr = 0;
static size_t s_admitRingActorCount = 0;
static size_t s_admitRingEpoch = 0;
static uint64_t s_admitCoveredMask = 0;
static int s_admitRingResets = 0;
static int s_admitLastCycleMs = 0;
static std::chrono::steady_clock::time_point s_admitCycleStart{};
static std::unordered_set<uintptr_t> s_admitPrevActors;

static bool QuickBotFnameCandidateMemo(
    uintptr_t actor, int& outChecked, int& outMemoSkip)
{
    const auto now = std::chrono::steady_clock::now();
    if (const auto it = s_quickFnameNeg.find(actor); it != s_quickFnameNeg.end()) {
        const auto ttl = std::chrono::seconds(8 + static_cast<int>((actor >> 4) & 7));
        if (now - it->second < ttl) {
            ++outMemoSkip;
            return false;
        }
        s_quickFnameNeg.erase(it);
    }
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
    const uintptr_t playerState =
        Memory::read<uintptr_t>(actor + Offsets::APlayerState);
    if (playerState && engine.IsValidPointer(playerState))
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
        if (WorldScan::LooksLikeContainerActor(actor, probe)
            || (!classFname.empty() && WorldScan::LooksLikeContainerActor(actor, classFname))
            || (!probe.empty() && FnameLooksLikeWorldContainer(probe))
            || (!classFname.empty() && FnameLooksLikeWorldContainer(classFname)))
            return false;
    }

    // (2) DEFINITIVE proof: validated ARC EnemyType data-asset (tech- style).
    // Bot-exclusive � DA_EnemyType_* / ResolveEnemyAssetBotLabel. Bare
    // constructable pointers alone leaked Camera/ghost junk into bot ESP.
    // Fresh spawns often have DA before RootComponent settles � fall through
    // so mesh/root/class can still admit instead of hard-failing here.
    if (HasStrongEnemyDataAsset(actor) && ResolveBotSceneRoot(actor) != 0)
        return true;

    // (3) No definitive proof ? weak candidate. Reject anything that carries
    //     gun / item / pickup / container structure. This is what kept your gun,
    //     ground boxes and containers OUT � they never reach the bot cache.
    if (HasWorldItemStructure(actor))
        return false;
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

    if (!meshOk)
        return false;

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
    actor.lastVelocityUpdate = static_cast<float>(nowMs);
}

Vector3 ReadComponentWorldPos(uintptr_t component)
{
    if (!component || !engine.IsValidPointer(component))
        return {};
    return Engine::ReadSceneWorldPos(component);
}

// Live bot tracking — mirror EntityList (NOCACHE WorldLocation, no RelativeLocation).
Vector3 ReadBotSceneWorldPosLive(uintptr_t component)
{
    return Engine::ReadWorldLocationNocache(component, /*allowRelativeFallback=*/false);
}

Vector3 ResolveBotWorldPos(uintptr_t actor, uintptr_t root, uintptr_t mesh)
{
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
                    std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                    if (f) {
                        const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        f << "{\"sessionId\":\"c190fb\",\"runId\":\"roller\",\"hypothesisId\":\"R3\","
                          << "\"location\":\"RobotList.cpp:ResolveBotWorldPos\",\"message\":\"bot_pos_fallback\","
                          << "\"data\":{\"source\":\"root_relative\",\"key\":" << actor
                          << ",\"posX\":" << static_cast<long long>(fromRel.x)
                          << ",\"posY\":" << static_cast<long long>(fromRel.y)
                          << ",\"posZ\":" << static_cast<long long>(fromRel.z) << "}"
                          << ",\"timestamp\":" << ts << "}\n";
                    }
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
        actor.WorldPos = ResolveBotWorldPos(key, actor.rootComponent, actor.Mesh);
        actor.CenterWorldPos = actor.WorldPos;
        if (!IsZeroWorldPos(actor.WorldPos) && actor.BotPartCount < Engine::WorldCacheEntry::kMaxBotParts) {
            actor.BotPartPos[actor.BotPartCount++] = actor.WorldPos;
        }
    }

    actor.hasBotHeadWorldPos = false;
    if (actor.Mesh && engine.IsValidPointer(actor.Mesh)) {
        uintptr_t boneMesh = 0;
        const uintptr_t boneArray =
            engine.ResolveBoneArray(key, actor.Mesh, &boneMesh);
        if (boneArray && boneMesh && engine.IsValidPointer(boneMesh)) {
            const FTransform ctw = Engine::ReadComponentToWorld(boneMesh);
            for (const auto& [gameIndex, uniBone] : engine.GameBoneMapArcRaiders) {
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
    if (!actor || actor == localPawn)
        return false;

    const uintptr_t playerState =
        Memory::read<uintptr_t>(actor + Offsets::APlayerState);
    if (playerState && engine.IsValidPointer(playerState))
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

    const uintptr_t playerState =
        Memory::read<uintptr_t>(actor + Offsets::APlayerState);
    if (playerState && engine.IsValidPointer(playerState))
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
// not because bots died. Double the grace before evicting.
static std::unordered_map<uintptr_t, uint8_t> s_botVisualMisses;
static constexpr uint8_t kBotVisualMissEvict = 10;

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
    // Soft-deprecate bIsBreaked@0x1220 (not in SDK). Prefer only
    // Constructable_bIsDestroyed@0x1210 (help/esp.txt + SDK). Do not treat
    // Health==0 as dead — spawn frames often read 0 HP and blocked admits.
    const uint8_t destroyed =
        Memory::read<uint8_t>(actor + Offsets::Constructable_bIsDestroyed);
    return destroyed == 1 ? 1 : 0;
}

static std::atomic<int> g_botDrawLabelMiss{ 0 };

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
    const std::string& fnameHint)
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

void Engine::RobotList()
{
    // Pre-size so runtime bot-type discovery inserts never rehash while the
    // render thread reads robotsList concurrently.
    static bool s_robotsListReserved = false;
    if (!s_robotsListReserved) {
        s_robotsListReserved = true;
        robotsList.reserve(256);
    }

    const bool wantRobots =
        var::showRobots || var::robotAimEnabled || var::show_radar;
    if (!wantRobots) {
        std::unique_lock<std::shared_mutex> lock(m_robotCacheMutex);
        robotCache.clear();
        return;
    }

    WorldScanContext ctx;
    if (!GatherWorldScanContext(ctx))
        return;

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
    // kept VisCheck rebuilding=1 with smc thousands (overlay lag).

    const float maxDistM = var::bot_esp_distance > 0.f ? var::bot_esp_distance : var::kMaxDistanceSliderM;
    const float maxDistSq = maxDistM * maxDistM * 10000.0f;

    std::unordered_map<uintptr_t, WorldCacheEntry> localCache;
    {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        localCache = robotCache;
    }

    for (auto it = localCache.begin(); it != localCache.end(); ) {
        if (!currentActorSet.contains(it->first)) {
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
    int dbgAdmitAnyBot = 0;
    int dbgFailNoRoot = 0;
    int dbgFailNoMesh = 0;
    int dbgFailNoId = 0;
    int dbgFailOther = 0;

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
    };

    std::vector<uintptr_t> admitCandidates;
    int dbgAdmitScatterExecs = 0;
    int dbgAdmitStructCand = 0;
    int dbgAdmitFnameChecked = 0;
    int dbgAdmitFnameMemoSkip = 0;
    int dbgAdmitPrioNew = 0;
    int dbgAdmitSliceActors = 0;
    size_t dbgAdmitSlice = 0;
    size_t dbgAdmitSliceBase = 0;
    size_t dbgAdmitSliceEnd = 0;
    size_t dbgAdmitN = 0;
    bool admitRingAdvanceOk = false;

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
            s_admitCoveredMask = 0;
            s_admitPrevActors.clear();
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

        const size_t slice = s_admitSliceCursor % kAdmitSlices;
        const size_t sliceBase = (N * slice) / kAdmitSlices;
        const size_t sliceEnd = (N * (slice + 1)) / kAdmitSlices;
        dbgAdmitSlice = slice;
        dbgAdmitSliceBase = sliceBase;
        dbgAdmitSliceEnd = sliceEnd;

        std::unordered_set<uintptr_t> probeSet;
        probeSet.reserve((sliceEnd - sliceBase) + kAdmitPrioNewMax + 8);
        for (size_t i = sliceBase; i < sliceEnd; ++i) {
            const uintptr_t actor = admitIndex[i];
            // Cached bots are owned by retain + PositionRefreshPass.
            if (localCache.contains(actor))
                continue;
            probeSet.insert(actor);
        }
        dbgAdmitSliceActors = static_cast<int>(probeSet.size());

        // Bounded newly-seen priority (CPU set-diff only — same scatter path).
        size_t prioAdded = 0;
        for (uintptr_t actor : admitIndex) {
            if (prioAdded >= kAdmitPrioNewMax)
                break;
            if (s_admitPrevActors.contains(actor))
                continue;
            if (localCache.contains(actor))
                continue;
            if (probeSet.insert(actor).second)
                ++prioAdded;
        }
        dbgAdmitPrioNew = static_cast<int>(prioAdded);

        std::vector<AdmitProbeRow> probeRows;
        probeRows.reserve(probeSet.size());
        for (uintptr_t actor : probeSet) {
            AdmitProbeRow row;
            row.actor = actor;
            probeRows.push_back(row);
        }

        constexpr size_t kAdmitChunk = 512;
        for (size_t base = 0; base < probeRows.size(); base += kAdmitChunk) {
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
            }
            if (prepOk && scatter.execute())
                ++dbgAdmitScatterExecs;
        }

        admitCandidates.reserve(64);
        for (const AdmitProbeRow& r : probeRows) {
            const uint32_t masked = ArcActorType::MaskActorTypeId(r.typeId);
            if (ArcActorType::IsPlayerClassId(masked))
                continue;
            if (r.playerState != 0 && Memory::IsValidPtrFast2(r.playerState))
                continue;
            if (engine.IsCachedPlayer(r.actor))
                continue;

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
                        && engine.IsValidPointer(static_cast<uintptr_t>(r.containerLoot))))
                    continue;
                if (!QuickBotFnameCandidateMemo(
                        r.actor, dbgAdmitFnameChecked, dbgAdmitFnameMemoSkip))
                    continue;
            }
            admitCandidates.push_back(r.actor);
        }

        // Snapshot current actors for next-pass new-actor diff. Cursor advances
        // only after gen-matched writeback so an aborted pass never skips a band.
        s_admitPrevActors.clear();
        s_admitPrevActors.insert(admitIndex.begin(), admitIndex.end());
        admitRingAdvanceOk = true;
    }

    if (doAdmission) {
    for (uintptr_t actor : admitCandidates)
    {
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
        if (IsBotEspPollutionName(fname))
            continue;

        // B1 (Fix #8): skip actors whose verify already failed recently.
        if (!localCache.contains(actor)) {
            if (const auto negIt = s_botVerifyNeg.find(actor);
                negIt != s_botVerifyNeg.end()) {
                const auto negTtl = std::chrono::seconds(
                    10 + static_cast<int>((actor >> 5) & 7));
                if (std::chrono::steady_clock::now() - negIt->second < negTtl)
                    continue;
                s_botVerifyNeg.erase(negIt);
            }
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

                // B1 (Fix #8): memoize the failure only when the name decoded
                // (undecrypted spawns must retry next pass).
                if (!fname.empty()) {
                    if (s_botVerifyNeg.size() > 16384)
                        s_botVerifyNeg.clear();
                    s_botVerifyNeg[actor] = std::chrono::steady_clock::now();
                }
            }
            continue;
        }
        s_botVerifyNeg.erase(actor);

        if (localCache.contains(actor))
            continue;

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
                continue;
            }
            s_botVisualEvicted.erase(evIt);
        }

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
        if (broken != 0 && !var::show_dead_bots) {
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
        entry.category = 3;
        ++dbgAdmitted;

        // #region agent log
        {
            const auto seenIt = s_botCandFirstSeen.find(actor);
            if (seenIt != s_botCandFirstSeen.end()) {
                const int latMs = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - seenIt->second).count());
                s_botCandFirstSeen.erase(seenIt);
                if (latMs >= 1500) {
                    std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                    if (f) {
                        char nameEsc[64]{};
                        snprintf(nameEsc, sizeof(nameEsc), "%.48s", itemName.c_str());
                        const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        f << "{\"sessionId\":\"c190fb\",\"runId\":\"post-fix\",\"hypothesisId\":\"B2\","
                          << "\"location\":\"RobotList.cpp:RobotList\",\"message\":\"bot_admit_latency\","
                          << "\"data\":{\"name\":\"" << nameEsc
                          << "\",\"latMs\":" << latMs
                          << ",\"key\":" << actor << "}"
                          << ",\"timestamp\":" << ts << "}\n";
                    }
                }
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
    int dbgBatchRootHits = 0;
    int dbgBatchPosHits = 0;
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
            if (!IsPlausibleWorldPos(scene) && IsPlausibleWorldPos(row.relative))
                scene = row.relative;
            if (!IsPlausibleWorldPos(scene)) {
                scene = ResolveBotWorldPos(
                    row.key, row.root, it->second.Mesh);
            }
            if (IsPlausibleWorldPos(scene)) {
                it->second.WorldPos = scene;
                ++dbgScenePosOk;
                ++dbgBatchPosHits;
                batchLiveFast[row.key] = true;
            } else {
                ++dbgScenePosFail;
            }
        }
    }

    for (auto it = localCache.begin(); it != localCache.end(); )
    {
        auto& actor = it->second;
        const uintptr_t key = it->first;

        std::string fname = engine.GetActorFNameStringCached(key);
        if (fname.empty())
            fname = engine.GetActorFNameString(key);

        if (IsBotEspPollutionName(fname) || IsBotEspPollutionLabel(actor.ActorName)) {
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

        if (IsBotEspPollutionLabel(actor.ActorName)) {
            ClearBotVisualMiss(key);
            it = localCache.erase(it);
            continue;
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
                ClearBotVisualMiss(key);
                s_lastBotVerify.erase(key);
                s_lastBotPartCache.erase(key);
                s_lastBotPartMesh.erase(key);
                it = localCache.erase(it);
                continue;
            }
            s_lastBotVerify[key] = nowRetain;
            if (s_lastBotVerify.size() > 4096)
                s_lastBotVerify.clear();
        } else {
            ++dbgVerifySkipped;
        }

        if (!getAllowType(actor.ActorName, 3)) {
            ClearBotVisualMiss(key);
            it = localCache.erase(it);
            continue;
        }

        // Prefer root filled by the P1 batch; cold-path resolve only if missing.
        if (!actor.rootComponent || !engine.IsValidPointer(actor.rootComponent))
            actor.rootComponent = ResolveBotSceneRoot(key);
        if (!actor.rootComponent) {
            ClearBotVisualMiss(key);
            it = localCache.erase(it);
            continue;
        }

        actor.category = 3;
        actor.Drawing = false;

        const uint8_t broken = ReadBotBrokenFlag(key);
        actor.IsBreaked = broken != 0;
        if (broken != 0 && !var::show_dead_bots) {
            ClearBotVisualMiss(key);
            it = localCache.erase(it);
            continue;
        }

        // Bot HP readers removed (wrong offsets; NDJSON probes deleted).
        actor.health = 0.f;
        actor.maxhealth = 0.f;

        if (!actor.Mesh || !IsValidPointer(actor.Mesh))
            actor.Mesh = GetActorSkeletalMesh(key);

        // P2: PopulateBotPartCache only on first admit, mesh change, or 500ms.
        // PositionRefreshPass delta-shifts BotPartPos between full rebuilds.
        bool needPartCache = actor.BotPartCount <= 0;
        if (!needPartCache) {
            const auto meshIt = s_lastBotPartMesh.find(key);
            if (meshIt == s_lastBotPartMesh.end() || meshIt->second != actor.Mesh)
                needPartCache = true;
        }
        if (!needPartCache) {
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
                    Vector3 delta = actor.WorldPos - cam.Location;
                    const float distanceSq = static_cast<float>(
                        delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                    actor.Distance = sqrtf(distanceSq) / 100.0f;
                    if (distanceSq <= maxDistSq) {
                        actor.Drawing = true;
                        ++dbgDrawing;
                    } else {
                        actor.Drawing = false;
                    }
                    ++it;
                } else {
                    // Ghost-flicker fix: reuse the last plausible position for a
                    // short window instead of blanking the box on one failed read.
                    bool froze = false;
                    if (const auto fit = s_botLastGoodPos.find(key);
                        fit != s_botLastGoodPos.end()
                        && nowRetain - fit->second.when <= kBotPosFreezeTtl) {
                        actor.WorldPos = fit->second.pos;
                        Vector3 delta = actor.WorldPos - cam.Location;
                        const float distanceSq = static_cast<float>(
                            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
                        actor.Distance = sqrtf(distanceSq) / 100.0f;
                        if (distanceSq <= maxDistSq) {
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
                                std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                                if (f) {
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
                                }
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
                            std::ofstream f("F:/Test/ARCs/debug-c190fb.log", std::ios::app);
                            if (f) {
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
                            }
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

        UpdateBotVelocity(actor, actor.WorldPos);

        actor.isVisible = true;

        Vector3 delta = actor.WorldPos - cam.Location;
        const float distanceSq = static_cast<float>(
            delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);

        actor.Distance = sqrtf(distanceSq) / 100.0f;
        if (distanceSq > maxDistSq) {
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

    {
        std::unique_lock<std::shared_mutex> lock(m_robotCacheMutex);
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

    if (var::show_debug_overlay) {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
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
            << " cover=0x" << std::hex << s_admitCoveredMask << std::dec
            << " cycleMs=" << s_admitLastCycleMs
            << " ringResets=" << s_admitRingResets
            << std::endl;
    }
}

namespace WorldScan {

void ClearRobotScannerStaticState()
{
    ClearRobotListStaticMaps();
}

} // namespace WorldScan
