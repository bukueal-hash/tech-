#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

namespace FeaturePolicy {

struct AimFeatures {
    bool playerAim = false;
    bool robotAim = false;
    bool trigger = false;
};

// Run the aim worker when any target-producing feature needs a target lock.
inline bool ShouldRunAimPass(const AimFeatures& features)
{
    return features.playerAim || features.robotAim || features.trigger;
}

// ONE profile: whichever way the aim pass is activated (aim key, or trigger),
// it targets players and ARC alike. Target KIND is not a feature switch — the
// candidate nearest the crosshair wins, which is what the scoring already does.
//
// The old split collected robots only while a second switch was on and dropped
// players in trigger+robot mode, so a player or bot could be drawn by ESP and be
// invisible to aim at the same time. That is indistinguishable, from the user's
// side, from a broken aim path: the bot is on screen, the crosshair is on it,
// and nothing locks. Kind no longer gates collection; the pass either runs or it
// does not.
inline bool ShouldCollectPlayerTargets(const AimFeatures& features)
{
    return ShouldRunAimPass(features);
}

inline bool ShouldCollectRobotTargets(const AimFeatures& features)
{
    return ShouldRunAimPass(features);
}

// The aimbot switch is the movement feature. Trigger-only operation may select
// and fire at a target, but must never move the pointer toward it.
inline bool ShouldSendAimHardware(const AimFeatures& features)
{
    return features.playerAim || features.robotAim;
}

// ── One aim profile: executed, not just asserted ──────────────────────────
// The invariant that matters most here — target kind never gates collection — is
// also pinned in Tests/feature_policy_tests.cpp, but that project has no compile
// targets in this repository and scripts/build.ps1 -Test refuses by policy, so it
// is checked inside the shipping exe instead (Project.cpp's overlay loop, beside
// the collision and admission self-tests) and logs its own record. Pure: no
// clocks, no state.
struct AimProfileSelfTest
{
    bool passed = false;
    int cases = 0;
    int failed = 0;
    std::string detail;
};

inline AimProfileSelfTest RunAimProfileSelfTest()
{
    AimProfileSelfTest r;

    // Every way the pass can be activated, including the legacy robot-aim switch
    // on its own (a saved config can still carry it) and with the trigger.
    const AimFeatures profiles[] = {
        AimFeatures{ true, false, false },   // aimbot
        AimFeatures{ false, true, false },   // legacy robot-aim switch
        AimFeatures{ false, false, true },   // trigger only
        AimFeatures{ true, true, true },     // everything
        AimFeatures{ false, true, true },    // trigger + legacy switch
        AimFeatures{}                        // disabled
    };
    const char* names[] = {
        "aimbot", "robot_switch", "trigger", "all", "trigger_robot", "disabled"
    };

    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        const bool ran = ShouldRunAimPass(profiles[i]);
        const bool players = ShouldCollectPlayerTargets(profiles[i]);
        const bool robots = ShouldCollectRobotTargets(profiles[i]);
        // Both kinds, or neither: that is what "one profile" means, and it is
        // exactly what the split could not do.
        const bool ok = (players == ran) && (robots == ran);
        ++r.cases;
        if (!ok)
            ++r.failed;
        r.detail += std::string(names[i]) + "=" + (ok ? "pass " : "fail ");
    }

    // The movement contract is untouched: trigger-only selects and fires at
    // either kind but must never move the pointer toward it.
    ++r.cases;
    const bool triggerNoMove = !ShouldSendAimHardware(AimFeatures{ false, false, true });
    if (!triggerNoMove)
        ++r.failed;
    r.detail += std::string("trigger_no_move=") + (triggerNoMove ? "pass " : "fail ");

    r.passed = (r.failed == 0);
    return r;
}

inline bool ShouldRenderRobotEsp(bool showRobots)
{
    return showRobots;
}

inline bool ShouldRenderFov(bool showFov, const AimFeatures& features)
{
    return showFov && ShouldSendAimHardware(features);
}

inline bool ShouldRefreshPlayerPositions(
    bool playerEsp, bool radar, const AimFeatures& features)
{
    return playerEsp || radar || ShouldCollectPlayerTargets(features);
}

inline bool ShouldRefreshRobotPositions(
    bool robotEsp, const AimFeatures& features, bool radar)
{
    return robotEsp || radar || ShouldCollectRobotTargets(features);
}

struct TriggerToggleState {
    bool toggled = false;
    bool previousHeld = false;
};

// Resolve activation independently of target availability so Toggle and Always
// modes behave consistently even while no target is currently in the frame.
inline bool UpdateTriggerActivation(
    bool enabled,
    int holdMode,
    bool keyHeld,
    TriggerToggleState& state)
{
    if (!enabled || holdMode == 2) {
        state.toggled = false;
        state.previousHeld = keyHeld;
        return enabled && holdMode == 2;
    }

    if (holdMode == 0) {
        state.toggled = false;
        state.previousHeld = keyHeld;
        return keyHeld;
    }

    // holdMode == 1: toggle once per physical key press.
    if (keyHeld && !state.previousHeld)
        state.toggled = !state.toggled;
    state.previousHeld = keyHeld;
    return state.toggled;
}

