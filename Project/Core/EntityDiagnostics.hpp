#pragma once

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace EntityDiagnostics {

inline std::string JsonEscape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 8);
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (ch < 0x20) {
                char escaped[7]{};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
                out += escaped;
            } else {
                out.push_back(static_cast<char>(ch));
            }
        }
    }
    return out;
}

inline std::string Sanitize(std::string_view value, size_t maxLength = 96)
{
    std::string out = JsonEscape(value);
    if (out.size() <= maxLength)
        return out;
    out.resize(maxLength);
    return out;
}

inline bool IsPlausiblePosition(float x, float y, float z)
{
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        return false;
    const double magnitudeSq = static_cast<double>(x) * x
        + static_cast<double>(y) * y + static_cast<double>(z) * z;
    return magnitudeSq > 10000.0 && magnitudeSq < 1.0e14;
}

inline uint32_t BuildIssueFlags(bool hasName, bool positionValid,
    bool positionInitialized, bool drawing, float health, float maxHealth,
    uint64_t positionAgeMs)
{
    uint32_t issues = 0;
    if (!hasName) issues |= 1u << 0;
    if (!positionValid) issues |= 1u << 1;
    if (!positionInitialized) issues |= 1u << 2;
    if (drawing && !positionValid) issues |= 1u << 3;
    if (!std::isfinite(health) || health < 0.0f
        || !std::isfinite(maxHealth) || maxHealth < 0.0f) {
        issues |= 1u << 4;
    }
    if (positionAgeMs > 5000) issues |= 1u << 5;
    return issues;
}

struct EntitySample {
    std::string_view kind;
    std::string_view event = "entity_sample";
    uintptr_t actorKey = 0;
    std::string name;
    std::string detail;
    int category = 0;
    float distance = 0.0f;
    float health = 0.0f;
    float maxHealth = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint64_t positionAgeMs = 0;
    uint32_t issueFlags = 0;
    bool drawing = false;
    bool visible = false;
    bool positionInitialized = true;
};

inline int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

inline std::string IssueText(uint32_t issues)
{
    std::string text;
    const auto add = [&](const char* value) {
        if (!text.empty()) text += ',';
        text += value;
    };
    if (issues & (1u << 0)) add("missing_name");
    if (issues & (1u << 1)) add("invalid_position");
    if (issues & (1u << 2)) add("position_uninitialized");
    if (issues & (1u << 3)) add("drawing_invalid_position");
    if (issues & (1u << 4)) add("invalid_health");
    if (issues & (1u << 5)) add("stale_position");
    return text.empty() ? "none" : text;
}

inline std::string FormatEntity(const EntitySample& sample)
{
    std::ostringstream out;
    out << "{\"timestampMs\":" << NowMs()
        << ",\"severity\":\"" << (sample.issueFlags ? "WARN" : "INFO")
        << "\",\"message\":\"" << sample.event
        << "\",\"data\":{\"kind\":\"" << JsonEscape(sample.kind)
        << "\",\"actorKey\":\"0x" << std::hex << sample.actorKey << std::dec
        << "\",\"name\":\"" << Sanitize(sample.name)
        << "\",\"detail\":\"" << Sanitize(sample.detail)
        << "\",\"category\":" << sample.category
        << ",\"distanceM\":" << sample.distance
        << ",\"health\":" << sample.health
        << ",\"maxHealth\":" << sample.maxHealth
        << ",\"position\":[" << sample.x << ',' << sample.y << ',' << sample.z << ']'
        << ",\"positionAgeMs\":" << sample.positionAgeMs
        << ",\"drawing\":" << (sample.drawing ? "true" : "false")
        << ",\"visible\":" << (sample.visible ? "true" : "false")
        << ",\"positionInitialized\":" << (sample.positionInitialized ? "true" : "false")
        << ",\"issueFlags\":" << sample.issueFlags
        << ",\"issues\":\"" << IssueText(sample.issueFlags) << "\"}}\n";
    return out.str();
}

struct CategorySummary {
    const char* kind = "";
    size_t total = 0;
    size_t drawing = 0;
    size_t issues = 0;
};

inline std::string FormatSummary(const std::string_view runId,
    const std::string_view featureState, const CategorySummary& players,
    const CategorySummary& bots, const CategorySummary& containers,
    const CategorySummary& items, uint64_t generation)
{
    const auto one = [](const CategorySummary& v) {
        return "{\"total\":" + std::to_string(v.total)
            + ",\"drawing\":" + std::to_string(v.drawing)
            + ",\"issues\":" + std::to_string(v.issues) + "}";
    };
    return "{\"timestampMs\":" + std::to_string(NowMs())
        + ",\"severity\":\"INFO\",\"message\":\"entity_summary\""
        + ",\"data\":{\"runId\":\"" + Sanitize(runId, 32)
        + "\",\"generation\":" + std::to_string(generation)
        + ",\"features\":\"" + Sanitize(featureState, 192)
        + "\",\"players\":" + one(players)
        + ",\"bots\":" + one(bots)
        + ",\"containers\":" + one(containers)
        + ",\"items\":" + one(items) + "}}\n";
}

class Logger {
public:
    static Logger& Instance()
    {
        static Logger logger;
        return logger;
    }

    void Init()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running.load(std::memory_order_acquire))
            return;
        m_stopRequested.store(false, std::memory_order_release);
        m_path = ResolvePath();
        m_running.store(true, std::memory_order_release);
        m_worker = std::thread([this] { Run(); });
    }

    void Shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running.load(std::memory_order_acquire))
                return;
            m_stopRequested.store(true, std::memory_order_release);
        }
        m_wake.notify_all();
        if (m_worker.joinable())
            m_worker.join();
        m_running.store(false, std::memory_order_release);
    }

    void Push(std::string line)
    {
        if (!m_running.load(std::memory_order_acquire) || line.empty())
            return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_queue.size() >= kMaxQueuedLines) {
                m_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            m_queue.push_back(std::move(line));
        }
        m_wake.notify_one();
    }

    std::filesystem::path GetPath() const { return m_path; }

