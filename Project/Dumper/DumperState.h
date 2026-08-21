#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace Dumper {

enum class DumpStatus {
    Idle,
    Running,
    Done,
    Error,
    Cancelled
};

enum class DumpMode {
    GlobalsOnly,
    FullSdk
};

struct GlobalHit {
    std::string name;
    uint64_t rva = 0;
    std::string confidence;
    std::string sig;
    std::string notes;
};

class DumperState {
public:
    static DumperState& Instance();

    void ResetForRun(DumpMode mode);
    void SetStatus(DumpStatus status);
    DumpStatus GetStatus() const;

    void SetProgress(int pct);
    int GetProgress() const;

    void RequestCancel();
    bool IsCancelRequested() const;
    void ClearCancel();

    void AppendLog(const std::string& line);
    std::vector<std::string> GetLogSnapshot() const;

    void SetError(const std::string& msg);
    std::string GetError() const;

    void SetGlobals(const std::vector<GlobalHit>& hits);
    std::vector<GlobalHit> GetGlobals() const;

    double GetElapsedSeconds() const;

private:
    DumperState() = default;

    mutable std::mutex mu_;
    DumpStatus status_ = DumpStatus::Idle;
    DumpMode mode_ = DumpMode::GlobalsOnly;
    std::atomic<int> progress_{0};
    std::atomic<bool> cancel_{false};
    std::vector<std::string> log_;
    std::string error_;
    std::vector<GlobalHit> globals_;
    std::chrono::steady_clock::time_point started_{};
    std::chrono::steady_clock::time_point finished_{};
};

} // namespace Dumper
