#include "../Core/Engine.h"
#include "../Core/ActorType.h"
#include "../Core/AssetNames.h"
#include "../Core/IntervalTimer.h"
#include "CollisionLos.h"
#include "RobotList.h"
#include "WorldScanCommon.h"

#include <chrono>
#include <cmath>
#include <atomic>
#include <iostream>
#include <thread>
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

std::string ResolveBotLabelFromFName(const std::string& name);
std::string ResolveBotLabelFromActor(uintptr_t actor, const std::string& fnameHint);
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

    if (!label.empty() && !isBotEntry && !IsAcceptedBotEspLabel(engine, label, fname))
        return {};

    return label;
}

std::string ResolveBotLabelFromFName(const std::string& name)
{
    if (name.empty())
        return {};
    return ResolveRobotTypeFromFName(engine, name);
}

std::string ResolveBotLabelFromActor(uintptr_t actor, const std::string& fnameHint)
{
    // Single chain — same as draw/admission.
    return ResolveBotTypeLabel(actor, fnameHint);
}

bool ActorHasKnownBotIdentity(uintptr_t actor, const std::string& fnameHint)
{
    return !ResolveBotLabelFromActor(actor, fnameHint).empty();
}

bool HasConstructableEnemyAsset(uintptr_t actor)
{
    return WorldScan::HasArcEnemyAssetPointer(actor);
}

