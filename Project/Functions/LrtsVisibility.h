#pragma once
// LRTS Visibility — encrypted LastRenderTimeOnScreen occlusion check
//
// Denuvo Anti-Cheat XOR-encrypts render timestamps on mesh components.
// This module auto-discovers the XOR key at runtime from a rendered mesh,
// then checks if LRTS is fresh (visible) or stale (behind wall).
//
// Thread-safe: all state is behind a mutex. No Sleep() calls.
// Non-blocking: scan verification is deferred across frames.

#include <cstdint>
#include <array>
#include <atomic>
#include <fstream>
#include <mutex>

namespace LrtsVis {

// Result of a visibility check
enum class Result {
    Unknown,    // warmup, no key yet, failed read
    Visible,    // LRTS ~= worldTime (not behind wall)
    Occluded,   // LRTS stale (behind wall)
};

// Per-instance state (one per mesh component being tracked).
// Holds only the scan machinery. The discovered offsets and keys live on
// SessionState because they are identical for every mesh in the process —
// keeping them here meant an occluded mesh could never learn them, since
// candidate collection requires the mesh to be in frustum.
struct MeshState {
    uint64_t meshComp = 0;      // USkeletalMeshComponent address
    uint32_t generation = 0;    // stale vs SessionState::generation => wipe

    // Scan state machine
    enum class ScanPhase {
        Idle,           // no scan in progress
        Collected,      // candidates collected, waiting for verification
        Verified,       // key locked
    };
    ScanPhase phase = ScanPhase::Idle;
    int scanAttempts = 0;

    // Pending verification (non-blocking deferred check)
    struct PendingCandidate {
        uint32_t offset = 0;
        uint32_t raw = 0;
        uint32_t key = 0;
        float worldTimeAtCapture = 0.f;
    };
    // Must cover the whole scan window. The Phase 1 filter cannot narrow
    // anything down — its key derivation cancels out, so every non-zero dword
    // qualifies — which meant a cap of 32 handed Phase 2 only the first quarter
    // of the range and silently discarded the rest. With the window widened to
    // 0x800 (512 dwords), the cap must hold every dword in the range so a
    // high-offset LRTS is never silently dropped.
    static constexpr int kMaxPending = 512;
    std::array<PendingCandidate, kMaxPending> pending{};
    int pendingCount = 0;

    void Reset() {
        phase = ScanPhase::Idle;
        scanAttempts = 0;
        pendingCount = 0;
    }
};

// Global LRTS discovery state (one for all meshes — offsets and keys are
// session-wide, so the first mesh that renders unlocks reads for all of them)
struct SessionState {
    std::mutex mu;
    uint64_t lastWorld = 0;
    bool verified = false;

    // Bumped on world change; MeshStates carrying an older value get wiped
    // lazily the next time they come through Scan or Check.
    uint32_t generation = 1;

    uint32_t lrtsOffset = 0;    // discovered LRTS offset on mesh
    uint32_t lrtOffset = 0;     // discovered LRT offset (frustum-based, for pairing)
    uint32_t lrtKey = 0;        // LRT XOR key

    static constexpr int kMaxKeys = 6;
    std::array<uint32_t, kMaxKeys> keys{};  // LRTS XOR keys
    int keyCount = 0;

    // Pending key verification (async, deferred)
    uint64_t pendMesh = 0;
    uint32_t pendKey = 0;
    uint32_t pendRaw = 0;   // encrypted dword captured at derivation; verify
                            // requires the value to move before accepting the key
    float pendTime = 0.f;

