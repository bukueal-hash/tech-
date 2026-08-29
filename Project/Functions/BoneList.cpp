#include <Windows.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <immintrin.h>
#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Core/SteamDecrypt.hpp"
#include "../Core/AgentLog.h"
#include <chrono>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <string>

extern Engine engine;

bool Engine::IsValidPointer(uintptr_t ptr) const {
    return ptr >= 0x1000 && ptr < 0x7FFFFFFFFFFF;
}

bool Engine::IsUsermodePtr(uintptr_t ptr)
{
    return ptr > 0x10000 && ptr < 0x00007FFFFFFFFFFF;
}

namespace {

bool MeshHasEncryptedBoneBlock(uintptr_t mesh)
{
    if (!mesh)
        return false;
    __m128i enc780{};
    if (Memory::ReadRaw(mesh + Offsets::Encrypted, &enc780, sizeof(enc780))
        && _mm_cvtsi128_si64(enc780) != 0)
        return true;
    __m128i enc830{};
    return Memory::ReadRaw(mesh + Offsets::LodSelect, &enc830, sizeof(enc830))
        && _mm_cvtsi128_si64(enc830) != 0;
}

} // namespace

uintptr_t Engine::GetActorSkeletalMesh(uintptr_t actor) const
{
    if (!actor)
        return 0;
    // NOTE: 0x438 first. Do NOT reorder to EmbarkMesh-first — trace data shows
    // many constructive-pawn bots bind their render flag in the 0x438 slot,
    // and swapping here regressed them. Bot-specific LRTS slot selection lives
    // in ResolveBotVisMesh (RobotList.cpp), which probes both.
    uintptr_t mesh = Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
    if (mesh && IsValidPointer(mesh))
        return mesh;
    mesh = Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
    if (mesh && IsValidPointer(mesh))
        return mesh;
    return 0;
}

uintptr_t Engine::GetActorBoneMesh(uintptr_t actor)
{
    if (!actor)
        return 0;
    const uintptr_t embark =
        Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh);
    const uintptr_t skel =
        Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
    if (embark && IsValidPointer(embark) && MeshHasEncryptedBoneBlock(embark))
        return embark;
    if (skel && IsValidPointer(skel) && MeshHasEncryptedBoneBlock(skel))
        return skel;
    return GetActorSkeletalMesh(actor);
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