bool HasVerifiedEnemyAsset(uintptr_t actor, const std::string& fname)
{
    if (!HasConstructableEnemyAsset(actor))
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

bool IsLikelyContainerActor(uintptr_t actor, const std::string& fname)
{
    return WorldScan::LooksLikeContainerActor(actor, fname);
}

bool IsLikelyWorldItemActor(uintptr_t actor)
{
    if (!actor)
        return false;

    if (HasConstructableEnemyAsset(actor))
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

    if (IsLikelyContainerActor(actor, fname))
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

// -------------------------------------------------------------------------
// Bot check-and-balance verifier.
//
// This is THE single authoritative gate. Every admission pass and every
// retention pass runs an actor through it, so a bot must independently prove
// itself twice before it draws. Guarantee: only real ARC enemies survive —
// never the local player, other players, guns, dropped items, boxes or
// containers. Fuzzy fname/mesh name matches ALONE are never enough; a bot must
// present real structural proof (an ARC enemy data-asset, the ARC class tag,
// or a constructable enemy pointer) backed by a resolvable identity.
// -------------------------------------------------------------------------
// Skip VerifyBotActor on props/containers — full verify on every actor was taking
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

    const std::string fnameCached = engine.GetActorFNameStringCached(actor);
    if (!fnameCached.empty()) {
        if (!ResolveRobotTypeFromFName(engine, fnameCached).empty())
            return true;
        if (!LookupEnemyBotByFName(fnameCached).empty())
            return true;
    }

    return false;
}

bool VerifyBotActor(uintptr_t actor, uintptr_t localPawn, const std::string& fname)
{
    if (!actor || !engine.IsValidPointer(actor))
        return false;
    if (localPawn && actor == localPawn)
        return false;

    // (1) Absolute player negatives — veto every positive signal below.
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

    // Arc entry / cargo / caches / socket containers are world ESP — never bots,
    // even when they carry constructable or enemy-type data assets.
    {
        std::string probe = fname;
        if (probe.empty())
            probe = engine.GetActorFNameStringCached(actor);
        if (probe.empty())
            probe = engine.GetActorFNameString(actor);
        const std::string classFname = engine.GetActorClassFName(actor);
        if (IsLikelyContainerActor(actor, probe)
            || (!classFname.empty() && IsLikelyContainerActor(actor, classFname))
            || (!probe.empty() && FnameLooksLikeWorldContainer(probe))
            || (!classFname.empty() && FnameLooksLikeWorldContainer(classFname)))
            return false;
    }

    // (2) DEFINITIVE proof: constructable enemy/AI pointer on a non-item actor.
    // Admit on pointer + scene root even when DA fname is not decrypted yet;
    // ResolveBotDrawLabel fills the real name at draw time.
    if (HasConstructableEnemyAsset(actor)) {
        if (HasWorldItemStructure(actor))
            return false;
        return ResolveBotSceneRoot(actor) != 0;
    }

    // (3) No definitive proof → weak candidate. Reject anything that carries
    //     gun / item / pickup / container structure. This is what kept your gun,
    //     ground boxes and containers OUT — they never reach the bot cache.
    if (HasWorldItemStructure(actor))
        return false;
    if (IsLikelyContainerActor(actor, fname))
        return false;

    // (4) A bot is a skeletal pawn with a resolvable scene root.
    const uintptr_t mesh = engine.GetActorSkeletalMesh(actor);
    if (!mesh || !engine.IsValidPointer(mesh))
        return false;
    if (ResolveBotSceneRoot(actor) == 0)
        return false;

    // (5) Corroborated weak proof: a constructable enemy/AI pointer OR the ARC
    //     class tag, BACKED by a resolvable bot identity. A fuzzy name match with
    //     no structural signal behind it is rejected — that was the leak that
    //     turned guns/boxes into "bots".
    const bool structuralBotSignal =
        HasConstructableEnemyAsset(actor)
        || ArcActorType::IsBotClassId(masked)
        || ArcActorType::IsTargetBotActor(actor);
    if (structuralBotSignal && ActorHasKnownBotIdentity(actor, fname))
        return true;

    return false;
}

// Internal cache label when struct detection succeeds but fname decrypt fails.
// Never shown on screen — draw uses ResolveBotDrawLabel for a real name.

std::string ResolveStructBotAdmissionLabel(uintptr_t actor, const std::string& fname)
{
    std::string label = FinalizeBotAdmissionLabel(actor, fname, true);
    if (!label.empty())
        return label;

    // Identity decrypt failed — still admit struct bots for boxes; draw resolves name.
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
    if (!actor)
        return true;
    if (localPawn && actor == localPawn)
        return true;
    if (WorldScan::LooksLikePlayerPawn(actor, localPawn))
        return true;

    if (engine.IsCachedPlayer(actor))
        return true;

    // Near-self skip: coincident ghost/duplicate pawn near local position.
    // Never skip proven ARC bots (melee/on-top Wasps must still admit).
    if (IsPlausibleWorldPos(localPos)) {
        const uintptr_t root = ResolveBotSceneRoot(actor);
        if (root) {
            const Vector3 botPos = ResolveBotWorldPos(actor, root, 0);
            if (IsPlausibleWorldPos(botPos)) {
                const double dx = botPos.x - localPos.x;
                const double dy = botPos.y - localPos.y;
                const double dz = botPos.z - localPos.z;
                const double distM = std::sqrt(dx * dx + dy * dy + dz * dz) / 100.0;
                if (distM < 0.5
                    && !ArcActorType::IsAnyBotActor(actor)
                    && !HasStrongEnemyDataAsset(actor)
                    && !WorldScan::HasArcEnemyAssetPointer(actor))
                    return true;
            }
        }
    }

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
            const double mag2 = static_cast<double>(newVel.x) * newVel.x
                + static_cast<double>(newVel.y) * newVel.y
                + static_cast<double>(newVel.z) * newVel.z;
            if (mag2 < (3000.0 * 3000.0)) {
                actor.cachedVelocity.x = actor.cachedVelocity.x * 0.5f + newVel.x * 0.5f;
                actor.cachedVelocity.y = actor.cachedVelocity.y * 0.5f + newVel.y * 0.5f;
                actor.cachedVelocity.z = actor.cachedVelocity.z * 0.5f + newVel.z * 0.5f;
            } else {
                actor.cachedVelocity = {};
            }
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

Vector3 ResolveBotWorldPos(uintptr_t actor, uintptr_t root, uintptr_t mesh)
{
    if (root) {
        const Vector3 fromRoot = ReadComponentWorldPos(root);
        if (!IsZeroWorldPos(fromRoot))
            return fromRoot;
    }

    if (mesh) {
        const Vector3 fromMesh = ReadComponentWorldPos(mesh);
        if (!IsZeroWorldPos(fromMesh))
            return fromMesh;
    }

    const uintptr_t embarkMesh = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
    if (embarkMesh && embarkMesh != mesh) {
        const Vector3 fromEmbark = ReadComponentWorldPos(embarkMesh);
        if (!IsZeroWorldPos(fromEmbark))
            return fromEmbark;
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
        if (IsPlausibleWorldPos(meshCenter)) {
            actor.CenterWorldPos = meshCenter;
            actor.WorldPos = meshCenter;
        } else {
            const float inv = 1.f / static_cast<float>(valid);
            actor.CenterWorldPos = Vector3{ sum.x * inv, sum.y * inv, sum.z * inv };
            actor.WorldPos = actor.CenterWorldPos;
        }
        return;
    }

    actor.WorldPos = ResolveBotWorldPos(key, actor.rootComponent, actor.Mesh);
    actor.CenterWorldPos = actor.WorldPos;
    if (!IsZeroWorldPos(actor.WorldPos) && actor.BotPartCount < Engine::WorldCacheEntry::kMaxBotParts) {
        actor.BotPartPos[actor.BotPartCount++] = actor.WorldPos;
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

    if (IsLikelyContainerActor(actor, fnameHint))
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

// Transient DMA / mesh transform reads can fail for a few passes; don't evict
// bots from cache on the first miss or ESP randomly blinks off.
static std::unordered_map<uintptr_t, uint8_t> s_botVisualMisses;
static constexpr uint8_t kBotVisualMissEvict = 15;

static bool BotVisualMissShouldEvict(uintptr_t key, bool visualOk)
{
    if (visualOk) {
        s_botVisualMisses.erase(key);
        return false;
    }
    const uint8_t misses = ++s_botVisualMisses[key];
    return misses >= kBotVisualMissEvict;
}

static void ClearBotVisualMiss(uintptr_t key)
{
    s_botVisualMisses.erase(key);
}

bool HasLiveBotVisual(uintptr_t actor, uintptr_t mesh)
{
    const uintptr_t root = Memory::read<uintptr_t>(actor + Offsets::RootComponent);
    if (IsPlausibleWorldPos(ResolveBotWorldPos(actor, root, mesh)))
        return true;

    if (ComponentHasPlausibleWorldPos(mesh))
        return true;

    if (mesh && engine.IsValidPointer(mesh)) {
        std::vector<uintptr_t> childComps;
        ReadChildComponentsLocal(mesh, childComps, 3);
        for (uintptr_t child : childComps) {
            if (ComponentHasPlausibleWorldPos(child))
                return true;
        }
    }

    const uintptr_t embarkMesh = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
    if (embarkMesh && embarkMesh != mesh && engine.IsValidPointer(embarkMesh)) {
        if (ComponentHasPlausibleWorldPos(embarkMesh))
            return true;
        std::vector<uintptr_t> embarkChildren;
        ReadChildComponentsLocal(embarkMesh, embarkChildren, 2);
        for (uintptr_t child : embarkChildren) {
            if (ComponentHasPlausibleWorldPos(child))
                return true;
        }
    }

    return false;
}

} // namespace

uint8_t ReadBotBrokenFlag(uintptr_t actor)
{
    // Constructable pawns: only trust explicit destroyed flag @0x1210.
    if (WorldScan::HasArcEnemyAssetPointer(actor)) {
        const uint8_t destroyed =
            Memory::read<uint8_t>(actor + Offsets::Constructable_bIsDestroyed);
        return destroyed == 1 ? 1 : 0;
    }

    uint8_t broken = Memory::read<uint8_t>(actor + Offsets::bIsBreaked);
    if (broken != 1)
        broken = Memory::read<uint8_t>(actor + Offsets::Constructable_bIsDestroyed);
    if (broken == 1)
        return 1;

    // Wasp-type / regular bots: treat depleted HealthComponent as dead.
    const double h = Engine::ReadHealthComponentStat(actor, Offsets::Health);
    const double m = Engine::ReadHealthComponentStat(actor, Offsets::MaxHealth);
    if (std::isfinite(h) && std::isfinite(m) && m > 0.0 && h <= 0.0)
        return 1;

    return 0;
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
    // Single ordered chain: fname → class → enemy DA → mesh → embark → husk.
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
    if (!cachedLabel.empty()
        && cachedLabel != kBotStructAdmissionToken
        && IsAcceptedBotEspLabel(engine, cachedLabel, fnameHint))
        return cachedLabel;

    const std::string resolved = ResolveBotTypeLabel(actor, fnameHint);
    if (resolved.empty())
        return {};
    if (!IsAcceptedBotEspLabel(engine, resolved, fnameHint))
        return {};
    return resolved;
}

void Engine::RobotList()
{
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
    if (var::obstruction_check)
        CollisionLos::ScheduleWorldRebuild(sGWorld, localPos);

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
            it = localCache.erase(it);
        } else
            ++it;
    }

    static IntervalTimer admissionTimer(50);
    const bool doAdmission = admissionTimer.fire() || localCache.empty();

    int dbgScanned = 0;
    int dbgAdmitted = 0;
    int dbgZeroPos = 0;
    int dbgDrawing = 0;
    int dbgVisSkip = 0;
    int dbgDistSkip = 0;
    int dbgStructHit = 0;

    if (doAdmission) {
    for (uint64_t actor : currentActors)
    {
        if (!actor)
            continue;

        if (sAcknowledgedPawn && actor == sAcknowledgedPawn)
            continue;

        if (ShouldSkipBotActor(actor, sAcknowledgedPawn, localPos))
            continue;

        if (!QuickBotCandidate(actor))
            continue;

        std::string fname = engine.GetActorFNameStringCached(actor);
        if (fname.empty())
            fname = engine.GetActorFNameString(actor);

        // CHECK #1 — authoritative verification at admission. Only actors that
        // prove they are ARC bots (and not players/guns/items/containers) pass.
        if (!VerifyBotActor(actor, sAcknowledgedPawn, fname)) {
            ++dbgStructHit;
            continue;
        }

        if (localCache.contains(actor))
            continue;

        ++dbgScanned;

        // Resolve a real name. Struct bots with encrypted fnames fall back to an
        // internal admission token; Esp re-resolves a display name at draw time.
        std::string itemName = ResolveStructBotAdmissionLabel(actor, fname);
        if (itemName.empty())
            itemName = ResolveRobotTypeFromFName(engine, fname);
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
    }
    }

    // Scatter-batch root transforms for the hot update pass.
    std::vector<std::unordered_map<uintptr_t, WorldCacheEntry>::iterator> updateIters;
    std::vector<Vector3> rootPosBufs;
    updateIters.reserve(localCache.size());
    rootPosBufs.reserve(localCache.size());

    for (auto it = localCache.begin(); it != localCache.end(); ++it) {
        const uintptr_t key = it->first;
        it->second.rootComponent = ResolveBotSceneRoot(key);
        if (!it->second.rootComponent)
            continue;
        updateIters.push_back(it);
        rootPosBufs.emplace_back();
    }

    if (!updateIters.empty()) {
        ScatterSession scatter;
        if (scatter.isValid()) {
            bool ok = true;
            for (size_t i = 0; i < updateIters.size(); ++i) {
                ok = ok && scatter.prepare(
                    updateIters[i]->second.rootComponent + Offsets::RelativeLocation,
                    rootPosBufs[i]);
            }
            if (ok && scatter.execute()) {
                for (size_t i = 0; i < updateIters.size(); ++i) {
                    Vector3 pos = rootPosBufs[i];
                    const Vector3 scene =
                        Engine::ReadSceneWorldPos(updateIters[i]->second.rootComponent);
                    if (IsPlausibleWorldPos(scene))
                        pos = scene;
                    if (IsPlausibleWorldPos(pos))
                        updateIters[i]->second.WorldPos = pos;
                }
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

        if (actor.ActorName.empty()
            || actor.ActorName == kBotStructAdmissionToken
            || !IsAcceptedBotEspLabel(engine, actor.ActorName, fname)) {
            if (const std::string resolved =
                    ResolveBotDrawLabel(key, actor.ActorName, fname);
                !resolved.empty())
                actor.ActorName = resolved;
        }

        // CHECK #2 — cheap retain gate (admission already ran full VerifyBotActor).
        if (!StillLooksLikeBot(key, sAcknowledgedPawn, actor.category, localPos)) {
            it = localCache.erase(it);
            continue;
        }

        if (!getAllowType(actor.ActorName, 3)) {
            it = localCache.erase(it);
            continue;
        }

        actor.rootComponent = ResolveBotSceneRoot(key);
        if (!actor.rootComponent) {
            it = localCache.erase(it);
            continue;
        }

        actor.category = 3;
        actor.Drawing = false;

        const uint8_t broken = ReadBotBrokenFlag(key);
        actor.IsBreaked = broken != 0;
        if (broken != 0 && !var::show_dead_bots) {
            it = localCache.erase(it);
            continue;
        }

        // Health: constructables via HealthService; regular bots via HealthComponent.
        if (WorldScan::HasArcEnemyAssetPointer(key)) {
            const uintptr_t healthSvc =
                Memory::read<uintptr_t>(key + static_cast<uint64_t>(Offsets::Constructable_HealthService));
            if (healthSvc && Memory::IsValidPtrFast2(healthSvc)) {
                const double h = Memory::read<double>(healthSvc + static_cast<uint64_t>(Offsets::BotHealthCached));
                const double m = Memory::read<double>(healthSvc + static_cast<uint64_t>(Offsets::BotHealthMax));
                if (std::isfinite(h) && std::isfinite(m) && m > 0.0) {
                    actor.health    = static_cast<float>(h);
                    actor.maxhealth = static_cast<float>(m);
                }
            }
        } else {
            const double h = Engine::ReadHealthComponentStat(key, Offsets::Health);
            const double m = Engine::ReadHealthComponentStat(key, Offsets::MaxHealth);
            if (std::isfinite(h) && std::isfinite(m) && m > 0.0) {
                actor.health    = static_cast<float>(h);
                actor.maxhealth = static_cast<float>(m);
            }
        }

        if (!actor.Mesh || !IsValidPointer(actor.Mesh))
            actor.Mesh = GetActorSkeletalMesh(key);

        PopulateBotPartCache(actor, key);

        // Destroyed bots can linger in the actor list with a stale root transform.
        if (!broken) {
            bool visualOk = HasLiveBotVisual(key, actor.Mesh);
            // Scatter-read WorldPos is authoritative once plausible — do not hide
            // bots when mesh child probes fail transiently.
            if (!visualOk && IsPlausibleWorldPos(actor.WorldPos))
                visualOk = true;
            if (!visualOk) {
                ++dbgVisSkip;
                if (BotVisualMissShouldEvict(key, false)) {
                    ClearBotVisualMiss(key);
                    it = localCache.erase(it);
                } else {
                    actor.Drawing = false;
                    ++it;
                }
                continue;
            }
            BotVisualMissShouldEvict(key, true);
        }

        if (!IsPlausibleWorldPos(actor.WorldPos)) {
            ++dbgZeroPos;
            it = localCache.erase(it);
            continue;
        }

        UpdateBotVelocity(actor, actor.WorldPos);

        Vector3 losTarget = IsPlausibleWorldPos(actor.CenterWorldPos)
            ? actor.CenterWorldPos
            : actor.WorldPos;

        actor.isVisible = var::visiblecheck
            ? (VisibleBotActor(key)
                && (!var::obstruction_check || HasLineOfSight(cam.Location, losTarget)))
            : true;

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

    if (m_worldGeneration.load(std::memory_order_acquire) != genAtStart)
        return;

    {
        std::unique_lock<std::shared_mutex> lock(m_robotCacheMutex);
        robotCache = std::move(localCache);
    }

    if (var::show_debug_overlay) {
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

        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        std::cout << "[debugRobot] scanned=" << dbgScanned
            << " admitted=" << dbgAdmitted
            << " drawing=" << dbgDrawing
            << " cache=" << robotCache.size()
            << " structHit=" << dbgStructHit
            << " fnameHit=" << dbgFnameHit
            << " visSkip=" << dbgVisSkip
            << " distSkip=" << dbgDistSkip
            << " maxDist=" << static_cast<int>(maxDistM)
            << " zeroPos=" << dbgZeroPos
            << " enemyCount=" << dbgEnemyCount
            << std::endl;
    }
}
