#pragma once

// =============================================================================
// BoneRoster — patch-proof gameIndex -> UniBone mapping.
//
// The static table (BoneID, dated CL-1233465) drifts with patches: when the
// skeleton gains or loses bones, every index above the change lands on the
// WRONG body part (observed on CL-1389382: the "head" bone resolving BELOW the
// pelvis, hands at the elbows). The decrypt and the FTransform math were fine —
// the index->body-part assignment was stale.
//
// The only ground truth that survives patches is the skeleton's own bone NAMES,
// so at runtime we read FReferenceSkeleton::RawBoneInfo out of the live
// USkeletalMesh (via the FName pipeline) and rebuild the map by name. Until a
// roster is proven the static CL-1233465 map stays in effect.
//
// Split so the logic is testable without DMA:
//   pure : ClassifyBoneName / BuildRoster  (pinned by Tests/bone_roster_tests)
//   DMA  : TryCalibrate (probes the RawBoneInfo TArray, installs once)
//
// Concurrency: the roster is written exactly once behind a mutex and published
// with a release/acquire flag AFTER the vectors are complete, so readers never
// observe a half-written map and never race with the install.
// =============================================================================

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "Core/Cache.hpp"        // UniBone
#include "Core/Vector.hpp"       // BoneID (static fallback table's indices)

#pragma warning(push)
#pragma warning(disable : 4201) // vmmdll.h nameless struct/union (ThirdParty)
#include "Core/SteamDecrypt.hpp" // MemRead, CachedNameString, Bones, Offsets
#include "Core/Reflection.hpp" // ResolveNameString (v20260922 FName pipeline)
#pragma warning(pop)

namespace BoneRoster {

// ---------------------------------------------------------------------------
// Classification (pure)
// ---------------------------------------------------------------------------

enum class Role : uint8_t {
    None = 0,
    Root,
    Pelvis,
    Spine,
    Chest,
    Neck,
    Head,
    Clavicle,
    UpperArm,
    ForeArm,
    Hand,
    Thigh,
    Calf,
    Foot,
};

struct BoneClass {
    Role role = Role::None;
    int  side = 0;   // -1 left, 0 neutral, +1 right
    int  order = 0;  // ordinal parsed from the name (spine_03 -> 3)
    int  exact = 0;  // 2 = exact token match, 1 = fuzzy substring match
};

namespace detail {

inline std::string NormalizeBoneName(const std::string& raw)
{
    std::string n;
    n.reserve(raw.size());
    for (char c : raw) {
        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lc == '.' || lc == '-' || lc == ' ' || lc == '/')
            lc = '_';
        if (lc == '_' && !n.empty() && n.back() == '_')
            continue;
        n.push_back(lc);
    }
    while (!n.empty() && n.back() == '_')
        n.pop_back();
    return n;
}

inline std::vector<std::string> SplitTokens(const std::string& n)
{
    std::vector<std::string> toks;
    std::string cur;
    for (char c : n) {
        if (c == '_') {
            if (!cur.empty())
                toks.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        toks.push_back(cur);
    return toks;
}

// Attachment / IK / finger / effect bones are never drawn body parts. Mapping
// one of these to a UniBone is exactly how bones end up off the body.
inline bool IsExcludedBoneName(const std::string& n, const std::vector<std::string>& toks)
{
    static const char* kBadSub[] = {
        "twist", "socket", "finger", "thumb", "pinky", "weapon", "prop", "ik",
    };
    static const char* kBadTok[] = {
        "ik", "vb", "roll", "gun", "cam", "camera", "target", "aim", "look",
        "hit", "fx", "index", "middle", "ring", "toe", "ball", "eye", "jaw",
        "dyn", "cloth", "virtual", "tag", "attach", "end", "tip",
    };
    for (const char* s : kBadSub)
        if (n.find(s) != std::string::npos)
            return true;
    for (const char* s : kBadTok)
        if (std::find(toks.begin(), toks.end(), s) != toks.end())
            return true;
    return false;
}

inline int ParseOrder(const std::string& n)
{
    for (size_t i = 0; i < n.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(n[i]))) {
            int v = 0;
            while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i]))) {
                v = v * 10 + (n[i] - '0');
                ++i;
            }
            return v;
        }
    }
    return 0;
}

} // namespace detail

