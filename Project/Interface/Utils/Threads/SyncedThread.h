#pragma once
#include <Windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <string>
#include <utility>

/**
 * A small, joinable background-job primitive used by diagnostic scans.
 * Jobs are deliberately not detached: the owner can always cancel and join
 * them before the process or engine tears down its dependencies.
 */
class ManagedJob {
public:
    ManagedJob() = default;
    ManagedJob(const ManagedJob&) = delete;
    ManagedJob& operator=(const ManagedJob&) = delete;

    ~ManagedJob() { stop(); }

    bool start(std::function<void()> task) {
        if (!task) return false;
        {
            std::lock_guard<std::mutex> lock(m_stateMu);
            if (m_running.load(std::memory_order_acquire)) return false;
        }
        // Never join while holding the error mutex: the worker records an
        // exception under that mutex before publishing m_running=false.
        if (m_worker.joinable()) m_worker.join();

        std::lock_guard<std::mutex> lock(m_stateMu);
        m_lastError.clear();
        m_running.store(true, std::memory_order_release);
        m_worker = std::thread([this, task = std::move(task)]() mutable {
            try {
                task();
            } catch (const std::exception& ex) {
                {
                    std::lock_guard<std::mutex> lock(m_stateMu);
                    m_lastError = ex.what();
                }
                std::cerr << "[ManagedJob] exception: " << ex.what() << '\n';
            } catch (...) {
                {
                    std::lock_guard<std::mutex> lock(m_stateMu);
                    m_lastError = "unknown exception";
                }
                std::cerr << "[ManagedJob] exception: unknown\n";
            }
            m_running.store(false, std::memory_order_release);
        });
        return true;
    }

    void stop() {
        m_running.store(false, std::memory_order_release);
        if (m_worker.joinable()) m_worker.join();
    }

    bool running() const { return m_running.load(std::memory_order_acquire); }

    std::string lastError() const {
        std::lock_guard<std::mutex> lock(m_stateMu);
        return m_lastError;
    }

private:
    mutable std::mutex m_stateMu;
    std::thread m_worker;
    std::atomic<bool> m_running{ false };
    std::string m_lastError;
};

class SyncedThread {
public:
    SyncedThread(const std::function<void()>& func, int interval_ms)
        : running(true), interval(interval_ms < 1 ? 1 : interval_ms), task(func)
    {
        worker = std::thread([this] { this->run(); });
    }

    ~SyncedThread() { stop(); }

    void stop() {
        running.store(false, std::memory_order_release);
        wake.notify_all();
        if (worker.joinable()) worker.join();
    }

    void set_priority(int priority) {
        if (worker.joinable())
            SetThreadPriority(worker.native_handle(), priority);
    }

private:
    std::thread worker;
    std::function<void()> task;
    std::atomic<bool> running;
    int interval;
    std::mutex wakeMutex;
    std::condition_variable wake;

    void run() {
        auto nextTime = std::chrono::steady_clock::now();

        while (running.load(std::memory_order_acquire)) {
            {
                std::unique_lock<std::mutex> lock(wakeMutex);
                if (running.load(std::memory_order_acquire)
                    && wake.wait_until(lock, nextTime, [this] {
                        return !running.load(std::memory_order_acquire);
                    })) {
                    break;
                }
            }
            if (!running.load(std::memory_order_acquire)) break;

            try {
                task();
            } catch (const std::exception& ex) {
                std::cerr << "[SyncedThread] task exception: " << ex.what() << '\n';
            } catch (...) {
                std::cerr << "[SyncedThread] task exception: unknown\n";
            }

            const auto after = std::chrono::steady_clock::now();
            nextTime += std::chrono::milliseconds(interval);
            if (nextTime < after) nextTime = after + std::chrono::milliseconds(interval);
        }
    }
};
