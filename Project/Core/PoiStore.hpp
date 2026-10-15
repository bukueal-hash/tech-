#pragma once
// File-backed POI store (D17). Deliberately separate from PoiParser.hpp: the
// parser stays free of Windows and file dependencies so Project.Tests can
// include it on its own, while this header owns the on-disk half.
//
// Every function is inline and the cache lives in an inline function-local
// static, so all translation units share ONE copy (C++ guarantees a single
// instance of an inline function's local static).
//
// Data\pois.json is the source of truth: it is read once and rewritten whole
// on save, exactly like auto_config.ini.

#include "PoiParser.hpp"

#include <Windows.h>

#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace PoiStore {

// Same convention as AutoConfig's ConfigPath(): narrow module path plus a
// filename, so plain std::ifstream works the way the rest of the app already
// does file IO.
inline std::string FilePath()
{
    char path[MAX_PATH]{};
    if (GetModuleFileNameA(nullptr, path, MAX_PATH) == 0)
        return "Data\\pois.json";
    std::string dir(path);
    const size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos)
        dir.resize(slash + 1);
    return dir + "Data\\pois.json";
}

inline std::vector<PoiParser::Poi>& Items()
{
    static std::vector<PoiParser::Poi> items;
    return items;
}

inline bool& Loaded()
{
    static bool loaded = false;
    return loaded;
}

enum class LoadStatus { NotLoaded, Missing, ReadFailed, Invalid, Empty, Loaded };
inline LoadStatus& Status()
{
    static LoadStatus status = LoadStatus::NotLoaded;
    return status;
}

inline bool& LoadSucceeded()
{
    static bool succeeded = false;
    return succeeded;
}

// Escape a string for embedding inside a JSON string literal. Control bytes
// are dropped rather than emitted, so the written file always re-parses.
inline std::string EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
                continue;
            out += c;
            break;
        }
    }
    return out;
}

/** Read Data\pois.json once. The loaded flag is set BEFORE the read so a
 *  missing file is not retried every frame — the radar draws nothing and the
 *  menu says so. Returns false when the file is absent, unreadable, malformed
 *  JSON. A valid empty array succeeds and has a distinct Empty status. */
inline bool EnsureLoaded()
{
    if (Loaded())
        return LoadSucceeded();
    Loaded() = true;

    const std::string path = FilePath();
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        const DWORD error = GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES
            ? GetLastError() : ERROR_SUCCESS;
        Status() = (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            ? LoadStatus::Missing : LoadStatus::ReadFailed;
        return false;
    }
    // Bound hand-edited input before buffering/parsing it on the paint thread.
    constexpr std::streamoff kMaxPoiBytes = 4 * 1024 * 1024;
    const auto size = in.tellg();
    if (size < std::streampos(0) || size > std::streampos(kMaxPoiBytes)) {
        Status() = LoadStatus::ReadFailed;
        return false;
    }
    in.seekg(0);
    std::string text(static_cast<size_t>(size), '\0');
    if (!text.empty())
        in.read(&text[0], static_cast<std::streamsize>(text.size()));
    if (!in || in.peek() != std::char_traits<char>::eof() || in.bad()) {
        Status() = LoadStatus::ReadFailed;
        return false;
    }
    const auto doc = nlohmann::json::parse(text, nullptr, false);
    if (!doc.is_array()) {
        Status() = LoadStatus::Invalid;
        return false;
    }
    auto parsed = PoiParser::ParsePoiDoc(text);
    // Do not overwrite a nonempty malformed document with newly captured POIs.
    if (parsed.size() != doc.size()) {
        Status() = LoadStatus::Invalid;
        return false;
    }
    Items() = std::move(parsed);
    Status() = Items().empty() ? LoadStatus::Empty : LoadStatus::Loaded;
    LoadSucceeded() = true;
    return true;
}

/** Retry a failed initial load without destroying the current in-memory list. */
inline bool RetryLoad()
{
    if (!Items().empty())
        return LoadSucceeded();
    Loaded() = false;
    LoadSucceeded() = false;
    return EnsureLoaded();
}

/** Rewrite Data\pois.json from the in-memory list. Never throws. */
inline bool Save()
{
    for (const auto& p : Items()) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
            return false;
    }
    const std::string path = FilePath();
    const std::string temporary = path + ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;

    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10);
    const std::vector<PoiParser::Poi>& items = Items();
    out << "[\n";
    for (size_t i = 0; i < items.size(); ++i) {
        const PoiParser::Poi& p = items[i];
        out << "  {\"name\":\"" << EscapeJson(p.name)
            << "\",\"category\":\"" << EscapeJson(p.category)
            << "\",\"x\":" << p.x
            << ",\"y\":" << p.y
            << ",\"z\":" << p.z;
        
        // Write boundary array if present
        if (!p.boundary.empty()) {
            out << ",\"boundary\":[";
            for (size_t b = 0; b < p.boundary.size(); ++b) {
                out << "{\"x\":" << p.boundary[b].x << ",\"y\":" << p.boundary[b].y << "}";
                if (b + 1 < p.boundary.size())
                    out << ",";
            }
            out << "]";
        }
        
        out << "}";
        if (i + 1 < items.size())
            out << ",";
        out << "\n";
    }
    out << "]\n";
    out.flush();
    const bool written = out.good() && out.tellp() <= std::streampos(4 * 1024 * 1024);
    out.close();
    if (!written || out.fail()) {
        DeleteFileA(temporary.c_str());
        return false;
    }
    if (!MoveFileExA(temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(temporary.c_str());
        return false;
    }
    return true;
}

/** Load (if needed), append, and persist. Returns false when the save failed —
 *  a failed write rolls back the entry so a retry cannot duplicate it. */
inline bool Add(const PoiParser::Poi& p)
{
    if (PoiParser::detail::TrimCopy(p.name).empty()
        || !std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z))
        return false;
    if (!EnsureLoaded() && Status() != LoadStatus::Missing)
        return false; // preserve unreadable/invalid existing user data
    Items().push_back(p);
    const bool saved = Save();
    if (!saved)
        Items().pop_back();
    if (saved) {
        LoadSucceeded() = true;
        Status() = LoadStatus::Loaded;
    }
    return saved;
}

} // namespace PoiStore
