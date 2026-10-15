#pragma once
#include "Crosshair.hpp"
#include <cstdlib>
#include <ostream>
#include <sstream>
#include <string>

namespace ArcCrosshair {
#define ARC_XHAIR_BOOL_OPTIONS(X) \
    X(crosshair_outline) X(crosshair_center_dot) X(crosshair_rounded) \
    X(crosshair_use_accent) X(crosshair_pulse)
#define ARC_XHAIR_FLOAT_OPTIONS(X) \
    X(crosshair_rotation, 0.f, -180.f, 180.f) \
    X(crosshair_horizontal_scale, 1.f, 0.5f, 2.f) \
    X(crosshair_vertical_scale, 1.f, 0.5f, 2.f) \
    X(crosshair_dot_size, 2.f, 1.f, 8.f) \
    X(crosshair_ring_scale, 1.f, 0.5f, 2.f) \
    X(crosshair_bracket_length, 8.f, 2.f, 30.f) \
    X(crosshair_chevron_angle, 45.f, 20.f, 80.f) \
    X(crosshair_segment_gap, 12.f, 2.f, 35.f) \
    X(crosshair_pulse_speed, 1.5f, 0.25f, 5.f) \
    X(crosshair_pulse_amount, 2.f, 0.f, 10.f)
inline void SaveOptions(std::ostream& out)
{
#define WRITE_BOOL(name) out << #name "=" << (var::name ? 1 : 0) << '\n';
    ARC_XHAIR_BOOL_OPTIONS(WRITE_BOOL)
#undef WRITE_BOOL
#define WRITE_FLOAT(name, fallback, lo, hi) out << #name "=" << Bound(var::name, fallback, lo, hi) << '\n';
    ARC_XHAIR_FLOAT_OPTIONS(WRITE_FLOAT)
#undef WRITE_FLOAT
    out << "crosshair_segment_count=" << var::crosshair_segment_count << '\n';
    auto color = [&](const char* key, const float c[4]) {
        out << key << '=';
        for (int i = 0; i < 4; ++i) out << (i ? "," : "") << Bound(c[i], 1.f, 0.f, 1.f);
        out << '\n';
    };
    color("crosshair_outline_color", var::crosshair_outline_color);
    color("crosshair_accent_color", var::crosshair_accent_color);
}
inline std::string CaptureOptions()
{
    std::ostringstream out; SaveOptions(out); return out.str();
}
inline bool LoadOption(const std::string& key, const std::string& value)
{
#define READ_BOOL(name) if (key == #name) { if (value == "1" || value == "true") var::name = true; else if (value == "0" || value == "false") var::name = false; return true; }
    ARC_XHAIR_BOOL_OPTIONS(READ_BOOL)
#undef READ_BOOL
#define READ_FLOAT(name, fallback, lo, hi) if (key == #name) { char* end = nullptr; const float v = std::strtof(value.c_str(), &end); if (end != value.c_str() && *end == '\0' && std::isfinite(v)) var::name = Bound(v, fallback, lo, hi); return true; }
    ARC_XHAIR_FLOAT_OPTIONS(READ_FLOAT)
#undef READ_FLOAT
    if (key == "crosshair_segment_count") {
        char* end = nullptr; const long n = std::strtol(value.c_str(), &end, 10);
        if (end != value.c_str() && *end == '\0') var::crosshair_segment_count = static_cast<int>(std::clamp(n, 3L, 12L));
        return true;
    }
    float* dest = key == "crosshair_outline_color" ? var::crosshair_outline_color
        : key == "crosshair_accent_color" ? var::crosshair_accent_color : nullptr;
    if (!dest) return false;
    std::istringstream input(value); float c[4]; char comma;
    for (int i = 0; i < 4; ++i) {
        if (!(input >> c[i]) || !std::isfinite(c[i])) return true;
        if (i < 3 && (!(input >> comma) || comma != ',')) return true;
    }
    input >> std::ws; if (!input.eof()) return true;
    for (int i = 0; i < 4; ++i) dest[i] = std::clamp(c[i], 0.f, 1.f);
    return true;
}
#undef ARC_XHAIR_BOOL_OPTIONS
#undef ARC_XHAIR_FLOAT_OPTIONS
} // namespace ArcCrosshair
