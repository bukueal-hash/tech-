#include "../Core/Engine.h"
#include <cmath>
#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <stdint.h>
#include <stdlib.h>

#include <Windows.h>
#include <vector>

#include "../Interface/Utils/Helper/helper.hpp"
#include "../Core/Cache.hpp"
#include "../Core/Memory.h"

void Engine::UpdateCamera()
{
	if (!PlayerCameraManager) return;

	auto cam =
		Memory::read<FMinimalViewInfo>(
			PlayerCameraManager + Offsets::ViewTargetTarget + 0x10
		);

	g_Camera.Location = cam.Location;
	g_Camera.Rotation = cam.Rotation;
	g_Camera.FOV = cam.FOV;
}

bool Engine::ProjectWorldLocationToScreen(
	Vector3 WorldLocation,
	Vector3& screen
)
{
	if (WorldLocation.x == 0.0 &&
		WorldLocation.y == 0.0 &&
		WorldLocation.z == 0.0)
		return false;

	auto CameraInfo = g_Camera;

	if (CameraInfo.FOV <= 1.0f || CameraInfo.FOV > 179.0f)
		return false;

	Vector3 Delta = WorldLocation - CameraInfo.Location;

	double distance =
		sqrt(Delta.x * Delta.x + Delta.y * Delta.y + Delta.z * Delta.z);

	if (distance < 1.0)
		return false;

	constexpr double Pi = 3.14159265358979323846;

	double Yaw = CameraInfo.Rotation.y * Pi / 180.0;
	double Pitch = CameraInfo.Rotation.x * Pi / 180.0;

	Vector3 Forward(
		cos(Pitch) * cos(Yaw),
		cos(Pitch) * sin(Yaw),
		sin(Pitch)
	);

	Vector3 Right(
		-sin(Yaw),
		cos(Yaw),
		0.0
	);

	Vector3 Up(
		-sin(Pitch) * cos(Yaw),
		-sin(Pitch) * sin(Yaw),
		cos(Pitch)
	);

	double CamX = Delta.Dot(Right);
	double CamY = Delta.Dot(Up);
	double CamZ = Delta.Dot(Forward);

	if (CamZ <= 1.0)
		return false;

	double Aspect =
		(double)GetSystemMetrics(SM_CXSCREEN) /
		(double)GetSystemMetrics(SM_CYSCREEN);

	double FovRad = CameraInfo.FOV * Pi / 180.0;
	double TanHalfFov = tan(FovRad / 2.0);

	if (TanHalfFov < 0.001)
		return false;

	screen.x =
		(GetSystemMetrics(SM_CXSCREEN) * 0.5) +
		(CamX / (CamZ * TanHalfFov * Aspect)) *
		(GetSystemMetrics(SM_CXSCREEN) * 0.5);

	screen.y =
		(GetSystemMetrics(SM_CYSCREEN) * 0.5) -
		(CamY / (CamZ * TanHalfFov)) *
		(GetSystemMetrics(SM_CYSCREEN) * 0.5);

	return true;
}

bool Engine::ProjectWorldLocationToRadar(
	const Vector3& myWorldLocation,
	const Vector3& enemyWorldLocation,
	float myYaw,
	Vector3& outRadar)
{
	Vector3 delta = enemyWorldLocation - myWorldLocation;
	delta.z = 0.0;

	float distance = sqrtf(delta.x * delta.x + delta.y * delta.y);

	const float radarRange = 80.0f;

	if (distance > radarRange)
	{
		delta.x *= radarRange / distance;
		delta.y *= radarRange / distance;
		distance = radarRange;
	}

	float yawRad = DegToRad(myYaw);

	float cosYaw = cosf(yawRad);
	float sinYaw = sinf(yawRad);

	float forward = delta.x * cosYaw + delta.y * sinYaw;
	float right = -delta.x * sinYaw + delta.y * cosYaw;

	outRadar.x = right / radarRange;
	outRadar.y = forward / radarRange;

	outRadar.z = distance / radarRange;

	return true;
}

bool Engine::Visible(uintptr_t mesh)
{
    if (!mesh)
        return false;

    float lastSubmitTime = Memory::read<float>(mesh + Offsets::LastSubmitTime);
    float lastRenderTime = Memory::read<float>(mesh + Offsets::LastSubmitTime + sizeof(float));
    float lastRenderTimeOnScreen = Memory::read<float>(mesh + Offsets::LastSubmitTime + sizeof(float) * 2);

    bool isOnScreen = (lastRenderTime == lastRenderTimeOnScreen);
    bool isRecent = (lastSubmitTime - lastRenderTime) <= 0.06f;

    return isOnScreen && isRecent;
}

bool Engine::Has(const std::string& s, const char* sub)
{
    return s.find(sub) != std::string::npos;
}

