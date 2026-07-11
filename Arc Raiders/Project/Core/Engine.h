#pragma once
#include "Vector.hpp"
#include "Cache.hpp"
#include "Offsets.h"

#include <random>
#include <deque>
#include <chrono>
#include <d3d9.h>
#include <shared_mutex>
#include <unordered_set>
#include "../Interface/Utils/Threads/SyncedThread.h"
#include "../Interface/Requeriments/imgui.h"
#include "../Interface/Utils/Helper/motorsynergy.h"
#include "../Interface/Utils/Variables/index.h"

#define M_PI	3.14159265358979323846

class Engine {
private:
    SyncedThread* worldThread = nullptr;
    SyncedThread* entitysThread = nullptr;
    SyncedThread* worldespThread = nullptr;
    SyncedThread* robotespThread = nullptr;
    SyncedThread* clearCacheThread = nullptr;
    SyncedThread* AimThread = nullptr;
    bool debugLog = false;
    bool debugWorld = false;
    bool entityStarted = false;

    // Thread synchronization
    mutable std::shared_mutex m_stateMutex;       // Protects state pointers (GWorld, PersistentLevel, etc.)
    mutable std::shared_mutex m_playerCacheMutex;  // Protects playerCache
    mutable std::shared_mutex m_worldCacheMutex;   // Protects worldCache
    mutable std::shared_mutex m_robotCacheMutex;   // Protects robotCache
    std::atomic<uint64_t> m_worldGeneration{ 0 };    // Incremented on world change
public:
    enum class EItemRarity : uint8_t
    {
        Common = 0,
        Uncommon = 1,
        Rare = 2,
        Epic = 3,
        Legendary = 4,
        MAX = 5,
        INVALID = 0xFF
    };

    BoneData boneData;

    void Update();
    void EntityList();
    void WorldList();
    void RobotList();

    void RenderEsp();
    void AimAssistence();

    uintptr_t GetGameInstance(uint64_t uworldAddr);
    uintptr_t GetCameraManagerFromActors();

    void UpdateCamera();
    bool ProjectWorldLocationToScreen(Vector3 world_location, Vector3& screen);
    bool ProjectWorldLocationToRadar(const Vector3& myWorldLocation, const Vector3& enemyWorldLocation, float myYaw, Vector3& outRadar);
    bool Visible(uintptr_t mesh);

    std::uintptr_t GetBoneArrayDecrypt(std::uintptr_t Meh);

    bool IsValidPointer(uintptr_t ptr);
    bool IsUsermodePtr(uintptr_t ptr);
    std::string getEntityType(const std::string& actorName);
    bool getAllowType(const std::string& actorName);
    bool Has(const std::string& s, const char* sub);

    bool InitConsts();
    int32_t GetActorFNameId(uint64_t actor_base);
    std::string GetActorFNameString(uint64_t actor_base);

    uint64_t DecryptFNameRaw(const __m128i& enc);
    uint32_t ComputeHashAndIndex(uint64_t base_addr, uint32_t& out_idx);
    uint8_t ComputeBlockIdx(uint64_t chunk_ptr);

    std::string GetActorFNameStringCached(uintptr_t actor_base);
    uint64_t DecryptBlockRaw(const __m128i& raw);

    void ClearFNameCache();

    std::string GetEnglishItemName(uint64_t actor);
    std::string GetWeaponName(const std::string& internal_name);

public: // Local Cache
    uintptr_t PlayerController;
    uintptr_t GWorld;
    uintptr_t m_lastWorldPtr;

    uintptr_t PersistentLevel;
    uintptr_t GameInstance;
    uintptr_t OwningGameInstance;
    uintptr_t localplayer;

    float PlayerPosition;

    uintptr_t AcknowledgedPawn;
    uintptr_t RootComponent;
    uintptr_t PlayerState;
    uintptr_t Mesh;
    uintptr_t PlayerCameraManager;
    uintptr_t Actors;

    int LocalPlayerTeam;

    uintptr_t CurrentGun;

    uintptr_t AGameStateBase;
public: // PlayerCache
    struct PlayerCacheEntry {
        uintptr_t rootComponent = NULL;
        uintptr_t actorState = NULL;
        uintptr_t HealthPoint = NULL;
        uintptr_t actorMesh = NULL;
        uintptr_t APawn = NULL;

        Vector3 WorldPos;
        Vector3 RadarPos;

        Vector3 head;
        Vector3 feet;

        Vector3 ScreenTop;
        Vector3 ScreenBottom;

