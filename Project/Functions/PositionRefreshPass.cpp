#include "../Core/BotPosSanity.hpp"
#include "../Core/Engine.h"
#include "../Core/FeaturePolicy.hpp"
#include "../Core/Offsets.h"
#include "../Core/AgentLog.h"
#include "../Core/EntityDiagnostics.hpp"
#include "../Interface/Utils/Variables/index.h"
#include "../Interface/Utils/UpgradeOptions.hpp"
#include "WorldScanCommon.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <unordered_map>
#include <vector>

namespace {

struct PosRefreshKey {
    enum class CacheKind : uint8_t { Container, Item, Robot, Player };

    CacheKind kind = CacheKind::Robot;
    uintptr_t key = 0;
};

struct PosRefreshWork {
    PosRefreshKey key{};
    uintptr_t root = 0;
    // The actor's mesh component, read in the SAME scatter as the root. See
    // meshBuf below for why the pass needs it.
    uintptr_t mesh = 0;
    Engine::FVector3d worldBuf{};
    // Second ComponentToWorld candidate (0x310 + Transform::Translation). The
    // pinned offsets slot (0x2D0) is implausible on every bot root; reading both
    // candidates in the SAME scatter is what lets this pass sample bots at all
    // without a per-root probe round trip.
    Engine::FVector3d worldAltBuf{};
    Vector3 relBuf{};
    // Mesh ComponentToWorld::Translation. Both buffers above sit on the ROOT, and
    // Offsets::ComponentToWorld_Alt documents its slot (0x2F0) as zeros on this
    // build -- so a bot whose root read missed had NO live second source: the
    // sample was refused as position_implausible, the entry kept its old
    // position until BotEspExpiry's 3s kill, and the bot left the render frame
    // as collect_gate. Measured live: 625 refusals, 618 of them the flying bots
    // (Wasp 192, Snitch 132, Hornet 127, Firefly 85, Tick 82), with samples aged
    // 17-112s. The mesh CTW is the second source ResolveBotWorldPos already
    // trusts, so this is the same ladder, read where the 16ms sampler can use it.
    Engine::FVector3d meshBuf{};
};

void LogBotPositionDecision(
    uintptr_t key, const Engine::WorldCacheEntry* entry, const char* reason)
{
    if (!var::debug_ghost_bots)
        return;
    // Keyed per (actor, reason): one slot per actor let two alternating reasons
    // for the same actor log every pass instead of once a second (same defect as
    // Esp.cpp LogGhostBotDecision, which this mirrors).
    uint64_t reasonHash = 1469598103934665603ull;
    for (const char* p = reason; p && *p; ++p) {
        reasonHash = (reasonHash ^ static_cast<uint64_t>(
            static_cast<unsigned char>(*p))) * 1099511628211ull;
    }
    static std::unordered_map<uint64_t, uint64_t> last;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t throttleKey = (key * 0x9E3779B97F4A7C15ull) ^ reasonHash;
    const auto throttleIt = last.find(throttleKey);
    if (throttleIt != last.end() && now >= throttleIt->second
        && now - throttleIt->second < 1000)
        return;
    last[throttleKey] = now;
    if (last.size() > 4096)
        last.clear();
    const bool posValid = entry && IsPlausibleWorldPos(entry->WorldPos);
    const uint64_t age = entry && entry->livePositionSampleMs != 0
        && now >= entry->livePositionSampleMs
        ? now - entry->livePositionSampleMs : 0;
    EntityDiagnostics::LogBotDecision(
        key, entry ? entry->ActorName : std::string_view{}, reason,
        entry ? entry->Drawing : false,
        entry ? entry->botIdentityProven : false,
        entry ? entry->IsBreaked : false,
        posValid, age, entry ? entry->Distance : -1.f);
}

} // namespace

