#include <Windows.h>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <immintrin.h>
#include "../Core/Engine.h"
#include "../Core/Memory.h"
#include "../Core/Offsets.h"
#include "../Core/SteamDecrypt.hpp"
#include "../Core/BoneRoster.hpp"
#include "../Core/AgentLog.h"
#include "../Core/EntityDiagnostics.hpp"
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
    ReadBudget::DefaultPurposeScope purpose(ReadBudget::Purpose::Link);
    if (!actor)
        return 0;
    // NOTE: Character.Mesh (0x450) first. Do NOT reorder to EmbarkMesh-first —
    // trace data shows many constructive-pawn bots bind their render flag in the
    // character-mesh slot, and swapping here regressed them. Probe the Character
    // mesh first, then EmbarkMesh.
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
    double headZ = 0.0, pelvZ = 0.0;
    bool hasHead = false;
    bool hasPelvis = false;

    for (const auto& [gameIndex, uniBone] : BoneRoster::GameBoneMap()) {
        const Vector3 world = eng.GetBone(gameIndex, boneArray, ctw);
        if (!IsPlausibleWorldPos(world) || IsNearZero(world))
            continue;
        ++score;
        if (uniBone == UniBone::Head) { hasHead = true; headZ = world.z; }
        if (uniBone == UniBone::Pelvis) { hasPelvis = true; pelvZ = world.z; }
    }

    // A standing/walking humanoid's head sits above its pelvis. Direct scoring
    // of a bone-space (local-to-parent) array scatters bones away from the
    // body, so this rejects it and lets the accumulated parent-chain pass win.
    if (!hasHead || !hasPelvis || !(headZ > pelvZ))
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
    double headZ = 0.0, pelvZ = 0.0;
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
        if (uniBone == UniBone::Head) { hasHead = true; headZ = worldPos.z; }
        if (uniBone == UniBone::Pelvis) { hasPelvis = true; pelvZ = worldPos.z; }
    }

    if (!hasHead || !hasPelvis || !(headZ > pelvZ))
        return 0;
    return score;
}

struct BoneArrayCandidate {
    uintptr_t array = 0;
    uintptr_t mesh = 0;
    int score = 0;
    bool accumulated = false; // true if parent-chain accumulation was used
};

