#include "../Core/Engine.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_set>

void Engine::WorldList()
{
    // Snapshot state under shared lock
    uintptr_t sGWorld, sPersistentLevel, sActors;
    {
        std::shared_lock<std::shared_mutex> slock(m_stateMutex);
        sGWorld = GWorld;
        sPersistentLevel = PersistentLevel;
        sActors = Actors;
    }

    if (sGWorld && sPersistentLevel && sActors) {

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
            (float)(var::world_distance *
                var::world_distance *
                10000.0f);

        // --- Copy cache to local (brief shared lock) ---
        std::unordered_map<uintptr_t, WorldCacheEntry> localCache;
        {
            std::shared_lock<std::shared_mutex> lock(m_worldCacheMutex);
            localCache = worldCache;
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

            auto name = GetActorFNameStringCached(actor);
            if (name.empty()) {
                continue;
            }

            std::string itemName = getEntityType(name);
            if (itemName == "Invalid")
                continue;

            if (robotsList.contains(itemName))
                continue;

            auto allowed = getAllowType(itemName);
            if (!allowed)
                continue;

            uintptr_t root =
                Memory::read<uintptr_t>(actor + Offsets::RootComponent);
            if (!root)
                continue;

            auto& entry = localCache.emplace(
                actor,
                WorldCacheEntry(itemName.c_str(), root, actor, 0)
            ).first->second;

            if (itemName == "Loot Item")
            {
                entry.ItemType = "Loot Item";

                std::string realName = GetEnglishItemName(actor);
                if (!realName.empty())
                {
                    entry.ItemDisplayName = realName;
                    entry.ActorName = realName;
                }
            }
            else
            {
                entry.ItemType = itemName;
            }
        }

        for (auto it = localCache.begin(); it != localCache.end(); )
        {
            auto& actor = it->second;
            uintptr_t key = it->first;

            auto allowed = getAllowType(actor.ItemType);
            if (!allowed)
            {
                it = localCache.erase(it);
                continue;
            }

            actor.category = 1; // World

            if (actor.ActorName == "Corpse")
            {
                auto playerState = Memory::read<uintptr_t>(key + Offsets::APlayerState);
                if (playerState && IsUsermodePtr(playerState))
                {
                    PlayerHealthInfo healthInfo = Memory::read<PlayerHealthInfo>(playerState + Offsets::HealthInfo);
                    float health = get_health(key);

                    if (health > 1.0f || healthInfo.bIsDbno)
                    {
                        it = localCache.erase(it);
                        continue;
                    }
                }
            }

            FTransform transform =
                Memory::read<FTransform>(
                    actor.rootComponent + Offsets::ComponentToWorld
                );

            actor.WorldPos = transform.Translation;

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
            actor.Drawing = true;
            ++it;
            entityStarted = true;
        }

        // Discard work if world changed during processing
        if (m_worldGeneration.load(std::memory_order_acquire) != gen)
            return;

        // --- Swap cache back (brief unique lock ~microseconds) ---
        {
            std::unique_lock<std::shared_mutex> lock(m_worldCacheMutex);
            worldCache = std::move(localCache);
        }
    }
}