std::uintptr_t Engine::GetBoneArrayDecrypt(std::uintptr_t skeletalmesh)
{
    if (!skeletalmesh || !IsValidPointer(skeletalmesh))
        return 0;

    constexpr uintptr_t offset_encrypted = 0x790;      // a1[121] * 16
    constexpr uintptr_t offset_lod_data = 0x7F0;      // a1[127] * 16
    constexpr uintptr_t offset_bone_count = 0xAE0;      // a1[174] * 16
    constexpr uintptr_t SHUFFLE_MASK_RVA = 0xAA4DA00;  // xmmword_AA4DA00

    __m128i encrypted{};
    Memory::ReadRaw(skeletalmesh + offset_encrypted, &encrypted, sizeof(__m128i));

    uint64_t low = _mm_cvtsi128_si64(encrypted);
    uint64_t high = _mm_extract_epi64(encrypted, 1);
    if (!low && !high)
        return 0;

    __m128i v3 = _mm_shufflelo_epi16(encrypted, 27);

    __m128i rotated = _mm_or_si128(_mm_slli_epi16(v3, 13), _mm_srli_epi16(v3, 3));

    static __m128i shuffle_mask{};
    static bool    mask_read = false;
    if (!mask_read)
    {
        Memory::ReadRaw(Memory::getBaseAddress() + SHUFFLE_MASK_RVA, &shuffle_mask, sizeof(__m128i));
        mask_read = true;
    }

    __m128i  decrypted = _mm_shuffle_epi8(rotated, shuffle_mask);
    uint64_t bone_array_base = static_cast<uint64_t>(_mm_cvtsi128_si64(decrypted));
    if (!bone_array_base || !IsValidPointer(bone_array_base))
        return 0;

    uint32_t lod_raw = Memory::read<uint32_t>(skeletalmesh + offset_lod_data);
    uint32_t idx = (lod_raw >> 27) & 0xFFFFFFF0;

    uint64_t final_address = bone_array_base + idx + 104;
    if (!IsValidPointer(final_address))
        return 0;

    uint64_t bone_array_ptr = Memory::read<uint64_t>(final_address);
    if (!bone_array_ptr || !IsValidPointer(bone_array_ptr))
        return 0;

    return bone_array_ptr;
}

uintptr_t Engine::GetGameInstance(uint64_t uworldAddr)
{
    if (!uworldAddr) return 0;

    __m128i encrypted = Memory::read<__m128i>(uworldAddr + 0x2d0);
    __m128i mask = Memory::read<__m128i>(Memory::getBaseAddress() + 0xb09c350);

    __m128i shuffled = _mm_shufflelo_epi16(encrypted, 0x93);
    __m128i rotated = _mm_or_si128(_mm_slli_epi16(shuffled, 1), _mm_srli_epi16(shuffled, 15));

    return _mm_extract_epi64(_mm_shuffle_epi8(rotated, mask), 0);
}

uintptr_t Engine::GetCameraManagerFromActors()
{
	uintptr_t actors_ptr = Memory::read<uintptr_t>(PersistentLevel + Offsets::AActors);
	int actor_count = Memory::read<int>(PersistentLevel + (Offsets::AActors + sizeof(uintptr_t)));
	if (!actors_ptr || actor_count == 0) return 0;

	uintptr_t local_pawn = Memory::read<uintptr_t>(PlayerController + Offsets::AcknowledgedPawn);

	for (int i = 0; i < actor_count; i++) {
		uintptr_t actor = Memory::read<uintptr_t>(actors_ptr + (i * sizeof(uintptr_t)));
		if (!actor) continue;

		uintptr_t view_target_target = Memory::read<uintptr_t>(actor + Offsets::ViewTargetTarget);

		if (view_target_target == PlayerController || (local_pawn && view_target_target == local_pawn)) {
			return actor;
		}
	}

	return 0;
}

bool Engine::getAllowType(const std::string& actorName)
{
    if (actorName == "Loot Item")
        return var::droppedItems;

    if (actorName == "Raider stock")
        return var::raiderStock;

    if (robotsList.contains(actorName))
        return var::showRobots;

    if (actorName == "Arc Cargoship")
        return var::showArc;

    if (actorName == "Corpse")
        return var::showDeadPlayers;

    return false;
}

