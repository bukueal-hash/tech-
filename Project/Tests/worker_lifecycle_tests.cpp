#include "Interface/Utils/Threads/SyncedThread.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "doctest/doctest.h"

TEST_CASE("ManagedJob joins a completed one-shot job")
{
    ManagedJob job;
    std::atomic<int> calls{ 0 };

    REQUIRE(job.start([&] { calls.fetch_add(1, std::memory_order_release); }));
    job.stop();

    CHECK_FALSE(job.running());
    CHECK(calls.load(std::memory_order_acquire) == 1);
}

TEST_CASE("ManagedJob records task exceptions and remains stoppable")
{
    ManagedJob job;

    REQUIRE(job.start([] { throw std::runtime_error("expected worker failure"); }));
    job.stop();

    CHECK_FALSE(job.running());
    CHECK(job.lastError() == "expected worker failure");
}

TEST_CASE("SyncedThread wakes promptly when stopped")
{
    std::atomic<int> calls{ 0 };
    SyncedThread worker([&] { calls.fetch_add(1, std::memory_order_release); }, 60000);

    const auto started = std::chrono::steady_clock::now();
    worker.stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    CHECK(elapsed < std::chrono::seconds(1));
    CHECK(calls.load(std::memory_order_acquire) <= 1);
}

TEST_CASE("SyncedThread supports repeated stop calls")
{
    SyncedThread worker([] {}, 60000);
    worker.stop();
    CHECK_NOTHROW(worker.stop());
}