        std::string ActorName;

        bool isVisible;
        bool Drawing;
        float Distance;
        bool isAlly;

        float health;
        float maxhealth;

        float shield;
        float maxshield;
        float shieldLevel;

        bool bIsDeathVerge;
        bool bIsABot;

        bool bIsDead;

        BoneData boneData;
        uintptr_t boneArray;

        std::string weaponName;
        int weaponQuality = -1;

        Vector3 cachedVelocity = { 0, 0, 0 };
        float lastVelocityUpdate = 0.0f;

        PlayerCacheEntry() {};

        PlayerCacheEntry(std::string name, uint64_t root, uintptr_t Pawn, uintptr_t mesh)
            : ActorName(name), rootComponent(root), APawn(Pawn), actorMesh(mesh)
        {
        };
    };
    std::unordered_map<uintptr_t, PlayerCacheEntry> playerCache;
    void GetBones(PlayerCacheEntry& actor);
    Vector3 GetBone(int boneIndex, uintptr_t boneArray, FTransform componentToWorld);

    bool GetAimPoint2DPredicted(
        const PlayerCacheEntry& actor,
        float bulletSpeed,
        Vector3& out);

    Vector3 PredictPosition(
        const Vector3& targetPos,
        const Vector3& targetVelocity,
        const Vector3& myPos,
        float bulletSpeed,
        int iterations);

    bool GetAimPoint2D(
        const PlayerCacheEntry& actor,
        Vector3& out);

    Vector3 GetActorVelocity(uintptr_t actor);

public:
    struct WorldCacheEntry {
        uintptr_t rootComponent = NULL;
        uintptr_t actorState = NULL;
        uintptr_t Mesh = NULL;
        uintptr_t APawn;

        Vector3 WorldPos;
        Vector3 ScreenPos;

        std::string ActorName;

        bool Drawing;
        bool IsBreaked;
        int category;
        float Distance;

        float health;
        float maxhealth;

        std::string ItemDisplayName;
        std::string ItemType;
        EItemRarity ItemRarity;

        WorldCacheEntry() {};

        WorldCacheEntry(std::string name, uint64_t root, uintptr_t Pawn, uintptr_t MeshComponent)
            : ActorName(name), rootComponent(root), APawn(Pawn), Mesh(MeshComponent)
        {
        };
    };
    std::unordered_map<uintptr_t, WorldCacheEntry> worldCache;
    std::unordered_map<uintptr_t, WorldCacheEntry> robotCache;
public:
    struct FMinimalViewInfo
    {
    public:
        char Pad_0[0x8]; // 0x00(0x10)
        struct Vector3 Location; // 0x08(0x18)
        char pad_20[0x8]; // 0x20(0x10)
        struct Vector3 Rotation; // 0x30(0x18)
        char pad_48[0x8]; // 0x48(0x10)
        float FOV; // 0x58(0x04)
    };

    struct CameraCache
    {
        Vector3 Location;
        Vector3 Rotation;
        float FOV;
    };
    CameraCache g_Camera;
public:
    std::unordered_map<int, UniBone> GameBoneMapArcRaiders = {
        // Cabeca / tronco
        { 0,  UniBone::Root   },   // BoneIndex::Root
        { 1,  UniBone::Pelvis},   // BoneIndex::Pelvis
        { 2,  UniBone::Spine1},   // BoneIndex::Spine01
        { 3,  UniBone::Spine2},   // BoneIndex::Spine02
        { 4,  UniBone::Spine3},   // BoneIndex::Spine03
        { 5,  UniBone::Chest },   // BoneIndex::Chest
        { 6,  UniBone::Neck  },   // BoneIndex::Neck
        { 7,  UniBone::Head  },   // BoneIndex::Head

        // Braco esquerdo
        { 8,  UniBone::ClavicleL }, // BoneIndex::ClavicleL
        { 9,  UniBone::UpperArmL }, // BoneIndex::UpperArmL
        { 10, UniBone::LowerArmL }, // BoneIndex::LowerArmL
        { 11, UniBone::HandL     }, // BoneIndex::HandL

        // Braco direito
        { 42, UniBone::ClavicleR }, // BoneIndex::ClavicleR
        { 43, UniBone::UpperArmR }, // BoneIndex::UpperArmR
        { 44, UniBone::LowerArmR }, // BoneIndex::LowerArmR
        { 45, UniBone::HandR     }, // BoneIndex::HandR

        // Perna esquerda
        { 65, UniBone::ThighL },    // BoneIndex::ThighL
        { 66, UniBone::CalfL  },    // BoneIndex::CalfL
        { 67, UniBone::FootL  },    // BoneIndex::FootL

        // Perna direita
        { 69, UniBone::ThighR },    // BoneIndex::ThighR
        { 70, UniBone::CalfR  },    // BoneIndex::CalfR
        { 71, UniBone::FootR  },    // BoneIndex::FootR
    };