std::string Engine::GetWeaponName(const std::string& internal_name) {
    static const std::unordered_map<std::string, std::string> processMap = {     
        { "Pneumatic_01",                     "Kettle" },
        { "AssaultRifle_LowTier_01",           "Rattler" },
        { "Burst_01",                          "Arpeggio" },
        { "Heavy_01",                          "Bettina" },
        { "AssaultRifle_Bullpup_01_C",         "Tempest" },
        { "SMG_LowTier_01",                    "Stitcher" },
        { "SMG_02",                            "Bobcat" },
        { "PumpAction_01",                     "Il Toro" },
        { "Shotgun_SemiAuto_01",               "Vulcano" },
        { "Sniper_BoltAction_01",              "Osprey" },
        { "Sniper_Energy_01",                  "Jupiter" },
        { "Pistol_Silenced_01",                "Hairpin" },
        { "Pistol_01",                         "Burletta" },
        { "SingleAction_01",                   "Anvil" },
        { "Pistol_HighPower_01",               "Venator" },
        {"Pistol_SemiAuto_02",                  "Venator"},
        { "LMG_Standard_01",                   "Torrente" },
        { "BreachAction_01",                   "Ferro" },
        { "LeverAction_01",                    "Renegade" },
        { "AssaultRifle_Bullpup_01",           "Arpeggio" },
        { "Launcher_AntiArc_Medium_01_C",      "Hullcracker" },
        { "LMG_Medium_01",                     "Torrente" },
        { "Special_BeamRifle_01",              "Equalizer" },
        { "Beam_01",                           "Equalizer"},
        { "SniperRifle_BoltAction_Medium_01",  "Osprey" },

        { "BasicMeleeWeapon",                  "Melee" },
        { "HealingHoT_Small",                  "Bandage" },
        { "HealingHoT_Improvised",             "Herbal Bandage" },
        { "AdrenalineShot",                    "Adrenaline Shot" },
        { "Consumable_ShieldOverTimePack",     "Shield Recharge" },
        { "Defibrillator",                     "Defibrillator" },
        { "JumpMine_Impulse",                  "Impulse Mine" },
        { "SmokeGrenade",                      "Smoke Grenade" },
        { "ScatterMissileGrenade",             "Wolfpack Grenade" },
        { "StunGrenade",                       "Showstopper Grenade" }
    };

    if (auto it = processMap.find(internal_name); it != processMap.end()) {
        return it->second;
    }

    for (const auto& survivor : processMap) {
        if (internal_name.find(survivor.first) != std::string::npos) {
            return survivor.second;
        }
    }

    return "Unarmed";
}

std::string Engine::getEntityType(const std::string& actorName)
{
    static const std::unordered_map<std::string, std::string> processMap = {
        {"c_snitchbot", "Snitch"},
        {"c_turretenemy", "Turret"},
        {"c_bullcrab", "Leaper"},
        {"c_peppermint", "The Queen"},
        {"c_spearmint", "Matriarch"},
        {"c_heavydrone", "Rocketeer"},
        {"bp_arc_cargoship", "Arc Cargoship"},

        {"c_chonkplatform_mortar", "Bombardier"},
        {"c_chonkplatform", "Bastion"},
        {"c_chonk", "Bastion"},

        {"pinger", "Spotter"},
        {"c_pinger", "Spotter"},
        {"c_sniper", "Sentinel"},
        {"c_runner", "Shredder"},

        {"c_lightdrone_elite", "Hornet"},
        {"c_lightdrone", "Wasp"},
        {"c_rollbot_flame", "Fireball"},
        {"c_rollbot_pop", "Pop"},
        {"c_rollbot_01_blockout", "Surveyor"},

        {"c_rollbot_boom_c", "Comet"},
        {"c_elitedrone_flamethrower_c", "Firefly"},

        {"bp_raidercache", "Raider stock"},
        {"bp_pioneercharacter", "Corpse"},

        { "bp_pickupbase", "Loot Item" },
        { "pickup", "Loot Item" },
        { "container", "Loot Item" },
    };

    const std::string actorNameLower = toLower(actorName);

    if (auto it = processMap.find(actorNameLower); it != processMap.end()) {
        return it->second;
    }

    for (const auto& survivor : processMap) {
        if (actorNameLower.find(survivor.first) != std::string::npos) {
            return survivor.second;
        }
    }

    return "Invalid";
}


inline std::string utf16_to_utf8(const uint16_t* data, size_t len) {
    if (!data || len == 0) return std::string();

    std::wstring wstr(reinterpret_cast<const wchar_t*>(data), len);
    if (wstr.empty()) return std::string();

    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return std::string();

    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], size_needed, nullptr, nullptr);
    return result;
}

std::string Engine::GetEnglishItemName(uint64_t actor)
{
    if (!actor)
        return "";

    uint64_t hover_base = actor + 0x550;

    uint64_t l0 = Memory::read<uint64_t>(hover_base);
    if (!l0 || !IsValidPointer(l0))
        goto fallback;
    {
        uint64_t l1 = Memory::read<uint64_t>(l0 + 0x10);
        if (!IsValidPointer(l1)) goto fallback;

        uint64_t key_ptr = Memory::read<uint64_t>(l1 + 0x18);
        if (!key_ptr || !IsValidPointer(key_ptr)) goto fallback;

        uint16_t kbuf[64] = {};
        Memory::ReadRaw(key_ptr, kbuf, sizeof(kbuf));
        std::string loc_key;
        for (int i = 0; i < 63 && kbuf[i]; ++i)
            loc_key += (char)(kbuf[i] & 0x7F);

        if (loc_key.find("ST_") != 0 && loc_key.find("ID_") != 0)
            goto fallback;

        uint64_t l2 = Memory::read<uint64_t>(l1 + 0x30);
        if (!IsValidPointer(l2)) goto fallback;

        uint64_t wstr = Memory::read<uint64_t>(l2 + 0x10);
        if (!IsValidPointer(wstr)) goto fallback;

        uint16_t buf[128] = {};
        Memory::ReadRaw(wstr, buf, sizeof(buf));
        std::string result;
        for (int i = 0; i < 127 && buf[i]; ++i) {
            uint16_t c = buf[i];
            if (c < 0x80) result += (char)c;
            else if (c < 0x800) {
                result += (char)(0xC0 | (c >> 6));
                result += (char)(0x80 | (c & 0x3F));
            }
            else {
                result += (char)(0xE0 | (c >> 12));
                result += (char)(0x80 | ((c >> 6) & 0x3F));
                result += (char)(0x80 | (c & 0x3F));
            }
        }
        if (result.length() >= 2)
            return result;
    }

fallback:
    {
        uint64_t da = Memory::read<uint64_t>(hover_base + 0x20);
        if (!da || !IsValidPointer(da))
            return "";

        std::string name = GetActorFNameString(da);
        for (const char* p : { "DA_", "WID_", "BP_", "Item_" }) {
            if (name.find(p) == 0) { name.erase(0, strlen(p)); break; }
        }
        for (auto& c : name) if (c == '_') c = ' ';
        if (!name.empty() && name.find("Default__") == std::string::npos)
            return name;
    }

    return "";
}