// ── Zero rows explain themselves (Phase 2, advantage 2) ───────────────────
//
// A count of zero is ambiguous: "no enemies" and "everything was culled" read
// the same. These are the player frame-collect counters, and the names are the
// same strings the miss ledger uses (Core/PlayerEspMissLog.hpp ReasonName), so a
// zero row and the ledger cannot disagree about what dropped a player.

struct FrameGateCounts {
    int frameNotDrawing = 0;
    int frameNotInitialized = 0;
    int frameAllyHidden = 0;
    int frameDistance = 0;
    int framePosition = 0;
};

// The gate that counted the most, in the same priority order the collect pass
// evaluates them (Functions/Esp.cpp). count receives its tally. Returns nullptr
// when nothing counted, i.e. the zero is not a cull.
inline const char* DominantFrameGate(const FrameGateCounts& counts, int& count)
{
    const struct { const char* name; int n; } gates[] = {
        { "frame_position", counts.framePosition },
        { "frame_ally_hidden", counts.frameAllyHidden },
        { "frame_not_initialized", counts.frameNotInitialized },
        { "frame_distance", counts.frameDistance },
        { "frame_not_drawing", counts.frameNotDrawing },
    };
    const char* best = nullptr;
    count = 0;
    for (const auto& gate : gates) {
        if (gate.n > count) {
            best = gate.name;
            count = gate.n;
        }
    }
    return best;
}

// ── Named refusal on the assist path (Phase 2, advantage 2) ────────────────
//
// A held aim key that does nothing is indistinguishable from a broken feature:
// "no enemies", "everything was culled" and "the device is not ready" all look
// like the same silence. Every refusing path in Functions/Aimbot.cpp names
// itself here, so a refusal can be shown as a reason instead of as nothing.
// Each value below is set by at least one real gate -- none is aspirational.

enum class AimRefusal : uint8_t {
    None = 0,                  // the pass was not refused (acted, or not running)
    FeatureDisabled,           // player aim, robot aim and trigger are all off
    KeyNotHeld,                // no aim key held and the trigger is not active
    WorldUnresolved,           // world / persistent level / pawn unreadable
    ControllerUnresolved,      // player controller unreadable
    ControlRotationReadFailed, // control rotation read failed or returned NaN
    NoFrame,                   // no published ESP frame in an active raid
    StaleFrame,                // frame older than EspFramePolicy::kMaxFrameAgeMs
    CameraInvalid,             // camera location implausible
    CameraFovInvalid,          // camera FOV outside (1, 179)
    ProjectionUnavailable,     // projection screen centre unusable
    NoTarget,                  // no candidate passed the range/FOV gates
    TargetPositionInvalid,     // best candidate has no plausible world position
    Deadzone,                  // error inside the aim deadzone: nothing to send
    HardwareUnavailable,       // device layer refused to move/click
    // Phase 3 -- the single input gate.
    ReleasePending,            // a release is outstanding; no new press may start
    // Phase 4 -- per-axis calibration (T4.1). A constant outside its range is
    // refused rather than clamped, and the pass says so instead of aiming on it.
    CalibrationInvalid,        // the calibration in force is out of range
    // Phase 4 -- the strict target gate (T4.2). The candidate was admitted by the
    // collectors and then failed the strict test, so the pass names which of the
    // strict requirements it failed.
    TargetNotValidated,        // strict: the aim point is interpolated, not a sample
    TargetStale,               // strict: the geometry behind the point is too old
    Count
};

inline const char* AimRefusalName(AimRefusal r)
{
    switch (r) {
    case AimRefusal::None: return "not_refused";
    case AimRefusal::FeatureDisabled: return "feature_disabled";
    case AimRefusal::KeyNotHeld: return "key_not_held";
    case AimRefusal::WorldUnresolved: return "world_unresolved";
    case AimRefusal::ControllerUnresolved: return "controller_unresolved";
    case AimRefusal::ControlRotationReadFailed: return "control_rotation_read_failed";
    case AimRefusal::NoFrame: return "no_frame";
    case AimRefusal::StaleFrame: return "stale_frame";
    case AimRefusal::CameraInvalid: return "camera_invalid";
    case AimRefusal::CameraFovInvalid: return "camera_fov_invalid";
    case AimRefusal::ProjectionUnavailable: return "projection_unavailable";
    case AimRefusal::NoTarget: return "no_target";
    case AimRefusal::TargetPositionInvalid: return "target_position_invalid";
    case AimRefusal::Deadzone: return "deadzone";
    case AimRefusal::HardwareUnavailable: return "hardware_unavailable";

    case AimRefusal::ReleasePending: return "release_pending";
    case AimRefusal::CalibrationInvalid: return "calibration_invalid";
    case AimRefusal::TargetNotValidated: return "target_not_validated";
    case AimRefusal::TargetStale: return "target_stale";
    case AimRefusal::Count:
    default: return "unknown";
    }
}

// True when the value means "the pass did not act".
inline bool AimRefusalBlocked(AimRefusal r)
{
    return r != AimRefusal::None && r != AimRefusal::Count;
}

// The refusal of the tick that just ran, published by the aim worker for the
// debug panel and the run report. One instance per process.
inline std::atomic<uint8_t>& RefusalCode()
{
    static std::atomic<uint8_t> code{ static_cast<uint8_t>(AimRefusal::None) };
    return code;
}