    std::vector<std::pair<UniBone, UniBone>> SkeletonLinksArcRaiders = {
        { UniBone::Root,   UniBone::Pelvis },

        { UniBone::Pelvis, UniBone::Spine1 },
        { UniBone::Spine1, UniBone::Spine2 },
        { UniBone::Spine2, UniBone::Spine3 },
        { UniBone::Spine3, UniBone::Chest },
        { UniBone::Chest,  UniBone::Neck },
        { UniBone::Neck,   UniBone::Head },

        { UniBone::Chest,      UniBone::ClavicleL },
        { UniBone::ClavicleL,  UniBone::UpperArmL },
        { UniBone::UpperArmL,  UniBone::LowerArmL },
        { UniBone::LowerArmL,  UniBone::HandL },

        { UniBone::Chest,      UniBone::ClavicleR },
        { UniBone::ClavicleR,  UniBone::UpperArmR },
        { UniBone::UpperArmR,  UniBone::LowerArmR },
        { UniBone::LowerArmR,  UniBone::HandR },

        { UniBone::Pelvis, UniBone::ThighL },
        { UniBone::ThighL, UniBone::CalfL },
        { UniBone::CalfL,  UniBone::FootL },

        { UniBone::Pelvis, UniBone::ThighR },
        { UniBone::ThighR, UniBone::CalfR },
        { UniBone::CalfR,  UniBone::FootR },
    };
public:
    double RadToDeg(double radians)
    {
        return radians * 180 / M_PI;
    }

    double DegToRad(double deg)
    {
        return deg * M_PI / 180.0;
    }

    uint32_t ROL4(uint32_t x, uint32_t n)
    {
        return (x << n) | (x >> (32 - n));
    }

    uint64_t ROL8(uint64_t x, uint32_t n)
    {
        return (x << n) | (x >> (64 - n));
    }

    uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    uint64_t rotl64(uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }

    uint64_t u64_lo(__m128i v) {
        alignas(16) uint64_t arr[2];
        _mm_store_si128(reinterpret_cast<__m128i*>(arr), v);
        return arr[0];
    }

    std::string toLower(std::string str)
    {
        std::transform(str.begin(), str.end(), str.begin(),
            [](unsigned char c) { return std::tolower(c); });
        return str;
    }

    D3DMATRIX to_matrix(Vector3 rot, Vector3 origin = Vector3(0, 0, 0))
    {
        float radpitch = (rot.x * M_PI / 180);
        float radyaw = (rot.y * M_PI / 180);
        float radroll = (rot.z * M_PI / 180);
        float sp = sinf(radpitch);
        float cp = cosf(radpitch);
        float sy = sinf(radyaw);
        float cy = cosf(radyaw);
        float sr = sinf(radroll);
        float cr = cosf(radroll);
        D3DMATRIX matrix{};
        matrix.m[0][0] = cp * cy;
        matrix.m[0][1] = cp * sy;
        matrix.m[0][2] = sp;
        matrix.m[0][3] = 0.f;
        matrix.m[1][0] = sr * sp * cy - cr * sy;
        matrix.m[1][1] = sr * sp * sy + cr * cy;
        matrix.m[1][2] = -sr * cp;
        matrix.m[1][3] = 0.f;
        matrix.m[2][0] = -(cr * sp * cy + sr * sy);
        matrix.m[2][1] = cy * sr - cr * sp * sy;

        matrix.m[2][2] = cr * cp;
        matrix.m[2][3] = 0.f;
        matrix.m[3][0] = origin.x;
        matrix.m[3][1] = origin.y;
        matrix.m[3][2] = origin.z;
        matrix.m[3][3] = 1.f;
        return matrix;
    }