// Name -> body part. Handles both separator styles ("upperarm_l", "LeftArm").
inline BoneClass ClassifyBoneName(const std::string& rawName)
{
    BoneClass out;
    const std::string n = detail::NormalizeBoneName(rawName);
    if (n.empty())
        return out;
    const std::vector<std::string> toks = detail::SplitTokens(n);
    if (detail::IsExcludedBoneName(n, toks))
        return out;

    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    auto tok = [&](const char* s) {
        return std::find(toks.begin(), toks.end(), s) != toks.end();
    };

    int side = 0;
    if (tok("l") || tok("left") || has("left"))
        side = -1;
    else if (tok("r") || tok("right") || has("right"))
        side = 1;

    auto set = [&](Role r, int sideOut, int exact) {
        out.role = r;
        out.side = sideOut;
        out.exact = exact;
        out.order = 0;
    };

    // Order matters: "forearm" before "arm", "upleg" before "leg".
    if (has("pelvis") || tok("hips"))
        set(Role::Pelvis, 0, has("pelvis") ? 2 : 1);
    else if (has("neck"))
        set(Role::Neck, 0, tok("neck") ? 2 : 1);
    else if (has("head"))
        set(Role::Head, 0, tok("head") ? 2 : 1);
    else if (has("chest"))
        set(Role::Chest, 0, tok("chest") ? 2 : 1);
    else if (has("spine")) {
        set(Role::Spine, 0, tok("spine") ? 2 : 1);
        out.order = detail::ParseOrder(n);
    } else if (has("clavicle") || has("collar") || has("scapula"))
        set(Role::Clavicle, side, 1);
    else if (has("forearm") || has("lowerarm") || has("lower_arm") || has("elbow"))
        set(Role::ForeArm, side, 1);
    else if (has("upperarm") || has("upper_arm") || has("uparm") ||
             has("shoulder") || has("arm"))
        set(Role::UpperArm, side, 1);
    else if (has("hand"))
        set(Role::Hand, side, tok("hand") ? 2 : 1);
    else if (has("thigh") || has("upleg") || has("upperleg") || has("upper_leg"))
        set(Role::Thigh, side, 1);
    else if (has("calf") || has("shin") || has("lowerleg") || has("lower_leg") ||
             has("leg"))
        set(Role::Calf, side, 1);
    else if (has("foot"))
        set(Role::Foot, side, tok("foot") ? 2 : 1);
    else if (has("root"))
        set(Role::Root, 0, tok("root") ? 2 : 1);

    // Limb with no marker token ("handL"): fall back to the trailing letter.
    if (out.side == 0 && !n.empty()) {
        switch (out.role) {
        case Role::Clavicle:
        case Role::UpperArm:
        case Role::ForeArm:
        case Role::Hand:
        case Role::Thigh:
        case Role::Calf:
        case Role::Foot:
            if (n.back() == 'l')
                out.side = -1;
            else if (n.back() == 'r')
                out.side = 1;
            break;
        default:
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Roster building (pure)
// ---------------------------------------------------------------------------

// names[i] / parents[i] describe game bone index i (FReferenceSkeleton order).
// On success fills outMap (sorted by game index) and outParents (verbatim
// parent chain for bone-space accumulation). Rejects any roster that cannot
// cover the joints the renderer links — a partial map would silently drop
// limbs, so anything short of full body keeps the static table in effect.
inline bool BuildRoster(const std::vector<std::string>& names,
                        const std::vector<int32_t>& parents,
                        std::vector<std::pair<int, UniBone>>& outMap,
                        std::vector<int32_t>& outParents)
{
    outMap.clear();
    outParents.clear();

    const int count = static_cast<int>(names.size());
    if (count < 16 || static_cast<int>(parents.size()) != count)
        return false;

    struct Hit {
        int idx = -1;
        BoneClass cls;
    };
    Hit slots[static_cast<int>(Role::Foot) + 1][3]; // [role][side + 1]
    std::vector<BoneClass> cls(names.size());
    std::vector<int> spines;
    int rootIdx = -1;

    auto beats = [](const BoneClass& a, int ai, const BoneClass& b, int bi) {
        if (a.exact != b.exact)
            return a.exact > b.exact;
        if (a.order != b.order)
            return a.order < b.order;
        return ai < bi;
    };

    for (int i = 0; i < count; ++i) {
        cls[i] = ClassifyBoneName(names[i]);
        const BoneClass& c = cls[i];
        if (c.role == Role::None)
            continue;
        if (c.role == Role::Spine) {
            spines.push_back(i);
            continue;
        }
        if (c.role == Role::Root) {
            if (rootIdx < 0 || i < rootIdx)
                rootIdx = i;
            continue;
        }
        int side = c.side;
        switch (c.role) {
        case Role::Pelvis:
        case Role::Chest:
        case Role::Neck:
        case Role::Head:
            side = 0;
            break;
        default:
            break;
        }
        Hit& h = slots[static_cast<int>(c.role)][side + 1];
        if (h.idx < 0 || beats(c, i, h.cls, h.idx))
            h = { i, c };
    }

    // Spine chain: explicit ordinals when the names carry them ("spine_03"),
    // declaration order otherwise (Mixamo "Spine", "Spine1", ...).
    std::sort(spines.begin(), spines.end(), [&](int a, int b) {
        if (cls[a].order != cls[b].order)
            return cls[a].order < cls[b].order;
        return a < b;
    });

    std::vector<std::pair<int, UniBone>> map;
    auto add = [&](int idx, UniBone u) {
        if (idx >= 0)
            map.push_back({ idx, u });
    };
    const Hit& chest = slots[static_cast<int>(Role::Chest)][1];
    const int chestIdx =
        chest.idx >= 0 ? chest.idx : (spines.empty() ? -1 : spines.back());

    add(slots[static_cast<int>(Role::Pelvis)][1].idx, UniBone::Pelvis);
    if (!spines.empty())
        add(spines[0], UniBone::Spine1);
    if (spines.size() > 1)
        add(spines[1], UniBone::Spine2);
    if (spines.size() > 2)
        add(spines[2], UniBone::Spine3);
    add(chestIdx, UniBone::Chest);
    add(slots[static_cast<int>(Role::Neck)][1].idx, UniBone::Neck);
    add(slots[static_cast<int>(Role::Head)][1].idx, UniBone::Head);

    add(slots[static_cast<int>(Role::Clavicle)][0].idx, UniBone::ClavicleL);
    add(slots[static_cast<int>(Role::UpperArm)][0].idx, UniBone::UpperArmL);
    add(slots[static_cast<int>(Role::ForeArm)][0].idx, UniBone::LowerArmL);
    add(slots[static_cast<int>(Role::Hand)][0].idx, UniBone::HandL);

    add(slots[static_cast<int>(Role::Clavicle)][2].idx, UniBone::ClavicleR);
    add(slots[static_cast<int>(Role::UpperArm)][2].idx, UniBone::UpperArmR);
    add(slots[static_cast<int>(Role::ForeArm)][2].idx, UniBone::LowerArmR);
    add(slots[static_cast<int>(Role::Hand)][2].idx, UniBone::HandR);

    add(slots[static_cast<int>(Role::Thigh)][0].idx, UniBone::ThighL);
    add(slots[static_cast<int>(Role::Calf)][0].idx, UniBone::CalfL);
    add(slots[static_cast<int>(Role::Foot)][0].idx, UniBone::FootL);

    add(slots[static_cast<int>(Role::Thigh)][2].idx, UniBone::ThighR);
    add(slots[static_cast<int>(Role::Calf)][2].idx, UniBone::CalfR);
    add(slots[static_cast<int>(Role::Foot)][2].idx, UniBone::FootR);

    // Root: the named root bone, else the skeleton root (parent == -1).
    if (rootIdx >= 0) {
        add(rootIdx, UniBone::Root);
    } else {
        for (int i = 0; i < count; ++i) {
            if (parents[i] == -1) {
                add(i, UniBone::Root);
                break;
            }
        }
    }

    // Validation: the renderer links a fixed joint set. Requiring all of it
    // means a half-parsed roster can never partially replace a working map.
    bool have[static_cast<size_t>(UniBone::Count)] = {};
    for (const auto& e : map)
        have[static_cast<size_t>(e.second)] = true;
    static const UniBone kRequired[] = {
        UniBone::Pelvis,  UniBone::Neck,      UniBone::Head,
        UniBone::UpperArmL, UniBone::UpperArmR,
        UniBone::HandL,   UniBone::HandR,
        UniBone::ThighL,  UniBone::ThighR,
        UniBone::FootL,   UniBone::FootR,
    };
    for (UniBone u : kRequired)
        if (!have[static_cast<size_t>(u)])
            return false;
    if (!have[static_cast<size_t>(UniBone::Spine1)] &&
        !have[static_cast<size_t>(UniBone::Chest)])
        return false;
    size_t assigned = 0;
    for (bool h : have)
        assigned += h ? 1 : 0;
    if (assigned < 16)
        return false;

    std::sort(map.begin(), map.end());
    outMap = std::move(map);
    outParents = parents;
    return true;
}

// ---------------------------------------------------------------------------
// Active map storage (published once, read concurrently)
// ---------------------------------------------------------------------------

inline std::vector<std::pair<int, UniBone>>& MapStorage()
{
    static std::vector<std::pair<int, UniBone>> s;
    return s;
}

inline std::vector<int32_t>& ParentsStorage()
{
    static std::vector<int32_t> s;
    return s;
}

inline std::atomic<bool>& ReadyFlag()
{
    static std::atomic<bool> s{ false };
    return s;
}

// Static CL-1233465 table (fremework::game::bones) — fallback until a roster
// is proven by name, and the reference the roster must agree with when the
// skeleton has not shifted.
inline const std::vector<std::pair<int, UniBone>>& FallbackMap()
{
    static const std::vector<std::pair<int, UniBone>> kFallback = {
        { BoneID::Root,      UniBone::Root },
        { BoneID::Pelvis,    UniBone::Pelvis },
        { BoneID::Spine01,   UniBone::Spine1 },
        { BoneID::Spine02,   UniBone::Spine2 },
        { BoneID::Spine03,   UniBone::Spine3 },
        { BoneID::Chest,     UniBone::Chest },
        { BoneID::Neck,      UniBone::Neck },
        { BoneID::Head,      UniBone::Head },
        { BoneID::L_Clavicle, UniBone::ClavicleL },
        { BoneID::L_UpperArm, UniBone::UpperArmL },
        { BoneID::L_Forearm, UniBone::LowerArmL },
        { BoneID::L_Hand,    UniBone::HandL },
        { BoneID::R_Clavicle, UniBone::ClavicleR },
        { BoneID::R_UpperArm, UniBone::UpperArmR },
        { BoneID::R_Forearm, UniBone::LowerArmR },
        { BoneID::R_Hand,    UniBone::HandR },
        { BoneID::L_Thigh,   UniBone::ThighL },
        { BoneID::L_Calf,    UniBone::CalfL },
        { BoneID::L_Foot,    UniBone::FootL },
        { BoneID::R_Thigh,   UniBone::ThighR },
        { BoneID::R_Calf,    UniBone::CalfR },
        { BoneID::R_Foot,    UniBone::FootR },
    };
    return kFallback;
}

inline bool IsCalibrated()
{
    return ReadyFlag().load(std::memory_order_acquire);
}

// The active gameIndex -> UniBone map.
inline const std::vector<std::pair<int, UniBone>>& GameBoneMap()
{
    return IsCalibrated() ? MapStorage() : FallbackMap();
}

// Real per-index parent chain for bone-space accumulation (calibrated only).
// nullptr -> callers keep their static parent tables.
inline const std::vector<int32_t>* GameBoneParents()
{
    return IsCalibrated() ? &ParentsStorage() : nullptr;
}

// Test seam: forget any installed roster.
inline void ResetRosterForTest()
{
    ReadyFlag().store(false, std::memory_order_release);
    MapStorage().clear();
    ParentsStorage().clear();
}

inline const char* UniBoneAbbr(UniBone u)
{
    static const char* kAbbr[] = {
        "Root", "Pel", "S1", "S2", "S3", "Ch", "Nk", "Hd",
        "ClL", "UAL", "LAL", "HnL", "ClR", "UAR", "LAR", "HnR",
        "ThL", "CfL", "FtL", "ThR", "CfR", "FtR",
    };
    const size_t i = static_cast<size_t>(u);
    return (i < sizeof(kAbbr) / sizeof(kAbbr[0])) ? kAbbr[i] : "?";
}

// ---------------------------------------------------------------------------
// DMA calibration
// ---------------------------------------------------------------------------

// Resolves an FName comparison index to text. Production default runs the
// game's FName decrypt; tests inject a plain lookup.
using BoneNameResolver = std::string (*)(uint32_t compIndex, uint64_t gameBase);

inline std::string DefaultResolveName(uint32_t compIndex, uint64_t gameBase)
{
    // The v20260922 FName pipeline first: the legacy plain walk starves on
    // this build (bone_roster_fail: resolved:0), and name calibration is what
    // keeps the roster honest across patches. CachedNameString stays as the
    // fallback for builds where the plain walk is the live scheme.
    const std::string name =
        Reflection::ResolveNameString(static_cast<int32_t>(compIndex), gameBase);
    if (!name.empty())
        return name;
    return steam_decrypt::CachedNameString(static_cast<int32_t>(compIndex), gameBase);
}

// FReferenceSkeleton::RawBoneInfo probe window on USkeletalMesh
// (ArcOffsets::SkeletalMesh::REF_SKELETON_PROBE_LO/HI).
constexpr uint64_t kRefSkeletonProbeLo = 0x140;
constexpr uint64_t kRefSkeletonProbeHi = 0x600; // widened: live headers sit past 0x3C0 on UE 5.7

// Probe diagnostics (read by BoneList's bone_roster_fail tap): why the last
// calibration attempt did not install. 136 attempts with 0 installs were
// previously invisible — every stage bailed silently.
struct ProbeDiag {
    std::atomic<int>      stage{ 0 };     // 1 no skel ptr, 2 no header, 3 no layout parsed, 4 BuildRoster rejected
    std::atomic<uint32_t> skelTried{ 0 }; // bitmask of skel-pointer candidates that resolved
    std::atomic<int>      hdrOff{ -1 };   // best RawBoneInfo header offset seen
    std::atomic<int>      count{ 0 };     // bone count at the best candidate
    std::atomic<int>      resolved{ 0 };  // names decoded at the best candidate
    std::atomic<uint32_t> roles{ 0 };     // classified Role bits at the best candidate
};
inline ProbeDiag& Diag() { static ProbeDiag d; return d; }

// Probe USkeletalMesh::RefSkeleton::RawBoneInfo and install a name-calibrated
// roster. Returns true once a roster is installed (this call or an earlier
// one). meshComponent is a USkeletalMeshComponent*.
inline bool TryCalibrate(uint64_t meshComponent, uint64_t gameBase,
                         BoneNameResolver resolveName = &DefaultResolveName)
{
    if (IsCalibrated())
        return true;
    static std::mutex s_probeMtx;
    std::lock_guard<std::mutex> lock(s_probeMtx);
    if (IsCalibrated())
        return true;
    if (!meshComponent || !resolveName) {
        Diag().stage.store(1);
        return false;
    }

    auto plausiblePtr = [](uint64_t p) {
        return p >= 0x10000ULL && p < 0x800000000000ULL;
    };

    // SkeletalMesh asset pointer — offset is contested between the dump
    // (0x720) and ArcOffsets (0x740/0x748); try them all, names validate.
    const uint64_t skelCands[] = {
        Offsets::SkeletalMeshAsset,      // 0x720 (dump)
        Offsets::SkeletalMeshAsset_Alt,  // 0x740 (ArcOffsets SKELETAL_MESH)
        0x748,                           // ArcOffsets SKINNED_ASSET
        0x730,                           // dump-side neighbour of 0x720
        0xB10,                           // ArcOffsets SKELETAL_MESH_ALT
    };
    std::vector<uint64_t> skels;
    for (size_t i = 0; i < sizeof(skelCands) / sizeof(skelCands[0]); ++i) {
        const uint64_t p =
            steam_decrypt::MemReadVal<uint64_t>(meshComponent + skelCands[i]);
        if (plausiblePtr(p)) {
            Diag().skelTried.fetch_or(1u << i);
            skels.push_back(p);
        }
    }
    if (skels.empty()) {
        Diag().stage.store(1);
        return false;
    }

    // FMeshBoneInfo layout is contested (ArcOffsets says stride 0x18 /
    // parent +0x08, classic UE is 0x0C). Wrong layouts read garbage name
    // indices, so the NAME PIPELINE validates the layout: a real layout
    // resolves most names and yields a plausible parent chain.
    struct Layout { uint64_t stride; uint64_t parentOff; };
    static const Layout kLayouts[] = {
        { 0x18, 0x08 }, // ArcOffsets::MeshBoneInfo
        { 0x0C, 0x08 }, // classic FMeshBoneInfo (FName + ParentIndex)
        { 0x10, 0x08 },
        { 0x18, 0x10 }, // FString ExportName between Name and ParentIndex
        { 0x18, 0x14 },
        { 0x10, 0x10 },
        { 0x20, 0x18 }, // modern FMeshBoneInfo: FName + FString ExportName + ParentIndex
        { 0x20, 0x10 },
    };

    int bestResolved = -1;
    bool sawHeader = false;
    for (uint64_t skel : skels) {
        for (uint64_t off = kRefSkeletonProbeLo; off <= kRefSkeletonProbeHi; off += 8) {
            struct alignas(8) RawBoneInfoHdr {
                uint64_t data;
                int32_t  count;
                int32_t  max;
            } hdr{};
            if (!steam_decrypt::MemRead(skel + off, &hdr, sizeof(hdr)))
                continue;
            if (hdr.count < 16 || hdr.count > Bones::MaxBoneCount)
                continue;
            if (hdr.max < hdr.count || hdr.max > 2048)
                continue;
            if (!plausiblePtr(hdr.data))
                continue;
            sawHeader = true;

            for (const Layout& lay : kLayouts) {
                std::vector<uint8_t> raw(
                    static_cast<size_t>(hdr.count) * lay.stride);
                if (!steam_decrypt::MemRead(hdr.data, raw.data(), raw.size()))
                    continue;

                std::vector<uint32_t> cis(hdr.count);
                std::vector<int32_t> parents(hdr.count);
                int plausibleParents = 0;
                for (int i = 0; i < hdr.count; ++i) {
                    const uint8_t* e =
                        raw.data() + static_cast<size_t>(i) * lay.stride;
                    uint64_t nameWord = 0;
                    std::memcpy(&nameWord, e, 8);
                    int32_t parent = -1;
                    std::memcpy(&parent, e + lay.parentOff, 4);
                    cis[i] = static_cast<uint32_t>(nameWord);
                    parents[i] = parent;
                    if (parent == -1 ||
                        (parent >= 0 && parent < hdr.count && parent != i))
                        ++plausibleParents;
                }
                if (plausibleParents < hdr.count - 2)
                    continue;

                std::vector<std::string> names(hdr.count);
                int resolved = 0;
                uint32_t roles = 0;
                for (int i = 0; i < hdr.count; ++i) {
                    if (cis[i] == 0 || cis[i] > 0x01000000u)
                        continue;
                    names[i] = resolveName(cis[i], gameBase);
                    if (!names[i].empty())
                        ++resolved;
                    roles |= 1u << static_cast<uint32_t>(
                        ClassifyBoneName(names[i]).role);
                }
                if (resolved > bestResolved) {
                    bestResolved = resolved;
                    Diag().hdrOff.store(static_cast<int>(off));
                    Diag().count.store(hdr.count);
                    Diag().resolved.store(resolved);
                    Diag().roles.store(roles);
                }
                if (resolved < hdr.count / 2)
                    continue;

                std::vector<std::pair<int, UniBone>> map;
                std::vector<int32_t> outParents;
                if (!BuildRoster(names, parents, map, outParents)) {
                    Diag().stage.store(4);
                    continue;
                }

                MapStorage() = std::move(map);
                ParentsStorage() = std::move(outParents);
                ReadyFlag().store(true, std::memory_order_release);
                return true;
            }
        }
    }
    Diag().stage.store(bestResolved >= 0 ? 3 : (sawHeader ? 4 : 2));
    return false;
}

} // namespace BoneRoster