inline void SetRefusal(AimRefusal r)
{
    RefusalCode().store(static_cast<uint8_t>(r), std::memory_order_relaxed);
}

inline AimRefusal CurrentRefusal()
{
    return static_cast<AimRefusal>(RefusalCode().load(std::memory_order_relaxed));
}

// ── Phase 3 -- advantages 3 and 4 ─────────────────────────────────────────
//
// T3.1: ONE gate every input submission passes. Acting on a frame the game has
// already moved on from is how an aim miss is produced rather than corrected. The
// inputs are explicit so the rules are unit-testable without a device or a game.

struct InputGateInputs {
    bool frameValid = false;
    uint64_t frameAgeMs = 0;      // UINT64_MAX when the frame age is unknown
    uint64_t maxFrameAgeMs = 0;   // caller's bound; "no worse than one frame"
    bool pressRequested = false;  // this submission would press a mouse button
    bool pressOutstanding = false;// a previous press has not been released
};

struct InputGateVerdict {
    bool allow = true;
    AimRefusal refusal = AimRefusal::None;
};

inline InputGateVerdict EvaluateInputGate(const InputGateInputs& in)
{
    if (!in.frameValid)
        return { false, AimRefusal::NoFrame };
    if (in.frameAgeMs > in.maxFrameAgeMs)
        return { false, AimRefusal::StaleFrame };

    // A release outstanding blocks a PRESS, not a move: refusing aim movement
    // because a button release is undelivered would turn a device hiccup into a
    // dead aim assist.
    if (in.pressRequested && in.pressOutstanding)
        return { false, AimRefusal::ReleasePending };

    return { true, AimRefusal::None };
}

// ── Candidate-level target gate (Phase 4 -- T4.2, advantages 6 and 7) ───────
//
// Phase 2 gave the pass a name for why it did nothing. That is not enough for
// the question a user actually asks -- "there are four raiders on screen, why is
// it not aiming?" -- because a pass-level reason cannot say WHICH candidate was
// rejected, or how many candidates shared a reason.
//
// These name the candidate-level outcome instead. Every value is set by a real
// gate in Functions/Aimbot.cpp; none is aspirational, and the counts are what
// T4.6 summarises per frame.

enum class TargetRejection : uint8_t {
    None = 0,             // the candidate passed every gate
    NotDrawing,           // actor is not currently drawing
    Ally,                 // resolved as a teammate
    Dead,                 // dead, downed, or a broken bot
    NotARobot,            // in the robot list but not a bot actor (player/duplicate)
    Distance,             // beyond the configured maximum for its class
    StaleMotion,          // position/velocity sample older than its bound
    BoneUnavailable,      // no bone / aim point could be resolved for this actor
    BoneStale,            // bone sample older than its bound
    ProjectionFailed,     // the world position did not project to the screen
    OutsideFov,           // outside the aim cone for this class
    BotUnidentified,      // robot label unresolved, so it cannot be drawn or aimed
    // Strict gate (T4.2). Note what is NOT here: there is no "not a confirmed
    // enemy" reason, because the non-ally fact is established at admission --
    // the collectors reject `isAlly` players and non-bot actors outright, so a
    // candidate in the pool is already a resolved non-ally and re-testing it
    // would be a reason that can never fire.
    AimDataUnvalidated,   // strict: the point is interpolated, not this tick's sample
    AimDataStale,         // strict: the geometry behind the point is older than the bound
    // The collision BVH found cover between the camera and the aim point. Only
    // ever set when a scene is published and the segment was judged: an empty
    // scene fails open (Functions/CollisionBvh.h, BlockedForConsumer), so this
    // reason cannot appear merely because the geometry never decoded.
    Occluded,

    Count
};

inline constexpr size_t kTargetRejectionCount =
    static_cast<size_t>(TargetRejection::Count);

inline const char* TargetRejectionName(TargetRejection r)
{
    switch (r) {
    case TargetRejection::None: return "admitted";
    case TargetRejection::NotDrawing: return "not_drawing";
    case TargetRejection::Ally: return "ally";
    case TargetRejection::Dead: return "dead";
    case TargetRejection::NotARobot: return "not_a_robot";
    case TargetRejection::Distance: return "distance";
    case TargetRejection::StaleMotion: return "stale_motion";
    case TargetRejection::BoneUnavailable: return "bone_unavailable";
    case TargetRejection::BoneStale: return "bone_stale";
    case TargetRejection::ProjectionFailed: return "projection_failed";
    case TargetRejection::OutsideFov: return "outside_fov";
    case TargetRejection::BotUnidentified: return "bot_unidentified";
    case TargetRejection::AimDataUnvalidated: return "aim_data_unvalidated";
    case TargetRejection::AimDataStale: return "aim_data_stale";
    case TargetRejection::Occluded: return "occluded";

    case TargetRejection::Count:
    default: return "unknown";
    }
}

// The strict gate's inputs, explicit so the rules are unit-testable without a
// game or a device.
//
// What "validated" means here, exactly -- because the words are the whole
// feature: the aim point is VALIDATED when it is a sample of this actor's own
// geometry taken on this tick (a plotted bone, or a bot point derived from that
// bot's bounds on this tick). It is not valid when it is an interpolation or a
// carry: the hold-drift glide blends two samples over 0.65 s, and a bot resolved
// from the cache holds an older snapshot. "Derived" (an inferred point) is a
// separate question -- that is AimPointConfidence below, which is DISPLAYED
// (T4.3) rather than refused, because refusing every inferred point would refuse
// every bot and turn a stricter mode into a dead one.
struct TargetGateInputs {
    bool strict = false;
    bool aimDataValidated = false;
    uint64_t aimDataAgeMs = 0;      // age of the geometry behind the point
    uint64_t maxAimDataAgeMs = 0;   // caller's bound
};

