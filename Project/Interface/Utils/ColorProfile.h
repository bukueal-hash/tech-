#pragma once
// Color profile application (D19). Bridges the pure ColorProfiles document to
// the live var:: color table: capture writes the current palette into a slot,
// apply writes a slot back over it. The document format and its failure rules
// live in Core/ColorProfiles.hpp so they stay testable on their own.

#include <string>

namespace ColorProfile {

/** -1 = no profile selected (colors are being edited freely). */
int ActiveSlot();
void SetActiveSlot(int slot);

/** Read Data\color_profiles.json once. A missing file is not an error. */
void EnsureLoaded();

/** Persist every slot. False when the file could not be written. */
bool SaveToDisk();

/** Copy a stored slot over the live colors. Keys the slot does not contain are
 *  left untouched, so an older profile cannot wipe a newly added color.
 *  No-op when the slot is empty or out of range. */
void ApplySlot(int slot);

/** Snapshot the live colors into a slot (named "Slot N" if unnamed), persist,
 *  and mark the config dirty. False when the save failed. */
bool CaptureSlot(int slot);

/** Forget a slot's stored colors and persist. False when nothing was there. */
bool ClearSlot(int slot);

/** Stored slot name, empty string when the slot does not exist. */
std::string SlotName(int slot);

/** How many config keys a complete profile covers — shown in the menu so it is
 *  obvious when a profile predates a newly added color. */
int ColorCount();

} // namespace ColorProfile
