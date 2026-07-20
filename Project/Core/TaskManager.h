#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <unordered_map>

/** Meaty-style timed DMA scheduler: camera-first, one-heavy-per-tick, skip catch-up. */
class TaskManager {
public:
    struct TimedTask {
        std::function<void()> function;
        const double* interval = nullptr;
        double elapsedTime = 0.0;
        double phaseOffsetMs = 0.0;
    };

    void addTask(const std::string& name, std::function<void()> func,
        const double* interval, double phaseOffsetMs = 0.0);
    void removeTask(const std::string& name);

    /** Run until stopFlag is false. Sleeps ~1ms per tick. */
    void run(const std::atomic<bool>& stopFlag);

    void updateTasks(double deltaTime);

    /** Prefer cameraTask name for camera-first within a manager. */
    void setCameraTaskName(std::string name) { m_cameraTaskName = std::move(name); }

private:
    std::unordered_map<std::string, TimedTask> tasks;
    std::chrono::steady_clock::time_point previousTime{};
    std::string m_cameraTaskName{ "UpdateCamera" };
};

bool IsHeavyDmaTask(const std::string& name);
