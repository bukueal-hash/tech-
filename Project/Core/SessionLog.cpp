#include "SessionLog.h"

#include <Windows.h>

#include <fstream>
#include <iostream>
#include <mutex>

namespace SessionLog {

namespace {

std::mutex g_mu;
std::ofstream g_file;

class FileBuf : public std::streambuf {
protected:
    int overflow(int c) override
    {
        if (c == EOF)
            return EOF;
        const char ch = static_cast<char>(c);
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_file.is_open()) {
            g_file.put(ch);
            g_file.flush();
        }
        return c;
    }

    std::streamsize xsputn(const char* s, std::streamsize n) override
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (g_file.is_open()) {
            g_file.write(s, n);
            g_file.flush();
        }
        return n;
    }
};

FileBuf g_buf;

std::filesystem::path ResolveHelpDir()
{
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return std::filesystem::path("help");
    const std::filesystem::path exeDir = std::filesystem::path(exePath).parent_path();
    if (exeDir.empty())
        return std::filesystem::path("help");
    return exeDir.parent_path() / "help";
}

} // namespace

void Init()
{
    std::error_code ec;
    const std::filesystem::path dir = ResolveHelpDir();
    std::filesystem::create_directories(dir, ec);

    const std::filesystem::path path = dir / "session_log.txt";
    g_file.open(path, std::ios::out | std::ios::trunc);
    std::cout.rdbuf(&g_buf);
    std::cout << "[session] logging to " << path.string() << std::endl;
}

std::filesystem::path GetPath()
{
    return ResolveHelpDir() / "session_log.txt";
}

} // namespace SessionLog
