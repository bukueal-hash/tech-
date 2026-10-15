#pragma once
// pois.json row parser (D17). Pure and header-only, split out for the same
// reason as ItemMetaParser.hpp: Project.Tests can exercise it on its own,
// without the Windows, file, DMA or Engine dependencies of the radar code that
// consumes it.
//
// Semantics (mirrors ItemMetaParser):
//   • row must be an object with a non-empty "name" after trimming
//   • "category" is optional and case-insensitive; anything outside the known
//     set folds into "custom" rather than being dropped
//   • "x"/"y"/"z" are world centimetres; non-numeric or non-finite values read 0
//   • never throws — malformed JSON yields an empty list

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace PoiParser {

// Fixed category set. A closed list keeps the on-disk format, the menu
// checkboxes and the radar colours in agreement without a second config file.
inline constexpr int kCategoryCount = 6;
inline constexpr const char* kCategoryNames[kCategoryCount] = {
    "exfil", "spawn", "hot", "vendor", "quest", "custom"
};

struct Poi {
    std::string name;
    std::string category = "custom";
    double x = 0.0;  // world cm (center point)
    double y = 0.0;
    double z = 0.0;
    
    // Boundary support: array of (x,y) points defining a polygon zone.
    // Empty = point marker only. 3+ points = draw as boundary polygon.
    struct BoundaryPoint { double x; double y; };
    std::vector<BoundaryPoint> boundary;
    
    bool HasBoundary() const { return boundary.size() >= 3; }
    bool IsPoint() const { return boundary.empty(); }
    
    // Point-in-polygon test (ray casting algorithm)
    bool ContainsPoint(double px, double py) const {
        if (!HasBoundary()) return false;
        
        bool inside = false;
        size_t j = boundary.size() - 1;
        for (size_t i = 0; i < boundary.size(); j = i++) {
            const double xi = boundary[i].x, yi = boundary[i].y;
            const double xj = boundary[j].x, yj = boundary[j].y;
            
            if (((yi > py) != (yj > py)) &&
                (px < (xj - xi) * (py - yi) / (yj - yi) + xi)) {
                inside = !inside;
            }
        }
        return inside;
    }
    
    // Vector3 overload (uses x,y components, ignores z)
    template<typename Vec3>
    bool ContainsPoint(const Vec3& pos) const {
        return ContainsPoint(pos.x, pos.y);
    }
};

namespace detail {

inline std::string ToLowerCopy(std::string s)
{
    for (char& ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

inline std::string TrimCopy(std::string s)
{
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

} // namespace detail

// Always returns 0..5: unknown text folds into the last entry ("custom") so a
// typo in the file can never index out of range at draw time.
inline int CategoryIndex(const std::string& category)
{
    const std::string c = detail::ToLowerCopy(detail::TrimCopy(category));
    for (int i = 0; i < kCategoryCount; ++i) {
        if (c == kCategoryNames[i])
            return i;
    }
    return kCategoryCount - 1;
}

// Parse one POI object. Returns nullopt when the row is not a usable POI
// (non-object, or no name once trimmed). Never throws.
inline std::optional<Poi> ParsePoi(const nlohmann::json& row)
{
    if (!row.is_object())
        return std::nullopt;

    // row.value(key, default) throws type_error 302 when the field is present
    // but of the wrong type; a hand-edited file must never be able to crash the
    // overlay, so every field is type-checked instead.
    const auto str = [&row](const char* key) -> std::string {
        const auto it = row.find(key);
        if (it != row.end() && it->is_string())
            return it->get<std::string>();
        return {};
    };
    const auto num = [&row](const char* key) -> double {
        const auto it = row.find(key);
        if (it == row.end() || !it->is_number())
            return 0.0;
        const double d = it->get<double>();
        return std::isfinite(d) ? d : 0.0;
    };

    Poi p;
    p.name = detail::TrimCopy(str("name"));
    if (p.name.empty())
        return std::nullopt;

    p.category = detail::ToLowerCopy(detail::TrimCopy(str("category")));
    if (p.category.empty())
        p.category = kCategoryNames[kCategoryCount - 1];

    p.x = num("x");
    p.y = num("y");
    p.z = num("z");
    
    // Parse boundary array if present
    const auto it = row.find("boundary");
    if (it != row.end() && it->is_array()) {
        for (const auto& pt : *it) {
            if (pt.is_object()) {
                const double bx = [&]() {
                    const auto x = pt.find("x");
                    if (x != pt.end() && x->is_number()) {
                        double d = x->get<double>();
                        return std::isfinite(d) ? d : 0.0;
                    }
                    return 0.0;
                }();
                const double by = [&]() {
                    const auto y = pt.find("y");
                    if (y != pt.end() && y->is_number()) {
                        double d = y->get<double>();
                        return std::isfinite(d) ? d : 0.0;
                    }
                    return 0.0;
                }();
                p.boundary.push_back({bx, by});
            }
        }
    }
    
    return p;
}

// Parse a full pois.json document (array of POI objects). Never throws:
// malformed JSON, a non-array document, or any parse error yields an empty list.
inline std::vector<Poi> ParsePoiDoc(const std::string& text)
{
    std::vector<Poi> pois;
    try {
        const nlohmann::json doc = nlohmann::json::parse(text);
        if (!doc.is_array())
            return pois;
        pois.reserve(doc.size());
        for (const auto& row : doc) {
            if (auto p = ParsePoi(row))
                pois.push_back(std::move(*p));
        }
    } catch (...) {
        // malformed JSON — no POIs
    }
    return pois;
}

} // namespace PoiParser
