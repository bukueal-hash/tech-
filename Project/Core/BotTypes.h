#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

inline const std::unordered_set<std::string> kRobotsList = {
    "Snitch", "Turret", "Leaper", "The Queen", "Bombardier", "Bastion",
    "Sentinel", "Shredder", "Hornet", "Wasp", "Fireball", "Pop",
    "Surveyor", "Spotter", "Rocketeer", "Matriarch", "Comet", "Firefly",
    "Turbine", "Vaporizer", "Tick", "Harvester", "Monolith",
    "Husk", "Husk S", "Husk M", "Husk L",
};

inline bool IsRobotsListType(const std::string& type)
{
    return kRobotsList.contains(type);
}

inline std::string NormalizeBotDisplayName(const std::string& display)
{
    static const std::unordered_map<std::string, std::string> kMap = {
        { "Light Drone", "Wasp" },
        { "Heavy Drone", "Rocketeer" },
        { "ARC Surveyor", "Surveyor" },
        { "ARC Turbine", "Turbine" },
        { "Blaze Hornet", "Firefly" },
        { "Queen", "The Queen" },
        { "Arc Husk", "Husk" },
        { "Rollbot", "Surveyor" },
        { "Drone", "Wasp" },
        { "Heavy", "Rocketeer" },
        { "Elite", "Hornet" },
    };
    if (auto it = kMap.find(display); it != kMap.end())
        return it->second;
    return display;
}