// ---- game offsets -----------------------------------------------------------

// ─────────────────────────────────────────────────────────────────────────────
// Scalar helpers
// ─────────────────────────────────────────────────────────────────────────────
static inline uint32_t fn_rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static inline uint64_t fn_rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }
static inline uint64_t fn_bswap64(uint64_t x) { return _byteswap_uint64(x); }

static inline uint64_t u64_lo_xmm(__m128i v) {
    uint64_t arr[2];
    _mm_storeu_si128(reinterpret_cast<__m128i*>(arr), v);
    return arr[0];
}
static inline uint32_t u32_lo_xmm(__m128i v) {
    return static_cast<uint32_t>(_mm_cvtsi128_si32(v));
}

// ─────────────────────────────────────────────────────────────────────────────
// Constants  (IDA RVAs; binary rebased to 0)
// ─────────────────────────────────────────────────────────────────────────────

// FNamePool globals
static constexpr uint64_t FNAME_GNAMES_BASE_OFF = 0xD92C900;              // ← NEW (was 0xD852900)
static constexpr uint64_t FNAME_KEY_TABLE_BASE_OFF = 0xD8707F4;              // ← NEW (was 0xD7967F4)
static constexpr uint64_t FNAME_KEY_TABLE_OFF = FNAME_KEY_TABLE_BASE_OFF + 0x98; // ← NEW (+76*2)

// ── Actor FName slot decrypt (xmmword_AB16AF0 / xmmword_AB16B00) ─────────────
alignas(16) static const uint8_t FNAME_ACTOR_SHUF[16] = {   // ← NEW
    0x06,0x05,0x03,0x01, 0x02,0x07,0x00,0x04,
    0x00,0x00,0x00,0x00, 0x00,0x00,0x00,0x00
};
static constexpr uint64_t FNAME_ACTOR_XOR = 0x4882C8C849A43F3BULL; // ← NEW

// ── Slot hash ─────────────────────────────────────────────────────────────────
static constexpr uint32_t FNAME_HASH_PRIME = 16777619u;     // confirmed unchanged
static constexpr uint32_t FNAME_HASH_ADD = 1887672108u;   // ← NEW (was 726292215)
static constexpr uint32_t FNAME_SLOT_XOR_CONST = 26836u;        // ← NEW

// ── ComputeLocation SIMD (replaces old FNAME_GIDX_* family entirely) ─────────
// xmmword_AB4F790 – "blend fill" (used where R=0, and also in sub_234070 step)
alignas(16) static const uint8_t FNAME_GIDX_ROL3_BLEND_FILL[16] = {  // ← NEW
    0xD8,0x49,0x03,0x4E, 0x7E,0x24,0x4D,0x67,
    0xD8,0x49,0x03,0x4E, 0x7E,0x24,0x4D,0x67
};
// xmmword_AB4F830 – "blend mask" (used where R=1)
alignas(16) static const uint8_t FNAME_GIDX_ROL3_BLEND_MASK[16] = {  // ← NEW
    0x27,0xB6,0xFC,0xB1, 0x81,0xDB,0xB2,0x98,
    0x27,0xB6,0xFC,0xB1, 0x81,0xDB,0xB2,0x98
};
// sub_234070 intermediate XOR applied to rol64(v18,61) before the extract step
// = lo64 of the inline __m128i literal 0x0CE9A48F00000000 (hi64=0)
static constexpr uint64_t FNAME_GIDX_STEP5_XOR = 0x0CE9A48F00000000ULL;   // ← NEW
// lo32 of xmmword_AB4F8F0 = 0xA48F0CE9  (bytes: E9 0C 8F A4)
static constexpr uint32_t FNAME_GIDX_EXTRACT_XOR = 0xA48F0CE9u;  // ← NEW