    D3DMATRIX MatrixMultiplication(D3DMATRIX pM1, D3DMATRIX pM2)
    {
        D3DMATRIX pOut;
        pOut._11 = pM1._11 * pM2._11 + pM1._12 * pM2._21 + pM1._13 * pM2._31 + pM1._14 * pM2._41;
        pOut._12 = pM1._11 * pM2._12 + pM1._12 * pM2._22 + pM1._13 * pM2._32 + pM1._14 * pM2._42;
        pOut._13 = pM1._11 * pM2._13 + pM1._12 * pM2._23 + pM1._13 * pM2._33 + pM1._14 * pM2._43;
        pOut._14 = pM1._11 * pM2._14 + pM1._12 * pM2._24 + pM1._13 * pM2._34 + pM1._14 * pM2._44;
        pOut._21 = pM1._21 * pM2._11 + pM1._22 * pM2._21 + pM1._23 * pM2._31 + pM1._24 * pM2._41;
        pOut._22 = pM1._21 * pM2._12 + pM1._22 * pM2._22 + pM1._23 * pM2._32 + pM1._24 * pM2._42;
        pOut._23 = pM1._21 * pM2._13 + pM1._22 * pM2._23 + pM1._23 * pM2._33 + pM1._24 * pM2._43;
        pOut._24 = pM1._21 * pM2._14 + pM1._22 * pM2._24 + pM1._23 * pM2._34 + pM1._24 * pM2._44;
        pOut._31 = pM1._31 * pM2._11 + pM1._32 * pM2._21 + pM1._33 * pM2._31 + pM1._34 * pM2._41;
        pOut._32 = pM1._31 * pM2._12 + pM1._32 * pM2._22 + pM1._33 * pM2._32 + pM1._34 * pM2._42;
        pOut._33 = pM1._31 * pM2._13 + pM1._32 * pM2._23 + pM1._33 * pM2._33 + pM1._34 * pM2._43;
        pOut._34 = pM1._31 * pM2._14 + pM1._32 * pM2._24 + pM1._33 * pM2._34 + pM1._34 * pM2._44;
        pOut._41 = pM1._41 * pM2._11 + pM1._42 * pM2._21 + pM1._43 * pM2._31 + pM1._44 * pM2._41;
        pOut._42 = pM1._41 * pM2._12 + pM1._42 * pM2._22 + pM1._43 * pM2._32 + pM1._44 * pM2._42;
        pOut._43 = pM1._41 * pM2._13 + pM1._42 * pM2._23 + pM1._43 * pM2._33 + pM1._44 * pM2._43;
        pOut._44 = pM1._41 * pM2._14 + pM1._42 * pM2._24 + pM1._43 * pM2._34 + pM1._44 * pM2._44;

        return pOut;
    }
public:
    uint32_t rol32(uint32_t v, int s) {
        return (v << s) | (v >> (32 - s));
    }

    void decode_in_place(std::vector<uint16_t>& buf, int max_len) {
        if (buf.empty() || max_len <= 0)
            return;

        auto len = min(max_len, (int)buf.size());
        auto hash = 0u;

        for (auto i = 0; i < len; ++i)
        {
            auto ch = (uint32_t)buf[i];
            if (ch == 0)
                break;

            hash = 16777619u * (hash + _rotl(16777619u * hash - 749601044u, 22));

            auto v6 = (int32_t)(int8_t)ch ^ (hash & 0x1F);

            auto v7 = 0;
            if ((uint32_t)(v6 - 80) < 0x2Fu) v7 = -47;
            if ((uint32_t)(v6 - 33) < 0x2Fu) v7 = 47;

            auto v8 = (uint32_t)(v7 + v6 - 48);
            auto v9 = (uint32_t)(v7 + v6 - 53);
            auto v10 = v6 + v7;
            auto v11 = 5 * (v9 >= 5) - 5;
            if (v8 < 5) v11 = 5;

            auto v12 = v10 + v11;
            auto v13 = 0;
            if ((uint32_t)(v11 + v10 - 110) < 0xDu) v13 = -13;
            if ((uint32_t)(v11 + v10 - 97) < 0xDu) v13 = 13;

            auto v14 = v12 + v13;
            auto v15 = 0;
            if ((uint32_t)(v13 + v12 - 78) < 0xDu) v15 = -13;
            if ((uint32_t)(v13 + v12 - 65) < 0xDu) v15 = 13;

            auto v16 = (int8_t)0;
            if ((uint32_t)(v15 + v14 - 80) < 0x2Fu) v16 = -47;
            if ((uint32_t)(v15 + v14 - 33) < 0x2Fu) v16 = 47;

            buf[i] = (uint8_t)(v14 + v15 + v16);
        }
    }