// Score using parent-chain accumulation for bone-space arrays.
static int ScoreBoneArrayAccumulated(
    Engine& eng,
    uintptr_t mesh,
    uintptr_t boneArray,
    const int* parentIndices,
    int maxBone)
{
    if (!mesh || !boneArray || !eng.IsValidPointer(boneArray))
        return 0;

    const FTransform ctw = Engine::ReadComponentToWorld(mesh);
    const D3DMATRIX ctwMat = ctw.ToMatrixWithScale();

    // Read all bone transforms.
    FTransform boneTransforms[97] = {};
    D3DMATRIX csMats[97] = {};
    for (int i = 0; i < maxBone; ++i) {
        boneTransforms[i] = Memory::read<FTransform>(
            boneArray + static_cast<uintptr_t>(i) * Bones::BoneStride);
        const D3DMATRIX local = boneTransforms[i].ToMatrixWithScale();
        const int parent = parentIndices[i];
        if (parent >= 0 && parent < maxBone && i > parent)
            csMats[i] = engine.MatrixMultiplication(local, csMats[parent]);
        else
            csMats[i] = local;
    }

    int score = 0;
    bool hasHead = false;
    bool hasPelvis = false;

    for (const auto& [gameIndex, uniBone] : eng.GameBoneMapArcRaiders) {
        if (gameIndex < 0 || gameIndex >= maxBone)
            continue;
        const D3DMATRIX w = engine.MatrixMultiplication(csMats[gameIndex], ctwMat);
        const Vector3 worldPos(w._41, w._42, w._43);
        if (!IsPlausibleWorldPos(worldPos) || IsNearZero(worldPos))
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
    bool accumulated = false; // true if parent-chain accumulation was used
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

        // Path 1: SIMD decrypt (old approach — fast, component-space arrays)
        const uintptr_t arr = steam_decrypt::GetBoneArrayDecrypt(mesh);
        if (arr && eng.IsValidPointer(arr)) {
            const int score = ScoreBoneArrayForMesh(eng, mesh, arr);
            if (score > best.score) {
                best = { arr, mesh, score, false };
            }
        }
    }

    // If decrypt didn't find a good array, try plaintext component-space
    // arrays on each mesh. These are the CachedComponentSpaceTransforms /
    // CachedBoneSpaceTransforms TArrays on the mesh.
    if (best.score < 2) {
        constexpr std::ptrdiff_t kArraySlots[] = {
            0x0B00, 0x0B80, 0x0B90, 0x0970, 0x0980
        };
        // Parent indices for the 22 drawn bones (standard UE humanoid).
        static const int kParentIndices[97] = {
            /*  0 Root */      -1,
            /*  1 Pelvis */     0,
            /*  2 Spine01 */    1,
            /*  3 Spine02 */    2,
            /*  4 Spine03 */    3,
            /*  5 Chest */      4,
            /*  6 Neck */       5,
            /*  7 Head */       6,
            /*  8 L_Clavicle */ 5,
            /*  9 L_UpperArm */ 8,
            /* 10 L_Forearm */  9,
            /* 11 L_Hand */    10,
            /* 12-41 */        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            /* 42 R_Clavicle */ 5,
            /* 43 R_UpperArm */ 42,
            /* 44 R_Forearm */  43,
            /* 45 R_Hand */    44,
            /* 46-64 */        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
            /* 65 L_Thigh */    1,
            /* 66 L_Calf */    65,
            /* 67 L_Foot */    66,
            /* 68 */           -1,
            /* 69 R_Thigh */    1,
            /* 70 R_Calf */    69,
            /* 71 R_Foot */    70,
            /* 72-96 */        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
        };

        for (uintptr_t mesh : meshes) {
            if (!mesh || !eng.IsValidPointer(mesh))
                continue;
            for (std::ptrdiff_t slot : kArraySlots) {
                const uintptr_t arr = Memory::read<uintptr_t>(mesh + slot);
                if (!arr || !eng.IsValidPointer(arr))
                    continue;
                const int count = Memory::read<int>(mesh + slot + 8);
                if (count <= 0 || count > Bones::MaxBoneCount)
                    continue;

                // Try direct (component-space) scoring first.
                int score = ScoreBoneArrayForMesh(eng, mesh, arr);
                if (score > best.score) {
                    best = { arr, mesh, score, false };
                }

                // If direct scoring fails, try accumulated (bone-space).
                if (score < 2) {
                    score = ScoreBoneArrayAccumulated(
                        eng, mesh, arr, kParentIndices, 97);
                    if (score > best.score) {
                        best = { arr, mesh, score, true };
                    }
                }
            }
        }
    }

    return best;
}