struct TargetGateVerdict {
    bool allow = true;
    TargetRejection rejection = TargetRejection::None;
};

inline TargetGateVerdict EvaluateTargetGate(const TargetGateInputs& in)
{
    if (!in.strict)
        return { true, TargetRejection::None };

    if (!in.aimDataValidated)
        return { false, TargetRejection::AimDataUnvalidated };
    if (in.aimDataAgeMs > in.maxAimDataAgeMs)
        return { false, TargetRejection::AimDataStale };

    return { true, TargetRejection::None };
}

// How the aim point was arrived at (T4.3). Displayed wherever the point is
// shown; never folded into a single "valid" flag, because a user reading a
// derived point as a plotted one is the mistake this distinction exists to stop.
enum class AimPointConfidence : uint8_t {
    Unknown = 0,   // nothing resolved this tick
    Confirmed,     // a plotted bone position from this tick's sample
    Estimated      // inferred from geometry, or blended between samples
};

inline const char* AimPointConfidenceName(AimPointConfidence c)
{
    switch (c) {
    case AimPointConfidence::Unknown: return "unknown";
    case AimPointConfidence::Confirmed: return "confirmed";
    case AimPointConfidence::Estimated: return "estimated";
    }
    return "unknown";
}

// The pass-level name for a candidate-level rejection, so the panel and the
// report speak one vocabulary: the candidate reasons explain the pool, and this
// explains the tick. Reasons with no pass-level equivalent (a candidate that was
// simply not the best) map to None -- the pass did not refuse, it chose.
inline AimRefusal RefusalForTargetRejection(TargetRejection r)
{
    switch (r) {
    case TargetRejection::AimDataUnvalidated: return AimRefusal::TargetNotValidated;
    case TargetRejection::AimDataStale: return AimRefusal::TargetStale;

    default: return AimRefusal::None;
    }
}

// Per-tick candidate accounting: T4.2 records it, T4.6 summarises it. Fixed size
// and free of allocation, so the aim thread can hold one as a local.
struct TargetGateCounts {
    std::array<uint16_t, kTargetRejectionCount> byReason{};
    uint16_t considered = 0;      // candidates the collectors examined
    uint16_t admitted = 0;        // candidates that passed every gate

    void Reset() { *this = {}; }

    void Note(TargetRejection r)
    {
        if (considered < 0xFFFF)
            ++considered;
        const size_t idx = static_cast<size_t>(r);
        if (idx < kTargetRejectionCount && byReason[idx] < 0xFFFF)
            ++byReason[idx];
        if (r == TargetRejection::None && admitted < 0xFFFF)
            ++admitted;
    }

    uint32_t Count(TargetRejection r) const
    {
        const size_t idx = static_cast<size_t>(r);
        return idx < kTargetRejectionCount ? byReason[idx] : 0;
    }

    // Candidates rejected for any reason other than "admitted".
    uint32_t Rejected() const
    {
        uint32_t total = 0;
        for (size_t i = 1; i < kTargetRejectionCount; ++i)
            total += byReason[i];
        return total;
    }
};

// The reason that dropped the most candidates. nullptr when nothing was dropped,
// so a caller never invents a reason for an empty frame.
inline const char* DominantTargetRejection(const TargetGateCounts& counts, uint32_t& count)
{
    const char* best = nullptr;
    count = 0;
    for (size_t i = 1; i < kTargetRejectionCount; ++i) {
        if (counts.byReason[i] > count) {
            count = counts.byReason[i];
            best = TargetRejectionName(static_cast<TargetRejection>(i));
        }
    }
    return best;
}

// One line, for the panel and the report: what was considered, what was admitted,
// and the top reasons for the rest. Empty when nothing was dropped.
inline std::string TargetGateSummaryLine(const TargetGateCounts& counts)
{
    std::string line = std::to_string(counts.admitted) + "/"
        + std::to_string(counts.considered) + " admitted";

    // Top three reasons, highest first, without a second container: three passes
    // over 16 counters is cheaper than the allocation it would replace.
    std::array<bool, kTargetRejectionCount> used{};
    for (int pick = 0; pick < 3; ++pick) {
        size_t bestIdx = 0;
        uint16_t bestN = 0;
        for (size_t i = 1; i < kTargetRejectionCount; ++i) {
            if (!used[i] && counts.byReason[i] > bestN) {
                bestN = counts.byReason[i];
                bestIdx = i;
            }
        }
        if (bestIdx == 0 || bestN == 0)
            break;
        used[bestIdx] = true;
        line += ", ";
        line += TargetRejectionName(static_cast<TargetRejection>(bestIdx));
        line += " ";
        line += std::to_string(bestN);
    }
    return line;
}

