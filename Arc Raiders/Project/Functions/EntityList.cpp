#include "../Core/Engine.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_set>

void Engine::EntityList()
{
    // Snapshot state under shared lock
    uintptr_t sGWorld, sPersistentLevel, sActors, sAcknowledgedPawn, sPlayerController;
    {
        std::shared_lock<std::shared_mutex> slock(m_stateMutex);
        sGWorld = GWorld;
        sPersistentLevel = PersistentLevel;
        sActors = Actors;
        sAcknowledgedPawn = AcknowledgedPawn;
        sPlayerController = PlayerController;
    }

    if (sGWorld && sPersistentLevel && sAcknowledgedPawn && sActors && sPlayerController) {

        // Save generation to detect world changes
        uint64_t gen = m_worldGeneration.load(std::memory_order_acquire);

        int actor_count =
            Memory::read<int>(
                sPersistentLevel + (Offsets::AActors + sizeof(uintptr_t))
            );

        if (sActors == 0 || actor_count <= 0 || actor_count > 10000)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }

        std::vector<uint64_t> currentActors(actor_count);
        if (!Memory::ReadRaw(
            sActors,
            currentActors.data(),
            actor_count * sizeof(uint64_t)))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return;
        }

        std::unordered_set<uint64_t> currentActorSet(
            currentActors.begin(),
            currentActors.end()
        );

        UpdateCamera();
        auto cam = g_Camera;

        const float maxDistSq =
            (float)(var::esp_distance *
                var::esp_distance *
                10000.0f);

        // --- Copy cache to local (brief shared lock) ---
        std::unordered_map<uintptr_t, PlayerCacheEntry> localCache;
        {
            std::shared_lock<std::shared_mutex> lock(m_playerCacheMutex);
            localCache = playerCache;
        }

        // === All work below is on localCache — NO lock held ===

        for (auto it = localCache.begin(); it != localCache.end(); )
        {
            if (!currentActorSet.contains(it->first))
                it = localCache.erase(it);
            else
                ++it;
        }

        for (uint64_t actor : currentActors)
        {
            if (!actor)
                continue;

            if (localCache.contains(actor))
                continue;

            if (actor == sAcknowledgedPawn)
                continue;

            uintptr_t playerstate =
                Memory::read<uintptr_t>(actor + Offsets::APlayerState);
            if (!playerstate)
                continue;

            PlayerHealthInfo healthInfo = Memory::read<PlayerHealthInfo>(playerstate + Offsets::HealthInfo);
            auto bIsDeathVerge = healthInfo.bIsDbno;

            float health = get_health(actor);
            if (health < 1.0f && bIsDeathVerge == false)
                continue;

            std::string playerName =
                GetPlayerName(playerstate + Offsets::PlayerNamePrivate);

            uintptr_t root =
                Memory::read<uintptr_t>(actor + Offsets::RootComponent);
            if (!root)
                continue;

            uintptr_t mesh =
                Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
            if (!mesh)
                continue;

            localCache.emplace(
                actor,
                PlayerCacheEntry(playerName.c_str(), root, actor, mesh)
            );
        }

        for (auto it = localCache.begin(); it != localCache.end(); )
        {
            auto& actor = it->second;
            uintptr_t key = it->first;

            if (key == sAcknowledgedPawn) {
                it = localCache.erase(it);
                continue;
            }

            uint8_t myTeamId = Memory::read<uint8_t>(sAcknowledgedPawn + Offsets::TeamID);
            uint8_t enemyTeamId = Memory::read<uint8_t>(key + Offsets::TeamID);

            if (myTeamId == enemyTeamId)
            {
                it = localCache.erase(it);
                continue;
            }

            auto playerState = Memory::read<uintptr_t>(key + Offsets::APlayerState);
            if (!playerState) {
                it = localCache.erase(it);
                continue;
            }
            PlayerHealthInfo healthInfo = Memory::read<PlayerHealthInfo>(playerState + Offsets::HealthInfo);
            actor.bIsDeathVerge = healthInfo.bIsDbno;

            actor.WorldPos =
                Memory::read<Vector3>(
                    actor.rootComponent + Offsets::RelativeLocation
                );

            if (actor.WorldPos.x == 0.0 &&
                actor.WorldPos.y == 0.0 &&
                actor.WorldPos.z == 0.0)
            {
                it = localCache.erase(it);
                continue;
            }

            Vector3 delta = actor.WorldPos - cam.Location;
            float distanceSq =
                delta.x * delta.x +
                delta.y * delta.y +
                delta.z * delta.z;

            if (distanceSq > maxDistSq)
            {
                it = localCache.erase(it);
                continue;
            }


            actor.Distance = sqrtf(distanceSq) / 100.0f;

            // -------- Status --------
            actor.health = get_health(key);
            actor.maxhealth = get_maxhealth(key);
            actor.shield = get_armor(key);
            actor.maxshield = get_maxarmor(key);

            /*if (actor.health < 1.0f && actor.bIsDeathVerge == false)
            {
                it = localCache.erase(it);
                continue;
            }*/

            uintptr_t currentWeapon = GetCurrentWeaponActor(key);
            auto quality = GetWeaponQuality(key);
            auto weaponNames = GetActorFNameString(currentWeapon);

            actor.weaponName = currentWeapon ? GetWeaponName(weaponNames) : "Unarmed";
            actor.weaponQuality = currentWeapon ? (quality + 1) : -1;

            // -------- Bones --------
            GetBones(actor);

            Vector3 headBone = actor.boneData.bonesDouble[(size_t)UniBone::Head];
            Vector3 footBone = actor.boneData.bonesDouble[(size_t)UniBone::FootL];

            if (!actor.boneData.isVisible)
            {
                actor.Drawing = false;
                ++it;
                continue;
            }

            // -------- Radar --------
            ProjectWorldLocationToRadar(
                cam.Location,
                actor.WorldPos,
                cam.Rotation.y,
                actor.RadarPos
            );

            // -------- Box (WORLD → SCREEN) --------
            if (headBone.x <= 0 || headBone.y <= 0 ||
                footBone.x <= 0 || footBone.y <= 0)
            {
                actor.Drawing = false;
                ++it;
                continue;
            }

            actor.ScreenTop = headBone;
            actor.ScreenBottom = footBone;

            // -------- Visibility --------
            actor.isVisible = Visible(key);
            actor.Drawing = true;
            ++it;
            entityStarted = true;
        }

        // Discard work if world changed during processing
        if (m_worldGeneration.load(std::memory_order_acquire) != gen)
            return;

        // --- Swap cache back (brief unique lock ~microseconds) ---
        {
            std::unique_lock<std::shared_mutex> lock(m_playerCacheMutex);
            playerCache = std::move(localCache);
        }
    }
}