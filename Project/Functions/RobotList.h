#pragma once

#include <cstdint>
#include <string>

/** Internal cache token when struct bot admitted but fname decrypt failed. */
inline constexpr const char* kBotStructAdmissionToken = "Constructable";

/** Constructable @0x1210 vs generic @0x1220 — shared by RobotList and aimbot. */
uint8_t ReadBotBrokenFlag(uintptr_t actor);

/** Increment when RenderRobotEspFromFrame fails ResolveBotDrawLabel (debug overlay). */
void RecordBotDrawLabelMiss();

/** Full bot display name from actor + fname (shared by admission and draw). */
std::string ResolveBotTypeLabel(uintptr_t actor, const std::string& fname);

/** Use cached label when accepted; else one ResolveBotTypeLabel pass. */
std::string ResolveBotDrawLabel(
    uintptr_t actor,
    const std::string& cachedLabel,
    const std::string& fnameHint);
