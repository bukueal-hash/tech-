#include "DumperState.h"

#include "DumperPaths.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace Dumper {

namespace {

std::ofstream g_dumperLogFile;

std::string TimestampNow()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmLocal{};
    localtime_s(&tmLocal, &t);
    std::ostringstream oss;
    oss << std::put_time(&tmLocal, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void OpenDumperLogFile()
{
    g_dumperLogFile.close();
    std::error_code ec;
    const auto path = GetDumperLogPath();
    std::filesystem::create_directories(path.parent_path(), ec);
    g_dumperLogFile.open(path, std::ios::out | std::ios::trunc);
    if (g_dumperLogFile.is_open()) {
        g_dumperLogFile << "=== Arc Raiders Dumper Log ===" << std::endl;
        g_dumperLogFile << "started: " << TimestampNow() << std::endl;
        g_dumperLogFile << "path: " << path.string() << std::endl;
        g_dumperLogFile << std::endl;
        g_dumperLogFile.flush();
    }
}

void WriteDumperLogLine(const std::string& line)
{
    if (!g_dumperLogFile.is_open())
        return;
    g_dumperLogFile << line << std::endl;
    g_dumperLogFile.flush();
}

} // namespace

DumperState& DumperState::Instance()
{
    static DumperState s;
    return s;
}

void DumperState::ResetForRun(DumpMode mode)
{
    std::lock_guard<std::mutex> lock(mu_);
    mode_ = mode;
    status_ = DumpStatus::Running;
    progress_.store(0, std::memory_order_relaxed);
    cancel_.store(false, std::memory_order_relaxed);
    log_.clear();
    error_.clear();
    globals_.clear();
    started_ = std::chrono::steady_clock::now();
    finished_ = {};
    OpenDumperLogFile();
}

void DumperState::SetStatus(DumpStatus status)
{
    double elapsed = 0.0;
    std::string errCopy;
    std::vector<GlobalHit> globalsCopy;
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_ = status;
        if (status != DumpStatus::Running)
            finished_ = std::chrono::steady_clock::now();
        const auto end = (finished_.time_since_epoch().count() != 0)
            ? finished_ : std::chrono::steady_clock::now();
        elapsed = std::chrono::duration<double>(end - started_).count();
        errCopy = error_;
        globalsCopy = globals_;
    }
    if (status != DumpStatus::Running && g_dumperLogFile.is_open()) {
        g_dumperLogFile << std::endl << "--- finished: " << TimestampNow()
                        << " status=" << static_cast<int>(status)
                        << " elapsed=" << elapsed << "s ---" << std::endl;
        if (!errCopy.empty())
            g_dumperLogFile << "error: " << errCopy << std::endl;
        for (const GlobalHit& hit : globalsCopy) {
            g_dumperLogFile << "global " << hit.name << " rva=" << std::hex << std::uppercase
                            << hit.rva << std::dec << " [" << hit.confidence << "]";
            if (!hit.notes.empty())
                g_dumperLogFile << " " << hit.notes;
            g_dumperLogFile << std::endl;
        }
        g_dumperLogFile.flush();
    }
}

DumpStatus DumperState::GetStatus() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}

void DumperState::SetProgress(int pct)
{
    progress_.store(std::clamp(pct, 0, 100), std::memory_order_relaxed);
}

int DumperState::GetProgress() const
{
    return progress_.load(std::memory_order_relaxed);
}

void DumperState::RequestCancel()
{
    cancel_.store(true, std::memory_order_relaxed);
}

bool DumperState::IsCancelRequested() const
{
    return cancel_.load(std::memory_order_relaxed);
}

void DumperState::ClearCancel()
{
    cancel_.store(false, std::memory_order_relaxed);
}

void DumperState::AppendLog(const std::string& line)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        log_.push_back(line);
        constexpr size_t kMaxLines = 200;
        if (log_.size() > kMaxLines)
            log_.erase(log_.begin(), log_.begin() + (log_.size() - kMaxLines));
    }
    WriteDumperLogLine(line);
}

void DumperState::SetError(const std::string& msg)
{
    std::lock_guard<std::mutex> lock(mu_);
    error_ = msg;
}

std::string DumperState::GetError() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

std::vector<std::string> DumperState::GetLogSnapshot() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return log_;
}

void DumperState::SetGlobals(const std::vector<GlobalHit>& hits)
{
    std::lock_guard<std::mutex> lock(mu_);
    globals_ = hits;
}

std::vector<GlobalHit> DumperState::GetGlobals() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return globals_;
}

double DumperState::GetElapsedSeconds() const
{
    std::lock_guard<std::mutex> lock(mu_);
    const auto end = (finished_.time_since_epoch().count() != 0) ? finished_ : std::chrono::steady_clock::now();
    return std::chrono::duration<double>(end - started_).count();
}

} // namespace Dumper