// ── Block decrypt XOR key (xmmword_AB4F7E0) ──────────────────────────────────
alignas(16) static const uint8_t FNAME_BLOCK_XOR_KEY[16] = {   // ← NEW
    0xEC,0x79,0xDB,0x6C, 0x3B,0x36,0x73,0x86,
    0xEC,0x79,0xDB,0x6C, 0x3B,0x36,0x73,0x86
};
// NOTE: FNAME_BLOCK_POST_XOR removed – no longer in new decrypt scheme

// ── FNV chain (sub_233720) ────────────────────────────────────────────────────
static constexpr uint64_t FNAME_FNV_PRIME = 0x100000001B3ULL;       // unchanged
static constexpr uint64_t FNAME_FNV_ADDITIVE = 0x5BD1CD23B605810AULL;  // ← NEW + now SUBTRACTED
static constexpr int       FNAME_FNV_ROL1 = 35;                     // ← NEW (was 30)
static constexpr int       FNAME_FNV_ROL2 = 51;                     // unchanged

// ── Chunk layout (sub_233720) ─────────────────────────────────────────────────
static constexpr uint32_t FNAME_CHUNK_HASH_SEED_OFF = 9472u;   // ← NEW (was 25936 / 0x6550)
static constexpr uint32_t FNAME_CHUNK_BLOCK_BASE_OFF = 9488u;   // ← NEW (was 25952 / 0x6560)
static constexpr uint32_t FNAME_BHASH_ADD = 1198831798u; // ← NEW (was 2855900422)

// ── Pointer XOR chain (sub_233720 → sub_234070 → sub_228D60) ─────────────────
static constexpr uint64_t FNAME_PTR_XOR_1 = 0x00000000DFCE6F64ULL; // ← NEW (was 0x8D45A629)
static constexpr uint64_t FNAME_PTR_XOR_2 = 0x008D6C5E00000000ULL; // ← NEW (was 0x0058B4AF00000000)
static constexpr uint64_t FNAME_PTR_XOR_3 = 0x64E2A28100000000ULL; // ← NEW (was 0x29FEF12200000000)

// ── Name string key schedule (sub_237070) ────────────────────────────────────
//   Old: key += 96 (linear), secondary = (key+48)&0x3F
//   New: key = -111*key+108 (nonlinear), secondary = (-87*key+94)&0x3F
static constexpr int FNAME_KEY_INIT_ADD = 87;   // ← NEW (was 64)
static constexpr int FNAME_KEY_SECONDARY_MULT = -87;   // ← NEW
static constexpr int FNAME_KEY_SECONDARY_ADD = 94;   // ← NEW
static constexpr int FNAME_KEY_STEP_MULT = -111;  // ← NEW
static constexpr int FNAME_KEY_STEP_ADD = 108;  // ← NEW

// ─────────────────────────────────────────────────────────────────────────────
// FNameDecryptor
// ─────────────────────────────────────────────────────────────────────────────
class FNameDecryptor {
public:
    FNameDecryptor(uint64_t module_base)
        : m_base(module_base), m_keyLoaded(false)
    {
        memset(m_keyTable, 0, sizeof(m_keyTable));
    }

    // Must be called once before GetName / GetCompIndex.
    bool Init() {
        if (m_keyLoaded) return true;
        uint64_t kt_addr = m_base + FNAME_KEY_TABLE_OFF;  // ← new offset
        if (!Memory::read(kt_addr, m_keyTable, 64 * sizeof(uint16_t)))
            return false;
        if (!m_keyTable[0] && !m_keyTable[1])
            return false;
        m_keyLoaded = true;
        return true;
    }

    bool IsInitialized() const { return m_keyLoaded; }

    // ── Step 1: slot index from object address ─────────────────────────────
    // Source: sub_34776A@0x347c04, sub_4AF870@0x4af8c3   ← FULLY NEW formula
    uint32_t GetSlotIndex(uint64_t obj_base) const {
        uint64_t addr = obj_base + 0x10;
        uint32_t lo = static_cast<uint32_t>(addr);
        uint32_t hi = static_cast<uint32_t>(addr >> 32);

        uint32_t h0 = FNAME_HASH_PRIME * fn_rotl32(lo, 25) - FNAME_HASH_ADD;
        uint32_t h1 = FNAME_HASH_PRIME * (h0 >> 3);
        uint32_t h2 = FNAME_HASH_PRIME * ((h1 + hi - FNAME_HASH_ADD) >> 7) - FNAME_HASH_ADD;
        uint32_t v = FNAME_HASH_PRIME * (h2 >> 3);

        return ((static_cast<uint8_t>(v) ^ static_cast<uint8_t>((v + FNAME_SLOT_XOR_CONST) >> 16)) & 3u) ^ 2u;
    }

