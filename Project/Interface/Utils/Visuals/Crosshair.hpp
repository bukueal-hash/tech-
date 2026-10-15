#pragma once
#include "../Variables/index.h"
#include "../../../ThirdParty/ImGui/imgui.h"
#include <algorithm>
#include <cmath>

namespace ArcCrosshair {
inline constexpr const char* kStyles[] = {
    "Classic", "Dot", "Tight", "Circle", "T static", "T spin", "Cross spin", "Tri spin",
    "Cross", "Ring", "T shape", "X cross", "Chevron up", "Chevron down", "Diamond",
    "Hollow square", "Corner brackets", "Segmented circle", "Cross + ring", "Dot + ring",
    "Triple dot horizontal", "Triple dot vertical", "Horizontal line", "Vertical line",
    "Shotgun ring", "Four corners", "Cross + diamond", "Cross + ring + dot", "Open X",
    "Double circle", "Circle + four dots", "Tactical brackets", "Small cross + outer ring",
    "Diamond + center dot", "Four direction dots", "Cross + diagonal accents", "Minimal plus"
};
inline float Bound(float v, float fallback, float lo, float hi)
{
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}
inline ImU32 Color(const float c[4])
{
    return ImGui::ColorConvertFloat4ToU32(ImVec4(
        Bound(c[0], 1.f, 0.f, 1.f), Bound(c[1], 1.f, 0.f, 1.f),
        Bound(c[2], 1.f, 0.f, 1.f), Bound(c[3], 1.f, 0.f, 1.f)));
}
inline void Draw(ImDrawList* dl, ImVec2 center, float previewRadius = 0.f)
{
    if (!dl || !var::show_crosshair) return;
    constexpr float pi = 3.14159265358979323846f;
    const int style = std::clamp(var::crosshair_style, 0, static_cast<int>(IM_ARRAYSIZE(kStyles)) - 1);
    float size = Bound(var::crosshair_size, 8.f, 2.f, 40.f);
    if (var::crosshair_pulse)
        size = (std::max)(1.f, size + std::sin(static_cast<float>(ImGui::GetTime()) *
            Bound(var::crosshair_pulse_speed, 1.5f, 0.25f, 5.f) * 2.f * pi) *
            Bound(var::crosshair_pulse_amount, 2.f, 0.f, 10.f));
    const float gap = Bound(var::crosshair_gap, 3.f, 0.f, 20.f);
    const float hs = Bound(var::crosshair_horizontal_scale, 1.f, 0.5f, 2.f);
    const float vs = Bound(var::crosshair_vertical_scale, 1.f, 0.5f, 2.f);
    const float ring = size * Bound(var::crosshair_ring_scale, 1.f, 0.5f, 2.f);
    const float bracket = Bound(var::crosshair_bracket_length, 8.f, 2.f, 30.f);
    const float extent = (std::max)(size + gap + bracket, ring) * (std::max)(hs, vs) + 12.f;
    const float fit = previewRadius > 0.f ? (std::min)(1.f, previewRadius / extent) : 1.f;
    float angle = Bound(var::crosshair_rotation, 0.f, -180.f, 180.f) * pi / 180.f;
    if (style == 5 || style == 6 || style == 7)
        angle += static_cast<float>(ImGui::GetTime()) *
            Bound(var::crosshair_spin_rpm, 20.f, 1.f, 120.f) * 2.f * pi / 60.f;
    const float cs = std::cos(angle), sn = std::sin(angle);
    auto point = [&](float x, float y) {
        x *= hs; y *= vs;
        return ImVec2(center.x + (x * cs - y * sn) * fit,
            center.y + (x * sn + y * cs) * fit);
    };
    const ImU32 primary = Color(var::crosshair_color);
    const ImU32 accent = var::crosshair_use_accent ? Color(var::crosshair_accent_color) : primary;
    const ImU32 outline = Color(var::crosshair_outline_color);
    const float thick = Bound(var::crosshair_thickness, 1.5f, 0.5f, 6.f) * fit;
    const float dotRadius = Bound(var::crosshair_dot_size, 2.f, 1.f, 8.f);
    for (int pass = var::crosshair_outline ? 0 : 1; pass < 2; ++pass) {
        const bool border = pass == 0;
        const float width = thick + (border ? 2.f * fit : 0.f);
        auto line = [&](float x0, float y0, float x1, float y1, ImU32 color) {
            dl->AddLine(point(x0, y0), point(x1, y1), border ? outline : color, width);
            if (var::crosshair_rounded) {
                dl->AddCircleFilled(point(x0, y0), width * 0.5f, border ? outline : color, 12);
                dl->AddCircleFilled(point(x1, y1), width * 0.5f, border ? outline : color, 12);
            }
        };
        auto dot = [&](float x, float y, float r, ImU32 color) {
            dl->AddCircleFilled(point(x, y), (std::max)(0.5f, r * fit) + (border ? fit : 0.f), border ? outline : color, 16);
        };
        auto cross = [&](float arm, float g, bool diagonal) {
            const float d = diagonal ? 0.70710678f : 1.f;
            if (diagonal) {
                line(-d*(arm+g), -d*(arm+g), -d*g, -d*g, primary);
                line(d*g, d*g, d*(arm+g), d*(arm+g), primary);
                line(-d*(arm+g), d*(arm+g), -d*g, d*g, primary);
                line(d*g, -d*g, d*(arm+g), -d*(arm+g), primary);
            } else {
                line(-arm-g, 0.f, -g, 0.f, primary); line(g, 0.f, arm+g, 0.f, primary);
                line(0.f, -arm-g, 0.f, -g, primary); line(0.f, g, 0.f, arm+g, primary);
            }
        };
        auto circle = [&](float r, ImU32 color) {
            ImVec2 pts[64];
            for (int i=0; i<64; ++i) {
                const float a=2.f*pi*static_cast<float>(i)/64.f;
                pts[i]=point(std::cos(a)*r,std::sin(a)*r);
            }
            dl->AddPolyline(pts,64,border ? outline : color,ImDrawFlags_Closed,width);
        };
        auto diamond = [&](float r, ImU32 color) {
            line(0.f,-r,r,0.f,color); line(r,0.f,0.f,r,color);
            line(0.f,r,-r,0.f,color); line(-r,0.f,0.f,-r,color);
        };
        auto corners = [&](float r,float len,bool shortVertical) {
            for (int x : {-1,1}) for (int y : {-1,1}) {
                const float xf=static_cast<float>(x), yf=static_cast<float>(y);
                line(xf*r,yf*r,xf*(r-len),yf*r,primary);
                line(xf*r,yf*r,xf*r,yf*(r-len*(shortVertical ? 0.55f : 1.f)),primary);
            }
        };
        bool inherentDot=false;
        switch(style) {
        case 0: cross((std::max)(0.f,size-gap),gap,false); dot(0.f,0.f,thick/fit*0.5f+0.5f,primary); inherentDot=true; break;
        case 1: dot(0.f,0.f,size*0.5f,primary); inherentDot=true; break;
        case 2: cross(size*0.65f,0.f,false); break;
        case 3: circle(size,primary); cross(size*0.35f,0.f,false); break;
        case 4: case 5: line(-size,-size*0.5f,size,-size*0.5f,primary); line(0.f,-size*0.5f,0.f,size,primary); break;
        case 6: cross((std::max)(0.f,size-gap),gap,false); break;
        case 7: {
            for(int i=0;i<3;++i) {
                const float a=-pi*0.5f+static_cast<float>(i)*2.f*pi/3.f;
                const float b=a+2.f*pi/3.f;
                line(std::cos(a)*size,std::sin(a)*size,std::cos(b)*size,std::sin(b)*size,primary);
            } break;
        }
        case 8: cross(size,gap,false); break;
        case 9: circle(ring,accent); break;
        case 10: line(-size,gap,size,gap,primary); line(0.f,-size,0.f,gap,primary); break;
        case 11: case 28: cross(size,style==28 ? (std::max)(gap,size*0.28f) : gap,true); break;
        case 12: case 13: {
            const float a=Bound(var::crosshair_chevron_angle,45.f,20.f,80.f)*pi/180.f;
            const float dir=style==12 ? 1.f : -1.f, peak=-dir*(gap+size*0.15f);
            line(0.f,peak,-size*std::cos(a),peak+dir*size*std::sin(a),primary);
            line(0.f,peak,size*std::cos(a),peak+dir*size*std::sin(a),primary); break;
        }
        case 14: diamond(size,primary); break;
        case 15: line(-size,-size,size,-size,primary); line(size,-size,size,size,primary); line(size,size,-size,size,primary); line(-size,size,-size,-size,primary); break;
        case 16: corners(size+gap,bracket,false); break;
        case 17: {
            const int n=std::clamp(var::crosshair_segment_count,3,12);
            const float arc=2.f*pi/static_cast<float>(n);
            const float g=(std::min)(Bound(var::crosshair_segment_gap,12.f,2.f,35.f)*pi/180.f,arc*0.8f);
            for(int i=0;i<n;++i) for(int j=0;j<15;++j) {
                const float a=static_cast<float>(i)*arc+g*0.5f+(arc-g)*static_cast<float>(j)/15.f;
                const float b=a+(arc-g)/15.f;
                line(std::cos(a)*ring,std::sin(a)*ring,std::cos(b)*ring,std::sin(b)*ring,accent);
            } break;
        }
        case 18: cross(size,gap,false); circle(ring,accent); break;
        case 19: circle(ring,accent); dot(0.f,0.f,dotRadius,primary); inherentDot=true; break;
        case 20: case 21: {
            const float d=gap+size*0.5f;
            dot(style==20 ? -d : 0.f,style==21 ? -d : 0.f,dotRadius,accent);
            dot(0.f,0.f,dotRadius,primary);
            dot(style==20 ? d : 0.f,style==21 ? d : 0.f,dotRadius,accent); inherentDot=true; break;
        }
        case 22: line(-size-gap,0.f,-gap,0.f,primary); line(gap,0.f,size+gap,0.f,primary); break;
        case 23: line(0.f,-size-gap,0.f,-gap,primary); line(0.f,gap,0.f,size+gap,primary); break;
        case 24: {
            const int n=std::clamp(var::crosshair_segment_count,3,12);
            for(int i=0;i<n;++i) {
                const float a=2.f*pi*static_cast<float>(i)/static_cast<float>(n);
                line(std::cos(a)*ring*0.78f,std::sin(a)*ring*0.78f,std::cos(a)*ring,std::sin(a)*ring,accent);
            } break;
        }
        case 25: corners(size+gap,bracket*0.45f,false); break;
        case 26: cross(size,gap,false); diamond(size*0.55f,accent); break;
        case 27: cross(size,gap,false); circle(ring,accent); dot(0.f,0.f,dotRadius,primary); inherentDot=true; break;
        case 29: circle(ring*0.62f,primary); circle(ring,accent); break;
        case 30: circle(ring,primary); dot(0.f,-ring,dotRadius,accent); dot(0.f,ring,dotRadius,accent); dot(-ring,0.f,dotRadius,accent); dot(ring,0.f,dotRadius,accent); break;
        case 31: corners(size+gap,bracket,true); break;
        case 32: cross(size*0.45f,gap*0.5f,false); circle(ring,accent); break;
        case 33: diamond(size,primary); dot(0.f,0.f,dotRadius,accent); inherentDot=true; break;
        case 34: {const float d=gap+size*0.5f; dot(0.f,-d,dotRadius,accent); dot(0.f,d,dotRadius,accent); dot(-d,0.f,dotRadius,accent); dot(d,0.f,dotRadius,accent); break;}
        case 35: cross(size,gap,false); for(int x : {-1,1}) for(int y : {-1,1}) {const float xf=static_cast<float>(x),yf=static_cast<float>(y); line(xf*size*0.72f,yf*size*0.72f,xf*size*0.34f,yf*size*0.34f,accent);} break;
        case 36: cross((std::max)(2.f,size*0.28f),(std::max)(1.f,gap*0.35f),false); break;
        default: break;
        }
        if(var::crosshair_center_dot && !inherentDot) dot(0.f,0.f,dotRadius,primary);
    }
}
} // namespace ArcCrosshair