// Bone-array decrypt probe (throttled 10 s): names the exact stage that fails
// on the live build — encrypted-seed present, SDK/legacy table base, LOD/selector
// descriptor, array+count, and the plaintext component-space slots. Without this
// the bone pipeline bails silently and only bone_roster_fail (score<2, so
// TryCalibrate never runs) is visible.
static void ProbeBoneArray(uintptr_t actor, uintptr_t meshA, uintptr_t meshB,
                           uintptr_t primaryMesh, int bestScore)
{
    static std::atomic<uint64_t> s_lastMs{ 0 };
    static std::atomic<uint64_t> s_lastHealthMs{ 0 };
    static std::atomic<uint64_t> s_calls{ 0 };
    static std::atomic<uint64_t> s_sane{ 0 };
    const uint64_t nowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());

    // Sanity gate (cheap read, BEFORE any throttle consume): props and other
    // non-characters can expose meshA=0x1 or a denormal CTW. Never let those
    // starve a real actor out of the probe window.
    s_calls.fetch_add(1, std::memory_order_relaxed);
    Engine::FVector3d cSan{};
    bool sane = false;
    uintptr_t saneMesh = 0;
    {
        const uintptr_t gateMeshes[] = { meshB, primaryMesh, meshA };
        for (uintptr_t gm : gateMeshes) {
            if (!gm || !engine.IsValidPointer(gm))
                continue;
            const Engine::FVector3d c =
                Memory::read<Engine::FVector3d>(gm + Offsets::WorldLocation);
            if (std::isfinite(c.x) && std::isfinite(c.y) &&
                std::isfinite(c.z) &&
                std::fabs(c.x) < 1e7 && std::fabs(c.y) < 1e7 &&
                std::fabs(c.z) < 1e7 &&
                (std::fabs(c.x) + std::fabs(c.y) + std::fabs(c.z)) > 1.0) {
                sane = true;
                saneMesh = gm;
                cSan = c;
                break;
            }
        }
    }
    if (sane)
        s_sane.fetch_add(1, std::memory_order_relaxed);

    const uintptr_t probeMesh = saneMesh ? saneMesh : meshA;

    const auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Health line: once per 10s regardless of sanity, so we can prove whether a
    // real actor mesh is ever offered to the probe.
    {
        uint64_t hp = s_lastHealthMs.load(std::memory_order_relaxed);
        if (nowMs - hp >= 10000 &&
            s_lastHealthMs.compare_exchange_strong(hp, nowMs)) {
            std::ostringstream oss;
            oss << "{\"sessionId\":\"c190fb\",\"runId\":\"bones\","
               << "\"location\":\"BoneList.cpp\","
               << "\"message\":\"bone_probe_health\",\"data\":{"
               << "\"calls\":" << s_calls.load(std::memory_order_relaxed)
               << ",\"sane\":" << s_sane.load(std::memory_order_relaxed)
               << ",\"saneNow\":" << (sane ? 1 : 0)
               << ",\"actor\":0x" << std::hex << actor
               << ",\"meshA\":0x" << meshA
               << ",\"saneMesh\":0x" << saneMesh
               << std::dec
               << ",\"ctw\":[" << cSan.x << "," << cSan.y << ","
               << cSan.z << "]},\"timestamp\":" << wallMs << "}\n";
            EntityDiagnostics::LogRaw(oss.str());
        }
    }

    if (!sane)
        return;

    uint64_t prev = s_lastMs.load(std::memory_order_relaxed);
    if (nowMs - prev < 10000)
        return;
    if (!s_lastMs.compare_exchange_strong(prev, nowMs))
        return;

    std::ostringstream f;
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"bones\","
      << "\"location\":\"BoneList.cpp\",\"message\":\"bone_array_probe\","
      << "\"data\":{\"actor\":0x" << std::hex << actor
      << ",\"meshA\":0x" << meshA
      << ",\"meshB\":0x" << meshB
      << ",\"primary\":0x" << primaryMesh
      << std::dec << ",\"bestScore\":" << bestScore;

    // Per-mesh detail so ONE probe run names every stage: whether the mesh is
    // a real scene component (cached vs nocache CTW), whether the encrypted
    // seed/decrypt path yields a plausible base, and which plaintext slots
    // (incl. the dump-authoritative 0xBE8/0xBF8) hold a sane TArray.
    // 0x0B98 is the plain LocalPose TArray header named by the October 13
    // skeleton reference: Data at +0x00, Count at +0x08, Capacity at +0x0C,
    // 0x60-byte plain double transforms, no decryptor. It is probed before any
    // pipeline change so the reference is checked against the live actor rather
    // than assumed -- the rest of this file reaches a bone array through the
    // encrypted block, and nothing here has ever read a plain pose array.
    static const std::ptrdiff_t kSlots[] = {
        0x0BF8, 0x0BE8, 0x0B00, 0x0B80, 0x0B90, 0x0B98, 0x0970, 0x0980
    };
    const uintptr_t order[] = { meshA, meshB, primaryMesh };
    f << ",\"meshes\":[";
    bool mFirst = true;
    for (uintptr_t mesh : order) {
        if (!mesh || !engine.IsValidPointer(mesh))
            continue;
        if (!mFirst) f << ',';
        mFirst = false;

        alignas(16) uint8_t seed[0x20]{};
        steam_decrypt::MemRead(mesh + Offsets::Encrypted, seed, sizeof(seed));
        const uint64_t seedLo = *reinterpret_cast<const uint64_t*>(seed);
        const uint64_t seedHi = *reinterpret_cast<const uint64_t*>(seed + 8);
        char seedStr[40]{};
        std::snprintf(seedStr, sizeof(seedStr), "%016llx%016llx",
            static_cast<unsigned long long>(seedLo),
            static_cast<unsigned long long>(seedHi));

        const uint64_t sdkBase =
            game::gasm::decode_bonearray_table_address_slot(seed);
        const uint64_t legacyBase = Bones::DecryptBoneArrayPointer(seed);

        const uint32_t lodDword =
            steam_decrypt::MemReadVal<uint32_t>(mesh + Offsets::LodSelect);
        const uint32_t lodIdx =
            (lodDword >> Bones::LodShiftRight) & Bones::LodBitMask;
        const uint32_t selDword =
            steam_decrypt::MemReadVal<uint32_t>(mesh + Offsets::BoneV922SelectorOffset);
        const uint32_t selector =
            (selDword >> Offsets::BoneV922SelectorShift) & Offsets::BoneV922SelectorMask;

        // Cached vs nocache CTW: separates a broken NoCache read path from a
        // genuinely-zero mesh transform.
        const Engine::FVector3d ctwCached =
            Memory::read<Engine::FVector3d>(mesh + Offsets::WorldLocation);
        const Engine::FVector3d ctwNc =
            Memory::read_nocache<Engine::FVector3d>(mesh + Offsets::WorldLocation);
        const Engine::FVector3d rel =
            Memory::read<Engine::FVector3d>(mesh + Offsets::RelativeLocation);

        // LeaderPoseComponent (0x728): when a mesh follows a leader, its own
        // CachedComponentSpaceTransforms stays empty and the pose lives on the
        // leader. Also read SkinnedAsset (0x720) and bNoSkeletonUpdate (0xCC0).
        const uint64_t skinnedAsset = steam_decrypt::MemReadVal<uint64_t>(mesh + 0x720);
        const uint64_t leaderPose = steam_decrypt::MemReadVal<uint64_t>(mesh + 0x728);
        const uint32_t noSkel = steam_decrypt::MemReadVal<uint8_t>(mesh + 0xCC0);
        auto dumpSlotSet = [&](uintptr_t base) {
            f << '[';
            if (base && engine.IsValidPointer(base)) {
                bool sf = true;
                for (std::ptrdiff_t s : kSlots) {
                    const uint64_t p = steam_decrypt::MemReadVal<uint64_t>(base + s);
                    const int32_t c = steam_decrypt::MemReadVal<int32_t>(base + s + 8);
                    if (!sf) f << ',';
                    sf = false;
                    f << "{\"s\":0x" << std::hex << s
                      << ",\"p\":0x" << p << std::dec << ",\"c\":" << c << "}";
                }
            }
            f << ']';
        };

        f << "{\"p\":0x" << std::hex << mesh
          << ",\"enc\":" << (MeshHasEncryptedBoneBlock(mesh) ? 1 : 0)
          << ",\"asset\":0x" << skinnedAsset
          << ",\"leader\":0x" << leaderPose
          << std::dec << ",\"noSkel\":" << noSkel
          << ",\"seed\":\"" << seedStr << "\""
          << ",\"sdkBase\":0x" << sdkBase
          << ",\"legacyBase\":0x" << legacyBase
          << ",\"lod\":0x" << lodDword
          << ",\"lodIdx\":0x" << lodIdx
          << ",\"sel\":0x" << selDword
          << std::dec << ",\"selector\":" << selector
          << ",\"ctwCached\":[" << ctwCached.x << "," << ctwCached.y << "," << ctwCached.z << "]"
          << ",\"ctwNc\":[" << ctwNc.x << "," << ctwNc.y << "," << ctwNc.z << "]"
          << ",\"rel\":[" << rel.x << "," << rel.y << "," << rel.z << "]"
          << ",\"ctwOff\":0x" << std::hex << Engine::ProbeComponentToWorldOffset(mesh) << std::dec
          << ",\"slots\":";
        dumpSlotSet(mesh);
        f << ",\"leaderSlots\":";
        dumpSlotSet(leaderPose);
        f << "}";
    }
    f << "]";

    // Runtime scan: locate the real plaintext TArray<FTransform> by validating
    // pointer + count + FTransform content (unit quaternion, finite
    // translation). Offset-agnostic, so it names the true slot even when the
    // encrypted-seed decrypt and the documented slots are both stale.
    int32_t boneArrCount = -1;
    f << ",\"scan\":[";
    {
        bool sFirst = true;
        const uintptr_t scanMeshes[] = { meshA, primaryMesh, meshB };
        for (uintptr_t mesh : scanMeshes) {
            if (!mesh || !engine.IsValidPointer(mesh))
                continue;
            for (std::ptrdiff_t off = 0x0; off <= 0x1400; off += 8) {
                const uintptr_t ptr =
                    steam_decrypt::MemReadVal<uint64_t>(mesh + off);
                if (ptr < 0x10000 || ptr >= 0x800000000000ULL)
                    continue;
                const int32_t count =
                    steam_decrypt::MemReadVal<int32_t>(mesh + off + 8);
                const int32_t maxc =
                    steam_decrypt::MemReadVal<int32_t>(mesh + off + 12);
                if (count < 20 || count > 300 || maxc < count || maxc > 4096)
                    continue;
                uint8_t raw[0x60 * 8]{};
                steam_decrypt::MemRead(ptr, raw, sizeof(raw));
                auto T = [&](int i, int c) {
                    return reinterpret_cast<const double*>(raw + i * 0x60)[c];
                };
                bool ok = true;
                for (int i = 0; i < 6 && ok; ++i) {
                    const double n2 = T(i,0)*T(i,0)+T(i,1)*T(i,1)+T(i,2)*T(i,2)+T(i,3)*T(i,3);
                    if (!(n2 > 0.90 && n2 < 1.10))
                        ok = false;
                    if (!(T(i,4) > -1e6 && T(i,4) < 1e6 &&
                          T(i,5) > -1e6 && T(i,5) < 1e6 &&
                          T(i,6) > -1e6 && T(i,6) < 1e6))
                        ok = false;
                }
                if (!ok)
                    continue;
                if (!sFirst) f << ',';
                sFirst = false;
                if (off == 0xb68)
                    boneArrCount = count;
                f << "{\"m\":0x" << std::hex << mesh
                  << ",\"off\":0x" << off << ",\"ptr\":0x" << ptr
                  << std::dec << ",\"c\":" << count << ",\"max\":" << maxc
                  << ",\"t0\":[" << T(0,4) << "," << T(0,5) << "," << T(0,6) << "]"
                  << ",\"t1\":[" << T(1,4) << "," << T(1,5) << "," << T(1,6) << "]"
                  << ",\"t2\":[" << T(2,4) << "," << T(2,5) << "," << T(2,6) << "]"
                  << ",\"t5\":[" << T(5,4) << "," << T(5,5) << "," << T(5,6) << "]"
                  << ",\"t7\":[" << T(7,4) << "," << T(7,5) << "," << T(7,6) << "]"
                  << ",\"s0\":[" << T(0,8) << "," << T(0,9) << "," << T(0,10) << "]";
                // Full profile: distinguishes component-space (absolute,
                // rising to ~character height) from bone-space (small local
                // offsets) and reveals the true bone order + struct layout.
                std::vector<uint8_t> full(static_cast<size_t>(count) * 0x60);
                steam_decrypt::MemRead(ptr, full.data(), full.size());
                f << ",\"z\":[";
                for (int i = 0; i < count && i < 97; ++i) {
                    const double zz =
                        reinterpret_cast<const double*>(full.data() + i * 0x60)[6];
                    if (i) f << ',';
                    f << static_cast<int>(std::lround(zz));
                }
                f << "],\"xyz\":[";
                for (int i = 0; i < count && i < 97; ++i) {
                    const double* tp =
                        reinterpret_cast<const double*>(full.data() + i * 0x60) + 4;
                    if (i) f << ',';
                    f << "[" << tp[0] << "," << tp[1] << "," << tp[2] << "]";
                }
                f << "],\"raw0\":\"";
                {
                    static const char* kHexD = "0123456789abcdef";
                    for (size_t b = 0; b < full.size() && b < 0x120; ++b)
                        f << kHexD[(full[b] >> 4) & 0xF] << kHexD[full[b] & 0xF];
                }
                f << "\"}";
            }
        }
    }
    f << "]";

    // Asset skeleton scan: locate FReferenceSkeleton::RawRefBoneInfo
    // (TArray<FMeshBoneInfo>, stride 0x0C) whose sibling RawRefBonePose
    // (TArray<FTransform>, stride 0x60) at +0x10 has the SAME count. Purely
    // structural (no name resolution) so it survives the dead name pipeline.
    // Dumps the full parent table so bone-space accumulation can be done.
    f << ",\"nscan\":[";
    {
        bool nFirst = true;
        const uint64_t asset = steam_decrypt::MemReadVal<uint64_t>(probeMesh + 0x720);
        if (asset >= 0x10000 && asset < 0x800000000000ULL) {
            for (std::ptrdiff_t off = 0x0; off <= 0x4000; off += 8) {
                const uintptr_t ptr =
                    steam_decrypt::MemReadVal<uint64_t>(asset + off);
                if (ptr < 0x10000 || ptr >= 0x800000000000ULL)
                    continue;
                const int32_t count =
                    steam_decrypt::MemReadVal<int32_t>(asset + off + 8);
                const int32_t maxc =
                    steam_decrypt::MemReadVal<int32_t>(asset + off + 12);
                if (count < 16 || count > 400 || maxc < count || maxc > 4096)
                    continue;
                // Sibling pose array at +0x10 must exist with the same count.
                const uintptr_t pptr =
                    steam_decrypt::MemReadVal<uint64_t>(asset + off + 0x10);
                const int32_t pcount =
                    steam_decrypt::MemReadVal<int32_t>(asset + off + 0x18);
                if (pptr < 0x10000 || pptr >= 0x800000000000ULL)
                    continue;
                if (pcount != count)
                    continue;
                uint8_t raw[0x0C * 8]{};
                steam_decrypt::MemRead(ptr, raw, sizeof(raw));
                if (!nFirst) f << ',';
                nFirst = false;
                f << "{\"off\":0x" << std::hex << off << ",\"ptr\":0x" << ptr
                  << ",\"pose\":0x" << pptr
                  << std::dec << ",\"c\":" << count << ",\"max\":" << maxc
                  << ",\"match\":" << (count == boneArrCount ? 1 : 0)
                  << ",\"e\":[";
                for (int i = 0; i < 8; ++i) {
                    const int32_t ni = *reinterpret_cast<const int32_t*>(raw + i * 0x0C);
                    const int32_t pi = *reinterpret_cast<const int32_t*>(raw + i * 0x0C + 8);
                    if (i) f << ',';
                    f << "{\"i\":" << ni << ",\"p\":" << pi << "}";
                }
                f << "],\"raw\":\"";
                {
                    static const char* kHexD = "0123456789abcdef";
                    for (int b = 0; b < 0x30; ++b)
                        f << kHexD[(raw[b] >> 4) & 0xF] << kHexD[raw[b] & 0xF];
                }
                f << "\"}";
                // Full parent table (one int per bone) so the parent chain can
                // be reconstructed exactly.
                std::vector<uint8_t> all(static_cast<size_t>(count) * 0x0C);
                steam_decrypt::MemRead(ptr, all.data(), all.size());
                f << "],\"par\":[";
                for (int i = 0; i < count; ++i) {
                    const int32_t pi =
                        *reinterpret_cast<const int32_t*>(all.data() + i * 0x0C + 8);
                    if (i) f << ',';
                    f << pi;
                }
                f << "]}";
            }
        }
    }
    f << "]";

    // Skeleton layout dump: the calibration found a header (hdrOff:1120,
    // count:80) but resolved:0 names. Dump the raw bytes at the found header
    // and the first few entries so the name-pipeline failure is visible.
    {
        const uintptr_t skelCands[] = { 0x720, 0x718, 0x740, 0x748, 0x730, 0xB10 };
        for (uintptr_t skel : skelCands) {
            if (!probeMesh || !engine.IsValidPointer(probeMesh))
                break;
            const uint64_t p = steam_decrypt::MemReadVal<uint64_t>(probeMesh + skel);
            if (p < 0x10000 || p >= 0x800000000000ULL)
                continue;

            // Scan for the header the calibration found (count=80, hdrOff=0x460)
            const uint64_t hdrOff = 0x460; // from bone_roster_fail
            const uint64_t hdrData = steam_decrypt::MemReadVal<uint64_t>(p + hdrOff);
            const int32_t  hdrCount = steam_decrypt::MemReadVal<int32_t>(p + hdrOff + 8);
            const int32_t  hdrMax   = steam_decrypt::MemReadVal<int32_t>(p + hdrOff + 12);

            f << ",\"skel\":{\"cand\":0x" << std::hex << skel
              << ",\"p\":0x" << p << std::dec
              << ",\"hdrOff\":0x" << std::hex << hdrOff
              << ",\"hdrData\":0x" << hdrData
              << std::dec << ",\"hdrCount\":" << hdrCount
              << ",\"hdrMax\":" << hdrMax;

            // Dump the TArray descriptor area around the found pose slot so the
            // sibling RawRefBoneInfo descriptor becomes visible.
            f << ",\"desc\":\"";
            {
                uint8_t d[0x80]{};
                steam_decrypt::MemRead(p + 0x440, d, sizeof(d));
                static const char* kHexD = "0123456789abcdef";
                for (size_t i = 0; i < sizeof(d); ++i)
                    f << kHexD[(d[i] >> 4) & 0xF] << kHexD[d[i] & 0xF];
            }
            f << "\"";

            // Dump first 0x60 bytes at hdrData (enough for 1 entry at stride
            // 0x18 or 4 entries at stride 0x0C)
            if (hdrData >= 0x10000 && hdrData < 0x800000000000ULL) {
                uint8_t raw[0x60]{};
                steam_decrypt::MemRead(hdrData, raw, sizeof(raw));
                f << ",\"raw\":\"";
                for (size_t i = 0; i < 0x60; ++i) {
                    f << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<int>(raw[i]);
                }
                f << std::dec << "\"";

                // Try resolving the first 3 name indices with the actual
                // resolver to see if the pipeline works for bone FNames.
                f << ",\"names\":[";
                for (int i = 0; i < 3 && i < hdrCount; ++i) {
                    if (i) f << ',';
                    const uint64_t e = hdrData + static_cast<uint64_t>(i) * 0x18;
                    const uint32_t nameIdx = steam_decrypt::MemReadVal<uint32_t>(e);
                    const std::string name = steam_decrypt::CachedNameString(
                        static_cast<int32_t>(nameIdx), Memory::getBaseAddress());
                    f << "{\"i\":" << nameIdx
                      << ",\"n\":\"" << name << "\"}";
                }
                f << "]";
            }
            f << "}";
            break; // only dump the first plausible candidate
        }
    }

    f << "},\"timestamp\":" << wallMs << "}\n";
    EntityDiagnostics::LogRaw(f.str());
}

