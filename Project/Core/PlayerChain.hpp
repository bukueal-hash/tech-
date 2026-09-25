#pragma once
//
// The player chain: one ordered ladder, one state, per-hop diagnostics.
//
// The tool used to chase every hop with several independent passes - the world
// from three places, the GameInstance from six, the controller from five, the
// local player from four, the pawn from two - and each pass published straight
// into its own field. Which value won therefore depended on the order of
// statements inside Engine::Update(), a later pass could silently overwrite an
// earlier, better one, and the only signal the panel got was four integer tags.
//
// Here each hop is one fixed ladder of *named* rungs, walked in order, and a
// rung may only publish when its own predicate accepts the value:
//
//   World        <- IsWorld (PersistentLevel validates), inner deref, GameState
//   GameInstance <- world back-ref, LP outer, SIMD decrypt, legacy slot, scan
//   Controller   <- engine flag, pair cache, camera manager, actor scan, GI array
//   LocalPlayer  <- GI array + PC back-ref, PC+0x4B0 decrypt, PC slot scan
//   Pawn/State   <- AcknowledgedPawn, its fallbacks, PC's PlayerState
//
// A wrong rung is therefore harmless (its candidate never passes the predicate),
// and *why* a hop is empty is visible: Trace() prints
// "World=slotInner(1/3) GI=owningBackRef(1/5) PC=camManager(2/5) LP=none(0/2)".
//
// The host supplies one primitive per rung (Engine::ChainHost in Engine.h is the
// production host). The test host is a plain struct of lambdas, so the order
// below is verified without the game, DMA hardware, or a live target.

#include <cstdint>
#include <cstdio>
#include <string>

namespace PlayerChain {

// The hops, in the order the ladder resolves them.
enum class Hop : int {
    World = 0,
    Level,
    GameInstance,
    Controller,
    LocalPlayer,
    Pawn,
    State,
    Root,
    CameraManager,
    Count
};

inline const char* HopName(Hop hop)
{
    switch (hop) {
    case Hop::World: return "World";
    case Hop::Level: return "Level";
    case Hop::GameInstance: return "GI";
    case Hop::Controller: return "PC";
    case Hop::LocalPlayer: return "LP";
    case Hop::Pawn: return "Pawn";
    case Hop::State: return "PS";
    case Hop::Root: return "Root";
    case Hop::CameraManager: return "PCM";
    default: return "?";
    }
}

// One rung per primitive the host offers. The names are what the panel, the log
// and the tests talk about, so a rung is never identified by an index again.
enum class Rung : int {
    None = 0,
    Cached,                 // last tick's value, re-proved before it is reused

