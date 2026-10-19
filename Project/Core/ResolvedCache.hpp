#pragma once
//
// Auto-learned offsets. Every resolved value that validates against live memory
// is written to Data\resolved.ini next to the exe, and read back first on the
// next launch. Nothing is typed by hand: a value is pinned the moment its
// validation passes, and a pinned value that stops validating falls back to the
// normal search, which then pins the new answer.
//
//   key=value      one line per key (value is a hex RVA, an integer, or text)

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>

namespace ResolvedCache {
namespace detail {

inline std::mutex& Mu()
{
    static std::mutex m;
    return m;
}

inline std::map<std::string, std::string>& Map()
{
    static std::map<std::string, std::string> m;
    return m;
}

inline bool& Loaded()
{
    static bool b = false;
    return b;
}

inline std::filesystem::path FilePath()
{
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return {};
    return std::filesystem::path(exePath).parent_path() / L"Data" / L"resolved.ini";
}

inline void LoadLocked()
{
    if (Loaded())
        return;
    Loaded() = true;
    std::ifstream in(FilePath());
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos || eq == 0)
            continue;
        Map()[line.substr(0, eq)] = line.substr(eq + 1);
    }
}

inline void FlushLocked()
{
    std::ofstream out(FilePath(), std::ios::trunc);
    for (const auto& kv : Map())
        out << kv.first << '=' << kv.second << '\n';
}

} // namespace detail

// The pinned value for key, or "" when nothing has validated for it yet.
inline std::string Get(const char* key)
{
    std::lock_guard<std::mutex> lock(detail::Mu());
    detail::LoadLocked();
    const auto it = detail::Map().find(key);
    return it == detail::Map().end() ? std::string{} : it->second;
}

// Pin a value that just validated. The file is written only when it changes.
inline void Put(const char* key, const std::string& value)
{
    std::lock_guard<std::mutex> lock(detail::Mu());
    detail::LoadLocked();
    const auto it = detail::Map().find(key);
    if (it != detail::Map().end() && it->second == value)
        return;
    detail::Map()[key] = value;
    detail::FlushLocked();
}

} // namespace ResolvedCache
