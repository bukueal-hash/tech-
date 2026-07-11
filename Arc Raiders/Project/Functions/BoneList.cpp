#include <Windows.h>
#include <vector>

#include "../Core/Engine.h"
#include <ranges>
#include <algorithm>

bool Engine::IsValidPointer(uintptr_t ptr) {
    return ptr >= 0x1000 && ptr < 0x7FFFFFFFFFFF;
}

bool Engine::IsUsermodePtr(uintptr_t ptr)
{
    return ptr > 0x10000 && ptr < 0x00007FFFFFFFFFFF;
}

Vector3 Engine::GetBone(
    int boneIndex,
    uintptr_t boneArray,
    FTransform componentToWorld
)
{
    if (!IsValidPointer(boneArray))
        return Vector3{};

    FTransform bone =
        Memory::read<FTransform>(boneArray + (boneIndex * 0x60));

    D3DMATRIX matrix = MatrixMultiplication(
        bone.ToMatrixWithScale(),
        componentToWorld.ToMatrixWithScale()
    );

    return Vector3(matrix._41, matrix._42, matrix._43);
}

void Engine::GetBones(PlayerCacheEntry& actor)
{
    if (!IsValidPointer(actor.actorMesh))
        return;

    actor.boneData.valid.reset();
    actor.boneData.isVisible = false;

    if (!actor.boneArray)
        actor.boneArray = GetBoneArrayDecrypt(actor.actorMesh);

    if (!actor.boneArray || !IsValidPointer(actor.boneArray))
        return;

    FTransform componentToWorld =
        Memory::read<FTransform>(
            actor.actorMesh + Offsets::ComponentToWorld
        );

    for (const auto& [gameIndex, uniBone] : GameBoneMapArcRaiders)
    {
        Vector3 worldPos =
            GetBone(gameIndex, actor.boneArray, componentToWorld);

        if (worldPos.x == 0.0 && worldPos.y == 0.0 && worldPos.z == 0.0)
            continue;

        Vector3 screenPos;
        if (!ProjectWorldLocationToScreen(worldPos, screenPos))
            continue;

        size_t idx = (size_t)uniBone;
        actor.boneData.bonesDouble[idx] =
            Vector3{ screenPos.x, screenPos.y, 0.0 };

        actor.boneData.bonesWorldDouble[idx] = worldPos;

        actor.boneData.valid.set(idx);
    }

    if (actor.boneData.valid.test((size_t)UniBone::Head) &&
        actor.boneData.valid.test((size_t)UniBone::Pelvis))
    {
        actor.boneData.isVisible = true;
    }
}