    // ── Step 2: decrypt FName slot → comp_index ───────────────────────────
    // Source: sub_34776A@0x347cbc, sub_4AF870@0x4af8f8   ← SIMD sequence changed
    //   Old: shuffle(8-byte mask) → rol32(17,per-dword) → XOR64
    //   New: rol64(55,per-qword) → shuffle(8-byte mask) → XOR64 → rol64(32)
    int32_t GetCompIndex(uint64_t obj_base) {
        uint32_t slot = GetSlotIndex(obj_base);
        uint64_t addr = obj_base + 0x20 + static_cast<uint64_t>(slot) * 0x20;

        alignas(16) uint8_t enc[16] = {};
        if (!Memory::read(addr, enc, 16))
            return 0;

        __m128i v = _mm_load_si128(reinterpret_cast<const __m128i*>(enc));

        // 1) rol64 by 55 per 64-bit lane  (= ror64 by 9)
        __m128i rotated = _mm_or_si128(_mm_slli_epi64(v, 55), _mm_srli_epi64(v, 9));
        // 2) pshufb with 8-byte mask (upper 8 bytes of mask = 0 → copy byte 0)
        __m128i shuf_m = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(FNAME_ACTOR_SHUF));
        __m128i shuffled = _mm_shuffle_epi8(rotated, shuf_m);
        // 3) XOR with 8-byte key, take lo64
        __m128i xor_v = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&FNAME_ACTOR_XOR));
        uint64_t raw = u64_lo_xmm(_mm_xor_si128(shuffled, xor_v));
        // 4) swap hi/lo 32-bit halves, return lo32 as comp_index
        uint64_t res = fn_rotl64(raw, 32);
        return static_cast<int32_t>(res & 0xFFFFFFFF);
    }

    // ── Step 3: comp_index → chunk + name_entry_byte_offset ───────────────
    // Source: sub_228D60 (blend) → sub_234070 (intermediate XOR chain) →
    //         sub_233720 (extract + FNV).
    //
    // Previous rev missed the sub_234070 intermediate transform entirely and
    // had the chunk_offset bit-extraction shifted one byte too low.  ← FIXED
    struct GNamesLoc {
        uint32_t chunk_offset;        // byte offset from gnames_base to chunk start
        uint64_t name_entry_offset;   // byte offset (2*v6) added to FNV result
    };

    GNamesLoc ComputeLocation(int32_t comp_index) const {
        // ── sub_228D60: blend step ────────────────────────────────────────
        // Expand comp_index via shufflelo_epi16(cvtsi32(ci), 20) → {w0,w1,w1,w0}
        __m128i ci = _mm_cvtsi32_si128(comp_index);
        __m128i v3 = _mm_shufflelo_epi16(ci, 20);

        // R = rol64(v3, 3) per 64-bit lane
        __m128i R = _mm_or_si128(_mm_srli_epi64(v3, 61), _mm_slli_epi64(v3, 3));

        // v18 = (R & BLEND_MASK) | (~slli3(v3) & BLEND_FILL)
        // Note: at bit positions 0-2, slli3=0 so ~slli3=1 → always BLEND_FILL bits;
        //       BLEND_MASK bits 0-2 are all 1 and BLEND_FILL bits 0-2 are all 0,
        //       so the formula is equivalent to (R & MASK) | (~R & FILL) there too.
        __m128i slli3 = _mm_slli_epi64(v3, 3);
        __m128i bmask = _mm_load_si128(reinterpret_cast<const __m128i*>(FNAME_GIDX_ROL3_BLEND_MASK));
        __m128i bfill = _mm_load_si128(reinterpret_cast<const __m128i*>(FNAME_GIDX_ROL3_BLEND_FILL));
        __m128i v18 = _mm_or_si128(_mm_and_si128(R, bmask), _mm_andnot_si128(slli3, bfill));

        // ── sub_234070: intermediate XOR chain ───────────────────────────
        // a) ror64(v18, 3)  [= slli64(v18,61) | srli64(v18,3)]
        __m128i ror3_v18 = _mm_or_si128(_mm_slli_epi64(v18, 61), _mm_srli_epi64(v18, 3));
        // b) XOR lo64 with FNAME_GIDX_STEP5_XOR (hi64 of literal = 0)
        __m128i xc1 = _mm_xor_si128(ror3_v18,
            _mm_loadl_epi64(reinterpret_cast<const __m128i*>(&FNAME_GIDX_STEP5_XOR)));
        // c) rol64(xc1, 3)  [= slli64(xc1,3) | srli64(xc1,61)]
        //    then XOR with BLEND_FILL  → v9
        __m128i v9 = _mm_xor_si128(
            _mm_or_si128(_mm_slli_epi64(xc1, 3), _mm_srli_epi64(xc1, 61)),
            bfill);

        // ── sub_233720: extract v5 from v9 ──────────────────────────────
        // ror64(v9, 3), shufflelo(235), XOR lo32 with EXTRACT_XOR
        __m128i rot61 = _mm_or_si128(_mm_slli_epi64(v9, 61), _mm_srli_epi64(v9, 3));
        __m128i shuf = _mm_shufflelo_epi16(rot61, 235);
        uint32_t v5 = u32_lo_xmm(_mm_xor_si128(shuf,
            _mm_cvtsi32_si128(static_cast<int>(FNAME_GIDX_EXTRACT_XOR))));

        // chunk_offset = bits[31:16] of v5 placed at byte positions [2:1]
        // = (v5 >> 8) & 0x00FFFF00   (IDA: v7 = &gnames + ((v5>>8) & 0xFFFF00))
        // FIXED: previous rev wrongly used v5 & 0x00FFFF00 (bits 8-23)
        GNamesLoc loc;
        loc.chunk_offset = (v5 >> 8) & 0x00FFFF00u;
        loc.name_entry_offset = static_cast<uint64_t>(static_cast<uint16_t>(v5)) * 2u;
        return loc;
    }

    // ── Step 4: block index for a chunk address ────────────────────────────
    // Source: sub_233720@0x23385a   ← FULLY NEW
    uint8_t ComputeBlockIdx(uint64_t chunk_addr) const {
        uint64_t seed = chunk_addr + FNAME_CHUNK_HASH_SEED_OFF;
        uint32_t lo = static_cast<uint32_t>(seed);
        uint32_t hi = static_cast<uint32_t>(seed >> 32);

        uint32_t s0 = FNAME_HASH_PRIME * fn_rotl32(lo, 20) + FNAME_BHASH_ADD;
        uint32_t s1 = FNAME_HASH_PRIME * fn_rotl32(s0, 28);
        uint32_t s2 = hi + s1 + FNAME_BHASH_ADD;
        uint32_t s3 = FNAME_HASH_PRIME * fn_rotl32(s2, 20) + FNAME_BHASH_ADD;
        uint32_t v8 = FNAME_HASH_PRIME * (s3 >> 4) + FNAME_BHASH_ADD;

        return static_cast<uint8_t>(
            (static_cast<uint8_t>(v8) ^ static_cast<uint8_t>(v8 >> 16)) & 7u);
    }

    // ── Step 5: decrypt one 128-bit block → 64-bit value ──────────────────
    // Source: sub_233720@0x233877   ← FULLY NEW
    //   Old: shufflelo(0x39) → XOR(key16) → rol32(9,per-dword) → post-XOR64
    //   New: XOR(AB4F7E0_128) → rol64(42,per-qword) → shufflelo(27)→lo64
    uint64_t DecryptBlock(const uint8_t raw[16]) const {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(raw));
        __m128i bkey = _mm_load_si128(reinterpret_cast<const __m128i*>(FNAME_BLOCK_XOR_KEY));
        __m128i xrd = _mm_xor_si128(v, bkey);
        // rol64 by 42 per 64-bit lane
        __m128i rot = _mm_or_si128(_mm_slli_epi64(xrd, 42), _mm_srli_epi64(xrd, 22));
        // shufflelo_epi16(rot, 27): 27=0b00011011→{w3,w2,w1,w0} (reverse lo words)
        return u64_lo_xmm(_mm_shufflelo_epi16(rot, 27));
    }

    // ── Step 6: resolve comp_index → live name_entry pointer ──────────────
    uint64_t ResolveNamePtr(int32_t comp_index) {
        GNamesLoc loc = ComputeLocation(comp_index);

        uint64_t gnames_base = m_base + FNAME_GNAMES_BASE_OFF;   // ← new RVA
        uint64_t chunk_addr = gnames_base + loc.chunk_offset;

        uint8_t  bidx = ComputeBlockIdx(chunk_addr);
        uint64_t block_base = chunk_addr + FNAME_CHUNK_BLOCK_BASE_OFF;

        alignas(16) uint8_t sb1[16] = {};
        alignas(16) uint8_t sb2[16] = {};
        uint64_t b1addr = block_base + 32ULL * (bidx & 7u);
        uint64_t b2addr = block_base + 32ULL * ((bidx + 1u) & 7u);
        if (!Memory::read(b1addr, sb1, 16)) return 0;
        if (!Memory::read(b2addr, sb2, 16)) return 0;

        uint64_t v12 = DecryptBlock(sb1);
        uint64_t v13 = DecryptBlock(sb2);

        // FNV chain  (rotation 35, additive now SUBTRACTED)  ← changed
        uint64_t fnv = FNAME_FNV_PRIME * fn_rotl64(v12, FNAME_FNV_ROL1) - FNAME_FNV_ADDITIVE;
        fnv = FNAME_FNV_PRIME * fn_rotl64(fnv, FNAME_FNV_ROL2) - FNAME_FNV_ADDITIVE;

        uint64_t R = v12 + (v13 ^ fnv) + loc.name_entry_offset;

        // 3-step XOR/byteswap pointer decryption  ← all new constants
        uint64_t a = fn_bswap64(R ^ FNAME_PTR_XOR_1);
        uint64_t b = a ^ FNAME_PTR_XOR_2;
        uint64_t name_ptr = fn_bswap64(b ^ FNAME_PTR_XOR_3);
        return name_ptr;
    }

    // ── Step 7: read + XOR-decrypt name string ────────────────────────────
    // Source: sub_237070   ← key schedule COMPLETELY NEW
    std::string DecryptNameString(uint64_t name_entry_ptr) {
        if (!name_entry_ptr || !m_keyLoaded) return {};

        uint16_t header = 0;
        if (!Memory::read(name_entry_ptr, &header, 2) || !header) return {};

        bool isWide = (header & 1) != 0;
        int  length = (header >> 1) & 0x3FF;
        if (length <= 0 || length > 1024) return {};

        int byteCount = isWide ? length * 2 : length;
        int readLen = (byteCount > 254) ? 254 : byteCount;

        uint8_t buf[256] = {};
        if (!Memory::read(name_entry_ptr + 2, buf, readLen)) return {};

        // Helper: compute both keys for a step
        auto primaryKey = [](int key) { return key & 0x3F; };
        auto secondaryKey = [](int key) {
            return (FNAME_KEY_SECONDARY_MULT * key + FNAME_KEY_SECONDARY_ADD) & 0x3F;
            };
        auto advanceKey = [](int key) {
            return FNAME_KEY_STEP_MULT * key + FNAME_KEY_STEP_ADD;
            };

        int key = length + FNAME_KEY_INIT_ADD;

        if (isWide) {
            auto* wbuf = reinterpret_cast<uint16_t*>(buf);
            int   wCharCount = readLen / 2;
            for (int i = 0; i + 1 < length; i += 2) {
                if (i < wCharCount) wbuf[i] ^= m_keyTable[primaryKey(key)];
                if (i + 1 < wCharCount) wbuf[i + 1] ^= m_keyTable[secondaryKey(key)];
                key = advanceKey(key);
            }
            if ((length & 1) && (length - 1) < wCharCount)
                wbuf[length - 1] ^= m_keyTable[primaryKey(key)];

            std::string result;
            result.reserve(wCharCount);
            for (int j = 0; j < wCharCount; ++j)
                if (wbuf[j]) result += static_cast<char>(wbuf[j] & 0xFF);
            return result;
        }
        else {
            for (int i = 0; i + 1 < length; i += 2) {
                if (i < readLen) buf[i] ^= static_cast<uint8_t>(m_keyTable[primaryKey(key)] >> 3);
                if (i + 1 < readLen) buf[i + 1] ^= static_cast<uint8_t>(m_keyTable[secondaryKey(key)] >> 3);
                key = advanceKey(key);
            }
            if ((length & 1) && (length - 1) < readLen)
                buf[length - 1] ^= static_cast<uint8_t>(m_keyTable[primaryKey(key)] >> 3);
            return std::string(reinterpret_cast<char*>(buf), readLen);
        }
    }

    // ── Full pipeline: object pointer → name string ────────────────────────
    std::string GetName(uint64_t obj_ptr) {
        if (!obj_ptr || !m_keyLoaded) return {};
        int32_t  comp = GetCompIndex(obj_ptr);
        if (!comp) return {};
        uint64_t nptr = ResolveNamePtr(comp);
        if (!nptr) return {};
        return DecryptNameString(nptr);
    }

    // ── Resolve comp_index → string (class names etc.) ─────────────────────
    std::string CompIndexToName(int32_t comp_index) {
        if (!comp_index || !m_keyLoaded) return {};
        uint64_t nptr = ResolveNamePtr(comp_index);
        if (!nptr) return {};
        return DecryptNameString(nptr);
    }