uintptr_t Engine::ResolveBoneArray(
    uintptr_t actor, uintptr_t primaryMesh,
    uintptr_t* outBoneMesh, std::ptrdiff_t* outCtwOffset,
    std::ptrdiff_t* outTransOff)
{
    const BoneArrayCandidate best = FindBestBoneArray(actor, primaryMesh);
    if (outBoneMesh)
        *outBoneMesh = best.mesh;
    if (outCtwOffset)
        *outCtwOffset = Offsets::ComponentToWorld;
    if (outTransOff)
        *outTransOff = 0x20;

    // Verify tap (throttled 15 s): which array won and how it was scored.
    if (best.score >= 2) {
        static std::chrono::steady_clock::time_point s_lastTrace{};
        const auto bNow = std::chrono::steady_clock::now();
        if (bNow - s_lastTrace >= std::chrono::seconds(15)) {
            s_lastTrace = bNow;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::ofstream bf(kArcVerifyPath, std::ios::app);
            if (bf) {
                bf << "{\"sessionId\":\"c190fb\",\"runId\":\"verify\","
                   << "\"location\":\"BoneList.cpp\",\"message\":\"bone_resolve\"," 
                   << "\"data\":{\"score\":" << best.score
                   << ",\"accum\":" << (best.accumulated ? 1 : 0)
                   << ",\"array\":0x" << std::hex << best.array
                   << ",\"mesh\":0x" << best.mesh << std::dec
                   << "},\"timestamp\":" << ms << "}\n";
            }
        }
    }
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
    // Stamp at read start: bone age at draw time = Present - stamp, which
    // includes the read itself (the DMA time IS part of the lag).
    actor.boneData.readStampMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
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
    std::ptrdiff_t ctwOffset = Offsets::ComponentToWorld;
    std::ptrdiff_t transOff = 0x20;
    actor.boneArray = ResolveBoneArray(
        actor.APawn, meshForBones, &resolvedMesh, &ctwOffset, &transOff);
    if (!actor.boneArray || !IsValidPointer(actor.boneArray))
        return;

    actor.boneMesh = resolvedMesh ? resolvedMesh : meshForBones;
    actor.actorMesh = actor.boneMesh;

    const size_t boneCount = GameBoneMapArcRaiders.size();

    // Check if the winning array was scored via accumulation (bone-space).
    // If so, use the DP accumulation path. Otherwise use the fast direct path.
    FTransform componentToWorld = Engine::ReadComponentToWorld(actor.boneMesh);

    // Parent indices for accumulation (only used for drawn bones).
    static const int kParentIdx[97] = {
        -1,0,1,2,3,4,5,6,5,8,9,10,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,5,42,43,44,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,1,65,66,-1,1,69,70,
        71,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1
    };

    // Batched bone read — only the 22 drawn bones (old approach).
    constexpr size_t kMaxDrawn = 32;
    FTransform boneTransforms[kMaxDrawn] = {};
    const size_t drawCount = (boneCount <= kMaxDrawn) ? boneCount : kMaxDrawn;
    {
        ScatterSession scatter;
        if (scatter.isValid()) {
            bool ok = true;
            for (size_t i = 0; i < boneCount && ok; ++i) {
                const int gameIndex = GameBoneMapArcRaiders[i].first;
                ok = scatter.prepare(
                    actor.boneArray + (gameIndex * 0x60),
                    &boneTransforms[i], sizeof(FTransform));
            }
            if (!ok || !scatter.execute()) {
                for (size_t i = 0; i < boneCount; ++i) {
                    const int gameIndex = GameBoneMapArcRaiders[i].first;
                    boneTransforms[i] = Memory::read<FTransform>(
                        actor.boneArray + (gameIndex * 0x60));
                }
            }
        } else {
            for (size_t i = 0; i < boneCount; ++i) {
                const int gameIndex = GameBoneMapArcRaiders[i].first;
                boneTransforms[i] = Memory::read<FTransform>(
                    actor.boneArray + (gameIndex * 0x60));
            }
        }
    }

    // Accumulate bone-space → component-space via parent chain.
    // Only accumulate bones we actually draw.
    D3DMATRIX csMats[97] = {};
    {
        // Build a map from gameIndex → boneTransforms position.
        int idxMap[97] = {};
        for (int i = 0; i < 97; ++i) idxMap[i] = -1;
        for (size_t i = 0; i < boneCount; ++i)
            idxMap[GameBoneMapArcRaiders[i].first] = static_cast<int>(i);
        // Accumulate: walk the parent chain for each drawn bone, reading
        // only the bones we have (scattered reads).
        for (size_t i = 0; i < boneCount; ++i) {
            const int gi = GameBoneMapArcRaiders[i].first;
            if (gi < 0 || gi >= 97 || csMats[gi]._44 != 0.0f) continue; // already done
            // Walk chain rootward, collecting unbuilt bones.
            int chain[32];
            int depth = 0;
            int cur = gi;
            while (cur >= 0 && cur < 97 && depth < 32 && csMats[cur]._44 == 0.0f) {
                chain[depth++] = cur;
                cur = kParentIdx[cur];
            }
            // Build from root to leaf.
            for (int d = depth - 1; d >= 0; --d) {
                const int bi = chain[d];
                const int mi = idxMap[bi];
                const D3DMATRIX local = (mi >= 0)
                    ? boneTransforms[mi].ToMatrixWithScale()
                    : Memory::read<FTransform>(
                        actor.boneArray + static_cast<uintptr_t>(bi) * Bones::BoneStride
                      ).ToMatrixWithScale();
                const int parent = kParentIdx[bi];
                if (parent >= 0 && parent < 97 && csMats[parent]._44 != 0.0f)
                    csMats[bi] = engine.MatrixMultiplication(local, csMats[parent]);
                else
                    csMats[bi] = local;
            }
        }
    }

    // Check if accumulated result is upside-down by comparing head vs pelvis
    // in component space (before CTW). Head Z should be > pelvis Z.
    const D3DMATRIX ctwMatrix = componentToWorld.ToMatrixWithScale();
    {
        const D3DMATRIX headW = engine.MatrixMultiplication(csMats[7], ctwMatrix);
        const D3DMATRIX pelW = engine.MatrixMultiplication(csMats[1], ctwMatrix);
        if (headW._43 < pelW._43) {
            // Upside down — use CTW-translation-only.
            D3DMATRIX ctwT = {};
            ctwT._11 = ctwT._22 = ctwT._33 = ctwT._44 = 1.0f;
            ctwT._41 = ctwMatrix._41;
            ctwT._42 = ctwMatrix._42;
            ctwT._43 = ctwMatrix._43;
            for (const auto& [gameIndex, uniBone] : GameBoneMapArcRaiders) {
                if (gameIndex < 0 || gameIndex >= 97) continue;
                const D3DMATRIX w = engine.MatrixMultiplication(csMats[gameIndex], ctwT);
                const Vector3 wp(w._41, w._42, w._43);
                if (!IsNearZero(wp) && IsPlausibleWorldPos(wp)) {
                    actor.boneData.bonesWorldDouble[static_cast<size_t>(uniBone)] = wp;
                    actor.boneData.valid.set(static_cast<size_t>(uniBone));
                }
            }
        } else {
            // Normal — use full CTW.
            for (const auto& [gameIndex, uniBone] : GameBoneMapArcRaiders) {
                if (gameIndex < 0 || gameIndex >= 97) continue;
                const D3DMATRIX w = engine.MatrixMultiplication(csMats[gameIndex], ctwMatrix);
                const Vector3 wp(w._41, w._42, w._43);
                if (!IsNearZero(wp) && IsPlausibleWorldPos(wp)) {
                    actor.boneData.bonesWorldDouble[static_cast<size_t>(uniBone)] = wp;
                    actor.boneData.valid.set(static_cast<size_t>(uniBone));
                }
            }
        }
    }

    if (actor.boneData.valid.test(static_cast<size_t>(UniBone::Head)) &&
        (actor.boneData.valid.test(static_cast<size_t>(UniBone::Pelvis)) ||
         actor.boneData.valid.test(static_cast<size_t>(UniBone::Chest))))
    {
        actor.boneData.isVisible = true;
    }

    // Alignment probe (1 Hz): box anchor vs bone anchors while moving.
    // Comparing ESP-box source (root/capsule) against skeleton source (mesh
    // CTW) over time separates a CONSTANT mesh↔capsule offset from an
    // OSCILLATING misalignment while the target runs.
    {
        static std::chrono::steady_clock::time_point s_lastAlign{};
        static uint64_t s_lastKey = 0;
        static Vector3 s_lastBox{};
        const auto aNow = std::chrono::steady_clock::now();
        if (aNow - s_lastAlign >= std::chrono::seconds(1)
            && actor.boneData.valid.test(static_cast<size_t>(UniBone::Head))
            && actor.boneData.valid.test(static_cast<size_t>(UniBone::Pelvis))) {
            s_lastAlign = aNow;
            const Vector3& pelvis =
                actor.boneData.bonesWorldDouble[static_cast<size_t>(UniBone::Pelvis)];
            const Vector3& headB =
                actor.boneData.bonesWorldDouble[static_cast<size_t>(UniBone::Head)];
            bool moving = false;
            if (s_lastKey == static_cast<uint64_t>(actor.APawn)) {
                const float dx = static_cast<float>(actor.WorldPos.x - s_lastBox.x);
                const float dy = static_cast<float>(actor.WorldPos.y - s_lastBox.y);
                const float dz = static_cast<float>(actor.WorldPos.z - s_lastBox.z);
                moving = (dx * dx + dy * dy + dz * dz) > 100.0f; // >10 cm between samples
            }
            s_lastKey = static_cast<uint64_t>(actor.APawn);
            s_lastBox = actor.WorldPos;
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::ofstream af(kArcVerifyPath, std::ios::app);
            if (af) {
                af << "{\"sessionId\":\"c190fb\",\"runId\":\"verify\"," 
                   << "\"location\":\"BoneList.cpp\",\"message\":\"align_probe\"," 
                   << "\"data\":{\"key\":" << static_cast<uint64_t>(actor.APawn)
                   << ",\"mv\":" << (moving ? 1 : 0)
                   << ",\"box\":[" << actor.WorldPos.x << "," << actor.WorldPos.y
                   << "," << actor.WorldPos.z << "]"
                   << ",\"pelv\":[" << pelvis.x << "," << pelvis.y << "," << pelvis.z << "]"
                   << ",\"headB\":[" << headB.x << "," << headB.y << "," << headB.z << "]"
                   << ",\"dPz\":" << (pelvis.z - actor.WorldPos.z)
                   << ",\"dHz\":" << (headB.z - actor.head.z)
                   << "},\"timestamp\":" << ms << "}\n";
            }
        }
    }

}