    std::string GetPlayerName(uintptr_t address) {


        if (address == 0) return "";


        uint64_t buf_ptr = Memory::read<uint64_t>(address);


        int32_t length = Memory::read<int32_t>(address + 0x8);


        if (buf_ptr == 0 || length <= 0 || length > 512) return "";


        std::vector<uint16_t> w(length + 1, 0);


        for (int i = 0; i < length; ++i) {
            w[i] = Memory::read<uint16_t>(buf_ptr + (i * 2));
        }
        w[length] = 0;


        if (length >= 2) {
            decode_in_place(w, length);
        }


        std::string out;
        out.reserve(length);

        for (int i = 0; i < length; ++i) {
            uint16_t c = w[i] & 0xFFFF;

            if (c == 0) break;

            if (c <= 0x7F) {
                out.push_back((char)c);
            }
            else {
                out.push_back('?');
            }
        }
        return out;
    }

    uint32_t get_name_private(uintptr_t actor) {
        if (!actor) return 0;

        uint32_t v4 = (uint32_t)actor + 16;
        uint32_t v5 = 16777619 * ((v4 << 27) | (v4 >> 5)) - 398134503;
        uint32_t v6 = (v5 << 23) | (v5 >> 9);
        uint32_t v7 = 16777619 * ((16777619 * ((16777619 * v6 + ((actor + 16) >> 32) - 398134503) >> 5) - 398134503) >> 9) - 398134503;

        uint32_t idx = ((uint8_t)v7 ^ (uint8_t)(v7 >> 16)) & 3 ^ 2;
        __m128i data = Memory::read<__m128i>(actor + 32 * idx + 32);

        __m128i key = Memory::read<__m128i>(Memory::getBaseAddress() + 0xAA83090);
        __m128i x = _mm_xor_si128(data, key);
        __m128i r = _mm_or_si128(_mm_srli_epi64(x, 0x1A), _mm_slli_epi64(x, 0x26));

        uint64_t res = _mm_extract_epi64(_mm_shufflelo_epi16(r, 0x4B), 0);
        return (uint32_t)(res >> 32) | (uint32_t)res;
    }

