#include <Windows.h>
#include <vector>
#include <cmath>

#include "../Core/Engine.h"
#include "../Core/SteamDecrypt.hpp"
#include <algorithm>

extern Engine engine;

bool Engine::IsValidPointer(uintptr_t ptr) const {
    return ptr >= 0x1000 && ptr < 0x7FFFFFFFFFFF;
}

bool Engine::IsUsermodePtr(uintptr_t ptr)
{
    return ptr > 0x10000 && ptr < 0x00007FFFFFFFFFFF;
}

static bool IsNearZero(const Vector3& v)
{
    return std::fabs(v.x) < 0.5 && std::fabs(v.y) < 0.5 && std::fabs(v.z) < 0.5;
}

static int ScoreBoneArrayForMesh(
    Engine& eng,
    uintptr_t mesh,
    uintptr_t boneArray)
{
    if (!mesh || !boneArray || !eng.IsValidPointer(boneArray))
        return 0;

    const FTransform ctw = Engine::ReadComponentToWorld(mesh);
    int score = 0;
    bool hasHead = false;
    bool hasPelvis = false;

    for (const auto& [gameIndex, uniBone] : eng.GameBoneMapArcRaiders) {
        const FTransform bone =
            Memory::read<FTransform>(boneArray + (gameIndex * 0x60));
        const D3DMATRIX matrix = eng.MatrixMultiplication(
            bone.ToMatrixWithScale(),
            ctw.ToMatrixWithScale());
        const Vector3 world(matrix._41, matrix._42, matrix._43);

        if (!IsPlausibleWorldPos(world) || IsNearZero(world))
            continue;

        ++score;
        if (uniBone == UniBone::Head)
            hasHead = true;
        if (uniBone == UniBone::Pelvis)
            hasPelvis = true;
    }

    if (!hasHead || !hasPelvis)
        return 0;

    return score;
}

struct BoneArrayCandidate {
    uintptr_t array = 0;
    uintptr_t mesh = 0;
    int score = 0;
};

static BoneArrayCandidate FindBestBoneArray(uintptr_t actor, uintptr_t primaryMesh)
{
    BoneArrayCandidate best{};

    const uintptr_t meshA = actor
        ? Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent) : 0;
    const uintptr_t meshB = actor
        ? Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh) : 0;

    const uintptr_t meshes[] = { meshB, primaryMesh, meshA };

    Engine& eng = engine;
    for (uintptr_t mesh : meshes) {
        if (!mesh || !eng.IsValidPointer(mesh))
            continue;

        const uintptr_t arr = steam_decrypt::GetBoneArrayDecrypt(mesh);
        if (!arr || !eng.IsValidPointer(arr))
            continue;

        const int score = ScoreBoneArrayForMesh(eng, mesh, arr);
        if (score > best.score) {
            best.array = arr;
            best.mesh = mesh;
            best.score = score;
        }
    }

    return best;
}

uintptr_t Engine::ResolveBoneArray(uintptr_t actor, uintptr_t primaryMesh, uintptr_t* outBoneMesh)
{
    const BoneArrayCandidate best = FindBestBoneArray(actor, primaryMesh);
    if (outBoneMesh)
        *outBoneMesh = best.mesh;
    return best.score >= 2 ? best.array : 0;
}

Vector3 Engine::GetBone(
    int boneIndex,
    uintptr_t boneArray,
    FTransform componentToWorld
)
{
    if (!IsValidPointer(boneArray))
        return Vector3{};

    const FTransform bone =
        Memory::read<FTransform>(boneArray + (boneIndex * 0x60));

    const D3DMATRIX matrix = MatrixMultiplication(
        bone.ToMatrixWithScale(),
        componentToWorld.ToMatrixWithScale());

    return Vector3(matrix._41, matrix._42, matrix._43);
}

void Engine::GetBones(PlayerCacheEntry& actor)
{
    actor.boneData.valid.reset();
    actor.boneData.isVisible = false;
    actor.boneArray = 0;
    actor.boneMesh = 0;

    if (!IsValidPointer(actor.actorMesh))
        return;

    actor.boneArray = GetBoneArrayDecrypt(actor.actorMesh);
    if (!actor.boneArray || !IsValidPointer(actor.boneArray))
        return;

    actor.boneMesh = actor.actorMesh;

    const FTransform componentToWorld = Engine::ReadComponentToWorld(actor.actorMesh);

    for (const auto& [gameIndex, uniBone] : GameBoneMapArcRaiders)
    {
        const Vector3 worldPos = GetBone(gameIndex, actor.boneArray, componentToWorld);

        if (worldPos.x == 0.0 && worldPos.y == 0.0 && worldPos.z == 0.0)
            continue;

        Vector3 screenPos{};
        if (!ProjectWorldLocationToScreen(worldPos, screenPos))
            continue;

        const size_t idx = static_cast<size_t>(uniBone);
        actor.boneData.bonesDouble[idx] =
            Vector3{ screenPos.x, screenPos.y, 0.0 };
        actor.boneData.bonesWorldDouble[idx] = worldPos;
        actor.boneData.valid.set(idx);
    }

    if (actor.boneData.valid.test(static_cast<size_t>(UniBone::Head)) &&
        actor.boneData.valid.test(static_cast<size_t>(UniBone::Pelvis)))
    {
        actor.boneData.isVisible = true;
    }
}