    // Diagnostics
    int visibleCount = 0;
    int occludedCount = 0;
    int unknownCount = 0;
    int readFailures = 0;
    // Every Unknown exit in Check(), split out. The aggregate counters above
    // cannot show which branch is actually eating the traffic, and two of
    // these exits previously returned without incrementing anything at all.
    int unkNoMesh = 0;      // MeshState carries no component address
    int unkNoKey = 0;       // session has no offset/keys yet, or worldTime bad
    int unkReadZero = 0;    // encrypted dword read back as zero
    int unkKeyMiss = 0;     // no known key decrypted to a sane float
    // Phase 1 gate breakdown. brrZero vs brrNoBit separates "the byte came
    // back as nothing" (bad offset or failed read) from "the mesh really is
    // not being rendered", which need completely different fixes.
    int scanBrrZero = 0;    // byte at meshComp+0x997 read as 0x00
    int scanBrrNoBit = 0;   // byte was non-zero but 0x20 was clear
    int scanBrrPass = 0;    // frustum gate passed
    int scanBulkFail = 0;   // bulk read of meshComp+0x400 failed
    int scanDropFull = 0;   // Phase 1: qualifying dwords dropped because
                            // pending[] hit kMaxPending. If this grows on a
                            // new build, the LRTS offset may sit past the cap
                            // and discovery will silently fail — raise the
                            // cap or widen the window.
    int directReadZero = 0; // CheckDirect: encrypted dword read as zero
    int directInsane = 0;   // CheckDirect: decrypted to a non-time value
    uint64_t lastMesh = 0;        // last mesh checked; the tick probe watches it
    uint8_t lastBrrByte = 0;      // raw flag byte, for picking the right bit
    float lastDirectValue = 0.f;  // last decrypt, kept even when rejected
    float lastRealTime = 0.f;     // UWorld::RealTimeSeconds, for base comparison
    int scanAttempts = 0;   // total scan best-effort passes
    int pendingCollected = 0; // candidates found awaiting verification
    float lastWorldTime = 0.f;
    float lastRawSubmit = 0.f;    // last raw LastSubmitTime read (diagnostics)
    float lastRawOnScreen = 0.f;  // last raw LastRenderTimeOnScreen read (diagnostics)

    void Reset() {
        lastWorld = 0;
        verified = false;
        ++generation;
        lrtsOffset = 0;
        lrtOffset = 0;
        lrtKey = 0;
        keys = {};
        keyCount = 0;
        pendMesh = 0;
        pendKey = 0;
        pendRaw = 0;
        pendTime = 0.f;
        visibleCount = 0;
        occludedCount = 0;
        unknownCount = 0;
        readFailures = 0;
        unkNoMesh = 0;
        unkNoKey = 0;
        unkReadZero = 0;
        unkKeyMiss = 0;
        scanBrrZero = 0;
        scanBrrNoBit = 0;
        scanBrrPass = 0;
        scanBulkFail = 0;
        scanDropFull = 0;
        directReadZero = 0;
        directInsane = 0;
        lastMesh = 0;
        lastBrrByte = 0;
        lastDirectValue = 0.f;
        lastRealTime = 0.f;
        scanAttempts = 0;
        pendingCollected = 0;
        lastWorldTime = 0.f;
        lastRawSubmit = 0.f;
        lastRawOnScreen = 0.f;
    }
};

// The one global session state
inline SessionState g_session;

// Aggregate verdict-flip counters (all VerdictSmoothers). Menu diagnostics:
// flicker = flipsToOccluded (a box dropping behind-wall) and flipsToVisible
// (a box popping through a wall); both should hover near zero during steady
// aim after the hysteresis lands.
inline std::atomic<uint64_t> g_flipsToVisible{ 0 };
inline std::atomic<uint64_t> g_flipsToOccluded{ 0 };
inline std::atomic<uint64_t> g_unknownHolds{ 0 };

// ─── Persisted discovery ────────────────────────────────────────────────
// The discovered offset+XOR key are stable within a game build but rotate
// across builds. Save them (once locked) so restarts of the same build skip
// the scan warmup; a stale file is harmless — runtime Scan replaces it within
// a pass or two, exactly as if no file existed.
inline constexpr const char* kPersistPath = "F:/Test/ARCs/lrts_key.txt";
inline std::atomic<bool> g_persistTried{ false };

inline void PersistDiscoveredKey(const SessionState& session)
{
    if (!session.lrtsOffset || session.keyCount <= 0)
        return;
    std::ofstream f(kPersistPath, std::ios::trunc);
    if (!f)
        return;
    f << std::hex << session.lrtsOffset << '\n'
      << session.keys[0] << '\n';
}

// Seed the session from the persisted file once per process. Never clobbers
// live-discovered state, and never runs twice (scan/restart races both end in
// the same seed).
inline void EnsurePersistedLoaded(SessionState& session)
{
    if (g_persistTried.exchange(true, std::memory_order_relaxed))
        return;
    std::ifstream f(kPersistPath);
    if (!f)
        return;
    uint32_t off = 0, key = 0;
    f >> std::hex >> off >> key;
    if (off && key && session.lrtsOffset == 0 && session.keyCount <= 0) {
        session.lrtsOffset = off;
        session.keys[0] = key;
        session.keyCount = 1;
        session.verified = true;
    }
}

// Configuration
inline constexpr float kVisibilityThreshold = 0.15f;  // seconds — LRTS must be within this of worldTime
inline constexpr int   kMaxScanAttempts = 10;
inline constexpr float kPendingVerifyDelay = 0.12f;    // seconds — delay before verifying candidates
inline constexpr float kPendingKeyVerifyDelay = 1.0f;  // seconds — delay before accepting new key

// Verdict hysteresis: tuned for wall-pops vs flicker tradeoff.
// 2 checks = ~400ms at 200ms cadence: fast enough for wall-pops,
// slow enough to reject DMA glitches (ghost flicker).
inline constexpr int kOccludedConfirmChecks = 2;
inline constexpr int kVisibleConfirmChecks  = 1;

struct VerdictSmoother {
    // Smoothed verdict. Defaults to Visible (matches the call sites' existing
    // Unknown-==-Visible bias) until the first confirmed Occluded flips it.
    bool visible = true;
    int pending = 0;              // consecutive occlusion-confirming checks
    uint64_t lastFlipMs = 0;      // wall-clock of last visible<->occluded flip
    int flipsToVisible = 0;       // lifetime flip counters (menu diagnostics)
    int flipsToOccluded = 0;
    int unknownHolds = 0;         // times an Unknown kept the current verdict

