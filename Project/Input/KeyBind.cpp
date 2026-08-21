#include "KeyBind.h"

#include "InputBind.h"

#include <Windows.h>

bool KeyBindIsHeld(int keyCode)
{
    if (keyCode == 0)
        keyCode = VK_SHIFT;
    return InputBindIsDown(keyCode);
}
