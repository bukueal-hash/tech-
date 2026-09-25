// BoneRoster suite — pins the name-driven gameIndex -> UniBone mapping that
// replaces the CL-1233465 static index table.
//
// The bug this locks down: static bone INDICES drift with patches. On
// CL-1389382 the "head" index resolved BELOW the pelvis and limbs landed on
// the wrong body parts, because the skeleton had gained bones since the table
// was written. The fix binds body parts to bone NAMES read from
// FReferenceSkeleton, so these tests assert that a SHIFTED skeleton still
// places every joint on its named bone.

#include "tests_main.hpp"
#include "fake_mem.hpp"
#include "Core/BoneRoster.hpp"

#include "doctest/doctest.h"

#include <map>
#include <string>
#include <vector>

namespace {

using BoneRoster::BoneClass;
using BoneRoster::ClassifyBoneName;
using BoneRoster::Role;

int Side(const std::string& name) { return ClassifyBoneName(name).side; }
Role What(const std::string& name) { return ClassifyBoneName(name).role; }

// FMeshBoneInfo: FName Name @ +0x00 (u32 ComparisonIndex + u32 Number),
// ParentIndex @ +0x08 (i32), stride 0x18.
constexpr uint64_t kInfoStride = 0x18;

struct BoneDef {
    const char* name;
    int32_t     parent;
};

std::map<uint32_t, std::string>& TestNames()
{
    static std::map<uint32_t, std::string> s;
    return s;
}

std::string TestResolveName(uint32_t ci, uint64_t)
{
    const auto it = TestNames().find(ci);
    return it == TestNames().end() ? std::string{} : it->second;
}

// The named body-part skeleton at LEGACY CL-1233465 indices (matches BoneID).
std::vector<BoneDef> LegacyDefs()
{
    std::vector<BoneDef> defs(72, { "joint", -1 });
    for (size_t i = 0; i < defs.size(); ++i)
        defs[i].parent = (i == 0) ? -1 : static_cast<int32_t>(i - 1);
    defs[0]  = { "root",      -1 };
    defs[1]  = { "pelvis",     0 };
    defs[2]  = { "spine_01",   1 };
    defs[3]  = { "spine_02",   2 };
    defs[4]  = { "spine_03",   3 };
    defs[5]  = { "chest",      4 };
    defs[6]  = { "neck_01",    5 };
    defs[7]  = { "head",       6 };
    defs[8]  = { "clavicle_l", 5 };
    defs[9]  = { "upperarm_l", 8 };
    defs[10] = { "lowerarm_l", 9 };
    defs[11] = { "hand_l",    10 };
    defs[42] = { "clavicle_r", 5 };
    defs[43] = { "upperarm_r", 42 };
    defs[44] = { "lowerarm_r", 43 };
    defs[45] = { "hand_r",    44 };
    defs[65] = { "thigh_l",    1 };
    defs[66] = { "calf_l",    65 };
    defs[67] = { "foot_l",    66 };
    defs[69] = { "thigh_r",    1 };
    defs[70] = { "calf_r",    69 };
    defs[71] = { "foot_r",    70 };
    return defs;
}

// The same skeleton + 5 leading junk bones: every body part moves +5.
// Index 7 is now "spine_01" — drawing "head" there is the observed bug.
std::vector<BoneDef> ShiftedDefs()
{
    std::vector<BoneDef> defs = {
        { "ik_foot_root", -1 },
        { "weapon_l",      0 },
        { "ik_hand_r",     1 },
        { "twist_spine",   2 },
        { "camera",        3 },
    };
    const std::vector<BoneDef> legacy = LegacyDefs();
    for (const BoneDef& d : legacy) {
        BoneDef shifted = d;
        shifted.parent = (d.parent < 0) ? -1 : d.parent + 5;
        defs.push_back(shifted);
    }
    // Root above the junk chain.
    for (size_t i = 0; i < defs.size(); ++i)
        defs[i].parent = (i == 0) ? -1 : static_cast<int32_t>(i - 1);
    return defs;
}

int FindIndex(const std::vector<std::pair<int, UniBone>>& map, UniBone u)
{
    for (const auto& e : map)
        if (e.second == u)
            return e.first;
    return -1;
}

} // namespace

TEST_CASE("BoneRoster classifies UE-style skeleton names")
{
    CHECK(What("root") == Role::Root);
    CHECK(What("pelvis") == Role::Pelvis);
    CHECK(What("spine_01") == Role::Spine);
    CHECK(ClassifyBoneName("spine_03").order == 3);
    CHECK(What("chest") == Role::Chest);
    CHECK(What("neck_01") == Role::Neck);
    CHECK(What("head") == Role::Head);
    CHECK(What("clavicle_l") == Role::Clavicle);
    CHECK(What("upperarm_l") == Role::UpperArm);
    CHECK(What("lowerarm_l") == Role::ForeArm);
    CHECK(What("forearm_r") == Role::ForeArm);
    CHECK(What("hand_l") == Role::Hand);
    CHECK(What("thigh_r") == Role::Thigh);
    CHECK(What("calf_l") == Role::Calf);
    CHECK(What("foot_r") == Role::Foot);

    CHECK(Side("upperarm_l") == -1);
    CHECK(Side("upperarm_r") == 1);
    CHECK(Side("pelvis") == 0);
    CHECK(Side("head") == 0);
}