// ── Aim pass heartbeat ────────────────────────────────────────────────────
//
// With the menu open the path worker deliberately skips every tick
// (Functions/EngineThreads.cpp), and a pass with no candidate in range examines
// nothing and submits nothing. Those two states are NOT the same, and both used
// to read as "refusal: not_refused, candidates: 0" -- which is indistinguishable
// from a dead worker, and is why "the key does nothing" could not be told apart
// from "the pass never ran". Publishing a heartbeat separates them: a frozen tick
// count means the worker is not running, an advancing one with zero candidates
// means it ran and found nothing.
//
// Lock-free on purpose: the aim thread updates this at its own cadence (about
// 250 times a second) and no reader may stall it.

enum class AimPassState : uint8_t {
    NotStarted = 0,   // the worker has not run in this process
    Running,          // the pass ran; lastTickMs says when
    SuspendedMenu,    // skipped because the menu is open (deliberate)
    NoRaid            // skipped because no world/raid is resolved
};

inline const char* AimPassStateName(AimPassState s)
{
    switch (s) {
    case AimPassState::NotStarted: return "not_started";
    case AimPassState::Running: return "running";
    case AimPassState::SuspendedMenu: return "suspended_menu";
    case AimPassState::NoRaid: return "no_raid";
    }
    return "not_started";
}

inline std::atomic<uint64_t>& AimPassTicks()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

inline std::atomic<uint64_t>& AimPassLastTickMs()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

inline std::atomic<uint64_t>& AimPassLastSkipMs()
{
    static std::atomic<uint64_t> v{ 0 };
    return v;
}

inline std::atomic<uint8_t>& AimPassStateSlot()
{
    static std::atomic<uint8_t> v{ static_cast<uint8_t>(AimPassState::NotStarted) };
    return v;
}

// How far the last aim tick got. The pass sets a named refusal at every gate it
// can stop at, so a tick that reports NO refusal and no candidate accounting has
// stopped somewhere that is not a gate -- which is exactly the shape a silently
// swallowed exception takes (Interface/Utils/Threads/SyncedThread.h catches them
// and the thread carries on). This marker says which checkpoint was last reached,
// so "it does nothing" can be localised instead of guessed at.
//    entered -> requested -> keys -> gate -> world -> frame -> collected ->
//    selected -> submitted -> done
enum class AimPassStage : uint8_t {
    NotStarted = 0, Requested, Keys, Gate, World, Frame, Collected, Selected,
    Submitted, Done
};

inline const char* AimPassStageName(AimPassStage s)
{
    switch (s) {
    case AimPassStage::NotStarted: return "not_started";
    case AimPassStage::Requested: return "entered";
    case AimPassStage::Keys: return "keys_read";
    case AimPassStage::Gate: return "key_gate_passed";
    case AimPassStage::World: return "world_resolved";
    case AimPassStage::Frame: return "frame_accepted";
    case AimPassStage::Collected: return "candidates_collected";
    case AimPassStage::Selected: return "target_selected";
    case AimPassStage::Submitted: return "input_gate_passed";
    case AimPassStage::Done: return "completed";
    }
    return "not_started";
}

inline std::atomic<uint8_t>& AimPassStageSlot()
{
    static std::atomic<uint8_t> v{ static_cast<uint8_t>(AimPassStage::NotStarted) };
    return v;
}

inline void NoteAimPassStage(AimPassStage s)
{
    AimPassStageSlot().store(static_cast<uint8_t>(s), std::memory_order_relaxed);
}

inline AimPassStage CurrentAimPassStage()
{
    return static_cast<AimPassStage>(
        AimPassStageSlot().load(std::memory_order_relaxed));
}

inline void NoteAimPassTick(uint64_t nowMs)
{
    AimPassTicks().fetch_add(1, std::memory_order_relaxed);
    AimPassLastTickMs().store(nowMs, std::memory_order_relaxed);
    AimPassStateSlot().store(static_cast<uint8_t>(AimPassState::Running),
        std::memory_order_relaxed);
}

inline void NoteAimPassSkipped(bool noRaid, uint64_t nowMs)
{
    AimPassLastSkipMs().store(nowMs, std::memory_order_relaxed);
    AimPassStateSlot().store(
        static_cast<uint8_t>(noRaid ? AimPassState::NoRaid : AimPassState::SuspendedMenu),
        std::memory_order_relaxed);
}

struct AimPassSnapshot {
    AimPassState state = AimPassState::NotStarted;
    AimPassStage stage = AimPassStage::NotStarted;
    uint64_t ticks = 0;
    uint64_t lastTickMs = 0;
    uint64_t lastSkipMs = 0;
};

inline AimPassSnapshot CurrentAimPass()
{
    AimPassSnapshot s;
    s.state = static_cast<AimPassState>(
        AimPassStateSlot().load(std::memory_order_relaxed));
    s.stage = CurrentAimPassStage();
    s.ticks = AimPassTicks().load(std::memory_order_relaxed);
    s.lastTickMs = AimPassLastTickMs().load(std::memory_order_relaxed);
    s.lastSkipMs = AimPassLastSkipMs().load(std::memory_order_relaxed);
    return s;
}