    void Reset() {
        visible = true;
        pending = 0;
    }

    // Feed one raw check result; returns the smoothed isVisible.
    // confirmedNotRendered = the bRecentlyRendered byte was READ SUCCESSFULLY
    // (non-zero) and bit5 is clear: the mesh is alive but the renderer is not
    // drawing it. In that state:
    //   - Occluded  -> fast-hide now (wall-pop, no 2-check wait).
    //   - Visible   -> CONTRADICTION. The LRTS stamp can keep tracking
    //     worldTime for 30-70s after a mesh stops rendering (measured on
    //     debug-c190fb: 199 runs, longest 71s — wasp stuck green behind a
    //     wall). A raw Visible must therefore COUNT toward occlusion instead
    //     of resetting the confirm counter, or the box never flips.
    // Any other case (byte still rendering, or byte == 0x00 from a failed DMA
    // read) keeps plain hysteresis: a 0x00 read must NEVER count as "not
    // rendered" or every DMA flap would blink the box (94ms ghost flicker).
    bool Update(Result r, uint64_t nowMs, bool confirmedNotRendered = false) {
        switch (r) {
        case Result::Visible:
            if (confirmedNotRendered) {
                // Stamp-vs-renderer contradiction: treat as occlusion evidence.
                if (++pending >= kOccludedConfirmChecks && visible) {
                    visible = false;
                    lastFlipMs = nowMs;
                    ++flipsToOccluded;
                    g_flipsToOccluded.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            pending = 0;
            if (!visible) {
                visible = true;
                lastFlipMs = nowMs;
                ++flipsToVisible;
                g_flipsToVisible.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case Result::Occluded:
            // Fast path: mesh alive and genuinely not rendered — hide now.
            if (confirmedNotRendered) {
                if (visible) {
                    visible = false;
                    lastFlipMs = nowMs;
                    ++flipsToOccluded;
                    g_flipsToOccluded.fetch_add(1, std::memory_order_relaxed);
                }
                pending = 0;
                break;
            }
            if (++pending >= kOccludedConfirmChecks && visible) {
                visible = false;
                lastFlipMs = nowMs;
                ++flipsToOccluded;
                g_flipsToOccluded.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case Result::Unknown:
        default:
            // Keep the current verdict: a transient read failure must not
            // pop boxes visible through walls, nor hide a visible target.
            ++unknownHolds;
            g_unknownHolds.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        return visible;
    }
};

// Raw fast-path (ARC method): plain floats, no XOR. onScreen lags submit behind a wall.
// Recently-rendered flag. Verified discriminating on CL-1341255: across one
// raid the bit was set on 10 meshes and clear on 131, with the byte itself
// non-zero throughout.
inline constexpr uint32_t BrrOffset = 0x997;
inline constexpr uint8_t  BrrMask   = 0x20;

inline constexpr uint32_t RawLastSubmitTime   = 0x4C4;
inline constexpr uint32_t RawLastRenderTimeOnScreen = 0x4CC;
inline constexpr float kRawFreshTolerance     = 0.06f; // seconds — submit vs on-screen gap
inline constexpr float kRawSubmitWorldSkew    = 2.0f;  // seconds — submit must track worldTime

// Core API — called from EntityList/RobotList per entity per frame
// These functions access game memory through the provided read lambdas.

// Scan: auto-discover LRTS offset + XOR key (non-blocking, multi-frame)
// readU8:  [](uint64_t addr) -> uint8_t
// readU32: [](uint64_t addr) -> uint32_t
// readBulk: [](uint64_t addr, void* buf, uint32_t size) -> bool
template<typename ReadU8, typename ReadU32, typename ReadBulk>
void Scan(MeshState& ms, SessionState& session,
          ReadU8 readU8, ReadU32 readU32, ReadBulk readBulk,
          float worldTime, uint64_t worldPtr)
{
    std::lock_guard<std::mutex> lock(session.mu);

    // Warm-start from the persisted key (once per process) so restarts of the
    // same game build skip the scan warmup. Stale file is harmless — runtime
    // discovery overwrites it within a pass or two.
    EnsurePersistedLoaded(session);

    session.lastWorldTime = worldTime;

    // Reset the whole session on world change — offsets and keys do not
    // survive a level transition, and neither do any per-mesh scans.
    if (worldPtr && session.lastWorld != worldPtr) {
        session.Reset();
        session.lastWorld = worldPtr;
        ms.Reset();
        ms.generation = session.generation;
        return;
    }

    // Carrying state from a previous world — wipe it before scanning
    if (ms.generation != session.generation) {
        ms.Reset();
        ms.generation = session.generation;
    }

    // Already locked session-wide — nothing to do
    if (session.lrtsOffset && session.keyCount > 0)
        return;

    // Too many attempts
    if (ms.scanAttempts >= kMaxScanAttempts)
        return;

    // Need a valid mesh and world time
    if (!ms.meshComp || worldTime < 10.f)
        return;

    session.scanAttempts++;

    // --- Phase 1: Collect candidates (first frame) ---
    if (ms.phase == MeshState::ScanPhase::Idle) {
        // Prefer meshes the frustum flag says are rendered, but do not require
        // it: for bots, the 0x438 slot frequently isn't a bound skeletal mesh,
        // so its flag byte may legitimately read 0x00 even when the bot is on
        // screen. Phase 2 is the real proof anyway — it demands the value
        // changed between two reads, which a stale mesh cannot fake.
        const uint8_t brr = readU8(ms.meshComp + 0x997);  // BRR_OFFSET
        if (brr & 0x20) {  // BRR_MASK
            session.scanBrrPass++;
        } else if (brr == 0) {
            session.scanBrrZero++;
        } else {
            session.scanBrrNoBit++;
            return;
        }

        ms.scanAttempts++;

        // Derive XOR key form from worldTime
        uint32_t wtBits;
        { float wf = worldTime; memcpy(&wtBits, &wf, 4); }
        const uint32_t wtSwapped = _byteswap_ulong(wtBits);

        // Bulk read mesh+0x400..0xC00 (widened from 0x600: the encrypted
        // render stamp moved outside the old 0x200 window on recent builds —
        // every 0x4C4 read returned zero, so discovery could never find it.
        // 0x800 covers 512 dwords; kMaxPending 512 holds the full range).
        constexpr uint32_t SCAN_LO = 0x400;
        constexpr uint32_t SCAN_SIZE = 0x800;
        uint8_t buf[SCAN_SIZE];
        if (!readBulk(ms.meshComp + SCAN_LO, buf, SCAN_SIZE)) {
            session.scanBulkFail++;
            return;
        }

        // Find dwords that decrypt to ~worldTime
        ms.pendingCount = 0;
        for (uint32_t rel = 0; rel <= SCAN_SIZE - 4; rel += 4) {
            uint32_t raw;
            memcpy(&raw, buf + rel, 4);
            if (raw == 0) continue;

            const uint32_t key = raw ^ wtSwapped;
            const uint32_t bits = _byteswap_ulong(raw ^ key);
            float v;
            memcpy(&v, &bits, sizeof(float));
            if (!std::isfinite(v) || v < 1.f) continue;
            if (std::fabs(worldTime - v) > 0.5f) continue;

            if (ms.pendingCount < MeshState::kMaxPending) {
                ms.pending[ms.pendingCount++] = {
                    SCAN_LO + rel, raw, key, worldTime
                };
            } else {
                // Array full: this dword qualifies but is silently discarded.
                // On the current build the discovered LRTS sits at dword 451,
                // so a non-zero drop count on a new build means discovery may
                // be missing a high-offset stamp.
                ++session.scanDropFull;
            }
        }

        if (ms.pendingCount > 0) {
            session.pendingCollected += ms.pendingCount;
            ms.phase = MeshState::ScanPhase::Collected;
        }
        return;
    }

    // --- Phase 2: Verify candidates (deferred, non-blocking) ---
    if (ms.phase == MeshState::ScanPhase::Collected) {
        // Check if enough time has passed since first candidate collection
        if (ms.pendingCount > 0) {
            const float elapsed = worldTime - ms.pending[0].worldTimeAtCapture;
            if (elapsed < kPendingVerifyDelay)
                return;  // not ready yet — come back next frame
        }

        int best = -1;
        float bestD = 999.f;

        for (int i = 0; i < ms.pendingCount; i++) {
            const auto& c = ms.pending[i];
            const uint32_t raw2 = readU32(ms.meshComp + c.offset);
            if (raw2 == c.raw)
                continue;  // static data, not a render time

            const uint32_t bits2 = _byteswap_ulong(raw2 ^ c.key);
            float v2;
            memcpy(&v2, &bits2, sizeof(float));
            const float d2 = std::fabs(worldTime + 0.12f - v2);
            if (std::isfinite(v2) && v2 > 1.f && d2 < 1.0f) {
                if (d2 < bestD) { bestD = d2; best = i; }
            }
        }

        if (best < 0) {
            // Verification failed — reset and retry
            ms.phase = MeshState::ScanPhase::Idle;
            ms.pendingCount = 0;
            return;
        }

        // Look for LRT/LRTS pair (gap of 4 or 8 bytes)
        const uint32_t off1 = ms.pending[best].offset;
        const uint32_t key1 = ms.pending[best].key;
        bool foundPair = false;

        for (int j = 0; j < ms.pendingCount; j++) {
            if (j == best) continue;
            const uint32_t gap = (ms.pending[j].offset > off1)
                ? ms.pending[j].offset - off1
                : off1 - ms.pending[j].offset;
            if (gap == 4 || gap == 8) {
                if (ms.pending[j].offset < off1) {
                    session.lrtOffset = ms.pending[j].offset;
                    session.lrtKey = ms.pending[j].key;
                    session.lrtsOffset = off1;
                    session.keys[0] = key1;
                } else {
                    session.lrtOffset = off1;
                    session.lrtKey = key1;
                    session.lrtsOffset = ms.pending[j].offset;
                    session.keys[0] = ms.pending[j].key;
                }
                session.keyCount = 1;
                ms.phase = MeshState::ScanPhase::Verified;
                session.verified = true;
                foundPair = true;
                break;
            }
        }

        if (!foundPair) {
            // Single offset — use as LRTS
            session.lrtsOffset = off1;
            session.keys[0] = key1;
            session.keyCount = 1;
            ms.phase = MeshState::ScanPhase::Verified;
            session.verified = true;
        }

        // Persist the discovered offset+key so the next launch of this build
        // starts with CheckDirect ready (no scan warmup). A moved stamp just
        // gets rediscovered and this file rewritten — self-healing.
        PersistDiscoveredKey(session);
        ms.pendingCount = 0;
        return;
    }
}

// CheckRaw: ARC raw fast-path — compares plain LastSubmitTime vs
// LastRenderTimeOnScreen. No XOR, no key discovery, no scan state.
// Returns Unknown when reads aren't sane so the caller can fall back
// to the encrypted Scan/Check path.
template<typename ReadU32>
Result CheckRaw(ReadU32 readU32, uint64_t meshComp, SessionState& session, float worldTime)
{
    if (!meshComp || worldTime <= 1.f)
        return Result::Unknown;

    const uint32_t rawSubmit  = readU32(meshComp + RawLastSubmitTime);
    const uint32_t rawOnScreen = readU32(meshComp + RawLastRenderTimeOnScreen);
    float submit, onScreen;
    memcpy(&submit, &rawSubmit, sizeof(float));
    memcpy(&onScreen, &rawOnScreen, sizeof(float));

    std::lock_guard<std::mutex> lock(session.mu);
    session.lastRawSubmit = submit;
    session.lastRawOnScreen = onScreen;

    if (!std::isfinite(submit) || !std::isfinite(onScreen))
        return Result::Unknown;
    if (submit < 1.f || onScreen < 1.f)
        return Result::Unknown;
    if (std::fabs(submit - worldTime) > kRawSubmitWorldSkew)
        return Result::Unknown;

    if (std::fabs(submit - onScreen) <= kRawFreshTolerance) {
        session.visibleCount++;
        return Result::Visible;
    }

    session.occludedCount++;
    return Result::Occluded;
}

// CheckRendered: use the recently-rendered flag byte as the answer instead of
// as a gate. It survives when everything downstream of it does not — the XOR
// keys go stale every build, but this bit is set by the renderer itself.
// A zero byte means the read gave us nothing, which is not the same as "not
// rendered", so that case stays Unknown and the caller falls back.
template<typename ReadU8>
Result CheckRendered(ReadU8 readU8, uint64_t meshComp, SessionState& session,
                     uint32_t brrOffset, uint8_t brrMask)
{
    if (!meshComp)
        return Result::Unknown;

    {
        std::lock_guard<std::mutex> lock(session.mu);
        session.lastMesh = meshComp;
    }

    const uint8_t brr = readU8(meshComp + brrOffset);
    if (brr == 0)
        return Result::Unknown;

    std::lock_guard<std::mutex> lock(session.mu);
    session.lastBrrByte = brr;
    if (brr & brrMask) {
        session.visibleCount++;
        return Result::Visible;
    }

    session.occludedCount++;
    return Result::Occluded;
}

// CheckDirect: decrypt LastRenderTimeOnScreen with the known offset/key pair
// instead of rediscovering them. Runtime discovery needs a mesh that is proven
// to be rendering, and the only signal for that on this build (the byte at
// mesh+0x997) reads 0x00 for every mesh — so discovery can never complete.
// Caller supplies the pair from Offsets so this header stays dependency-free.
template<typename ReadU32>
Result CheckDirect(ReadU32 readU32, uint64_t meshComp, SessionState& session,
                   float worldTime, uint32_t lrtsOffset, uint32_t lrtsKey)
{
    if (!meshComp || worldTime <= 1.f)
        return Result::Unknown;

    const uint32_t raw = readU32(meshComp + lrtsOffset);
    if (raw == 0) {
        std::lock_guard<std::mutex> lock(session.mu);
        session.directReadZero++;
        return Result::Unknown;
    }

    const uint32_t bits = _byteswap_ulong(raw ^ lrtsKey);
    float v;
    memcpy(&v, &bits, sizeof(v));

    std::lock_guard<std::mutex> lock(session.mu);
    session.lastDirectValue = v;

    // A render stamp is a world time: finite, not negative, never ahead of now.
    if (!std::isfinite(v) || v < 0.f || v > worldTime + 1.f) {
        session.directInsane++;
        return Result::Unknown;
    }

    const float delta = worldTime - v;
    if (delta < kVisibilityThreshold) {
        session.visibleCount++;
        return Result::Visible;
    }

    session.occludedCount++;
    return Result::Occluded;
}

// Check: returns visibility for a mesh (must be called after Scan)
template<typename ReadU8, typename ReadU32>
Result Check(MeshState& ms, SessionState& session,
             ReadU8 readU8, ReadU32 readU32,
             float worldTime, uint64_t worldPtr)
{
    std::lock_guard<std::mutex> lock(session.mu);

    // Reset the whole session on world change
    if (worldPtr && session.lastWorld != worldPtr) {
        session.Reset();
        session.lastWorld = worldPtr;
        ms.Reset();
        ms.generation = session.generation;
        return Result::Unknown;
    }

    if (ms.generation != session.generation) {
        ms.Reset();
        ms.generation = session.generation;
    }

    if (!ms.meshComp) {
        session.unkNoMesh++;
        return Result::Unknown;
    }

    // Need scan results
    if (!session.lrtsOffset || session.keyCount <= 0 || worldTime <= 1.f) {
        session.unkNoKey++;
        return Result::Unknown;
    }

    // --- Pending key verification (async, deferred) ---
    if (session.pendMesh && worldTime - session.pendTime > kPendingKeyVerifyDelay) {
        const uint32_t pr = readU32(session.pendMesh + session.lrtsOffset);
        const uint32_t bits = _byteswap_ulong(pr ^ session.pendKey);
        float pv;
        memcpy(&pv, &bits, sizeof(float));
        // pr must differ from the captured raw: a static dword (wrong/stale
        // offset) decrypts with the derived key to exactly the capture-time
        // worldTime, which would otherwise pass the 1.5s window ~1s later.
        // Requiring the value to move is the same proof Scan Phase 2 uses.
        if (pr != session.pendRaw && std::isfinite(pv)
            && std::fabs(worldTime - pv) < 1.5f) {
            bool dup = false;
            for (int k = 0; k < session.keyCount; k++)
                if (session.keys[k] == session.pendKey) { dup = true; break; }
            if (!dup && session.keyCount < SessionState::kMaxKeys)
                session.keys[session.keyCount++] = session.pendKey;
        }
        session.pendMesh = 0;
    }

    // --- Decrypt LRTS with known keys ---
    const uint32_t raw = readU32(ms.meshComp + session.lrtsOffset);
    if (raw == 0) {
        session.readFailures++;
        session.unkReadZero++;
        return Result::Unknown;
    }

    float bestVal = 0.f;
    bool found = false;
    float bestDist = 1e30f;
    for (int k = 0; k < session.keyCount; k++) {
        const uint32_t bits = _byteswap_ulong(raw ^ session.keys[k]);
        float v;
        memcpy(&v, &bits, sizeof(float));
        // Prefer the key whose decrypt lands closest to worldTime: with up to
        // six keys (one stale build key plus later re-discoveries), the first
        // sane-looking value can come from a stale key and flip a verdict a
        // fresher key would override.
        if (std::isfinite(v) && v > -2000.f && v < worldTime + 10.f) {
            const float d = std::fabs(worldTime - v);
            if (d < bestDist) {
                bestDist = d;
                bestVal = v;
                found = true;
            }
        }
    }

    // --- Key not working — try to derive new key from rendered mesh ---
    if (!found) {
        const uint8_t brr = readU8(ms.meshComp + 0x997);
        if ((brr & 0x20) && !session.pendMesh && session.keyCount < SessionState::kMaxKeys) {
            uint32_t wtBits;
            { float wf = worldTime; memcpy(&wtBits, &wf, 4); }
            session.pendKey = raw ^ _byteswap_ulong(wtBits);
            session.pendRaw = raw;
            session.pendMesh = ms.meshComp;
            session.pendTime = worldTime;
        }
        session.unknownCount++;
        session.unkKeyMiss++;
        return Result::Unknown;
    }

    // --- Compare: visible if LRTS ~= worldTime ---
    const float delta = worldTime - bestVal;
    if (delta < kVisibilityThreshold && delta > -kVisibilityThreshold) {
        session.visibleCount++;
        return Result::Visible;
    }

    session.occludedCount++;
    return Result::Occluded;
}

// Reset all state (call on world change or disable)
inline void ResetAll(SessionState& session) {
    std::lock_guard<std::mutex> lock(session.mu);
    session.Reset();
}

} // namespace LrtsVis
