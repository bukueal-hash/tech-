#include "SessionLog.h"

#include <Windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <mutex>

namespace SessionLog {
namespace {

std::mutex g_mu;
std::ofstream g_file;
std::streambuf* g_original = nullptr;
std::filesystem::path g_path;
uintmax_t g_written = 0;
constexpr uintmax_t kMaxBytes = 16ull * 1024ull * 1024ull;

bool Rotate()
{
    g_file.flush();
    if (!g_file)
        return false;
    g_file.close();
    if (g_file.fail())
        return false;
    const std::filesystem::path previous(g_path.wstring() + L".1");
    std::error_code ec;
    std::filesystem::remove(previous, ec);
    if (!ec)
        std::filesystem::rename(g_path, previous, ec);
    if (ec) {
        g_file.open(g_path, std::ios::out | std::ios::app | std::ios::binary);
        return false;
    }
    g_file.open(g_path, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!g_file)
        return false;
    g_written = 0;
    return true;
}

class FileBuf : public std::streambuf {
protected:
    int_type overflow(int_type c) override
    {
        if (traits_type::eq_int_type(c, traits_type::eof()))
            return traits_type::not_eof(c);
        const char ch = traits_type::to_char_type(c);
        return xsputn(&ch, 1) == 1 ? c : traits_type::eof();
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override
    {
        if (n <= 0)
            return 0;
        std::lock_guard<std::mutex> lock(g_mu);
        std::streamsize written = 0;
        while (written < n && g_file) {
            if (g_written >= kMaxBytes && !Rotate())
                break;
            const auto count = static_cast<std::streamsize>((std::min)(
                static_cast<uintmax_t>(n - written), kMaxBytes - g_written));
            g_file.write(s + written, count);
            if (!g_file)
                break;
            g_written += static_cast<uintmax_t>(count);
            written += count;
        }
        if (written != n)
            OutputDebugStringA("[session] Log write/rotation failed.\n");
        return written;
    }

    int sync() override
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_file.flush();
        if (g_file)
            return 0;
        OutputDebugStringA("[session] Log flush failed.\n");
        return -1;
    }
};

FileBuf g_buf;

std::filesystem::path ResolveHelpDir()
{
    wchar_t exePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return std::filesystem::path("help");
    const std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    return exeDir.empty() ? std::filesystem::path("help") : exeDir.parent_path() / "help";
}

} // namespace

void Init()
{
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_original)
            return;
        std::error_code ec;
        const std::filesystem::path dir = ResolveHelpDir();
        std::filesystem::create_directories(dir, ec);
        if (ec) {
            OutputDebugStringA("[session] Could not create log directory.\n");
            return;
        }
        g_path = dir / "session_log.txt";
        g_file.open(g_path, std::ios::out | std::ios::trunc | std::ios::binary);
        if (!g_file) {
            OutputDebugStringA("[session] Could not open log; console stream retained.\n");
            return;
        }
        g_written = 0;
        g_original = std::cout.rdbuf(&g_buf);
    }
    std::cout << "[session] logging to " << g_path.string() << std::endl;
}

void Shutdown()
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_original)
        return;
    // Call after all logging workers join, before static FileBuf destruction.
    std::cout.rdbuf(g_original);
    g_original = nullptr;
    g_file.flush();
    if (!g_file)
        OutputDebugStringA("[session] Final log flush failed.\n");
    g_file.close();
}

std::filesystem::path GetPath()
{
    return ResolveHelpDir() / "session_log.txt";
}

} // namespace SessionLog
