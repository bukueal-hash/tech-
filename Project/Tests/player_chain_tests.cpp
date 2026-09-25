// Player-chain ladder suite — hop order, identity proofs and per-hop diagnostics.
//
// Core/PlayerChain.hpp is a template over a host precisely so this is possible:
// the fake host below answers every rung with a canned value and records the
// order it was asked, so the suite verifies the *one* resolution order without
// the game, DMA hardware, or an Engine instance.
//
// What it pins down:
//   - each hop falls through its rungs in the documented order,
//   - a rung only publishes when its predicate accepts (the host enforces that),
//   - a hop whose parent is missing runs NO rung (tried == 0, not a rejection),
//   - the GameInstance hop re-runs its outer rung once the LP hop can feed it,
//   - the trace names the winning rung per hop with the tried/rejected counts.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/PlayerChain.hpp"

#include <string>
#include <vector>

namespace {

using PlayerChain::Hop;
using PlayerChain::Rung;

// The fake host. Every primitive is a member a test sets; every call is
// recorded so the order can be asserted directly.
struct FakeHost {
    std::vector<std::string> calls;

    // World
    uintptr_t worldSlot = 0;
    uintptr_t worldSlotInner = 0;
    uintptr_t worldFromGameState = 0;
    bool directWorldValid = true;
    bool innerWorldValid = true;
    bool gameStateWorldValid = true;
    uintptr_t level = 0;
    uintptr_t actors = 0;
    int actorCount = 0;

    // GameInstance
    uintptr_t giOwning = 0;
    bool giOwningBackRef = false;
    uintptr_t giOuter = 0;
    Rung giOuterRung = Rung::None;
    uintptr_t giDecrypt = 0;
    uintptr_t giLegacy = 0;
    uintptr_t giScan = 0;

    // Controller
    uintptr_t pcCachedFlag = 0;
    uintptr_t pcCachedPair = 0;
    uintptr_t pcPairPawn = 0;
    uintptr_t pcCamManager = 0;
    uintptr_t pcCamPawn = 0;
    uintptr_t pcmFromCamManager = 0;
    uintptr_t pcActorScan = 0;
    uintptr_t pcScanPawn = 0;
    uintptr_t pcGiArray = 0;
    uintptr_t pcGiArrayPawn = 0;
    bool pcGiArrayFlag = true;

    // LocalPlayer
    uintptr_t lpGiArray = 0;
    bool lpGiArrayBackRef = false;
    uintptr_t lpController = 0;
    Rung lpControllerRung = Rung::PcDecryptBackRef;
    bool lpControllerBackRef = true;

    // Pawn / state / root / camera manager
    uintptr_t pawn = 0;
    Rung pawnRung = Rung::PcAcknowledged;
    uintptr_t ps = 0;
    uintptr_t psFromPc = 0;
    uintptr_t root = 0;
    uintptr_t pcmFromPc = 0;
    uintptr_t pcmFromActors = 0;

    void Log(const char* what) { calls.push_back(what); }
    bool Saw(const char* what) const
    {
        for (const std::string& c : calls)
            if (c == what)
                return true;
        return false;
    }

    // ── World ──
    uintptr_t WorldSlot(uint64_t) { Log("WorldSlot"); return worldSlot; }
    bool IsWorld(uintptr_t w)
    {
        Log("IsWorld");
        if (!w)
            return false;
        return (directWorldValid && w == worldSlot)
            || (innerWorldValid && w == worldSlotInner)
            || (gameStateWorldValid && w == worldFromGameState);
    }
    uintptr_t WorldSlotInner(uintptr_t) { Log("WorldSlotInner"); return worldSlotInner; }
    uintptr_t WorldFromGameStateGlobal(uint64_t)
    {
        Log("WorldFromGameStateGlobal");
        return worldFromGameState;
    }
    uintptr_t LevelOfWorld(uintptr_t) { Log("LevelOfWorld"); return level; }
    bool LevelActors(uintptr_t, uintptr_t& a, int& n)
    {
        Log("LevelActors");
        a = actors;
        n = actorCount;
        return actors != 0 && actorCount > 0;
    }