// The aim point as the last tick computed it, published for the debug panel, the
// practice reference and the run report (T4.3). Provenance travels WITH the point:
// no display reconstructs how the point was arrived at, so no display can show an
// estimate as a confirmed point.
struct AimPointDebug {
    uint8_t confidence = 0;      // AimPointConfidence
    bool validated = false;      // this tick's own sample, not an interpolation
    float ageMs = 0.f;           // age of the geometry behind the point
    float errorPxX = 0.f;        // screen error at the moment of the command, X
    float errorPxY = 0.f;        // ...and Y, kept separate: the pair is what the
                                 // per-axis calibration is read against
    float cmdX = 0.f;            // mouse counts actually commanded, X
    float cmdY = 0.f;            // ...and Y
    uint64_t targetKey = 0;
    bool isRobot = false;
    bool strict = false;         // the strict gate was in force for this point
    uint64_t stampMs = 0;
};

inline std::mutex& AimPointDebugMutex()
{
    static std::mutex m;
    return m;
}

inline AimPointDebug& AimPointDebugSlot()
{
    static AimPointDebug slot;
    return slot;
}

inline void PublishAimPointDebug(const AimPointDebug& point)
{
    std::lock_guard<std::mutex> lock(AimPointDebugMutex());
    AimPointDebugSlot() = point;
}

inline AimPointDebug CurrentAimPointDebug()
{
    std::lock_guard<std::mutex> lock(AimPointDebugMutex());
    return AimPointDebugSlot();
}

// The last completed tick's candidate accounting, published by the aim thread for
// the debug panel (T4.6) and the run report. One struct copy under one mutex: a
// fixed 40 bytes, cheaper than an allocation per tick, and both readers run far
// slower than the aim loop. The publish stamp lets a reader tell "a quiet tick"
// from "the aim thread has not run", which a bare count cannot.
inline std::mutex& TargetGateCountsMutex()
{
    static std::mutex m;
    return m;
}

inline TargetGateCounts& TargetGateCountsSlot()
{
    static TargetGateCounts counts;
    return counts;
}

inline std::atomic<uint64_t>& TargetGateCountsStamp()
{
    static std::atomic<uint64_t> stamp{ 0 };
    return stamp;
}

inline void PublishTargetGateCounts(const TargetGateCounts& counts, uint64_t nowMs)
{
    std::lock_guard<std::mutex> lock(TargetGateCountsMutex());
    TargetGateCountsSlot() = counts;
    TargetGateCountsStamp().store(nowMs, std::memory_order_relaxed);
}

inline TargetGateCounts CurrentTargetGateCounts(uint64_t* outStampMs = nullptr)
{
    std::lock_guard<std::mutex> lock(TargetGateCountsMutex());
    if (outStampMs)
        *outStampMs = TargetGateCountsStamp().load(std::memory_order_relaxed);
    return TargetGateCountsSlot();
}

// ── Age of a pool sample, measured where it was captured ─────────────────
//
// The aim pool is a frame SNAPSHOT (Engine::EspRenderFrame). The position
// sampler stamps a cache entry, the collect copies it, the frame is published,
// and an aim tick reads the copy -- so every stamp inside a copy is older than
// the reader's clock by the whole collect→read latency, while the sample itself
// was fresh when it was copied. Measured on this build 2026-10-13: the collect
// takes 340 ms, the frame an aim tick reads is 296 ms old, the copies' motion
// stamps were 646 ms old at the gate, and the live sampler behind them was
// stamping every 16 ms. Charging that latency to the candidate made a 150 ms
// bound refuse EVERY candidate in the pool, forever, with a healthy sampler.
//
// These bounds ask how old the sample was WHEN IT WAS CAPTURED. End-to-end
// latency is a different question and is bounded where it belongs
// (EspFramePolicy::kMaxFrameAgeMs at frame acceptance), then measured and
// published as AimGateTrace::snapshotLatencyMs rather than charged here.
//
//   capturedAtMs == 0 -> a live cache entry: age against nowMs
//   capturedAtMs != 0 -> a snapshot copy: age against the snapshot's capture
//
// Returns UINT64_MAX when there is no honest age: never sampled, or a stamp the
// reference clock says is in the future. A snapshot copy cannot carry a stamp
// newer than its own capture point, so such a reading is an inconsistency and
// is refused rather than clamped to zero.
inline uint64_t SnapshotSampleAgeMs(
    uint64_t sampleMs, uint64_t nowMs, uint64_t capturedAtMs)
{
    if (!sampleMs)
        return UINT64_MAX;
    const uint64_t reference = capturedAtMs ? capturedAtMs : nowMs;
    return sampleMs > reference ? UINT64_MAX : reference - sampleMs;
}

// ── How stale each shared-frame field may be (single source of truth) ───────
//
// A field's probe cadence and the consumers of that field must not each carry
// a private idea of "fresh". That is exactly how the same data ended up probed
// twice at two different rates: the health probe's own 500 ms literal lived in
// the retain worker while its readers assumed the newest frame. Each field
// names its tolerance here, the probe interval is derived from it, and a
// consumer asks FieldFresh() instead of trusting arrival order.
namespace Staleness {
inline constexpr uint64_t PositionMs = 16;    // one PositionRefresh pass
inline constexpr uint64_t BoundsMs = 250;
inline constexpr uint64_t HealthMs = 500;
inline constexpr uint64_t BrokenMs = 500;
inline constexpr uint64_t NameMs = 1000;
inline constexpr uint64_t KitMs = 1000;
}

