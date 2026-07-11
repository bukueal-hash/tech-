#pragma once

bool KeyBindIsHeld(int keyCode);
bool KeyBindAimPrimaryHeld();
/** Primary aim hotkey only (aimbot). */
bool KeyBindAimHeld();
/** Kept for worker wake; same as primary aim key. */
bool KeyBindAimOrTriggerHeld();
void KeyBindClearAimLatch();
bool KeyBindIsGamepadCode(int keyCode);
const char* KeyBindDisplayName(int keyCode);