    // ── GameInstance ──
    uintptr_t GiOwningSlot(uintptr_t, bool& backRef)
    {
        Log("GiOwningSlot");
        backRef = giOwningBackRef;
        return giOwning;
    }
    uintptr_t GiFromOuter(uintptr_t, Rung& which)
    {
        Log("GiFromOuter");
        which = giOuterRung;
        return giOuter;
    }
    uintptr_t GiFromDecrypt(uintptr_t) { Log("GiFromDecrypt"); return giDecrypt; }
    uintptr_t GiFromLegacySlot(uintptr_t) { Log("GiFromLegacySlot"); return giLegacy; }
    uintptr_t GiFromScan(uintptr_t) { Log("GiFromScan"); return giScan; }
    bool IsGameInstance(uintptr_t gi) { Log("IsGameInstance"); return gi != 0; }

    // ── Controller ──
    bool PcFromCachedFlag(uintptr_t, uintptr_t& pc)
    {
        Log("PcFromCachedFlag");
        if (!pcCachedFlag)
            return false;
        pc = pcCachedFlag;
        return true;
    }
    bool PcFromCachedPair(uintptr_t, uintptr_t, uintptr_t& pc, uintptr_t& p)
    {
        Log("PcFromCachedPair");
        if (!pcCachedPair)
            return false;
        pc = pcCachedPair;
        p = pcPairPawn;
        return true;
    }
    bool PcFromCamManager(uintptr_t, uintptr_t, int, uintptr_t& pc, uintptr_t& p, uintptr_t& m)
    {
        Log("PcFromCamManager");
        if (!pcCamManager)
            return false;
        pc = pcCamManager;
        p = pcCamPawn;
        m = pcmFromCamManager;
        return true;
    }
    bool PcFromActorScan(uintptr_t, uintptr_t, int, uintptr_t, uintptr_t& pc, uintptr_t& p)
    {
        Log("PcFromActorScan");
        if (!pcActorScan)
            return false;
        pc = pcActorScan;
        p = pcScanPawn;
        return true;
    }
    bool PcFromGiArray(uintptr_t, uintptr_t& pc, uintptr_t& p, bool& flagProved)
    {
        Log("PcFromGiArray");
        flagProved = pcGiArrayFlag;
        if (!pcGiArray)
            return false;
        pc = pcGiArray;
        p = pcGiArrayPawn;
        return true;
    }
    uintptr_t PcmFromPc(uintptr_t) { Log("PcmFromPc"); return pcmFromPc; }
    uintptr_t PcmFromActors() { Log("PcmFromActors"); return pcmFromActors; }

    // ── LocalPlayer ──
    uintptr_t LpFromGiArray(uintptr_t, uintptr_t, bool& backRef)
    {
        Log("LpFromGiArray");
        backRef = lpGiArrayBackRef;
        return lpGiArray;
    }
    uintptr_t LpFromController(uintptr_t, Rung& which, bool& backRef)
    {
        Log("LpFromController");
        which = lpControllerRung;
        backRef = lpControllerBackRef;
        return lpController;
    }

    // ── Pawn / state / root ──
    uintptr_t PawnFromPc(uintptr_t, Rung& which)
    {
        Log("PawnFromPc");
        which = pawnRung;
        return pawn;
    }
    uintptr_t StateFromPawn(uintptr_t) { Log("StateFromPawn"); return ps; }
    uintptr_t StateFromPc(uintptr_t) { Log("StateFromPc"); return psFromPc; }
    uintptr_t RootFromPawn(uintptr_t) { Log("RootFromPawn"); return root; }
};

// A world with a level and an actor array: the minimum a controller hop needs.
void GiveWorld(FakeHost& h, uintptr_t world)
{
    h.worldSlot = world;
    h.level = 0x2000;
    h.actors = 0x3000;
    h.actorCount = 10;
}

PlayerChain::State Run(FakeHost& h, const PlayerChain::Seed& seed)
{
    PlayerChain::State st;
    PlayerChain::Resolve(st, h, seed);
    return st;
}

const PlayerChain::HopState& H(const PlayerChain::State& st, Hop hop)
{
    return st.Get(hop);
}

} // namespace

TEST_CASE("ladder: the world hop falls through its rungs in order")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    h.level = 0x2000;
    h.actors = 0x3000;
    h.actorCount = 4;
    h.worldSlot = 0;               // no direct slot read
    h.worldSlotInner = 0x1000;     // GWorld** slot answers
    h.worldFromGameState = 0x9000; // must never be asked
    h.directWorldValid = false;

    const PlayerChain::State st = Run(h, seed);

    CHECK(H(st, Hop::World).ptr == 0x1000);
    CHECK(H(st, Hop::World).rung == Rung::SlotInner);
    CHECK(H(st, Hop::World).tried == 2);
    CHECK(H(st, Hop::World).rejected == 1);
    CHECK(h.Saw("WorldSlotInner"));
    CHECK_FALSE(h.Saw("WorldFromGameStateGlobal"));
    CHECK(st.worldFailStep == 0);
}

