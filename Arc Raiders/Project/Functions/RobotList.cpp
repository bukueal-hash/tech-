#include "../Core/Engine.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <unordered_set>

void Engine::RobotList()
{
    // Snapshot state under shared lock (brief)
    uintptr_t sGWorld, sPersistentLevel, sActors;
    {
        std::shared_lock<std::shared_mutex> slock(m_stateMutex);
        sGWorld = GWorld;
        sPersistentLevel = PersistentLevel;
        sActors = Actors;
    }

    if (!sGWorld || !sPersistentLevel || !sActors)
        return;

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
        std::shared_lock<std::shared_mutex> lock(m_robotCacheMutex);
        localCache = robotCache;
    }

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

        std::string itemName = getEntityType(name);
        if (itemName == "Invalid")
            continue;

        if (!robotsList.contains(itemName))
            continue;

        auto IsBreaked = Memory::read<bool>(actor + Offsets::bIsBreaked);
        if (IsBreaked)
            continue;

        auto allowed = getAllowType(itemName);
        if (!allowed)
            continue;

        uintptr_t root =
            Memory::read<uintptr_t>(actor + Offsets::RootComponent);
        if (!root)
            continue;

        uintptr_t mesh =
            Memory::read<uintptr_t>(actor + Offsets::USkeletalMeshComponent);
        if (!mesh)
            continue;

        auto& entry = localCache.emplace(
            actor,
            WorldCacheEntry(itemName.c_str(), root, actor, mesh)
        ).first->second;

        entry.ItemType = itemName;
    }

    for (auto it = localCache.begin(); it != localCache.end(); )
    {
        auto& actor = it->second;
        uintptr_t key = it->first;

        auto allowed = getAllowType(actor.ActorName);
        if (!allowed)
        {
            it = localCache.erase(it);
            continue;
        }

        actor.category = 3; // Robot

        actor.IsBreaked = Memory::read<bool>(key + Offsets::bIsBreaked);

        if (actor.IsBreaked) {
            it = localCache.erase(it);
            continue;
        }

        actor.WorldPos = Memory::read<Vector3>(actor.rootComponent + Offsets::RelativeLocation);

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
    }

    // Discard work if world changed during processing
    if (m_worldGeneration.load(std::memory_order_acquire) != gen)
        return;

    // --- Swap cache back (brief unique lock ~microseconds) ---
    {
        std::unique_lock<std::shared_mutex> lock(m_robotCacheMutex);
        robotCache = std::move(localCache);
    }
}