TEST_CASE("BoneRoster classifies separator-less (Mixamo-style) names")
{
    CHECK(What("Hips") == Role::Pelvis);
    CHECK(What("Spine1") == Role::Spine);
    CHECK(What("Neck") == Role::Neck);
    CHECK(What("Head") == Role::Head);
    CHECK(What("LeftArm") == Role::UpperArm);
    CHECK(What("RightForeArm") == Role::ForeArm);
    CHECK(What("LeftHand") == Role::Hand);
    CHECK(What("LeftUpLeg") == Role::Thigh);
    CHECK(What("LeftLeg") == Role::Calf);
    CHECK(What("RightFoot") == Role::Foot);

    CHECK(Side("LeftArm") == -1);
    CHECK(Side("RightFoot") == 1);
}

TEST_CASE("BoneRoster never maps IK/twist/finger/attachment bones")
{
    CHECK(What("ik_foot_root") == Role::None);
    CHECK(What("ik_hand_l") == Role::None);
    CHECK(What("hand_ik_r") == Role::None);
    CHECK(What("upperarm_twist_01_l") == Role::None);
    CHECK(What("forearm_roll_l") == Role::None);
    CHECK(What("thumb_01_l") == Role::None);
    CHECK(What("index_01_l") == Role::None);
    CHECK(What("ball_l") == Role::None);
    CHECK(What("weapon_l") == Role::None);
    CHECK(What("weaponsocket") == Role::None);
    CHECK(What("camera") == Role::None);
}

TEST_CASE("BuildRoster reproduces the known CL-1233465 layout")
{
    const std::vector<BoneDef> defs = LegacyDefs();
    std::vector<std::string> names;
    std::vector<int32_t> parents;
    for (const BoneDef& d : defs) {
        names.push_back(d.name);
        parents.push_back(d.parent);
    }

    std::vector<std::pair<int, UniBone>> map;
    std::vector<int32_t> outParents;
    REQUIRE(BoneRoster::BuildRoster(names, parents, map, outParents));

    // When names sit at legacy indices the roster must agree with the
    // fallback table entry for entry.
    const auto& fallback = BoneRoster::FallbackMap();
    REQUIRE(map.size() == fallback.size());
    for (size_t i = 0; i < map.size(); ++i) {
        CHECK(map[i].first == fallback[i].first);
        CHECK(map[i].second == fallback[i].second);
    }
    CHECK(outParents == parents);
}

TEST_CASE("BuildRoster binds body parts by NAME when the skeleton has shifted")
{
    const std::vector<BoneDef> defs = ShiftedDefs();
    std::vector<std::string> names;
    std::vector<int32_t> parents;
    for (const BoneDef& d : defs) {
        names.push_back(d.name);
        parents.push_back(d.parent);
    }

    std::vector<std::pair<int, UniBone>> map;
    std::vector<int32_t> outParents;
    REQUIRE(BoneRoster::BuildRoster(names, parents, map, outParents));

    // Everything moved +5. Index 7 is "spine_01" now — the exact regression:
    // a stale table draws the head at 7, here the head must bind to 12.
    CHECK(FindIndex(map, UniBone::Head) == 12);
    CHECK(FindIndex(map, UniBone::Pelvis) == 6);
    CHECK(FindIndex(map, UniBone::Neck) == 11);
    CHECK(FindIndex(map, UniBone::HandR) == 50);
    CHECK(FindIndex(map, UniBone::FootL) == 72);
    CHECK(FindIndex(map, UniBone::Root) == 5);

    // And nothing bound to a junk/IK bone.
    for (const auto& e : map)
        CHECK(e.first >= 5);
}

TEST_CASE("BuildRoster buckets the spine chain and picks the named chest")
{
    // Five spine bones, no explicit chest: Chest takes the topmost spine.
    std::vector<std::string> names = {
        "root", "pelvis", "spine_01", "spine_02", "spine_03", "spine_04",
        "spine_05", "neck_01", "head",
        "clavicle_l", "upperarm_l", "lowerarm_l", "hand_l",
        "clavicle_r", "upperarm_r", "lowerarm_r", "hand_r",
        "thigh_l", "calf_l", "foot_l", "thigh_r", "calf_r", "foot_r",
    };
    std::vector<int32_t> parents(names.size());
    for (size_t i = 0; i < names.size(); ++i)
        parents[i] = (i == 0) ? -1 : static_cast<int32_t>(i - 1);

    std::vector<std::pair<int, UniBone>> map;
    std::vector<int32_t> outParents;
    REQUIRE(BoneRoster::BuildRoster(names, parents, map, outParents));

    CHECK(FindIndex(map, UniBone::Spine1) == 2);
    CHECK(FindIndex(map, UniBone::Spine2) == 3);
    CHECK(FindIndex(map, UniBone::Spine3) == 4);
    CHECK(FindIndex(map, UniBone::Chest) == 6); // spine_05 (topmost)
    CHECK(FindIndex(map, UniBone::Head) == 8);
}