TEST_CASE("ladder: the GameState world rung is the last resort, and the raw slot is kept")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    h.level = 0x2000;
    h.actors = 0x3000;
    h.actorCount = 4;
    h.worldSlot = 0;
    h.worldSlotInner = 0;
    h.worldFromGameState = 0x9000;
    h.directWorldValid = false;
    h.gameStateWorldValid = true;

    const PlayerChain::State st = Run(h, seed);

    CHECK(H(st, Hop::World).rung == Rung::GameStateGlobal);
    CHECK(H(st, Hop::World).ptr == 0x9000);
    CHECK(H(st, Hop::World).tried == 3);
    CHECK(H(st, Hop::World).rejected == 2);
}

TEST_CASE("ladder: without a world every downstream rung is skipped, not rejected")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    h.worldSlot = 0x777;   // a slot read that is not a world
    h.directWorldValid = false;
    h.innerWorldValid = false;
    h.gameStateWorldValid = false;
    h.level = 0x2000;

    const PlayerChain::State st = Run(h, seed);

    CHECK(st.worldFailStep == 2);
    CHECK(st.worldRaw == 0x777);          // the panel still shows the raw read
    CHECK_FALSE(H(st, Hop::World).Ok());
    for (int i = static_cast<int>(Hop::Level); i < static_cast<int>(Hop::Count); ++i) {
        const PlayerChain::HopState& hop = st.hops[i];
        CHECK(hop.tried == 0);            // no parent -> no rung ran
        CHECK(hop.rejected == 0);
        CHECK(hop.rung == Rung::None);
    }
    CHECK_FALSE(h.Saw("GiOwningSlot"));
    CHECK_FALSE(h.Saw("PcFromCachedFlag"));
}

TEST_CASE("ladder: the controller hop stops at the first rung whose proof passes")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    GiveWorld(h, 0x1000);
    seed.pc = 0x5000;         // a cached controller to re-prove
    h.pcCachedFlag = 0x5000;  // engine flag accepts it
    h.pcCamManager = 0x6000;  // must never be reached
    h.pcActorScan = 0x7000;

    const PlayerChain::State st = Run(h, seed);

    CHECK(H(st, Hop::Controller).ptr == 0x5000);
    CHECK(H(st, Hop::Controller).rung == Rung::CachedFlag);
    CHECK(H(st, Hop::Controller).tried == 1);
    CHECK_FALSE(h.Saw("PcFromCachedPair"));
    CHECK_FALSE(h.Saw("PcFromCamManager"));
    CHECK_FALSE(h.Saw("PcFromActorScan"));
}

TEST_CASE("ladder: camera manager before actor scan before the GI array")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    GiveWorld(h, 0x1000);
    h.giOwning = 0x4000;
    h.giOwningBackRef = true;
    h.pcCamManager = 0x6000;   // the camera manager answers first
    h.pcActorScan = 0x7000;

    const PlayerChain::State st = Run(h, seed);

    CHECK(H(st, Hop::Controller).rung == Rung::CamManager);
    CHECK(H(st, Hop::Controller).ptr == 0x6000);
    CHECK_FALSE(h.Saw("PcFromActorScan"));
    CHECK_FALSE(h.Saw("PcFromGiArray"));
    // And the GameInstance rung that won is the back-ref one.
    CHECK(H(st, Hop::GameInstance).ptr == 0x4000);
    CHECK(H(st, Hop::GameInstance).rung == Rung::OwningBackRef);
    CHECK(H(st, Hop::GameInstance).confirmed);
}

TEST_CASE("ladder: the GI array controller rung says whether the flag or the pawn proved it")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    GiveWorld(h, 0x1000);
    h.giOwning = 0x4000;
    h.pcGiArray = 0x6000;
    h.pcGiArrayFlag = false;   // only the pawn proved it

    const PlayerChain::State st = Run(h, seed);

    CHECK(H(st, Hop::Controller).rung == Rung::LpArrayChain);
    CHECK(h.Saw("PcFromActorScan"));
}

