#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

// NDJSON instrumentation. Paint/Present must NEVER open files — use ArcAsyncLog_Push.
#ifndef ARC_AGENT_NDJSON
#define ARC_AGENT_NDJSON 0
#endif

inline int64_t ArcAgentLogNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline std::string ArcAgentLogJsonEscape(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) >= 0x20)
                out += c;
            break;
        }
    }
    return out;
}

/** Dedicated writer thread: ofstream lives here only, never on Present. */
struct ArcAsyncLogQueue {
    struct Entry {
        std::string path;
        std::string line;
    };

    std::queue<Entry> q;
    std::mutex mtx;
    std::condition_variable cv;
    std::thread worker;
    std::atomic<bool> running{ false };
    static constexpr size_t kMaxQueued = 256;

    void start()
    {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true))
            return;
        worker = std::thread([this] {
            while (true) {
                Entry e;
                {
                    std::unique_lock<std::mutex> lock(mtx);
                    cv.wait(lock, [this] {
                        return !q.empty() || !running.load(std::memory_order_relaxed);
                    });
                    if (q.empty() && !running.load(std::memory_order_relaxed))
                        break;
                    if (q.empty())
                        continue;
                    e = std::move(q.front());
                    q.pop();
                }
                std::ofstream f(e.path, std::ios::app);
                if (f)
                    f << e.line;
            }
        });
    }

    void push(std::string path, std::string line)
    {
        if (!running.load(std::memory_order_relaxed))
            start();
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (q.size() >= kMaxQueued)
                return; // drop — never block paint waiting on disk
            q.push(Entry{ std::move(path), std::move(line) });
        }
        cv.notify_one();
    }

    void stop()
    {
        if (!running.exchange(false))
            return;
        cv.notify_one();
        if (worker.joinable())
            worker.join();
    }
};

inline ArcAsyncLogQueue& ArcAsyncLogInstance()
{
    static ArcAsyncLogQueue inst;
    return inst;
}

inline void ArcAsyncLog_Start()
{
    ArcAsyncLogInstance().start();
}

inline void ArcAsyncLog_Stop()
{
    ArcAsyncLogInstance().stop();
}

inline void ArcAsyncLog_Push(std::string path, std::string line)
{
    ArcAsyncLogInstance().push(std::move(path), std::move(line));
}

inline constexpr const char* kArcDebugLogPath = "F:/Test/ARCs/debug-c190fb.log";

inline void ArcAgentLog(
    const char* runId,
    const char* hypothesisId,
    const char* location,
    const char* message,
    const std::string& dataJson)
{
#if !ARC_AGENT_NDJSON
    (void)runId;
    (void)hypothesisId;
    (void)location;
    (void)message;
    (void)dataJson;
#else
    std::string line;
    line.reserve(256 + dataJson.size());
    line += "{\"sessionId\":\"c190fb\"";
    line += ",\"runId\":\"";
    line += runId;
    line += "\",\"hypothesisId\":\"";
    line += hypothesisId;
    line += "\",\"location\":\"";
    line += location;
    line += "\",\"message\":\"";
    line += message;
    line += "\",\"data\":";
    line += dataJson;
    line += ",\"timestamp\":";
    line += std::to_string(ArcAgentLogNowMs());
    line += "}\n";
    ArcAsyncLog_Push(kArcDebugLogPath, std::move(line));
#endif
}