    std::string get_name(uint32_t name_private) {
        static uint16_t key_table[64] = {};
        static bool key_table_ready = false;

        if (name_private <= 0)
            return "";

        if (!key_table_ready) {
            uint64_t key_addr = Memory::getBaseAddress() + 0xDA69930;
            for (int i = 0; i < 64; i++)
                key_table[i] = Memory::read<uint16_t>(key_addr + i * 2);
            if (!key_table[0] && !key_table[1])
                return "";
            key_table_ready = true;
        }

        uint64_t chunk_offs = (name_private >> 8) & 0xFFFF00;
        uint16_t entry_idx = (uint16_t)name_private;
        uint64_t pool_chunk = Memory::getBaseAddress() + 0xD862800 + chunk_offs;

        uint64_t hash_addr = pool_chunk + 0x24E0;
        uint32_t lo = (uint32_t)hash_addr;
        uint32_t hi = (uint32_t)(hash_addr >> 32);

        uint32_t s1 = _rotl(lo, 21);
        uint32_t s2 = 0x01000193 * s1 - 0x93674AB3;
        uint32_t s3 = _rotl(s2, 26);
        uint32_t s4 = hi + 0x01000193 * s3 - 0x93674AB3;
        uint32_t s5 = _rotl(s4, 21);
        uint32_t s6 = 0x01000193 * s5 - 0x93674AB3;
        uint32_t sel = 0x01000193 * (s6 >> 6) - 0x93674AB3;

        uint8_t blk_idx = (uint8_t)sel ^ (uint8_t)(sel >> 16);

        auto decrypt_block = [&](uint8_t idx) -> uint64_t {
            uint64_t addr = pool_chunk + 32 * (idx & 7) + 0x24F0;
            __m128i blk = Memory::read<__m128i>(addr);
            __m128i rot = _mm_or_si128(_mm_slli_epi16(blk, 6), _mm_srli_epi16(blk, 10));
            __m128i shuf = _mm_shufflelo_epi16(rot, 0x93);
            __m128i key = _mm_set_epi64x(1, 0xD0B37A3FA16AB297ULL);
            return _mm_xor_si128(shuf, key).m128i_u64[0];
            };

        uint64_t dec1 = decrypt_block(blk_idx);
        uint64_t dec2 = decrypt_block(blk_idx + 1);

        uint64_t fnv = 0x100000001B3ULL * _rotl64(dec1, 49) + 0x76763239714AD543ULL;
        fnv = 0x100000001B3ULL * _rotl64(fnv, 48) + 0x76763239714AD543ULL;

        uint64_t name_entry = dec1 + (dec2 ^ fnv) + 2 * (uint64_t)entry_idx;

        uint16_t header = Memory::read<uint16_t>(name_entry);
        if (!header)
            return "";

        bool is_wide = (header & 0x20) != 0;
        uint32_t length = header >> 6;
        if (!length || length > 1024)
            return "";

        uint64_t str_addr = name_entry + 2;

        if (!is_wide) {
            int len = (length > 254) ? 254 : (int)length;
            uint8_t enc[256] = {};
            for (int i = 0; i < len; i++)
                enc[i] = Memory::read<uint8_t>(str_addr + i);
            char out[256] = {};
            for (int i = 0; i < len; i++)
                out[i] = enc[i] ^ (uint8_t)(key_table[i & 0x3F] >> 3);
            return std::string(out, len);
        }

        int len = (length > 254) ? 254 : (int)length;
        uint16_t buf[256] = {};
        for (int i = 0; i < len; i++)
            buf[i] = Memory::read<uint16_t>(str_addr + i * 2);
        for (int i = 0; i < len; i++)
            buf[i] ^= key_table[i & 0x3F];
        std::wstring ws(reinterpret_cast<wchar_t*>(buf), len);
        int sz = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
        if (sz <= 0)
            return "";
        std::string result(sz, 0);
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), (int)ws.size(), &result[0], sz, nullptr, nullptr);
        return result;
    }

    double get_health(uintptr_t actor)
    {
        if (!actor) return 0.0;
        uintptr_t health_comp = Memory::read<uintptr_t>(actor + Offsets::HealthComponent);
        if (!health_comp) return 0.0;
        return Memory::read<double>(health_comp + Offsets::Health);
    }

    double get_maxhealth(uintptr_t actor)
    {
        if (!actor) return 0.0;
        uintptr_t health_comp = Memory::read<uintptr_t>(actor + Offsets::HealthComponent);
        if (!health_comp) return 0.0;
        return Memory::read<double>(health_comp + Offsets::MaxHealth);
    }

    double get_armor(uintptr_t actor)
    {
        if (!actor) return 0.0;
        uintptr_t health_comp = Memory::read<uintptr_t>(actor + Offsets::HealthComponent);
        if (!health_comp) return 0.0;
        return Memory::read<double>(health_comp + Offsets::Shield);
    }

    double get_maxarmor(uintptr_t actor)
    {
        if (!actor) return 0.0;
        uintptr_t health_comp = Memory::read<uintptr_t>(actor + Offsets::HealthComponent);
        if (!health_comp) return 0.0;
        return Memory::read<double>(health_comp + Offsets::Shield + 0x8);
    }

    struct PlayerHealthInfo {
        double Health;
        double MaxHealth;
        double Armor;
        double MaxArmor;
        bool bHasBrokenArmor;
        bool bIsDbno;
        char _pad[6];
    };
public:
    struct FTArrayRaw {
        uint64_t Data;
        uint32_t Count;
        uint32_t Max;
    };
    uintptr_t GetCurrentWeaponActor(uintptr_t actor) {
        if (!actor) return 0;
        auto inventoryComponent = Memory::read<uintptr_t>(actor + Offsets::InventoryComponent);
        if (!inventoryComponent)
            return 0;

        FTArrayRaw LocalCurrentItemActors = Memory::read<FTArrayRaw>(inventoryComponent + Offsets::LocalCurrentItemActors);

        if (!LocalCurrentItemActors.Data || LocalCurrentItemActors.Count <= 0)
            return 0;

        return Memory::read<uintptr_t>(LocalCurrentItemActors.Data);
    }

    int GetWeaponQuality(uintptr_t actor) {
        if (!actor) return -1;
        auto inventoryComponent = Memory::read<uintptr_t>(actor + Offsets::InventoryComponent);
        if (!inventoryComponent)
            return -1;

        FTArrayRaw itemArray = Memory::read<FTArrayRaw>(inventoryComponent + Offsets::LocalCurrentItemActors);

        if (!itemArray.Data || itemArray.Count <= 0 || itemArray.Count > 100)
            return -1;

        uintptr_t currentWeapon = Memory::read<uintptr_t>(itemArray.Data);
        if (!currentWeapon)
            return -1;

        uint8_t WeaponQuality = Memory::read<uint8_t>(currentWeapon + Offsets::WeaponQuality);

        return static_cast<int>(WeaponQuality);
    }
