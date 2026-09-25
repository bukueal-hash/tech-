#pragma once

#include <cstdint>

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

// Player aiming owns player targeting. Trigger-only mode also targets players
// unless the user has explicitly enabled robot targeting.
inline bool ShouldCollectPlayerTargets(const AimFeatures& features)
{
    return features.playerAim || (features.trigger && !features.robotAim);
}

inline bool ShouldCollectRobotTargets(const AimFeatures& features)
{
    return features.robotAim;
}

// Aim and robot-aim are movement features. Trigger-only operation may select
// and fire at a target, but must never move the pointer toward it.
inline bool ShouldSendAimHardware(const AimFeatures& features)
{
    return features.playerAim || features.robotAim;
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
    return robotEsp || features.robotAim || radar;
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

} // namespace FeaturePolicy
