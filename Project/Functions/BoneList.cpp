#include <Windows.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <immintrin.h>
#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Core/SteamDecrypt.hpp"
#include "../Core/BoneRoster.hpp"
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

    for (const auto& [gameIndex, uniBone] : BoneRoster::GameBoneMap()) {
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

    // Read all bone transforms. Sized to the parent table — the calibrated
    // roster spans the full skeleton, not just the 97 legacy slots.
    std::vector<FTransform> boneTransforms((maxBone > 0) ? maxBone : 1);
    std::vector<D3DMATRIX> csMats((maxBone > 0) ? maxBone : 1);
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

    for (const auto& [gameIndex, uniBone] : BoneRoster::GameBoneMap()) {
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

                // If direct scoring fails, try accumulated (bone-space) —
                // with the calibrated parent chain when one is proven.
                if (score < 2) {
                    const std::vector<int32_t>* calParents =
                        BoneRoster::GameBoneParents();
                    score = ScoreBoneArrayAccumulated(
                        eng, mesh, arr,
                        calParents ? calParents->data() : kParentIndices,
                        calParents ? static_cast<int>(calParents->size()) : 97);
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
    std::ptrdiff_t* outTransOff, bool* outBoneSpace)
{
    const BoneArrayCandidate best = FindBestBoneArray(actor, primaryMesh);
    if (outBoneMesh)
        *outBoneMesh = best.mesh;
    if (outBoneSpace)
        *outBoneSpace = best.accumulated;
    // Report the same block the readers use: the probe picks between the
    // live-pinned ComponentToWorld and its derived fallback per mesh.
    if (outCtwOffset)
        *outCtwOffset = Engine::ProbeComponentToWorldOffset(best.mesh);
    if (outTransOff)
        *outTransOff = Offsets::Transform_Translation;

    // Bind gameIndex -> body part by bone NAME (patch-proof). Before a roster
    // is proven the static CL-1233465 table stays in effect.
    if (best.score >= 2 && best.mesh)
        BoneRoster::TryCalibrate(best.mesh, Memory::getBaseAddress());

    // One-time roster tap: which name won each body part.
    if (BoneRoster::IsCalibrated()) {
        static std::atomic<bool> s_rosterLogged{ false };
        if (!s_rosterLogged.exchange(true)) {
            const auto rMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::ofstream rf(kArcVerifyPath, std::ios::app);
            if (rf) {
                rf << "{\"sessionId\":\"c190fb\",\"runId\":\"verify\","
                   << "\"location\":\"BoneList.cpp\",\"message\":\"bone_roster\","
                   << "\"data\":{\"map\":\"";
                const auto& rm = BoneRoster::GameBoneMap();
                for (size_t ri = 0; ri < rm.size(); ++ri) {
                    if (ri) rf << ',';
                    rf << rm[ri].first << ':' << BoneRoster::UniBoneAbbr(rm[ri].second);
                }
                rf << "\"},\"timestamp\":" << rMs << "}\n";
            }
        }
    }

    // bone_roster_fail tap (throttled 15 s, only while uncalibrated): names
    // the exact stage that keeps rejecting the live skeleton — 136 probe
    // attempts with 0 installs were previously silent.
    if (!BoneRoster::IsCalibrated()) {
        static std::chrono::steady_clock::time_point s_lastFailTrace{};
        const auto fNow = std::chrono::steady_clock::now();
        if (fNow - s_lastFailTrace >= std::chrono::seconds(15)) {
            s_lastFailTrace = fNow;
            const auto& d = BoneRoster::Diag();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::ofstream f(kArcVerifyPath, std::ios::app);
            if (f) {
                f << "{\"sessionId\":\"c190fb\",\"runId\":\"verify\","
                  << "\"location\":\"BoneList.cpp\",\"message\":\"bone_roster_fail\","
                  << "\"data\":{\"stage\":" << d.stage.load()
                  << ",\"skelTried\":" << d.skelTried.load()
                  << ",\"hdrOff\":" << d.hdrOff.load()
                  << ",\"count\":" << d.count.load()
                  << ",\"resolved\":" << d.resolved.load()
                  << ",\"roles\":\"0x" << std::hex << d.roles.load() << std::dec << "\""
                  << "},\"timestamp\":" << ms << "}\n";
            }
        }
    }

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
                   << ",\"roster\":" << (BoneRoster::IsCalibrated() ? 1 : 0)
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
    bool boneSpaceArray = false;
    actor.boneArray = ResolveBoneArray(
        actor.APawn, meshForBones, &resolvedMesh, &ctwOffset, &transOff,
        &boneSpaceArray);
    if (!actor.boneArray || !IsValidPointer(actor.boneArray))
        return;

    actor.boneMesh = resolvedMesh ? resolvedMesh : meshForBones;
    actor.actorMesh = actor.boneMesh;

    // Active roster: name-calibrated when proven (Core/BoneRoster.hpp), the
    // static CL-1233465 table otherwise. Bound once so the whole read uses one
    // consistent view.
    const auto& boneMap = BoneRoster::GameBoneMap();
    const std::vector<int32_t>* calParents = BoneRoster::GameBoneParents();
    const size_t boneCount = boneMap.size();

    // Slots are addressed by GAME bone index; the calibrated roster spans the
    // full skeleton, not just the 97 legacy slots.
    int nSlots = 97;
    for (const auto& entry : boneMap)
        if (entry.first + 1 > nSlots)
            nSlots = entry.first + 1;
    if (calParents && static_cast<int>(calParents->size()) > nSlots)
        nSlots = static_cast<int>(calParents->size());

    // The winning array was scored either way: component-space transforms
    // read directly, or bone-space transforms accumulated up the parent
    // chain. Honor that choice below - they are not interchangeable.
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
    auto parentOf = [&](int gi) -> int {
        if (calParents && gi >= 0 && gi < static_cast<int>(calParents->size()))
            return (*calParents)[gi];
        return (gi >= 0 && gi < 97) ? kParentIdx[gi] : -1;
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
                const int gameIndex = boneMap[i].first;
                ok = scatter.prepare(
                    actor.boneArray + (gameIndex * 0x60),
                    &boneTransforms[i], sizeof(FTransform));
            }
            if (!ok || !scatter.execute()) {
                for (size_t i = 0; i < boneCount; ++i) {
                    const int gameIndex = boneMap[i].first;
                    boneTransforms[i] = Memory::read<FTransform>(
                        actor.boneArray + (gameIndex * 0x60));
                }
            }
        } else {
            for (size_t i = 0; i < boneCount; ++i) {
                const int gameIndex = boneMap[i].first;
                boneTransforms[i] = Memory::read<FTransform>(
                    actor.boneArray + (gameIndex * 0x60));
            }
        }
    }

    std::vector<D3DMATRIX> csMats(static_cast<size_t>(nSlots));
    if (!boneSpaceArray) {
        // Fast direct path: the array is component-space, so every transform
        // is already relative to the mesh and is used as read. Walking the
        // parent chain over component-space transforms folds the pose into a
        // crumple - each chain step re-applies rotations it already contains.
        for (size_t i = 0; i < boneCount; ++i) {
            const int gi = boneMap[i].first;
            if (gi >= 0 && gi < nSlots)
                csMats[gi] = boneTransforms[i].ToMatrixWithScale();
        }
    } else {
        // Accumulate bone-space → component-space via parent chain.
        // Only accumulate bones we actually draw.
        // Build a map from gameIndex → boneTransforms position.
        std::vector<int> idxMap(static_cast<size_t>(nSlots), -1);
        for (size_t i = 0; i < boneCount; ++i)
            idxMap[boneMap[i].first] = static_cast<int>(i);
        // Accumulate: walk the parent chain for each drawn bone, reading
        // only the bones we have (scattered reads).
        for (size_t i = 0; i < boneCount; ++i) {
            const int gi = boneMap[i].first;
            if (gi < 0 || gi >= nSlots || csMats[gi]._44 != 0.0f) continue; // already done
            // Walk chain rootward, collecting unbuilt bones.
            std::vector<int> chain;
            int cur = gi;
            while (cur >= 0 && cur < nSlots && chain.size() < 64 && csMats[cur]._44 == 0.0f) {
                chain.push_back(cur);
                cur = parentOf(cur);
            }
            // Build from root to leaf.
            for (int d = static_cast<int>(chain.size()) - 1; d >= 0; --d) {
                const int bi = chain[static_cast<size_t>(d)];
                const int mi = idxMap[bi];
                const D3DMATRIX local = (mi >= 0)
                    ? boneTransforms[mi].ToMatrixWithScale()
                    : Memory::read<FTransform>(
                        actor.boneArray + static_cast<uintptr_t>(bi) * Bones::BoneStride
                      ).ToMatrixWithScale();
                const int parent = parentOf(bi);
                if (parent >= 0 && parent < nSlots && csMats[parent]._44 != 0.0f)
                    csMats[bi] = engine.MatrixMultiplication(local, csMats[parent]);
                else
                    csMats[bi] = local;
            }
        }
    }

    // Component-space → world with the full CTW. (An old "upside-down" check
    // swapped in a rotation-less CTW whenever head Z fell below pelvis Z;
    // with the array semantics honored above that only misfires on prone or
    // crawling players and wrecks their pose, so the transform is now
    // unconditional.)
    const D3DMATRIX ctwMatrix = componentToWorld.ToMatrixWithScale();
    for (const auto& [gameIndex, uniBone] : boneMap) {
        if (gameIndex < 0 || gameIndex >= nSlots) continue;
        const D3DMATRIX w = engine.MatrixMultiplication(csMats[gameIndex], ctwMatrix);
        const Vector3 wp(w._41, w._42, w._43);
        if (!IsNearZero(wp) && IsPlausibleWorldPos(wp)) {
            actor.boneData.bonesWorldDouble[static_cast<size_t>(uniBone)] = wp;
            actor.boneData.valid.set(static_cast<size_t>(uniBone));
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
                   << ",\"dHz\":" << (headB.z - actor.WorldPos.z)
                   << ",\"hpz\":" << (headB.z - pelvis.z)
                   << "},\"timestamp\":" << ms << "}\n";
            }
        }
    }

}
