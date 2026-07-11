#pragma once

#include <thread>
#include <chrono>
#include <atomic>
#include <functional>
#include <string>

class SyncedThread {
public:
    SyncedThread(const std::function<void()>& func, int interval_ms)
        : running(true), interval(interval_ms), task(func)
    {
        worker = std::thread([this] { this->run(); });
    }

    ~SyncedThread() {
        stop();
    }

    void stop() {
        if (!running.exchange(false)) return;
        if (worker.joinable())
            worker.join();
    }

private:
    std::thread worker;
    std::function<void()> task;
    std::atomic<bool> running;
    int interval;

    void run() {
        auto next_time = std::chrono::steady_clock::now();

        while (running) {
            auto now = std::chrono::steady_clock::now();
            if (now < next_time)
                std::this_thread::sleep_until(next_time);

            if (!running) break;

            task();

            next_time += std::chrono::milliseconds(interval);
        }
    }
};
