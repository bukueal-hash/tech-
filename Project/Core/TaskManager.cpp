#include "TaskManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

bool IsHeavyDmaTask(const std::string& name)
{
    return name == "RobotList"
        || name == "ContainerList"
        || name == "ItemList"
        || name == "VisRebuild";
}

void TaskManager::addTask(const std::string& name, std::function<void()> func,
    const double* interval, double phaseOffsetMs)
{
    TimedTask task;
    task.function = std::move(func);
    task.interval = interval;
    task.elapsedTime = -phaseOffsetMs;
    task.phaseOffsetMs = phaseOffsetMs;
    tasks[name] = std::move(task);
}

void TaskManager::removeTask(const std::string& name)
{
    tasks.erase(name);
}

void TaskManager::run(const std::atomic<bool>& stopFlag)
{
    previousTime = std::chrono::steady_clock::now();

    while (!stopFlag.load(std::memory_order_acquire)) {
        try {
            const auto currentTime = std::chrono::steady_clock::now();
            double deltaTime = std::chrono::duration<double, std::milli>(
                currentTime - previousTime).count();
            previousTime = currentTime;
            deltaTime = std::clamp(deltaTime, 0.0, 100.0);
            updateTasks(deltaTime);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } catch (const std::exception& e) {
            std::cerr << "[TaskManager] exception: " << e.what() << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        } catch (...) {
            std::cerr << "[TaskManager] unknown exception" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
}

void TaskManager::updateTasks(double deltaTime)
{
    auto IsDue = [](const TimedTask& task) -> bool {
        return task.interval
            && task.function
            && *task.interval > 0.0
            && task.elapsedTime >= *task.interval;
    };

    auto RunTask = [](const std::string& /*name*/, TimedTask& task) {
        try {
            task.function();
        } catch (const std::exception& e) {
            std::cerr << "[TaskManager] task threw: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[TaskManager] task threw unknown" << std::endl;
        }
        // Skip missed cycles — do not back-to-back catch up.
        task.elapsedTime = 0.0;
    };

    for (auto& [name, task] : tasks) {
        if (!task.interval || !task.function || *task.interval <= 0.0)
            continue;
        task.elapsedTime += deltaTime;
    }

    // Camera-first within this manager (hot lane uses UpdateCamera).
    if (!m_cameraTaskName.empty()) {
        auto cameraIt = tasks.find(m_cameraTaskName);
        if (cameraIt != tasks.end() && IsDue(cameraIt->second))
            RunTask(cameraIt->first, cameraIt->second);
    }

    std::string selectedHeavy;
    double highestOverdue = -1.0;
    for (auto& [name, task] : tasks) {
        if (!IsHeavyDmaTask(name) || !IsDue(task))
            continue;
        const double overdue = task.elapsedTime - *task.interval;
        if (overdue > highestOverdue) {
            highestOverdue = overdue;
            selectedHeavy = name;
        }
    }

    for (auto& [name, task] : tasks) {
        if (!m_cameraTaskName.empty() && name == m_cameraTaskName)
            continue;
        if (!IsDue(task))
            continue;
        if (IsHeavyDmaTask(name) && !selectedHeavy.empty() && name != selectedHeavy)
            continue;
        RunTask(name, task);
    }
}