public:
    struct AimTarget {
        uint64_t entityKey = 0;
        Vector3 aimPos;       // Screen position
        Vector3 worldPos;     // World position (NOVO)
        float distToCenter = FLT_MAX;
        float score = -FLT_MAX;
        bool isRobot = false;
        int32_t partID = -1;
        int32_t resistGroup = 0;
    };

    struct RobotPartInfo {
        int32_t  partID = -1;
        int32_t  resistanceGroup = 0;
        uintptr_t componentAddr = 0;
        Vector3 worldPos;
        bool     isValid = false;
    };

    bool GetRobotAimPoint2D(
        const WorldCacheEntry& robot,
        float fovRadius,
        Vector3& outScreenPos,
        Vector3& outWorldPos,
        int32_t& outPartID,
        int32_t& outResistGroup);

    void AimAssistRobot(
        const Vector3& screenCenter,
        float fovRadius,
        float currentTime,
        std::vector<AimTarget>& targets);

    void AimAssistPlayer(
        const Vector3& screenCenter,
        float fovRadius,
        float bulletSpeed,
        float currentTime,
        std::vector<AimTarget>& targets);

    std::vector<RobotPartInfo> ReadRobotParts(uintptr_t actorAddr);
public:
    std::unordered_map<std::string, ImColor> itemImColors = {
        // Laranja
        {"loot item",              ImColor(255, 165, 0, 255)},

        // Vermelho
        {"ai robot",             ImColor(227, 101, 101, 255)},
        {"turret",             ImColor(227, 101, 101, 255)},
        {"leaper",             ImColor(227, 101, 101, 255)},
        {"roll bot",             ImColor(227, 101, 101, 255)},
        {"fireball",             ImColor(227, 101, 101, 255)},
        {"pop",             ImColor(227, 101, 101, 255)},
        {"surveyor",             ImColor(227, 101, 101, 255)},
        {"hornet",             ImColor(227, 101, 101, 255)},
        {"rocketeer",             ImColor(227, 101, 101, 255)},
        {"the queen",             ImColor(227, 101, 101, 255)},
        {"matriarch",             ImColor(227, 101, 101, 255)},
        {"pinger robot",             ImColor(227, 101, 101, 255)},
        {"bastion",             ImColor(227, 101, 101, 255)},
        {"bombardier",             ImColor(227, 101, 101, 255)},
        {"sentinel",             ImColor(227, 101, 101, 255)},
        {"shredder",             ImColor(227, 101, 101, 255)},
        {"snitch",             ImColor(227, 101, 101, 255)},
        {"wasp",             ImColor(227, 101, 101, 255)},
        {"spotter",             ImColor(227, 101, 101, 255)},
        {"comet",             ImColor(227, 101, 101, 255)},
        {"firefly",             ImColor(227, 101, 101, 255)},

        // Azul
        {"arc cargoship",             ImColor(0, 255, 255, 255)},

        // Magenta
        {"raider stock",              (255, 0, 255, 255)},

        // Verde claro
        {"corpse",             (144, 238, 144, 255)},
    };
public:
    std::unordered_set<std::string> robotsList = {
        "Snitch",
        "Turret",
        "Leaper",
        "The Queen",
        "Bombardier",
        "Bastion",
        "Sentinel",
        "Shredder",
        "Hornet",
        "Wasp",
        "Fireball",
        "Pop",
        "Surveyor",
        "Spotter",
        "Rocketeer",
        "Matriarch",
        "Comet",
        "Firefly"
    };
public:
    class FNameCache {
    private:
        std::unordered_map<int32_t, std::string> m_cache;
        mutable std::shared_mutex m_mutex;

        FNameCache() = default;

    public:
        static FNameCache& Instance() {
            static FNameCache instance;
            return instance;
        }

        bool TryGet(int32_t fnameId, std::string& outName) const {
            if (fnameId == 0)
                return false;

            std::shared_lock<std::shared_mutex> lock(m_mutex);
            auto it = m_cache.find(fnameId);
            if (it != m_cache.end()) {
                outName = it->second;
                return true;
            }
            return false;
        }

        void Add(int32_t fnameId, const std::string& name) {
            if (fnameId == 0 || name.empty())
                return;

            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_cache[fnameId] = name;
        }

        bool Contains(int32_t fnameId) const {
            std::shared_lock<std::shared_mutex> lock(m_mutex);
            return m_cache.find(fnameId) != m_cache.end();
        }

        void Clear() {
            std::unique_lock<std::shared_mutex> lock(m_mutex);
            m_cache.clear();
        }

        size_t Size() const {
            std::shared_lock<std::shared_mutex> lock(m_mutex);
            return m_cache.size();
        }
    };
