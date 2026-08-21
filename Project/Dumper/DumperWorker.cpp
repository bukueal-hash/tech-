#include "DumperWorker.h"

#include "DumperPaths.h"
#include "GlobalDiscovery.h"
#include "ModuleImageCache.h"
#include "SdkDumper.h"

#include "../Core/SteamDecrypt.hpp"
#include "../DMA/Memory.h"

#include <exception>
#include <thread>

namespace Dumper {

namespace {

std::thread g_worker;
std::mutex g_workerMu;

struct FNameStateGuard {
    steam_decrypt::FNameStateSnapshot snap;
    FNameStateGuard() : snap(steam_decrypt::SnapshotFNameState()) {}
    ~FNameStateGuard() { steam_decrypt::RestoreFNameState(snap); }
};

void RunDumpJob(DumpMode mode)
{
    auto& state = DumperState::Instance();
    const FNameStateGuard fnameGuard;
    try {
        const uint64_t moduleBase = g_mem.GetBase();
        const uint32_t imageSize = g_mem.GetImageSize();
        if (!moduleBase || !imageSize) {
            state.SetError("DMA not attached or image size unknown");
            state.SetStatus(DumpStatus::Error);
            return;
        }

        ModuleImageCache cache;
        if (!cache.Load(moduleBase, imageSize, state)) {
            if (state.IsCancelRequested()) {
                state.SetStatus(DumpStatus::Cancelled);
                return;
            }
            const auto logs = state.GetLogSnapshot();
            std::string detail = "Failed to cache module image";
            for (auto it = logs.rbegin(); it != logs.rend(); ++it) {
                if (it->find("Module cache:") != std::string::npos) {
                    detail = *it;
                    break;
                }
            }
            state.SetError(detail);
            state.SetStatus(DumpStatus::Error);
            return;
        }

        if (state.IsCancelRequested()) {
            state.SetStatus(DumpStatus::Cancelled);
            return;
        }

        const DiscoveredGlobals globals = DiscoverGlobals(moduleBase, cache, state);
        if (!WriteGlobalsJson(GetGlobalsJsonPath(), moduleBase, imageSize, globals)) {
            state.AppendLog("Warning: failed to write globals.json");
        } else {
            state.AppendLog("Wrote " + GetGlobalsJsonPath().string());
        }

        std::string sdkSummary;
        if (mode == DumpMode::FullSdk) {
            if (state.IsCancelRequested()) {
                state.SetStatus(DumpStatus::Cancelled);
                return;
            }
            const SdkDumpResult sdk = RunSdkDump(moduleBase, globals, GetDumpTxtPath(), state);
            sdkSummary = sdk.summary;
            if (sdk.classesWritten > 0)
                state.AppendLog("Wrote " + GetDumpTxtPath().string());
        }

        AppendDumpReport(GetDumpReportPath(), globals, sdkSummary);
        state.AppendLog("Wrote " + GetDumpReportPath().string());
        state.SetProgress(100);
        state.SetStatus(state.IsCancelRequested() ? DumpStatus::Cancelled : DumpStatus::Done);
    } catch (const std::exception& ex) {
        state.SetError(ex.what());
        state.SetStatus(DumpStatus::Error);
    } catch (...) {
        state.SetError("Unknown dumper error");
        state.SetStatus(DumpStatus::Error);
    }
}

} // namespace

void StartDump(DumpMode mode)
{
    std::lock_guard<std::mutex> lock(g_workerMu);
    if (g_worker.joinable())
        g_worker.join();

    auto& state = DumperState::Instance();
    if (state.GetStatus() == DumpStatus::Running)
        return;

    state.ResetForRun(mode);
    state.AppendLog(mode == DumpMode::FullSdk ? "Starting full SDK dump" : "Starting globals scan");
    state.AppendLog("Tip: best run from menu with ESP off");

    g_worker = std::thread([mode]() {
        RunDumpJob(mode);
    });
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_workerMu);
    DumperState::Instance().RequestCancel();
    if (g_worker.joinable())
        g_worker.join();
}

} // namespace Dumper