TEST_CASE("BuildRoster rejects rosters that cannot cover the drawn joints")
{
    std::vector<BoneDef> defs = LegacyDefs();
    defs[7].name = "glorp"; // no bone word anywhere -> Head can never bind
    std::vector<std::string> names;
    std::vector<int32_t> parents;
    for (const BoneDef& d : defs) {
        names.push_back(d.name);
        parents.push_back(d.parent);
    }

    std::vector<std::pair<int, UniBone>> map;
    std::vector<int32_t> outParents;
    CHECK_FALSE(BoneRoster::BuildRoster(names, parents, map, outParents));

    // Too short to be a body.
    std::vector<std::string> tiny(8, "pelvis");
    CHECK_FALSE(BoneRoster::BuildRoster(tiny, std::vector<int32_t>(8, -1),
                                        map, outParents));
}

TEST_CASE("TryCalibrate installs a roster from RefSkeleton memory")
{
    ScopedFakeMem fm;
    BoneRoster::ResetRosterForTest();
    TestNames().clear();

    const uint64_t kMeshComp = 0x2000000;
    const uint64_t kSkel     = 0x3000000;
    const uint64_t kEntries  = 0x4000000;
    const uint64_t kBase     = 0x1000000;

    const std::vector<BoneDef> defs = ShiftedDefs();
    const int32_t count = static_cast<int32_t>(defs.size());

    fm.mem.writeU64(kMeshComp + Offsets::SkeletalMeshAsset, kSkel);

    // RawBoneInfo TArray header at a probed RefSkeleton slot.
    fm.mem.writeU64(kSkel + 0x180, kEntries);
    fm.mem.writeU32(kSkel + 0x188, static_cast<uint32_t>(count));
    fm.mem.writeU32(kSkel + 0x18C, static_cast<uint32_t>(count));

    for (int32_t i = 0; i < count; ++i) {
        const uint64_t ci = static_cast<uint64_t>(100 + i);
        TestNames()[static_cast<uint32_t>(ci)] = defs[i].name;
        fm.mem.writeU32(kEntries + i * kInfoStride, static_cast<uint32_t>(ci));
        fm.mem.writeU32(kEntries + i * kInfoStride + 4, 0); // FName Number
        fm.mem.writeU32(kEntries + i * kInfoStride + 8,
                        static_cast<uint32_t>(defs[i].parent));
    }

    REQUIRE(BoneRoster::TryCalibrate(kMeshComp, kBase, &TestResolveName));
    CHECK(BoneRoster::IsCalibrated());

    const auto& map = BoneRoster::GameBoneMap();
    CHECK(FindIndex(map, UniBone::Head) == 12);   // NOT the legacy 7
    CHECK(FindIndex(map, UniBone::Pelvis) == 6);
    CHECK(FindIndex(map, UniBone::FootR) == 76);

    const std::vector<int32_t>* parents = BoneRoster::GameBoneParents();
    REQUIRE(parents != nullptr);
    REQUIRE(parents->size() == defs.size());
    CHECK((*parents)[12] == 11);

    // Idempotent once installed.
    CHECK(BoneRoster::TryCalibrate(kMeshComp, kBase, &TestResolveName));
}

TEST_CASE("TryCalibrate keeps the static fallback when names do not resolve")
{
    ScopedFakeMem fm;
    BoneRoster::ResetRosterForTest();
    TestNames().clear();

    const uint64_t kMeshComp = 0x2000000;
    const uint64_t kSkel     = 0x3000000;
    const uint64_t kEntries  = 0x4000000;
    const std::vector<BoneDef> defs = ShiftedDefs();
    const int32_t count = static_cast<int32_t>(defs.size());

    fm.mem.writeU64(kMeshComp + Offsets::SkeletalMeshAsset, kSkel);
    fm.mem.writeU64(kSkel + 0x180, kEntries);
    fm.mem.writeU32(kSkel + 0x188, static_cast<uint32_t>(count));
    fm.mem.writeU32(kSkel + 0x18C, static_cast<uint32_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        fm.mem.writeU32(kEntries + i * kInfoStride,
                        static_cast<uint32_t>(100 + i)); // names stay empty
        fm.mem.writeU32(kEntries + i * kInfoStride + 8,
                        static_cast<uint32_t>(defs[i].parent));
    }

    CHECK_FALSE(BoneRoster::TryCalibrate(kMeshComp, 0x1000000, &TestResolveName));
    CHECK_FALSE(BoneRoster::IsCalibrated());
    CHECK(BoneRoster::GameBoneMap().data() == BoneRoster::FallbackMap().data());
}