void Engine::PositionRefreshPass()
{
    if (!IsEspRaidActive())
        return;

    std::vector<PosRefreshWork> work;
    work.reserve(256);

    auto queueEntry = [&](PosRefreshKey::CacheKind kind, uintptr_t key, uintptr_t root,
                          uintptr_t mesh = 0) {
        if (!root || !IsValidPointer(root)) {
            if (kind == PosRefreshKey::CacheKind::Robot)
                LogBotPositionDecision(key, nullptr, "position_no_root");
            return;
        }
        if (mesh == root || !IsValidPointer(mesh))
            mesh = 0;
        work.push_back({ { kind, key }, root, mesh, {}, {}, {}, {} });
    };

    // Players + bots only. Containers/items are stationary — WorldPos set at admit.
    if (var::enableesp || UpgradeOptions::warnings || var::show_radar) {
        std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
        for (const auto& [key, entry] : playerCache) {
            // Uninitialized first-admission entries must be refreshed even when
            // Drawing is still false; otherwise they can never become drawable.
            // Refresh every cached player with a root, not just ones still
            // flagged Drawing. Otherwise one lost Drawing flag (EntityList's
            // ring runs at 2.4s) parks that player's position indefinitely.
            // ShouldDrawPlayerEsp still applies the distance gate, so queueing
            // unconditionally cannot draw a player who is out of range.
            (void)entry;
            queueEntry(PosRefreshKey::CacheKind::Player, key, entry.rootComponent,
                entry.actorMesh);
        }
    }

    if (var::showRobots || var::show_radar) {
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        for (const auto& [key, entry] : robotCache) {
            // Refresh every cached bot with a root, not only ones already
            // flagged Drawing. RobotList's ring cycle is ~7.4s, so gating the
            // 16ms sampler on a flag that ring owns left bot positions 364ms
            // old at the median (843ms p90) with velocity stuck at zero.
            if (!entry.rootComponent)
                continue;
            queueEntry(PosRefreshKey::CacheKind::Robot, key, entry.rootComponent,
                entry.Mesh);
        }
    }

    if (work.empty())
        return;

    int dbgPlayersQueued = 0;
    int dbgBotsQueued = 0;
    for (const PosRefreshWork& item : work) {
        if (item.key.kind == PosRefreshKey::CacheKind::Player)
            ++dbgPlayersQueued;
        else if (item.key.kind == PosRefreshKey::CacheKind::Robot)
            ++dbgBotsQueued;
    }

    // NOCACHE — cached VMM pages froze bot WorldPos (boxes stuck at spawn footprint).
    // Same class of bug EntityList fixed for players with read_nocache.
    ScatterSession scatter(/*cached=*/false);
    if (!scatter.isValid())
        return;

    size_t prepared = 0;
    for (PosRefreshWork& item : work) {
        scatter.prepare(item.root + Offsets::WorldLocation, item.worldBuf);
        scatter.prepare(
            item.root + Offsets::ComponentToWorld_Alt + Offsets::Transform_Translation,
            item.worldAltBuf);
        scatter.prepare(item.root + Offsets::RelativeLocation, item.relBuf);
        // Mesh CTW translation: the only second source that still moves when a
        // bot's root read misses. Prepared only when the actor has a mesh.
        if (item.mesh)
            scatter.prepare(item.mesh + Offsets::ComponentToWorld_Translation, item.meshBuf);
        ++prepared;
    }
    // VMMDLL_Scatter_ExecuteRead reports FALSE for the WHOLE batch when any one
    // prepared address is unreadable, and this function used to `return` there —
    // so a single stale root froze every bot and player position at once. The
    // probe proved it: velocityAgeMs was 0 in 1151/1151 bot samples, i.e. the
    // applyVelocity below never ran once. Buffers are value-initialised, so a
    // read that did not land stays zeroed and per-item IsPlausibleWorldPos
    // rejects it; never discard the whole batch.
    const bool execOk = prepared > 0 && scatter.execute();

    int dbgPlayerOk = 0;
    int dbgPlayerFail = 0;
    int dbgBotOk = 0;
    int dbgBotFail = 0;
    int dbgBotAlt = 0;
    int dbgBotJumpRejected = 0;
    int dbgBotReanchored = 0;
    int dbgPlayerAlt = 0;
    std::vector<uint64_t> dbgBotPrevAges;
    dbgBotPrevAges.reserve(prepared);

    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // One clock read for the whole pass. The visible/not-visible colour is then
    // refreshed every 16ms tick instead of only inside the ~200-1000ms scans,
    // which is what made the box colour trail behind cover. The per-bone
    // raycast (Core/EmbreeVisCheck.hpp) supplies that colour.

    auto applyVelocity = [&](
        Vector3& worldPos,
        Vector3& lastWorldPos,
        uint64_t& lastVelocityUpdate,
        Vector3& cachedVelocity,
        const Vector3& freshPos) {
        worldPos = freshPos;
        if (lastVelocityUpdate > 0.f) {
            const float dtSec =
                (nowMs - static_cast<uint64_t>(lastVelocityUpdate)) * 0.001f;
            if (dtSec > 0.001f && dtSec < 0.5f) {
                const Vector3 delta = freshPos - lastWorldPos;
                Vector3 newVel{
                    delta.x / dtSec,
                    delta.y / dtSec,
                    delta.z / dtSec};
                WorldScan::BlendCachedVelocity(cachedVelocity, newVel);
            }
        }
        lastWorldPos = freshPos;
        lastVelocityUpdate = nowMs;
};

// One unique_lock per cache kind — not per entity.
    // Collect any diagnostic payloads here and emit AFTER releasing the writer
    // lock so paint / aim / radar readers aren't blocked by a std::to_string
    // storm or the logger push.
    struct PendingRefreshFail {
        uint64_t key;
        uint64_t root;
        Vector3 w;
        Vector3 wa;
        Vector3 rel;
    };
    std::vector<PendingRefreshFail> pendingPlayerFails;
    {
        std::unique_lock<std::shared_mutex> lock(m_playerCacheMutex);
        for (PosRefreshWork& item : work) {
            if (item.key.kind != PosRefreshKey::CacheKind::Player)
                continue;
            Vector3 pos = ToVector3(item.worldBuf);
            if (!IsPlausibleWorldPos(pos)) {
                const Vector3 alt = ToVector3(item.worldAltBuf);
                if (IsPlausibleWorldPos(alt)) {
                    pos = alt;
                    ++dbgPlayerAlt;
                }
            }
            if (!IsPlausibleWorldPos(pos) && IsPlausibleWorldPos(item.relBuf))
                pos = item.relBuf;
            if (!IsPlausibleWorldPos(pos)) {
                ++dbgPlayerFail;
                static auto s_lastPf = std::chrono::steady_clock::time_point{};
                const auto nowPf = std::chrono::steady_clock::now();
                if (nowPf - s_lastPf >= std::chrono::seconds(2)) {
                    s_lastPf = nowPf;
                    pendingPlayerFails.push_back(PendingRefreshFail{
                        item.key.key,
                        item.root,
                        ToVector3(item.worldBuf),
                        ToVector3(item.worldAltBuf),
                        item.relBuf});
                }
                continue;
            }
            auto it = playerCache.find(item.key.key);
            if (it == playerCache.end()) {
                ++dbgPlayerFail;
                continue;
            }
            applyVelocity(
                it->second.WorldPos,
                it->second.lastWorldPos,
                it->second.lastVelocityUpdate,
                it->second.cachedVelocity,
                pos);
            it->second.positionInitialized = true;
            it->second.positionSampleMs = nowMs;
            ++dbgPlayerOk;
            // A first successful refresh completes admission. EntityList will
            // apply the strict distance decision on its next pass.
            it->second.Drawing = true;
        }
    }
    // Lock is released — now safe to build strings and push to the async logger
    // without blocking paint / aim readers.
    for (const PendingRefreshFail& pf : pendingPlayerFails) {
        std::string line = "{\"ev\":\"player_refresh_fail\",\"key\":\""
            + std::to_string(pf.key)
            + "\",\"root\":\"" + std::to_string(pf.root) + "\""
            + ",\"w\":[" + std::to_string(pf.w.x) + "," + std::to_string(pf.w.y) + "," + std::to_string(pf.w.z) + "]"
            + ",\"wAlt\":[" + std::to_string(pf.wa.x) + "," + std::to_string(pf.wa.y) + "," + std::to_string(pf.wa.z) + "]"
            + ",\"rel\":[" + std::to_string(pf.rel.x) + "," + std::to_string(pf.rel.y) + "," + std::to_string(pf.rel.z) + "]"
            + "}\n";
        EntityDiagnostics::LogRaw(line);
    }
    {
        std::unique_lock<std::shared_mutex> lock(m_robotCacheMutex);
        for (PosRefreshWork& item : work) {
            if (item.key.kind != PosRefreshKey::CacheKind::Robot)
                continue;
            auto it = robotCache.find(item.key.key);
            Vector3 pos = ToVector3(item.worldBuf);
            if (!IsPlausibleWorldPos(pos)) {
                const Vector3 alt = ToVector3(item.worldAltBuf);
                if (IsPlausibleWorldPos(alt)) {
                    pos = alt;
                    ++dbgBotAlt;
                }
            }
            // The two sources below name a DIFFERENT object than the bot's root
            // (its mesh component), so they are held to a step test: a shared or
            // freed mesh pointer still reads as a plausible vector, just somewhere
            // else in the level. Refusing it keeps the failure mode "no new sample"
            // -- the bot holds its last position -- instead of "box drawn on the
            // wrong object", which is worse than a missing box. 6,000cm over one
            // 16ms sample cannot be the same bot.
            constexpr double kMaxFallbackStepCm = 6000.0;
            const auto fallbackStepOk = [&](const Vector3& cand) {
                if (!IsPlausibleWorldPos(cand))
                    return false;
                if (it == robotCache.end() || !IsPlausibleWorldPos(it->second.WorldPos))
                    return true;  // nothing to compare against: first sample
                const Vector3& prev = it->second.WorldPos;
                const double dx = cand.x - prev.x;
                const double dy = cand.y - prev.y;
                const double dz = cand.z - prev.z;
                return std::sqrt(dx * dx + dy * dy + dz * dz) <= kMaxFallbackStepCm;
            };
            // Mesh CTW before the relative fallback: a mesh component's relative
            // location is a parent offset, while its CTW is true world space.
            if (!IsPlausibleWorldPos(pos) && item.mesh
                && fallbackStepOk(ToVector3(item.meshBuf))) {
                pos = ToVector3(item.meshBuf);
                ++dbgBotAlt;
            }
            // Last resort: a RootComponent has no parent, so its RelativeLocation
            // IS its world position -- the same fallback ResolveBotWorldPos applies
            // for RollBot/Fireball. Guarded on root != mesh: ResolveBotSceneRoot
            // substitutes the mesh when the root read fails, and a mesh's relative
            // is a parent offset, so using it there would place the box wrongly
            // instead of leaving the bot hidden. Only reached when every CTW read
            // above missed.
            if (!IsPlausibleWorldPos(pos) && item.root != item.mesh
                && fallbackStepOk(item.relBuf))
                pos = item.relBuf;
            if (!IsPlausibleWorldPos(pos)) {
                LogBotPositionDecision(item.key.key,
                    it == robotCache.end() ? nullptr : &it->second,
                    "position_implausible");
                ++dbgBotFail;
                continue;
            }
            if (it == robotCache.end()) {
                LogBotPositionDecision(item.key.key, nullptr, "position_cache_miss");
                ++dbgBotFail;
                continue;
            }
            // Plausibility says the vector is a coordinate at all; it cannot say
            // it is THIS bot's coordinate, and a poisoned read is usually an
            // ordinary point somewhere else in the level. The entry's own history
            // can: a bot cannot cross a kilometre between two samples 16 ms apart.
            // Refusal means "no new sample" -- the previous position is kept and
            // no stamp advances, so Core/BotEspExpiry.hpp owns what happens to a
            // bot that has stopped sampling. See Core/BotPosSanity.hpp for the two
            // anchors and the re-anchor that stops a poisoned cache from rejecting
            // every correct read afterwards.
            {
                const BotPosSanity::Sample newest{
                    it->second.WorldPos,
                    it->second.positionSampleMs,
                    IsPlausibleWorldPos(it->second.WorldPos)};
                const BotPosSanity::Sample older{
                    it->second.prevSampleWorldPos,
                    it->second.prevSampleMs,
                    it->second.prevSampleMs != 0
                        && IsPlausibleWorldPos(it->second.prevSampleWorldPos)};
                const BotPosSanity::Verdict verdict = BotPosSanity::Judge(
                    newest, older, pos, nowMs, it->second.posJumpRejects);
                if (!verdict.accept) {
                    if (it->second.posJumpRejects < 255)
                        ++it->second.posJumpRejects;
                    ++dbgBotJumpRejected;
                    LogBotPositionDecision(
                        item.key.key, &it->second, "position_jump_rejected");
                    continue;
                }
                it->second.posJumpRejects = 0;
                if (verdict.reanchored) {
                    ++dbgBotReanchored;
                    LogBotPositionDecision(
                        item.key.key, &it->second, "position_reanchor");
                }
            }
            ++dbgBotOk;
            // Age of the sample being replaced: how stale the 16ms sampler found
            // this bot. If this is large the sampler is not the bottleneck; if it
            // is ~16ms the boxes were stale because paint read a slow cache.
            if (it->second.positionSampleMs != 0 && nowMs > it->second.positionSampleMs)
                dbgBotPrevAges.push_back(nowMs - it->second.positionSampleMs);
            // Keep the previous sample so paint can interpolate between the two
            // newest ones instead of stepping on arrival.
            if (it->second.positionSampleMs != 0) {
                it->second.prevSampleWorldPos = it->second.WorldPos;
                it->second.prevSampleMs = it->second.positionSampleMs;
            }
            const Vector3 oldPos = it->second.WorldPos;
            applyVelocity(
                it->second.WorldPos,
                it->second.lastWorldPos,
                it->second.lastVelocityUpdate,
                it->second.cachedVelocity,
                pos);
            it->second.positionSampleMs = nowMs;
            it->second.livePositionSampleMs = nowMs;
            // Keep part/aim helpers glued to the live root so boxes cannot stay
            // planted on stale mesh CTW after WorldPos moves.
            const Vector3 d{
                it->second.WorldPos.x - oldPos.x,
                it->second.WorldPos.y - oldPos.y,
                it->second.WorldPos.z - oldPos.z};
            if (d.x != 0.0 || d.y != 0.0 || d.z != 0.0) {
                if (IsPlausibleWorldPos(it->second.CenterWorldPos)) {
                    it->second.CenterWorldPos.x += d.x;
                    it->second.CenterWorldPos.y += d.y;
                    it->second.CenterWorldPos.z += d.z;
                }
                if (it->second.hasBotHeadWorldPos
                    && IsPlausibleWorldPos(it->second.BotHeadWorldPos)) {
                    it->second.BotHeadWorldPos.x += d.x;
                    it->second.BotHeadWorldPos.y += d.y;
                    it->second.BotHeadWorldPos.z += d.z;
                }
                for (int i = 0; i < it->second.BotPartCount; ++i) {
                    if (!IsPlausibleWorldPos(it->second.BotPartPos[i]))
                        continue;
                    it->second.BotPartPos[i].x += d.x;
                    it->second.BotPartPos[i].y += d.y;
                    it->second.BotPartPos[i].z += d.z;
                }
            }
        }
    }

    // #region agent log
    // pos_refresh: proves whether the 16ms sampler actually lands on bot roots.
    //   execOk 0 with bots > 0 => the scatter batch failed (whole-pass loss).
    //   bOk 0 / bFail == bots  => every root address read implausible.
    //   bPrevAgeP50 ~ bPrevAgeMax ~ pass cadence => sampler is healthy.
    {
        static uint64_t s_lastPosProbeMs = 0;
        const uint64_t nowProbeMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        if (nowProbeMs - s_lastPosProbeMs >= 500) {
            s_lastPosProbeMs = nowProbeMs;
            uint64_t ageP50 = 0;
            uint64_t ageMax = 0;
            if (!dbgBotPrevAges.empty()) {
                std::sort(dbgBotPrevAges.begin(), dbgBotPrevAges.end());
                ageP50 = dbgBotPrevAges[dbgBotPrevAges.size() / 2];
                ageMax = dbgBotPrevAges.back();
            }
            
            std::ostringstream oss;
            oss << "{\"sessionId\":\"c190fb\",\"runId\":\"pos-refresh\","
               << "\"location\":\"PositionRefreshPass.cpp\","
               << "\"message\":\"pos_refresh\",\"data\":{"
               << "\"work\":" << work.size()
               << ",\"players\":" << dbgPlayersQueued
               << ",\"bots\":" << dbgBotsQueued
               << ",\"prepared\":" << prepared
               << ",\"execOk\":" << (execOk ? 1 : 0)
               << ",\"pOk\":" << dbgPlayerOk
               << ",\"pFail\":" << dbgPlayerFail
               << ",\"bOk\":" << dbgBotOk
               << ",\"bAlt\":" << dbgBotAlt
               << ",\"pAlt\":" << dbgPlayerAlt
               << ",\"bFail\":" << dbgBotFail
               << ",\"jRej\":" << dbgBotJumpRejected
               << ",\"jReanchor\":" << dbgBotReanchored
               << ",\"bPrevAgeP50\":" << ageP50
               << ",\"bPrevAgeMax\":" << ageMax
               << "},\"ts\":" << nowProbeMs << "}\n";
            EntityDiagnostics::LogRaw(oss.str());
        }
    }
    // #endregion
}

void Engine::BuildEspRenderFrameWorker()
{
    if (!IsEspRaidActive())
        return;

    EspRenderFrame frame{};
    if (!CollectEspRenderFrame(frame))
        return;

    // A world transition may invalidate the caches while the frame worker is
    // doing DMA. Never publish that result into the next world.
    if (frame.worldGeneration != m_worldGeneration.load(std::memory_order_acquire)) {
        RecordEspFrameFailure(EspFrameResult::GenerationChanged);
        return;
    }

    std::unique_lock<std::shared_mutex> lock(m_espFrameMutex);
    // Recheck while holding the publication lock. ClearEspCaches() increments
    // the generation before taking this lock, so a transition cannot publish a
    // stale result between the optimistic check and the swap.
    if (frame.worldGeneration != m_worldGeneration.load(std::memory_order_acquire)) {
        RecordEspFrameFailure(EspFrameResult::GenerationChanged);
        return;
    }
    m_espFrameShared = std::make_shared<EspRenderFrame>(std::move(frame));
    m_lastEspFrameValid.store(m_espFrameShared->valid, std::memory_order_release);
}