/**
 * Age of a live-cache field stamp. UINT64_MAX when the field was never
 * sampled, so "unread" can never be mistaken for "fresh" — the same rule
 * SnapshotSampleAgeMs applies to snapshot copies.
 */
inline uint64_t FieldAgeMs(uint64_t stampMs, uint64_t nowMs)
{
    return SnapshotSampleAgeMs(stampMs, nowMs, 0);
}

/** True when the field has a stamp and is inside its tolerance. */
inline bool FieldFresh(uint64_t stampMs, uint64_t nowMs, uint64_t toleranceMs)
{
    const uint64_t age = FieldAgeMs(stampMs, nowMs);
    return age != UINT64_MAX && age <= toleranceMs;
}

// ── Why a candidate never reached the strict gate (phase 4 evidence) ───────
//
// Counting rejections says HOW MANY candidates shared a reason; it cannot say
// what produced the reason. "stale_motion 3" is consistent with a sampler
// running 40 ms late and with three entries whose root was never resolvable,
// and those two have different fixes. This records the facts behind the motion
// refusal and the strict gate's own decision on every candidate that reached
// it, so the evidence shows what the gate was looking at rather than only what
// it removed.
//
// Fixed size and allocation-free: the aim thread records one per tick.

inline constexpr size_t kAimTraceCandidates = 8;

struct AimTraceCandidate {
    uint64_t key = 0;
    bool isRobot = false;
    uint8_t outcome = 0;       // TargetRejection of the gate that decided it
    // Age of the motion sample AT ITS CAPTURE (SnapshotSampleAgeMs), UINT64_MAX
    // when there is no stamp to age.
    uint64_t motionAgeMs = 0;
    bool hasRoot = false;      // there was a root component to sample at all
    bool everSampled = false;  // this entry has had at least one real sample
    // Strict gate: filled only for a candidate that survived admission.
    bool strictEvaluated = false;
    bool strictInForce = false;
    bool validated = false;
    float aimDataAgeMs = 0.f;
    uint64_t maxAimDataAgeMs = 0;
    float distToCenterPx = 0.f;
};

struct AimGateTrace {
    uint64_t stampMs = 0;
    uint16_t recorded = 0;          // candidates captured (bounded)
    // Motion refusals, with the facts that explain the age.
    uint16_t motionStale = 0;
    uint16_t motionStaleAged = 0;   // how many contributed to the age range
    uint64_t motionStaleMinAgeMs = UINT64_MAX;
    uint64_t motionStaleMaxAgeMs = 0;
    uint16_t motionStaleNoRoot = 0;
    uint16_t motionStaleNeverSampled = 0;
    uint16_t motionStaleNoStamp = 0;
    // The latency the tick's pool was NOT charged with: how old the snapshot
    // carrying its motion samples was (0 when the collectors read the live cache
    // instead). Published beside the age range so the two are read together --
    // a small age behind a large latency is a healthy sampler under a slow
    // frame, while a small age behind a small latency is a live pool.
    uint64_t snapshotLatencyMs = 0;
    // What the strict gate decided on the candidates that DID reach it.
    bool strictInForce = false;
    uint16_t strictEvaluated = 0;
    uint16_t strictAdmitted = 0;
    uint16_t strictRefused = 0;
    std::array<uint16_t, kTargetRejectionCount> strictByReason{};
    std::array<AimTraceCandidate, kAimTraceCandidates> candidates{};

    void Reset() { *this = {}; }

    void Note(const AimTraceCandidate& c)
    {
        if (recorded >= kAimTraceCandidates)
            return;
        candidates[recorded++] = c;
    }

    void NoteMotionStale(
        uint64_t ageMs, bool hasRoot, bool everSampled, bool stampMissing)
    {
        if (motionStale < 0xFFFF)
            ++motionStale;
        if (!stampMissing && ageMs != UINT64_MAX) {
            ++motionStaleAged;
            if (ageMs < motionStaleMinAgeMs)
                motionStaleMinAgeMs = ageMs;
            if (ageMs > motionStaleMaxAgeMs)
                motionStaleMaxAgeMs = ageMs;
        }
        if (!hasRoot)
            ++motionStaleNoRoot;
        if (!everSampled)
            ++motionStaleNeverSampled;
        if (stampMissing)
            ++motionStaleNoStamp;
    }

    void NoteStrict(bool inForce, bool allow, TargetRejection rejection)
    {
        strictInForce = inForce;
        ++strictEvaluated;
        if (allow)
            ++strictAdmitted;
        else
            ++strictRefused;
        const size_t idx = static_cast<size_t>(rejection);
        if (idx < kTargetRejectionCount && strictByReason[idx] < 0xFFFF)
            ++strictByReason[idx];
    }
};

// The observed sample age range behind the motion refusals, or empty when
// nothing was refused for that reason -- a zero is not a range.
inline std::string MotionStaleSummary(const AimGateTrace& t)
{
    if (t.motionStale == 0)
        return std::string();
    std::string line = std::to_string(t.motionStale) + " stale_motion";
    if (t.motionStaleAged > 0) {
        line += " (age ";
        line += std::to_string(t.motionStaleMinAgeMs);
        line += "-";
        line += std::to_string(t.motionStaleMaxAgeMs);
        line += " ms";
        if (t.motionStaleNoRoot)
            line += ", no_root " + std::to_string(t.motionStaleNoRoot);
        if (t.motionStaleNeverSampled)
            line += ", never_sampled " + std::to_string(t.motionStaleNeverSampled);
        if (t.motionStaleNoStamp)
            line += ", no_stamp " + std::to_string(t.motionStaleNoStamp);
        if (t.snapshotLatencyMs)
            line += ", snapshot " + std::to_string(t.snapshotLatencyMs) + " ms";
        line += ")";
    }
    return line;
}