private:
    static constexpr size_t kMaxQueuedLines = 4096;
    static constexpr uintmax_t kMaxFileBytes = 16ull * 1024ull * 1024ull;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::string> m_queue;
    std::thread m_worker;
    std::atomic<bool> m_running{ false };
    std::atomic<bool> m_stopRequested{ false };
    std::atomic<uint64_t> m_dropped{ 0 };
    std::filesystem::path m_path;

    static std::filesystem::path ResolvePath()
    {
        wchar_t exePath[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
            return std::filesystem::path(L"help") / L"entity_diagnostics.ndjson";
        const std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
        return exeDir.parent_path() / L"help" / L"entity_diagnostics.ndjson";
    }

    static void RotateIfNeeded(const std::filesystem::path& path, uintmax_t nextBytes)
    {
        std::error_code ec;
        const uintmax_t current = std::filesystem::file_size(path, ec);
        if (!ec && current + nextBytes <= kMaxFileBytes)
            return;
        const std::filesystem::path old(path.wstring() + L".1");
        std::filesystem::remove(old, ec);
        std::filesystem::rename(path, old, ec);
    }

    void WriteLine(std::ofstream& file, std::string line)
    {
        RotateIfNeeded(m_path, line.size());
        file << line;
    }

    void Run()
    {
        std::error_code ec;
        const std::filesystem::path parent = m_path.parent_path();
        std::filesystem::create_directories(parent, ec);
        std::ofstream file(m_path, std::ios::out | std::ios::trunc);
        if (file) {
            const int pid = static_cast<int>(GetCurrentProcessId());
            file << "{\"timestampMs\":" << NowMs()
                 << ",\"severity\":\"INFO\",\"message\":\"session_start\""
                 << ",\"data\":{\"schema\":1,\"pid\":" << pid
                 << ",\"path\":\"" << JsonEscape(m_path.string()) << "\"}}\n";
            file.flush();
        }

        while (true) {
            std::deque<std::string> batch;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_wake.wait(lock, [this] {
                    return m_stopRequested.load(std::memory_order_acquire)
                        || !m_queue.empty();
                });
                batch.swap(m_queue);
            }

            if (file) {
                for (auto& line : batch)
                    WriteLine(file, std::move(line));
                file.flush();
            }

            if (m_stopRequested.load(std::memory_order_acquire) && batch.empty())
                break;
        }

        if (file) {
            const uint64_t dropped = m_dropped.load(std::memory_order_relaxed);
            file << "{\"timestampMs\":" << NowMs()
                 << ",\"severity\":\"" << (dropped ? "WARN" : "INFO")
                 << "\",\"message\":\"session_end\""
                 << ",\"data\":{\"droppedLines\":" << dropped << "}}\n";
            file.flush();
        }
    }
};

inline void Init() { Logger::Instance().Init(); }
inline void Shutdown() { Logger::Instance().Shutdown(); }
inline std::filesystem::path GetPath() { return Logger::Instance().GetPath(); }
inline void LogEntity(const EntitySample& sample)
{
    Logger::Instance().Push(FormatEntity(sample));
}
inline void LogSummary(const std::string_view runId,
    const std::string_view featureState, const CategorySummary& players,
    const CategorySummary& bots, const CategorySummary& containers,
    const CategorySummary& items, uint64_t generation)
{
    Logger::Instance().Push(FormatSummary(
        runId, featureState, players, bots, containers, items, generation));
}
inline void LogScan(const std::string_view scanner, const std::string& dataJson)
{
    Logger::Instance().Push("{\"timestampMs\":" + std::to_string(NowMs())
        + ",\"severity\":\"INFO\",\"message\":\"scan_summary\""
        + ",\"data\":{\"scanner\":\"" + Sanitize(scanner, 32)
        + "\",\"stats\":" + dataJson + "}}\n");
}
inline void LogError(const std::string_view source, const std::string_view message)
{
    Logger::Instance().Push("{\"timestampMs\":" + std::to_string(NowMs())
        + ",\"severity\":\"ERROR\",\"message\":\"diagnostic_error\""
        + ",\"data\":{\"source\":\"" + Sanitize(source, 64)
        + "\",\"detail\":\"" + Sanitize(message) + "\"}}\n");
}
inline void LogBotDecision(uintptr_t actorKey, std::string_view name,
    std::string_view reason, bool drawing, bool identityProven, bool broken,
    bool positionValid, uint64_t positionAgeMs, float distanceM)
{
    Logger::Instance().Push("{\"timestampMs\":" + std::to_string(NowMs())
        + ",\"severity\":\"WARN\",\"message\":\"bot_decision\""
        + ",\"data\":{\"actorKey\":\"0x" + std::to_string(actorKey)
        + "\",\"name\":\"" + Sanitize(name, 64)
        + "\",\"reason\":\"" + Sanitize(reason, 48)
        + "\",\"drawing\":" + (drawing ? "true" : "false")
        + ",\"identityProven\":" + (identityProven ? "true" : "false")
        + ",\"broken\":" + (broken ? "true" : "false")
        + ",\"positionValid\":" + (positionValid ? "true" : "false")
        + ",\"positionAgeMs\":" + std::to_string(positionAgeMs)
        + ",\"distanceM\":" + std::to_string(distanceM) + "}}\n");
}

} // namespace EntityDiagnostics