// Parent indices for the 22 drawn bones (standard UE humanoid). Shared by the
// exhaustive scan and the remembered-layout fast path.
static const int kBoneParentIndices[97] = {
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

// Where the winning bone array was found, remembered across actors and frames.
//
// The array's location is a property of the build's mesh component, not of an
// actor: which of the three mesh pointers carries it, which slot holds it, and
// whether it is component-space or must be accumulated up the parent chain are
// the same for every character. Discovering that costs an exhaustive scan — up
// to 27 candidate arrays, each scored with ~100 scattered reads — measured at
// 36.7 ms per call on the live build (avgResolveMs in the bone_cost record).
// GetBones ran that scan for every player on every frame, so it was ~97% of the
// ESP frame build (frame.boneMilliseconds 191 ms) and held the frame pool near
// 5 fps. The winner is remembered and re-validated with a descriptor read
// instead; the exhaustive scan runs again only when the remembered recipe stops
// scoring. Fields are atomics so the fast path takes no lock: a torn read can
// only cost one validation miss, which falls back to the scan.
struct BoneArrayRecipe {
    std::atomic<bool> valid{ false };
    std::atomic<bool> viaDecrypt{ false };
    std::atomic<std::ptrdiff_t> slot{ -1 };
    std::atomic<bool> accumulated{ false };
    std::atomic<uint64_t> hits{ 0 };
    std::atomic<uint64_t> misses{ 0 };
};

static BoneArrayRecipe& BoneArrayRecipeCache()
{
    static BoneArrayRecipe recipe;
    return recipe;
}

static void RememberBoneArrayRecipe(bool viaDecrypt, std::ptrdiff_t slot,
                                    bool accumulated)
{
    BoneArrayRecipe& r = BoneArrayRecipeCache();
    r.viaDecrypt.store(viaDecrypt, std::memory_order_relaxed);
    r.slot.store(slot, std::memory_order_relaxed);
    r.accumulated.store(accumulated, std::memory_order_relaxed);
    r.valid.store(true, std::memory_order_release);
}

// The two bones that carry the scan's validity decision.
struct AnchorBones {
    int head = -1;
    int pelvis = -1;
};

static AnchorBones ResolveAnchorBones()
{
    AnchorBones a{};
    for (const auto& [gameIndex, uniBone] : BoneRoster::GameBoneMap()) {
        if (uniBone == UniBone::Head && a.head < 0)
            a.head = gameIndex;
        else if (uniBone == UniBone::Pelvis && a.pelvis < 0)
            a.pelvis = gameIndex;
    }
    return a;
}

// One bone's world position, computed the way the scan computes it, touching
// only that bone's parent chain when the array is bone-space.
static Vector3 BoneWorldForValidation(Engine& eng, uintptr_t arr,
                                      const FTransform& ctw, int gameIndex,
                                      bool accumulated)
{
    if (!accumulated)
        return eng.GetBone(gameIndex, arr, ctw);

    const std::vector<int32_t>* calParents = BoneRoster::GameBoneParents();
    const int maxBone = calParents ? static_cast<int>(calParents->size()) : 97;
    std::vector<int> chain; // leaf first
    for (int cur = gameIndex; cur >= 0 && cur < maxBone && chain.size() < 64;) {
        chain.push_back(cur);
        const int next = calParents
            ? (*calParents)[static_cast<size_t>(cur)]
            : ((cur < 97) ? kBoneParentIndices[cur] : -1);
        // The scan only accumulates over a parent that precedes its child
        // (parent < child); anything else is treated as a chain root there too.
        if (next < 0 || next >= cur)
            break;
        cur = next;
    }

    D3DMATRIX acc{};
    bool have = false;
    for (int i = static_cast<int>(chain.size()) - 1; i >= 0; --i) {
        const int bi = chain[static_cast<size_t>(i)];
        const FTransform t = Memory::read<FTransform>(
            arr + static_cast<uintptr_t>(bi) * Bones::BoneStride);
        const D3DMATRIX local = t.ToMatrixWithScale();
        acc = have ? eng.MatrixMultiplication(local, acc) : local;
        have = true;
    }
    if (!have)
        return Vector3{};

    const D3DMATRIX w = eng.MatrixMultiplication(acc, ctw.ToMatrixWithScale());
    return Vector3(w._41, w._42, w._43);
}

// Validate a remembered candidate from its two anchors. For a single candidate
// the scan's score is 0 unless head and pelvis both land at plausible, non-origin
// world positions with the head above the pelvis, and >= 2 exactly when that
// holds (ScoreBoneArrayForMesh / ScoreBoneArrayAccumulated) — so sweeping all 22
// bones decides nothing these two do not, and that sweep measured 3.5 ms of the
// ~5 ms per-actor refresh. Component-space is tried first, then the parent-chain
// accumulation, in the scan's own order.
static bool ValidateCandidateByAnchors(Engine& eng, uintptr_t mesh, uintptr_t arr,
                                       bool* outAccumulated)
{
    const AnchorBones anchors = ResolveAnchorBones();
    if (anchors.head < 0 || anchors.pelvis < 0)
        return false;

    const FTransform ctw = Engine::ReadComponentToWorld(mesh);
    auto valid = [&](bool accumulated) -> bool {
        const Vector3 head =
            BoneWorldForValidation(eng, arr, ctw, anchors.head, accumulated);
        if (!IsPlausibleWorldPos(head) || IsNearZero(head))
            return false;
        const Vector3 pelvis =
            BoneWorldForValidation(eng, arr, ctw, anchors.pelvis, accumulated);
        if (!IsPlausibleWorldPos(pelvis) || IsNearZero(pelvis))
            return false;
        return head.z > pelvis.z;
    };

    if (valid(false)) {
        *outAccumulated = false;
        return true;
    }
    if (valid(true)) {
        *outAccumulated = true;
        return true;
    }
    return false;
}

// Fast path: stand the remembered recipe back up on this actor's meshes. Costs a
// descriptor read plus one scoring pass, against the scan's 27.
static bool TryRememberedBoneArray(Engine& eng, const uintptr_t* meshes,
                                   size_t meshCount, BoneArrayCandidate* out)
{
    BoneArrayRecipe& r = BoneArrayRecipeCache();
    if (!r.valid.load(std::memory_order_acquire))
        return false;

    const bool viaDecrypt = r.viaDecrypt.load(std::memory_order_relaxed);
    const std::ptrdiff_t slot = r.slot.load(std::memory_order_relaxed);

    for (size_t i = 0; i < meshCount; ++i) {
        const uintptr_t mesh = meshes[i];
        if (!mesh || !eng.IsValidPointer(mesh))
            continue;

        uintptr_t arr = 0;
        if (viaDecrypt) {
            arr = steam_decrypt::GetBoneArrayDecrypt(mesh);
        } else {
            if (slot < 0 || slot > 0x2000)
                continue;
            arr = Memory::read<uintptr_t>(mesh + slot);
            if (!arr || !eng.IsValidPointer(arr))
                continue;
            const int count = Memory::read<int>(mesh + slot + 8);
            if (count <= 0 || count > Bones::MaxBoneCount)
                continue;
            // Same descriptor-stability rule the scan applies: the animator
            // refreshes this descriptor while a pose is sampled, and a
            // descriptor that moved mid-read describes a different array.
            if (Memory::read<uintptr_t>(mesh + slot) != arr
                || Memory::read<int>(mesh + slot + 8) != count)
                continue;
        }
        if (!arr || !eng.IsValidPointer(arr))
            continue;

        bool accumulated = false;
        if (!ValidateCandidateByAnchors(eng, mesh, arr, &accumulated))
            continue;

        // score is only used to pick between competing candidates; this path has
        // exactly one, and it was validated, so it carries the scan's pass mark.
        *out = BoneArrayCandidate{ arr, mesh, 2, accumulated };
        r.hits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    r.misses.fetch_add(1, std::memory_order_relaxed);
    return false;
}

static BoneArrayCandidate FindBestBoneArray(uintptr_t actor, uintptr_t primaryMesh)
{
    const uintptr_t meshA = actor
        ? Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent) : 0;
    const uintptr_t meshB = actor
        ? Memory::read<uintptr_t>(actor + Offsets::EmbarkMesh) : 0;

    // Embark mesh usually owns the encrypted bone block on this build.
    const uintptr_t meshes[] = { meshB, primaryMesh, meshA };
    Engine& eng = engine;

    if (BoneArrayCandidate remembered{};
        TryRememberedBoneArray(eng, meshes, sizeof(meshes) / sizeof(meshes[0]),
                               &remembered)) {
        ProbeBoneArray(actor, meshA, meshB, primaryMesh, remembered.score);
        return remembered;
    }

    BoneArrayCandidate best{};

    for (uintptr_t mesh : meshes) {
        if (!mesh || !eng.IsValidPointer(mesh))
            continue;

        // Path 1: SIMD decrypt (old approach — fast, component-space arrays)
        const uintptr_t arr = steam_decrypt::GetBoneArrayDecrypt(mesh);
        if (arr && eng.IsValidPointer(arr)) {
            const int score = ScoreBoneArrayForMesh(eng, mesh, arr);
            if (score > best.score) {
                best = { arr, mesh, score, false };
                if (score >= 2)
                    RememberBoneArrayRecipe(true, -1, false);
            }
        }
    }

    // If decrypt didn't find a good array, try plaintext component-space
    // arrays on each mesh. These are the CachedComponentSpaceTransforms /
    // CachedBoneSpaceTransforms TArrays on the mesh.
    if (best.score < 2) {
        // Authoritative merged-SDK-dump slots (USkeletalMeshComponent):
        //   0x0BE8 CachedBoneSpaceTransforms      (TArray<FTransform>, bone-space)
        //   0x0BF8 CachedComponentSpaceTransforms (TArray<FTransform>, component-space)
        // The old 0x0B00/0x0B80/0x0B90/0x0970/0x0980 slots all fall inside the
        // dump's "MISSED" unknown-data regions, so they never held a real array.
        // 0x0B68 is the hidden TArray<FTransform> (count=97) that the runtime
        // scan found: it exactly fills the dump's UnknownData01_6[0x10] gap
        // before RootBoneTranslation (0x0B78), so the dumper labelled this
        // real, anti-cheat-hidden bone array as unknown data.
        // 0x0B98 is the plain LocalPose TArray (USkeletalMeshComponent): Data at
        // +0x00, Count at +0x08, Capacity at +0x0C, 0x60-byte plain double
        // transforms, no decryptor (October 13 skeleton reference). Measured on a
        // live character mesh: ptr=0x4e1630010, count=97 -- the 97 poses the
        // reference reports -- while the slots below gave 0x0BF8 count=29 and no
        // array ever scored (bestScore 0), which is why no skeleton was drawn.
        // It is listed first so an equal-scoring later slot cannot displace it.
        constexpr std::ptrdiff_t kArraySlots[] = {
            0x0B98,
            0x0B68,
            0x0BF8, 0x0BE8,
            0x0B00, 0x0B80, 0x0B90, 0x0970, 0x0980
        };
        // Parent indices for the 22 drawn bones (standard UE humanoid) now live
        // at file scope as kBoneParentIndices, shared with the remembered-layout
        // fast path below.

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

                // The animator refreshes this descriptor while a pose is
                // sampled, so re-read it and require both fields unchanged
                // before the array is trusted (October 13 skeleton reference:
                // "recheck Data and Count after reading; retry if either
                // changed"). A descriptor that moved mid-read is retried next
                // pass instead of being scored from a pointer that no longer
                // describes `count`.
                if (Memory::read<uintptr_t>(mesh + slot) != arr
                    || Memory::read<int>(mesh + slot + 8) != count)
                    continue;

                // Try direct (component-space) scoring first.
                int score = ScoreBoneArrayForMesh(eng, mesh, arr);
                if (score > best.score) {
                    best = { arr, mesh, score, false };
                    if (score >= 2)
                        RememberBoneArrayRecipe(false, slot, false);
                }

                // If direct scoring fails, try accumulated (bone-space) —
                // with the calibrated parent chain when one is proven.
                if (score < 2) {
                    const std::vector<int32_t>* calParents =
                        BoneRoster::GameBoneParents();
                    score = ScoreBoneArrayAccumulated(
                        eng, mesh, arr,
                        calParents ? calParents->data() : kBoneParentIndices,
                        calParents ? static_cast<int>(calParents->size()) : 97);
                    if (score > best.score) {
                        best = { arr, mesh, score, true };
                        if (score >= 2)
                            RememberBoneArrayRecipe(false, slot, true);
                    }
                }
            }
        }
    }

    ProbeBoneArray(actor, meshA, meshB, primaryMesh, best.score);
    return best;
}