    // World
    SlotDirect, SlotInner, GameStateGlobal,
    // GameInstance
    OwningSlot, OwningBackRef,
    OuterSdkSlot, OuterSdkSlotRol, OuterDrop, OuterPlainA0, OuterPlain20,
    SimdDecrypt, LegacySlot, Scan,
    // Controller
    CachedFlag, CachedPair, CamManager, ActorScan, LpArrayFlag, LpArrayChain,
    // LocalPlayer
    GiArrayBackRef, GiArraySlot0, PcDecryptBackRef, PcDecryptOnly, PcSlotScan,
    // Controller rung that is a GI-array read needs to say whether the engine
    // flag or only the pawn proved it (LpArrayFlag / LpArrayChain above).
    // Pawn
    PcAcknowledged, PcAcknowledgedAlt, PcCharacter,
    // PlayerState / Root
    PawnPlayerState, PcPlayerState, PawnRoot,
    // Camera manager
    PcCameraSlot, PcmActorScan
};

inline const char* RungName(Rung rung)
{
    switch (rung) {
    case Rung::None: return "none";
    case Rung::Cached: return "cached";
    case Rung::SlotDirect: return "slotDirect";
    case Rung::SlotInner: return "slotInner";
    case Rung::GameStateGlobal: return "gameStateGlobal";
    case Rung::OwningSlot: return "owningSlot";
    case Rung::OwningBackRef: return "owningBackRef";
    case Rung::OuterSdkSlot: return "outerSdkSlot";
    case Rung::OuterSdkSlotRol: return "outerSdkSlotRol";
    case Rung::OuterDrop: return "outerDrop";
    case Rung::OuterPlainA0: return "outerPlainA0";
    case Rung::OuterPlain20: return "outerPlain20";
    case Rung::SimdDecrypt: return "simdDecrypt";
    case Rung::LegacySlot: return "legacySlot";
    case Rung::Scan: return "scan";
    case Rung::CachedFlag: return "cachedFlag";
    case Rung::CachedPair: return "cachedPair";
    case Rung::CamManager: return "camManager";
    case Rung::ActorScan: return "actorScan";
    case Rung::LpArrayFlag: return "lpArrayFlag";
    case Rung::LpArrayChain: return "lpArrayChain";
    case Rung::GiArrayBackRef: return "giArray+ref";
    case Rung::GiArraySlot0: return "giArraySlot0";
    case Rung::PcDecryptBackRef: return "pc4B0+ref";
    case Rung::PcDecryptOnly: return "pc4B0";
    case Rung::PcSlotScan: return "pcSlotScan";
    case Rung::PcAcknowledged: return "pcAckPawn";
    case Rung::PcAcknowledgedAlt: return "pcAckAlt";
    case Rung::PcCharacter: return "pcCharacter";
    case Rung::PawnPlayerState: return "pawnState";
    case Rung::PcPlayerState: return "pcState";
    case Rung::PawnRoot: return "pawnRoot";
    case Rung::PcCameraSlot: return "pcCameraSlot";
    case Rung::PcmActorScan: return "pcmActorScan";
    default: return "?";
    }
}

struct HopState {
    uintptr_t ptr = 0;
    Rung rung = Rung::None;
    bool confirmed = false;   // the rung's own identity proof passed
    int tried = 0;            // rungs that could run (parents present)
    int rejected = 0;         // rungs that ran and found nothing acceptable
    bool Ok() const { return ptr != 0; }
};

// The single source of truth for one tick of the chain.
struct State {
    HopState hops[static_cast<int>(Hop::Count)]{};
    uintptr_t worldRaw = 0;   // raw read at base+UWorld, even when it is not a world
    int actorCount = 0;
    int worldFailStep = 0;    // 0=ok 1=null slot 2=not a world 3=no level 4=level without actors
    long long tick = 0;

    HopState& Get(Hop hop) { return hops[static_cast<int>(hop)]; }
    const HopState& Get(Hop hop) const { return hops[static_cast<int>(hop)]; }
    uintptr_t Ptr(Hop hop) const { return hops[static_cast<int>(hop)].ptr; }
    Rung RungOf(Hop hop) const { return hops[static_cast<int>(hop)].rung; }
    bool Confirmed(Hop hop) const { return hops[static_cast<int>(hop)].confirmed; }
    bool Ok(Hop hop) const { return hops[static_cast<int>(hop)].Ok(); }