private:
    uint64_t       m_base;
    bool           m_keyLoaded;
    uint16_t       m_keyTable[64];
};

// ── Free-function shims (original call-site API) ──────────────────────────
std::string Engine::GetActorFNameString(uint64_t actor_base)
{
    FNameDecryptor dec(Memory::getBaseAddress());
    dec.Init();
    return dec.GetName(actor_base);
}

int32_t Engine::GetActorFNameId(uint64_t actor_base)
{
    FNameDecryptor dec(Memory::getBaseAddress());
    return dec.GetCompIndex(actor_base);
}

std::string Engine::GetActorFNameStringCached(uintptr_t actor_base)
{
    if (!IsValidPointer(GWorld) || !IsValidPointer(PersistentLevel))
    {
        FNameCache::Instance().Clear();
        return "";
    }

    if (!IsValidPointer(actor_base))
        return "";

    // Garante que as constantes SIMD estão carregadas
    if (!g_bReadSimdConsts) {
        // Chama GetActorFNameString uma vez para inicializar
        return GetActorFNameString(actor_base);
    }

    // Pega o FNameId2 (mesmo que GetActorFNameString usa internamente)
    int32_t fnameId2 = GetActorFNameId(actor_base);
    if (fnameId2 == 0)
        return "";

    // Tenta pegar do cache
    std::string cachedName;
    if (FNameCache::Instance().TryGet(fnameId2, cachedName)) {
        return cachedName;
    }

    // Cache miss - faz a descriptografia completa
    std::string name = GetActorFNameString(actor_base);

    // Salva no cache
    if (!name.empty()) {
        FNameCache::Instance().Add(fnameId2, name);
    }

    return name;
}

void Engine::ClearFNameCache()
{
    FNameCache::Instance().Clear();
}