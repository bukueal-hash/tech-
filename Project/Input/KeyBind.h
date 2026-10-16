#pragma once

#include "InputBind.h"
#include <Windows.h>

inline bool KeyBindIsHeld(int keyCode)
{
    if (keyCode == 0)
        keyCode = VK_SHIFT;
    return InputBindIsDown(keyCode);
}