// What the strict gate did with the candidates that reached it, and why.
inline std::string StrictDecisionSummary(const AimGateTrace& t)
{
    std::string line = t.strictInForce ? "in force" : "off";
    line += ": evaluated " + std::to_string(t.strictEvaluated);
    line += ", admitted " + std::to_string(t.strictAdmitted);
    line += ", refused " + std::to_string(t.strictRefused);
    for (size_t i = 1; i < kTargetRejectionCount; ++i) {
        if (t.strictByReason[i] == 0)
            continue;
        line += ", ";
        line += TargetRejectionName(static_cast<TargetRejection>(i));
        line += " " + std::to_string(t.strictByReason[i]);
    }
    return line;
}

inline std::mutex& AimGateTraceMutex()
{
    static std::mutex m;
    return m;
}

inline AimGateTrace& AimGateTraceSlot()
{
    static AimGateTrace trace;
    return trace;
}

inline void PublishAimGateTrace(const AimGateTrace& trace, uint64_t nowMs)
{
    std::lock_guard<std::mutex> lock(AimGateTraceMutex());
    AimGateTraceSlot() = trace;
    AimGateTraceSlot().stampMs = nowMs;
}

// The trace of the last aim tick that reached the collectors. One struct copy
// under one mutex, the same contract as the candidate counts beside it.
inline AimGateTrace CurrentAimGateTrace()
{
    std::lock_guard<std::mutex> lock(AimGateTraceMutex());
    return AimGateTraceSlot();
}

// The motion facts only describe a motion refusal. A candidate that the strict
// gate admitted has no sample-age story, and emitting a default there would read
// as "no root" -- the exact misreading the field exists to prevent.
inline bool TraceCandidateHasMotionFacts(const AimTraceCandidate& c)
{
    return c.outcome == static_cast<uint8_t>(TargetRejection::StaleMotion);
}

// ── How the assist is actually paced (measured, not described) ─────────────
//
// "The aim feels slow" has at least three candidate causes that read identically
// from outside: the pass is not running, it runs but the humanizer drops the
// tick, or it acts on every tick and the pull is simply weak. A refusal name
// cannot separate them, so the output path counts what it did: ticks that sent
// at least one count, ticks the humanizer held off, and when the last count
// actually left. Cumulative, because a rate is read by differencing two samples
// and a per-tick number cannot tell "slow" from "stopped".
struct AimTimingTrace {
    uint64_t stampMs = 0;
    float tickMs = 0.f;           // interval between aim pass entries
    uint64_t submits = 0;         // ticks that sent at least one count
    uint64_t humanizerSkips = 0;  // ticks the humanizer refused to update on
    uint64_t lastSubmitMs = 0;    // steady ms of the last sent count
    float lastDistPx = 0.f;       // error at that submission
    float lastCloseFrac = 0.f;    // fraction of the error the pull asked for
    float lastGain = 0.f;         // counts-per-pixel gain actually in force

    void Reset() { *this = {}; }
};

inline std::mutex& AimTimingMutex()
{
    static std::mutex m;
    return m;
}

inline AimTimingTrace& AimTimingSlot()
{
    static AimTimingTrace trace;
    return trace;
}

inline void PublishAimTimingTrace(const AimTimingTrace& trace, uint64_t nowMs)
{
    std::lock_guard<std::mutex> lock(AimTimingMutex());
    AimTimingSlot() = trace;
    AimTimingSlot().stampMs = nowMs;
}

inline AimTimingTrace CurrentAimTimingTrace()
{
    std::lock_guard<std::mutex> lock(AimTimingMutex());
    return AimTimingSlot();
}

// T3.2: one owner for press/release, so a press can never be started while a
// release is still outstanding, and a release is attempted a bounded number of
// times instead of being sent once and assumed.
class PressReleaseState {
public:
    static constexpr int kMaxReleaseAttempts = 3;

    // False means: do not press (the caller counts the refusal).
    bool BeginPress()
    {
        if (m_pressOutstanding)
            return false;
        m_pressOutstanding = true;
        return true;
    }

    // Number of release attempts the caller must make. 0 when nothing is held.
    // The state is cleared here: the caller reports a device that could not take
    // the release, rather than this machine forgetting that it tried.
    int BeginRelease()
    {
        if (!m_pressOutstanding)
            return 0;
        m_pressOutstanding = false;
        return kMaxReleaseAttempts;
    }

    bool PressOutstanding() const { return m_pressOutstanding; }

    void NoteReleaseUndeliverable()
    {
        ++m_undeliverable;
        // Undelivered is not released: keep the risk visible instead of claiming
        // the button came up.
        m_pressOutstanding = true;
    }

    uint32_t UndeliverableReleases() const { return m_undeliverable; }

private:
    bool m_pressOutstanding = false;
    uint32_t m_undeliverable = 0;
};

} // namespace FeaturePolicy
