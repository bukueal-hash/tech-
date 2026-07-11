#include "KeyBind.h"

#include "InputBind.h"
#include "../Interface/Utils/Variables/index.h"

#include <Windows.h>

bool KeyBindIsHeld(int keyCode)
{
    if (keyCode == 0)
        keyCode = VK_SHIFT;
    return InputBindIsDown(keyCode);
}

bool KeyBindAimPrimaryHeld()
{
    return KeyBindIsHeld(var::aim_hold_key);
}

bool KeyBindAimHeld()
{
    return KeyBindAimPrimaryHeld();
}

bool KeyBindAimOrTriggerHeld()
{
    return KeyBindAimPrimaryHeld();
}

void KeyBindClearAimLatch()
{
}

bool KeyBindIsGamepadCode(int keyCode)
{
    return InputBindCodeIsGamepad(keyCode);
}

const char* KeyBindDisplayName(int keyCode)
{
    if (InputBindCodeIsGamepad(keyCode))
        return InputBindCodeLabel(keyCode);
    return nullptr;
}