public:
    class MotorSynergyHumanizer {
    private:
        // Ornstein-Uhlenbeck state
        double ou_x = 0.0;
        double ou_y = 0.0;

        // Tremor state
        double tremor_phase_x = 0.0;
        double tremor_phase_y = 0.0;
        double tremor_freq = 10.0;
        double tremor_amp = 0.12;

        // Timing
        double lastTimeMs = 0.0;

        // RNG
        std::mt19937_64 rng;

        // Config
        double ou_theta = 3.5;      // Mean reversion rate
        double ou_sigma = 0.4;      // Drift magnitude (reduzido para não atrapalhar)
        double sdn_k = 0.015;       // Signal-dependent noise (reduzido)

        double GetTimeMs() {
            using namespace std::chrono;
            static auto start = steady_clock::now();
            return duration_cast<duration<double, std::milli>>(steady_clock::now() - start).count();
        }

    public:
        MotorSynergyHumanizer() : rng(std::random_device{}()) {
            std::uniform_real_distribution<double> dist(0.0, 2.0 * M_PI);
            tremor_phase_x = dist(rng);
            tremor_phase_y = dist(rng);

            std::uniform_real_distribution<double> freqDist(8.0, 12.0);
            tremor_freq = freqDist(rng);

            std::uniform_real_distribution<double> ampDist(0.06, 0.15);
            tremor_amp = ampDist(rng);

            lastTimeMs = GetTimeMs();
        }

        void Reset() {
            ou_x = 0.0;
            ou_y = 0.0;
            lastTimeMs = GetTimeMs();
        }

        // Aplica humanização na rotação (pitch/yaw em graus)
        void ApplyToRotation(double& pitch, double& yaw, double movementSpeed = 1.0) {
            double currentTime = GetTimeMs();
            double dt_ms = currentTime - lastTimeMs;
            if (dt_ms <= 0.0) dt_ms = 8.0;
            if (dt_ms > 100.0) dt_ms = 100.0;  // Clamp para evitar valores absurdos
            lastTimeMs = currentTime;

            double dt_s = dt_ms / 1000.0;

            std::normal_distribution<double> normal(0.0, 1.0);

            // 1. Ornstein-Uhlenbeck drift (mean-reverting noise)
            ou_x += -ou_theta * ou_x * dt_s + ou_sigma * std::sqrt(dt_s) * normal(rng);
            ou_y += -ou_theta * ou_y * dt_s + ou_sigma * std::sqrt(dt_s) * normal(rng);

            // 2. Physiological tremor (8-12 Hz)
            double t_s = currentTime / 1000.0;

            // Tremor diminui com velocidade (proprioceptive suppression)
            double trem_mod = 1.0 / (1.0 + movementSpeed * 0.5);

            double tr_pitch = tremor_amp * trem_mod * std::sin(2.0 * M_PI * tremor_freq * t_s + tremor_phase_x);
            double tr_yaw = tremor_amp * trem_mod * std::sin(2.0 * M_PI * tremor_freq * t_s + tremor_phase_y);

            // 3. Signal-dependent noise (Harris-Wolpert)
            double sdn_pitch = sdn_k * movementSpeed * normal(rng);
            double sdn_yaw = sdn_k * movementSpeed * normal(rng);

            // Aplica (valores pequenos em graus)
            pitch += (ou_x + tr_pitch + sdn_pitch) * 0.1;  // Escala para graus
            yaw += (ou_y + tr_yaw + sdn_yaw) * 0.1;
        }
    };
public:
    bool g_bReadFNameKeyTable = false;
    bool g_bReadSimdConsts = false;

    void CheckWorldChange(uintptr_t newWorld)
    {
        if (newWorld != m_lastWorldPtr)
        {
            FNameCache::Instance().Clear();

            {
                std::unique_lock<std::shared_mutex> lk(m_playerCacheMutex);
                playerCache.clear();
            }
            {
                std::unique_lock<std::shared_mutex> lk(m_worldCacheMutex);
                worldCache.clear();
            }
            {
                std::unique_lock<std::shared_mutex> lk(m_robotCacheMutex);
                robotCache.clear();
            }

            m_lastWorldPtr = newWorld;
            m_worldGeneration.fetch_add(1, std::memory_order_release);

            g_bReadSimdConsts = false;
            g_bReadFNameKeyTable = false;
        }
    }
};