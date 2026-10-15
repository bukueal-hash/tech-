#pragma once
//
// String Utilities
// Centralized string helpers shared across AssetNames, ItemMetaParser, PoiParser, etc.
// Prevents duplication of ToLowerCopy, TrimCopy, TrimInPlace across multiple files.
//

#include <string>
#include <algorithm>
#include <cctype>

namespace StringUtils {

// Convert string to lowercase (ASCII only)
inline std::string ToLowerCopy(std::string s)
{
    for (char& ch : s) {
        if (ch >= 'A' && ch <= 'Z')
            ch = ch - 'A' + 'a';
    }
    return s;
}

// Trim whitespace from both ends (returns new string)
inline std::string TrimCopy(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    
    // Trim left
    auto left = std::find_if(s.begin(), s.end(), notSpace);
    if (left == s.end())
        return std::string{};
    
    // Trim right
    auto right = std::find_if(s.rbegin(), s.rend(), notSpace).base();
    
    return std::string(left, right);
}

// Trim whitespace from end (modifies in place)
inline void TrimInPlace(std::string& s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || 
                          s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
}

} // namespace StringUtils

