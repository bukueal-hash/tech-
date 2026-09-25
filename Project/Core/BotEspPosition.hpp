#pragma once

#include "Vector.hpp"

namespace BotEspPosition {

template <typename IsValid>
inline bool Select(
    const Vector3& root,
    const Vector3& center,
    IsValid&& isValid,
    Vector3& out)
{
    if (isValid(root)) {
        out = root;
        return true;
    }
    if (isValid(center)) {
        out = center;
        return true;
    }
    return false;
}

} // namespace BotEspPosition
