#pragma once

#include "../Core/Vector.hpp"
#include <cstddef>
#include <cstdint>

namespace CollisionLos {

/** Ray-triangle LOS against static-mesh KD-tree. true = clear line of sight. */
bool IsVisible(const Vector3& from, const Vector3& to);

/** Background rebuild when local player moves 3000uu or every 20s. */
void ScheduleWorldRebuild(uintptr_t uworld, const Vector3& localPos);

std::size_t TriangleCount();
/** Static mesh components collected in last rebuild attempt. */
std::size_t LastSmcCount();
bool IsRebuilding();

} // namespace CollisionLos