    // Clears the chain but keeps the tick counter (it identifies the tick).
    void Clear()
    {
        for (int i = 0; i < static_cast<int>(Hop::Count); ++i)
            hops[i] = HopState{};
        worldRaw = 0;
        actorCount = 0;
        worldFailStep = 0;
    }
};

// What the host carries over from the previous tick. Every cached value is
// re-proved by the rung that reuses it, so a stale entry cannot be published.
struct Seed {
    uint64_t base = 0;
    uintptr_t world = 0;
    uintptr_t pc = 0;
    uintptr_t pawn = 0;
    uintptr_t root = 0;
    uintptr_t gi = 0;
    uintptr_t lp = 0;
    uintptr_t pcm = 0;
    uintptr_t ps = 0;
    bool worldStillValid = false;   // PersistentLevel still validates on the cached world
    bool retainValid = false;       // host's last-known-good window is still open
};

// "World=slotDirect(1/3) GI=owningBackRef(1/5) PC=camManager(2/5) ..." - one line
// naming the winning rung per hop and how many rungs it took. `+` marks a rung
// whose identity proof passed.
inline std::string Trace(const State& st)
{
    std::string out;
    char buf[80];
    for (int i = 0; i < static_cast<int>(Hop::Count); ++i) {
        const Hop hop = static_cast<Hop>(i);
        const HopState& h = st.hops[i];
        if (!out.empty())
            out += ' ';
        if (h.Ok())
            std::snprintf(buf, sizeof(buf), "%s=%s(%d/%d)%s",
                HopName(hop), RungName(h.rung), h.tried, h.rejected,
                h.confirmed ? "+" : "");
        else
            std::snprintf(buf, sizeof(buf), "%s=none(%d/%d)",
                HopName(hop), h.tried, h.rejected);
        out += buf;
    }
    return out;
}

namespace detail {
inline void Attempt(State& st, Hop hop) { ++st.Get(hop).tried; }
inline void Reject(State& st, Hop hop)
{
    HopState& h = st.Get(hop);
    ++h.tried;
    ++h.rejected;
}
inline void Accept(State& st, Hop hop, uintptr_t ptr, Rung rung, bool confirmed = true)
{
    HopState& h = st.Get(hop);
    h.ptr = ptr;
    h.rung = rung;
    h.confirmed = confirmed;
}
} // namespace detail

// ── the ladder ───────────────────────────────────────────────────────────────
// Host contract - one primitive per rung, each already applying its own proof:
//
//   uintptr_t WorldSlot(uint64_t base)                    raw read at base+UWorld
//   bool      IsWorld(uintptr_t world)                    PersistentLevel validates
//   uintptr_t WorldSlotInner(uintptr_t slot)              slot -> inner (uncached)
//   uintptr_t WorldFromGameStateGlobal(uint64_t base)     GameState-owning world
//   uintptr_t LevelOfWorld(uintptr_t world)               UWorld::PersistentLevel
//   bool      LevelActors(uintptr_t level, uintptr_t&, int&)
//   uintptr_t GiOwningSlot(uintptr_t world, bool& backRef)
//   uintptr_t GiFromOuter(uintptr_t lp, Rung& which)      OuterLink rungs
//   uintptr_t GiFromDecrypt(uintptr_t world)
//   uintptr_t GiFromLegacySlot(uintptr_t world)
//   uintptr_t GiFromScan(uintptr_t world)
//   bool      IsGameInstance(uintptr_t gi)
//   bool      PcFromCachedFlag(uintptr_t cachedPc, uintptr_t& pc)
//   bool      PcFromCachedPair(uintptr_t cachedPc, uintptr_t cachedPawn,
//                              uintptr_t& pc, uintptr_t& pawn)
//   bool      PcFromCamManager(uintptr_t level, uintptr_t actors, int count,
//                              uintptr_t& pc, uintptr_t& pawn, uintptr_t& pcm)
//   bool      PcFromActorScan(uintptr_t level, uintptr_t actors, int count,
//                             uintptr_t gi, uintptr_t& pc, uintptr_t& pawn)
//   bool      PcFromGiArray(uintptr_t gi, uintptr_t& pc, uintptr_t& pawn,
//                           bool& flagProved)
//   uintptr_t PcmFromPc(uintptr_t pc)
//   uintptr_t PcmFromActors()
//   uintptr_t LpFromGiArray(uintptr_t gi, uintptr_t pc, bool& backRef)
//   uintptr_t LpFromController(uintptr_t pc, Rung& which, bool& backRef)
//   uintptr_t PawnFromPc(uintptr_t pc, Rung& which)
//   uintptr_t StateFromPawn(uintptr_t pawn)
//   uintptr_t StateFromPc(uintptr_t pc)
//   uintptr_t RootFromPawn(uintptr_t pawn)
template <class Host>
void Resolve(State& st, Host& host, const Seed& seed)
{
    st.Clear();

    // ── World ────────────────────────────────────────────────────────────────
    // The raw slot is always read (it is what the panel shows, even on a miss).
    // The live SDK path is authoritative: read the global, then its
    // intermediary, then validate the resulting UWorld. Cached state is only a
    // last-resort continuity hold after every live source has failed; otherwise
    // a stale cached world can hide a broken global forever (WorldSrc=cached).
    st.worldRaw = host.WorldSlot(seed.base);
    uintptr_t world = 0;
    detail::Attempt(st, Hop::World);
    if (st.worldRaw && host.IsWorld(st.worldRaw)) {
        detail::Accept(st, Hop::World, st.worldRaw, Rung::SlotDirect);
        world = st.worldRaw;
    } else {
        ++st.Get(Hop::World).rejected;
    }
    if (!world) {
        detail::Attempt(st, Hop::World);
        const uintptr_t inner = host.WorldSlotInner(st.worldRaw);
        if (inner && host.IsWorld(inner)) {
            detail::Accept(st, Hop::World, inner, Rung::SlotInner);
            world = inner;
        } else {
            ++st.Get(Hop::World).rejected;
        }
    }
    if (!world) {
        detail::Attempt(st, Hop::World);
        const uintptr_t gs = host.WorldFromGameStateGlobal(seed.base);
        if (gs) {
            detail::Accept(st, Hop::World, gs, Rung::GameStateGlobal);
            world = gs;
        } else {
            ++st.Get(Hop::World).rejected;
        }
    }
    if (!world && seed.world && seed.worldStillValid) {
        detail::Attempt(st, Hop::World);
        detail::Accept(st, Hop::World, seed.world, Rung::Cached, false);
        world = seed.world;
    }
    if (!world) {
        st.worldFailStep = st.worldRaw ? 2 : 1;
        return;   // nothing downstream can resolve without a world
    }
    st.worldFailStep = 0;

    // ── PersistentLevel + actors ─────────────────────────────────────────────
    // Level is what proves the world; a missing actor array only disables the
    // rungs that need it (they report 0 tries rather than a rejection).
    const uintptr_t level = host.LevelOfWorld(world);
    if (!level) {
        st.worldFailStep = 3;
        return;
    }
    detail::Accept(st, Hop::Level, level, Rung::SlotDirect);
    uintptr_t actors = 0;
    int actorCount = 0;
    if (!host.LevelActors(level, actors, actorCount)) {
        st.worldFailStep = 4;   // soft: no actor scan rungs from here
        actors = 0;
        actorCount = 0;
    }
    st.actorCount = actorCount;

    // ── GameInstance ─────────────────────────────────────────────────────────
    // Dump-backed slot first (world back-ref is the strongest cheap proof), then
    // the LP outer hop, then the retired CLs' schemes, then a scan of UWorld.
    uintptr_t gi = 0;
    {
        detail::Attempt(st, Hop::GameInstance);
        bool backRef = false;
        const uintptr_t v = host.GiOwningSlot(world, backRef);
        if (v) {
            detail::Accept(st, Hop::GameInstance, v,
                backRef ? Rung::OwningBackRef : Rung::OwningSlot, backRef);
            gi = v;
        } else {
            ++st.Get(Hop::GameInstance).rejected;
        }
    }
    if (!gi && seed.lp) {
        detail::Attempt(st, Hop::GameInstance);
        Rung which = Rung::None;
        const uintptr_t v = host.GiFromOuter(seed.lp, which);
        if (v) {
            detail::Accept(st, Hop::GameInstance, v, which);
            gi = v;
        } else {
            ++st.Get(Hop::GameInstance).rejected;
        }
    }
    if (!gi) {
        detail::Attempt(st, Hop::GameInstance);
        const uintptr_t v = host.GiFromDecrypt(world);
        if (v) {
            detail::Accept(st, Hop::GameInstance, v, Rung::SimdDecrypt);
            gi = v;
        } else {
            ++st.Get(Hop::GameInstance).rejected;
        }
    }
    if (!gi) {
        detail::Attempt(st, Hop::GameInstance);
        const uintptr_t v = host.GiFromLegacySlot(world);
        if (v) {
            detail::Accept(st, Hop::GameInstance, v, Rung::LegacySlot);
            gi = v;
        } else {
            ++st.Get(Hop::GameInstance).rejected;
        }
    }
    if (!gi) {
        detail::Attempt(st, Hop::GameInstance);
        const uintptr_t v = host.GiFromScan(world);
        if (v) {
            detail::Accept(st, Hop::GameInstance, v, Rung::Scan);
            gi = v;
        } else {
            ++st.Get(Hop::GameInstance).rejected;
        }
    }
    if (!gi && seed.gi && host.IsGameInstance(seed.gi)) {
        detail::Accept(st, Hop::GameInstance, seed.gi, Rung::Cached);
        gi = seed.gi;
    }

    // ── PlayerController ─────────────────────────────────────────────────────
    // Order preserved from the (pre-ladder) discovery: the engine's own flag and
    // the pair cache are one read each, the camera manager is dump-backed, the
    // actor scan is the heavy one, and the GI array is the last resort because
    // GI->LocalPlayers is the hop that is encrypted on some CLs.
    uintptr_t pc = 0, pawn = 0, pcm = 0;
    Rung pcRung = Rung::None;
    if (seed.pc) {
        detail::Attempt(st, Hop::Controller);
        uintptr_t v = 0;
        if (host.PcFromCachedFlag(seed.pc, v)) {
            pc = v;
            pcRung = Rung::CachedFlag;
        } else {
            ++st.Get(Hop::Controller).rejected;
        }
    }
    if (!pc && seed.pc && seed.pawn) {
        detail::Attempt(st, Hop::Controller);
        uintptr_t v = 0, p = 0;
        if (host.PcFromCachedPair(seed.pc, seed.pawn, v, p)) {
            pc = v;
            pawn = p;
            pcRung = Rung::CachedPair;
        } else {
            ++st.Get(Hop::Controller).rejected;
        }
    }
    if (!pc && level && actors) {
        detail::Attempt(st, Hop::Controller);
        uintptr_t v = 0, p = 0, m = 0;
        if (host.PcFromCamManager(level, actors, actorCount, v, p, m)) {
            pc = v;
            pawn = p;
            pcm = m;
            pcRung = Rung::CamManager;
        } else {
            ++st.Get(Hop::Controller).rejected;
        }
    }
    if (!pc && level && actors && actorCount > 0) {
        detail::Attempt(st, Hop::Controller);
        uintptr_t v = 0, p = 0;
        if (host.PcFromActorScan(level, actors, actorCount, gi, v, p)) {
            pc = v;
            pawn = p;
            pcRung = Rung::ActorScan;
        } else {
            ++st.Get(Hop::Controller).rejected;
        }
    }
    if (!pc && gi) {
        detail::Attempt(st, Hop::Controller);
        uintptr_t v = 0, p = 0;
        bool flagProved = false;
        if (host.PcFromGiArray(gi, v, p, flagProved)) {
            pc = v;
            pawn = p;
            pcRung = flagProved ? Rung::LpArrayFlag : Rung::LpArrayChain;
        } else {
            ++st.Get(Hop::Controller).rejected;
        }
    }
    if (!pc && seed.retainValid && seed.pc) {
        detail::Accept(st, Hop::Controller, seed.pc, Rung::Cached);
        pc = seed.pc;
        pawn = seed.pawn;
        pcRung = Rung::Cached;
    }
    if (pc)
        detail::Accept(st, Hop::Controller, pc, pcRung, pcRung != Rung::Cached);

    // ── LocalPlayer ──────────────────────────────────────────────────────────
    uintptr_t lp = 0;
    if (gi) {
        detail::Attempt(st, Hop::LocalPlayer);
        bool backRef = false;
        const uintptr_t v = host.LpFromGiArray(gi, pc, backRef);
        if (v) {
            lp = v;
            detail::Accept(st, Hop::LocalPlayer, v,
                backRef ? Rung::GiArrayBackRef : Rung::GiArraySlot0, backRef || pc == 0);
        } else {
            ++st.Get(Hop::LocalPlayer).rejected;
        }
    }
    if (!lp && pc) {
        detail::Attempt(st, Hop::LocalPlayer);
        Rung which = Rung::None;
        bool backRef = false;
        const uintptr_t v = host.LpFromController(pc, which, backRef);
        if (v) {
            lp = v;
            detail::Accept(st, Hop::LocalPlayer, v, which, backRef);
        } else {
            ++st.Get(Hop::LocalPlayer).rejected;
        }
    }
    if (!lp && seed.lp) {
        detail::Accept(st, Hop::LocalPlayer, seed.lp, Rung::Cached, false);
        lp = seed.lp;
    }

    // The LP hop is the one hop that can feed an earlier one: a ULocalPlayer's
    // Outer *is* its GameInstance, so when the GI was still empty, run that rung
    // now - it is the same rung, in a fixed place, with its only parent present.
    if (!gi && lp) {
        detail::Attempt(st, Hop::GameInstance);
        Rung which = Rung::None;
        const uintptr_t v = host.GiFromOuter(lp, which);
        if (v) {
            detail::Accept(st, Hop::GameInstance, v, which);
            gi = v;
        } else {
            ++st.Get(Hop::GameInstance).rejected;
        }
    }

    // ── Pawn ─────────────────────────────────────────────────────────────────
    // The pawn a controller rung handed over keeps that rung's name, so the panel
    // can tell "the camera manager's pawn" from "AcknowledgedPawn on the PC".
    if (pawn) {
        Rung which = Rung::None;
        (void)host.PawnFromPc(pc, which);
        detail::Accept(st, Hop::Pawn, pawn, which == Rung::None ? pcRung : which,
            pcRung != Rung::Cached);
    } else if (pc) {
        detail::Attempt(st, Hop::Pawn);
        Rung which = Rung::None;
        const uintptr_t v = host.PawnFromPc(pc, which);
        if (v) {
            pawn = v;
            detail::Accept(st, Hop::Pawn, v, which);
        } else {
            ++st.Get(Hop::Pawn).rejected;
        }
    } else if (seed.pawn) {
        pawn = seed.pawn;
        detail::Accept(st, Hop::Pawn, seed.pawn, Rung::Cached, false);
    }

    // ── PlayerState ──────────────────────────────────────────────────────────
    uintptr_t ps = 0;
    if (pawn) {
        detail::Attempt(st, Hop::State);
        const uintptr_t v = host.StateFromPawn(pawn);
        if (v) {
            ps = v;
            detail::Accept(st, Hop::State, v, Rung::PawnPlayerState);
        } else if (pc) {
            ++st.Get(Hop::State).rejected;
        }
    }
    if (!ps && pc) {
        detail::Attempt(st, Hop::State);
        const uintptr_t v = host.StateFromPc(pc);
        if (v) {
            ps = v;
            detail::Accept(st, Hop::State, v, Rung::PcPlayerState);
        } else {
            ++st.Get(Hop::State).rejected;
        }
    }
    if (!ps && seed.ps) {
        ps = seed.ps;
        detail::Accept(st, Hop::State, seed.ps, Rung::Cached, false);
    }

    // ── Root component ───────────────────────────────────────────────────────
    if (pawn) {
        detail::Attempt(st, Hop::Root);
        const uintptr_t root = host.RootFromPawn(pawn);
        if (root)
            detail::Accept(st, Hop::Root, root, Rung::PawnRoot);
        else if (seed.root)
            detail::Accept(st, Hop::Root, seed.root, Rung::Cached, false);
        else
            ++st.Get(Hop::Root).rejected;
    } else if (seed.root) {
        detail::Accept(st, Hop::Root, seed.root, Rung::Cached, false);
    }

    // ── Camera manager ───────────────────────────────────────────────────────
    // Resolved from the controller when that rung already produced one (the
    // camera manager's PC owns it, so this never disagrees with the controller),
    // otherwise from the actor scan. It cannot change the controller: identity
    // decisions belong to the Controller hop.
    if (!pcm && pc) {
        detail::Attempt(st, Hop::CameraManager);
        const uintptr_t v = host.PcmFromPc(pc);
        if (v) {
            pcm = v;
            detail::Accept(st, Hop::CameraManager, v, Rung::PcCameraSlot);
        } else {
            ++st.Get(Hop::CameraManager).rejected;
        }
    }
    if (!pcm) {
        detail::Attempt(st, Hop::CameraManager);
        const uintptr_t v = host.PcmFromActors();
        if (v) {
            pcm = v;
            detail::Accept(st, Hop::CameraManager, v, Rung::PcmActorScan);
        } else {
            ++st.Get(Hop::CameraManager).rejected;
        }
    }
    if (!pcm && seed.pcm) {
        pcm = seed.pcm;
        detail::Accept(st, Hop::CameraManager, seed.pcm, Rung::Cached, false);
    }
}

} // namespace PlayerChain
