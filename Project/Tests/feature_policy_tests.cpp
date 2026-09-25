#include "tests_main.hpp"
#include "Core/FeaturePolicy.hpp"

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

TEST_CASE("Feature policy keeps aim and trigger masters independent")
{
    using FeaturePolicy::AimFeatures;

    const AimFeatures disabled{};
    CHECK_FALSE(FeaturePolicy::ShouldRunAimPass(disabled));
    CHECK_FALSE(FeaturePolicy::ShouldSendAimHardware(disabled));
    CHECK_FALSE(FeaturePolicy::ShouldCollectPlayerTargets(disabled));
    CHECK_FALSE(FeaturePolicy::ShouldCollectRobotTargets(disabled));

    const AimFeatures playerAim{true, false, false};
    CHECK(FeaturePolicy::ShouldRunAimPass(playerAim));
    CHECK(FeaturePolicy::ShouldSendAimHardware(playerAim));
    CHECK(FeaturePolicy::ShouldCollectPlayerTargets(playerAim));
    CHECK_FALSE(FeaturePolicy::ShouldCollectRobotTargets(playerAim));

    const AimFeatures robotAim{false, true, false};
    CHECK(FeaturePolicy::ShouldSendAimHardware(robotAim));
    CHECK_FALSE(FeaturePolicy::ShouldCollectPlayerTargets(robotAim));
    CHECK(FeaturePolicy::ShouldCollectRobotTargets(robotAim));

    const AimFeatures triggerOnly{false, false, true};
    CHECK(FeaturePolicy::ShouldRunAimPass(triggerOnly));
    CHECK_FALSE(FeaturePolicy::ShouldSendAimHardware(triggerOnly));
    CHECK(FeaturePolicy::ShouldCollectPlayerTargets(triggerOnly));
    CHECK_FALSE(FeaturePolicy::ShouldCollectRobotTargets(triggerOnly));

    const AimFeatures triggerRobot{false, true, true};
    CHECK_FALSE(FeaturePolicy::ShouldCollectPlayerTargets(triggerRobot));
    CHECK(FeaturePolicy::ShouldCollectRobotTargets(triggerRobot));
}

TEST_CASE("Feature policy prevents robot aim from enabling bot ESP")
{
    CHECK(FeaturePolicy::ShouldRenderRobotEsp(true));
    CHECK_FALSE(FeaturePolicy::ShouldRenderRobotEsp(false));
}

TEST_CASE("Feature policy shows FOV only for a hardware movement feature")
{
    using FeaturePolicy::AimFeatures;
    const AimFeatures triggerOnly{false, false, true};
    const AimFeatures playerAim{true, false, true};

    CHECK_FALSE(FeaturePolicy::ShouldRenderFov(true, triggerOnly));
    CHECK(FeaturePolicy::ShouldRenderFov(true, playerAim));
    CHECK_FALSE(FeaturePolicy::ShouldRenderFov(false, playerAim));
}

TEST_CASE("Feature policy gates high-frequency position work")
{
    using FeaturePolicy::AimFeatures;
    const AimFeatures disabled{};
    const AimFeatures playerAim{true, false, false};
    const AimFeatures triggerOnly{false, false, true};
    const AimFeatures robotAim{false, true, false};

    CHECK_FALSE(FeaturePolicy::ShouldRefreshPlayerPositions(false, false, disabled));
    CHECK(FeaturePolicy::ShouldRefreshPlayerPositions(true, false, disabled));
    CHECK(FeaturePolicy::ShouldRefreshPlayerPositions(false, true, disabled));
    CHECK(FeaturePolicy::ShouldRefreshPlayerPositions(false, false, playerAim));
    CHECK(FeaturePolicy::ShouldRefreshPlayerPositions(false, false, triggerOnly));

    CHECK_FALSE(FeaturePolicy::ShouldRefreshRobotPositions(false, disabled, false));
    CHECK(FeaturePolicy::ShouldRefreshRobotPositions(true, disabled, false));
    CHECK(FeaturePolicy::ShouldRefreshRobotPositions(false, robotAim, false));
    CHECK(FeaturePolicy::ShouldRefreshRobotPositions(false, disabled, true));
}

TEST_CASE("Trigger activation is edge-safe and independent of target availability")
{
    FeaturePolicy::TriggerToggleState state;

    CHECK_FALSE(FeaturePolicy::UpdateTriggerActivation(false, 1, true, state));
    CHECK_FALSE(state.toggled);

    CHECK(FeaturePolicy::UpdateTriggerActivation(true, 2, false, state));
    CHECK_FALSE(state.toggled);

    CHECK(FeaturePolicy::UpdateTriggerActivation(true, 0, true, state));
    CHECK(FeaturePolicy::UpdateTriggerActivation(true, 0, true, state));
    CHECK_FALSE(FeaturePolicy::UpdateTriggerActivation(true, 0, false, state));

    CHECK(FeaturePolicy::UpdateTriggerActivation(true, 1, true, state));
    CHECK(FeaturePolicy::UpdateTriggerActivation(true, 1, true, state));
    CHECK(FeaturePolicy::UpdateTriggerActivation(true, 1, false, state));
    CHECK_FALSE(FeaturePolicy::UpdateTriggerActivation(true, 1, true, state));
    CHECK_FALSE(FeaturePolicy::UpdateTriggerActivation(true, 1, true, state));
}