TEST_CASE("ladder: the local player rung names the decrypt, the scan and the GI array")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    GiveWorld(h, 0x1000);
    h.giOwning = 0x4000;
    h.pcCamManager = 0x6000;
    h.lpController = 0x8000;
    h.lpControllerRung = Rung::PcDecryptOnly;   // live object, 0xA0 unproven
    h.lpControllerBackRef = false;

    const PlayerChain::State st = Run(h, seed);

    CHECK(H(st, Hop::LocalPlayer).ptr == 0x8000);
    CHECK(H(st, Hop::LocalPlayer).rung == Rung::PcDecryptOnly);
    CHECK_FALSE(H(st, Hop::LocalPlayer).confirmed);
    CHECK(h.Saw("LpFromGiArray"));   // GI is tried first; it rejects, then PC decrypt answers
}

TEST_CASE("ladder: the GI hop re-runs its outer rung once the LP hop can feed it")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    GiveWorld(h, 0x1000);
    h.giOwning = 0;            // nothing at the owning slot
    h.lpGiArray = 0;           // and no GI array to read
    h.pcCamManager = 0x6000;
    h.lpController = 0x8000;
    h.giOuter = 0x4000;        // the LP's own outer is the GameInstance
    h.giOuterRung = Rung::OuterDrop;

    const PlayerChain::State st = Run(h, seed);

    CHECK(H(st, Hop::GameInstance).ptr == 0x4000);
    CHECK(H(st, Hop::GameInstance).rung == Rung::OuterDrop);
    // owning slot + decrypt + legacy + scan, all rejected, then the late rung.
    CHECK(H(st, Hop::GameInstance).tried == 5);
    CHECK(H(st, Hop::GameInstance).rejected == 4);
    CHECK(h.Saw("LpFromController"));
}

TEST_CASE("ladder: a seed keeps the chain alive when no rung can run")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    GiveWorld(h, 0x1000);
    h.worldSlot = 0;          // force live rungs to miss; cache is last resort
    h.worldSlotInner = 0;
    h.worldFromGameState = 0;
    seed.world = 0x1000;
    seed.worldStillValid = true;
    h.giOwning = 0;
    h.pcCamManager = 0;
    h.pcActorScan = 0;
    h.pcGiArray = 0;
    seed.retainValid = true;
    seed.pc = 0x5000;
    seed.pawn = 0x5100;
    seed.root = 0x5200;
    seed.lp = 0x5300;
    seed.ps = 0x5400;
    seed.pcm = 0x5500;

    const PlayerChain::State st = Run(h, seed);

    CHECK(H(st, Hop::World).rung == Rung::Cached);
    CHECK(H(st, Hop::Controller).rung == Rung::Cached);
    CHECK(H(st, Hop::Pawn).ptr == 0x5100);
    CHECK(H(st, Hop::Root).ptr == 0x5200);
    CHECK(H(st, Hop::State).ptr == 0x5400);
    CHECK(H(st, Hop::CameraManager).ptr == 0x5500);
    // A cached rung never claims an identity proof.
    CHECK_FALSE(H(st, Hop::Controller).confirmed);
    CHECK_FALSE(H(st, Hop::LocalPlayer).confirmed);
}

TEST_CASE("ladder: the camera manager hop cannot change the controller")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    GiveWorld(h, 0x1000);
    h.pcCachedFlag = 0x5000;   // the controller is decided here
    seed.pc = 0x5000;
    h.pcmFromActors = 0x9000;  // resolved from the actor scan

    const PlayerChain::State st = Run(h, seed);

    CHECK(H(st, Hop::Controller).rung == Rung::CachedFlag);
    CHECK(H(st, Hop::CameraManager).ptr == 0x9000);
    CHECK(H(st, Hop::CameraManager).rung == Rung::PcmActorScan);
    CHECK(st.Ptr(Hop::Pawn) == 0);
}

TEST_CASE("ladder: the trace names every hop and its rung counts")
{
    FakeHost h;
    PlayerChain::Seed seed;
    seed.base = 0x140000000ull;
    GiveWorld(h, 0x1000);
    h.giOwning = 0x4000;
    h.giOwningBackRef = true;
    h.pcCamManager = 0x6000;
    h.lpController = 0x8000;

    const PlayerChain::State st = Run(h, seed);
    const std::string trace = PlayerChain::Trace(st);

    CHECK(trace.find("World=slotDirect(1/0)") != std::string::npos);
    CHECK(trace.find("GI=owningBackRef(1/0)+") != std::string::npos);
    CHECK(trace.find("PC=camManager(") != std::string::npos);
    CHECK(trace.find("LP=pc4B0+ref(") != std::string::npos);
    // The hop that could not run says so instead of pretending to be rejected.
    CHECK(trace.find("Pawn=none(1/1)") != std::string::npos);
}