// How often an uncalibrated roster may be probed. The probe is a build-level
// question with a build-level answer, so this only bounds how long a freshly
// patched skeleton stays on the static fallback table.
constexpr uint64_t kRosterCalibrateIntervalMs = 1500;

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
    //
    // Attempt calibration on ANY valid mesh component, not just score>=2: the
    // name check is independent of the (possibly stale) static index table, and
    // gating on score deadlocks a shifted skeleton — no calibration without
    // score>=2, no score>=2 without calibration. TryCalibrate itself validates
    // names + parents + BuildRoster before installing, so a bad mesh cannot
    // poison the roster.
    const uintptr_t calMesh = best.mesh ? best.mesh : primaryMesh;
    if (calMesh) {
        // Throttled: one probe walks the whole RefSkeleton window (0x140..0x600,
        // step 8) on up to six skeleton candidates and decrypts the names it
        // finds — ~550 scattered reads. The answer is a property of the build,
        // not of this actor, so asking once per actor per frame bought nothing
        // and cost everything: measured on the live build it was ~40 ms per
        // player, 97% of the ESP frame build (frame.boneMilliseconds ~80 ms with
        // only two players) and pinned the frame pool near 5 fps. An uncalibrated
        // roster now costs one probe per interval instead of one per actor.
        // The attempt stamp is claimed with a CAS so concurrent frame builders do
        // not all probe at once, and only a claimed attempt enters TryCalibrate.
        static std::atomic<uint64_t> s_lastCalAttemptMs{ 0 };
        const uint64_t calNowMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        uint64_t lastCalAttempt =
            s_lastCalAttemptMs.load(std::memory_order_relaxed);
        if (lastCalAttempt == 0
            || calNowMs - lastCalAttempt >= kRosterCalibrateIntervalMs) {
            if (s_lastCalAttemptMs.compare_exchange_strong(
                    lastCalAttempt, calNowMs, std::memory_order_relaxed))
                BoneRoster::TryCalibrate(calMesh, Memory::getBaseAddress());
        }
    }

    // One-time roster tap: which name won each body part.
    if (BoneRoster::IsCalibrated()) {
        static std::atomic<bool> s_rosterLogged{ false };
        if (!s_rosterLogged.exchange(true)) {
            const auto rMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            std::ostringstream rf;
            rf << "{\"sessionId\":\"c190fb\",\"runId\":\"verify\","
               << "\"location\":\"BoneList.cpp\",\"message\":\"bone_roster\","
               << "\"data\":{\"map\":\"";
            const auto& rm = BoneRoster::GameBoneMap();
            for (size_t ri = 0; ri < rm.size(); ++ri) {
                if (ri) rf << ',';
                rf << rm[ri].first << ':' << BoneRoster::UniBoneAbbr(rm[ri].second);
            }
            rf << "\"},\"timestamp\":" << rMs << "}\n";
            EntityDiagnostics::LogRaw(rf.str());
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
            std::ostringstream f;
            f << "{\"sessionId\":\"c190fb\",\"runId\":\"verify\","
              << "\"location\":\"BoneList.cpp\",\"message\":\"bone_roster_fail\","
              << "\"data\":{\"stage\":" << d.stage.load()
              << ",\"skelTried\":" << d.skelTried.load()
              << ",\"hdrOff\":" << d.hdrOff.load()
              << ",\"count\":" << d.count.load()
              << ",\"resolved\":" << d.resolved.load()
              << ",\"roles\":\"0x" << std::hex << d.roles.load() << std::dec << "\""
              << "},\"timestamp\":" << ms << "}\n";
            EntityDiagnostics::LogRaw(f.str());
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
            std::ostringstream bf;
            bf << "{\"sessionId\":\"c190fb\",\"runId\":\"verify\","
               << "\"location\":\"BoneList.cpp\",\"message\":\"bone_resolve\"," 
               << "\"data\":{\"score\":" << best.score
               << ",\"accum\":" << (best.accumulated ? 1 : 0)
               << ",\"roster\":" << (BoneRoster::IsCalibrated() ? 1 : 0)
               << ",\"array\":0x" << std::hex << best.array
               << ",\"mesh\":0x" << best.mesh << std::dec
               << "},\"timestamp\":" << ms << "}\n";
            EntityDiagnostics::LogRaw(bf.str());
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

namespace {
// Bone-refresh cost probe. The frame build's bone refresh measured ~80 ms per
// frame and barely moved when the player count changed (two players and five
// players both sat at ~80 ms), so the cost is not simply per-actor and the
// calibration probe is not it either (that is throttled to 1.5 s and the 80 ms
// is on every frame). This splits the four stages so the one that actually
// spends the time is measured instead of guessed. Accumulates on the frame
// builder; logs one record every 2 s.
struct BoneStageProbe {
    using Clock = std::chrono::steady_clock;
    Clock::time_point last = Clock::now();
    double meshMs = 0.0;
    double resolveMs = 0.0;
    double ctwMs = 0.0;
    double readMs = 0.0;
    double mathMs = 0.0;

    double Mark()
    {
        const Clock::time_point now = Clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        return ms;
    }

    ~BoneStageProbe()
    {
        struct Acc {
            std::mutex mtx;
            double mesh = 0.0, resolve = 0.0, ctw = 0.0, read = 0.0, math = 0.0;
            double worst = 0.0;
            uint64_t calls = 0;
            Clock::time_point lastLog{};
        };
        static Acc acc;

        std::lock_guard<std::mutex> lock(acc.mtx);
        acc.mesh += meshMs;
        acc.resolve += resolveMs;
        acc.ctw += ctwMs;
        acc.read += readMs;
        acc.math += mathMs;
        ++acc.calls;
        const double total = meshMs + resolveMs + ctwMs + readMs + mathMs;
        if (total > acc.worst)
            acc.worst = total;

        const Clock::time_point now = Clock::now();
        if (now - acc.lastLog < std::chrono::seconds(2))
            return;
        const double n = static_cast<double>(acc.calls);
        std::ostringstream os;
        os << "{\"message\":\"bone_cost\",\"data\":{"
           << "\"calls\":" << acc.calls
           << ",\"avgMeshMs\":" << (acc.mesh / n)
           << ",\"avgResolveMs\":" << (acc.resolve / n)
           << ",\"avgCtwMs\":" << (acc.ctw / n)
           << ",\"avgReadMs\":" << (acc.read / n)
           << ",\"avgMathMs\":" << (acc.math / n)
           << ",\"worstMs\":" << acc.worst << "}}";
        EntityDiagnostics::LogRaw(os.str());

        acc.mesh = acc.resolve = acc.ctw = acc.read = acc.math = 0.0;
        acc.worst = 0.0;
        acc.calls = 0;
        acc.lastLog = now;
    }
};
} // namespace

void Engine::GetBones(PlayerCacheEntry& actor)
{
    BoneStageProbe probe;

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
    probe.meshMs = probe.Mark();
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
    probe.resolveMs = probe.Mark();
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
    probe.ctwMs = probe.Mark();

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

    probe.readMs = probe.Mark();

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
            std::ostringstream af;
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
            EntityDiagnostics::LogRaw(af.str());
        }
    }

    probe.mathMs = probe.Mark();
}
