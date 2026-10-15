#include "Dumper.h"
#include "includes.h"
#include "Driver.h"
#include "../Core/Memory.h"
#include <sstream>
#include <thread>
#include <mutex>

namespace Dumper {

    std::string g_dumpOutput;
    bool g_isDumping = false;
    std::mutex g_dumpMutex;

    void DumpThread() {
        game::dump::dump_sdk();

        std::lock_guard<std::mutex> lock(g_dumpMutex);
        g_isDumping = false;
    }

    void RunDump() {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        if (g_isDumping) {
            return;
        }

        Sigma::Driver::Base = Memory::getBaseAddress();
        if (!Sigma::Driver::Base) {
            g_dumpOutput = "Error: Game not attached or base address not found.";
            return;
        }

        g_isDumping = true;
        g_dumpOutput = "Dump started...\n";
        
        std::thread(DumpThread).detach();
    }

    const std::string& GetDumpOutput() {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        return g_dumpOutput;
    }

    bool IsDumping() {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        return g_isDumping;
    }

    void Log(const std::string& message) {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        g_dumpOutput += message;
    }

    void ClearLog() {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        g_dumpOutput.clear();
    }
}
