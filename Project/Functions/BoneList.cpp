#include <Windows.h>
#include <vector>
#include <cmath>
#include <algorithm>

#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/SteamDecrypt.hpp"

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
        const Vector3 world = eng.GetBone(gameIndex, boneArray, ctw);
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

    // Embark mesh usually owns the encrypted bone block on this build.
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

    uintptr_t meshForBones = GetActorBoneMesh(actor.APawn);
    if (!meshForBones || !IsValidPointer(meshForBones))
        meshForBones = actor.actorMesh;
    if (!meshForBones || !IsValidPointer(meshForBones))
        return;

    uintptr_t resolvedMesh = 0;
    actor.boneArray = ResolveBoneArray(actor.APawn, meshForBones, &resolvedMesh);
    if (!actor.boneArray || !IsValidPointer(actor.boneArray))
        return;

    actor.boneMesh = resolvedMesh ? resolvedMesh : meshForBones;
    actor.actorMesh = actor.boneMesh;

    const size_t boneCount = GameBoneMapArcRaiders.size();

    FTransform componentToWorld{};
    std::vector<FTransform> boneTransforms(boneCount);

    bool batched = false;
    {
        ScatterSession scatter;
        if (scatter.isValid()) {
            bool ok = scatter.prepare(
                actor.boneMesh + Offsets::ComponentToWorld,
                &componentToWorld, sizeof(FTransform));
            for (size_t i = 0; i < boneCount && ok; ++i) {
                const int gameIndex = GameBoneMapArcRaiders[i].first;
                ok = scatter.prepare(
                    actor.boneArray + (gameIndex * 0x60),
                    &boneTransforms[i], sizeof(FTransform));
            }
            if (ok && scatter.execute())
                batched = true;
        }
    }

    if (!batched) {
        componentToWorld = Engine::ReadComponentToWorld(actor.boneMesh);
        for (size_t i = 0; i < boneCount; ++i) {
            const int gameIndex = GameBoneMapArcRaiders[i].first;
            boneTransforms[i] = Memory::read<FTransform>(
                actor.boneArray + (gameIndex * 0x60));
        }
    } else {
        // Prefer a fresh CompToWorld — scatter CTW can race mesh motion.
        componentToWorld = Engine::ReadComponentToWorld(actor.boneMesh);
    }

    const D3DMATRIX ctwMatrix = componentToWorld.ToMatrixWithScale();

    for (size_t i = 0; i < boneCount; ++i) {
        const UniBone uniBone = GameBoneMapArcRaiders[i].second;
        const D3DMATRIX matrix = MatrixMultiplication(
            boneTransforms[i].ToMatrixWithScale(), ctwMatrix);
        const Vector3 worldPos(matrix._41, matrix._42, matrix._43);

        if (IsNearZero(worldPos))
            continue;
        if (!IsPlausibleWorldPos(worldPos))
            continue;

        const size_t idx = static_cast<size_t>(uniBone);
        actor.boneData.bonesWorldDouble[idx] = worldPos;
        actor.boneData.valid.set(idx);
    }

    if (actor.boneData.valid.test(static_cast<size_t>(UniBone::Head)) &&
        (actor.boneData.valid.test(static_cast<size_t>(UniBone::Pelvis)) ||
         actor.boneData.valid.test(static_cast<size_t>(UniBone::Chest))))
    {
        actor.boneData.isVisible = true;
    }
}
