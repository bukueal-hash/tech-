#include "visuals.hpp"
#include <algorithm>
#include <cmath>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
namespace Visuals {

namespace {

ImVec2 LerpVec2(const ImVec2& a, const ImVec2& b, float t)
{
    return ImVec2(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t);
}

ImU32 RedGreenBlend(float t)
{
    t = std::clamp(t, 0.f, 1.f);
    return IM_COL32(
        static_cast<int>((1.f - t) * 255.f),
        static_cast<int>(t * 255.f),
        0,
        255);
}

// HP quad: 4 segments with gaps; red (left) -> green (right) blend per slice.
void DrawHealthBarSegmentedGradient(
    ImDrawList* drawlist,
    const ImVec2& h1,
    const ImVec2& h2,
    const ImVec2& h3m,
    const ImVec2& h4m,
    float barWidth,
    int health0to100)
{
    health0to100 = std::clamp(health0to100, 0, 100);
    const float healthWidth = (barWidth * static_cast<float>(health0to100)) / 100.f;
    if (healthWidth <= 0.f)
        return;

    constexpr int kSegments = 4;
    const float gapWidth = barWidth / 20.f;
    const float totalGapWidth = gapWidth * static_cast<float>(kSegments - 1);
    const float segWidth = (barWidth - totalGapWidth) / static_cast<float>(kSegments);

    for (int seg = 0; seg < kSegments; ++seg) {
        const float d0 = static_cast<float>(seg) * (segWidth + gapWidth);
        const float d1 = d0 + segWidth;
        if (d0 >= healthWidth)
            break;

        const float fillEnd = std::min(d1, healthWidth);
        if (fillEnd <= d0)
            continue;

        const float t0 = d0 / barWidth;
        const float t1 = fillEnd / barWidth;
        const ImVec2 bl = LerpVec2(h1, h4m, t0);
        const ImVec2 br = LerpVec2(h1, h4m, t1);
        const ImVec2 tl = LerpVec2(h2, h3m, t0);
        const ImVec2 tr = LerpVec2(h2, h3m, t1);

        constexpr int kSlicesPerSeg = 10;
        for (int slice = 0; slice < kSlicesPerSeg; ++slice) {
            const float u0 = static_cast<float>(slice) / static_cast<float>(kSlicesPerSeg);
            const float u1 = static_cast<float>(slice + 1) / static_cast<float>(kSlicesPerSeg);
            const float midX = d0 + (fillEnd - d0) * ((u0 + u1) * 0.5f);
            const ImU32 col = RedGreenBlend(midX / barWidth);

            const ImVec2 bl0 = LerpVec2(bl, br, u0);
            const ImVec2 bl1 = LerpVec2(bl, br, u1);
            const ImVec2 tl0 = LerpVec2(tl, tr, u0);
            const ImVec2 tl1 = LerpVec2(tl, tr, u1);
            drawlist->AddQuadFilled(bl0, tl0, tl1, bl1, col);
        }
    }
}

} // namespace

void DrawHealthBar(ImDrawList* dl, float x, float y, int shield, int max_shield, int armorType, int health, float scale,
    const char* overlayLine1, const char* overlayLine2, bool overlay1BlackCenteredInHp, float overlayTextScale,
    const ImVec4* overlayTextColor) {
    if (scale <= 0.f) scale = 1.f;
    const float ots = std::max(0.25f, overlayTextScale);
    const float Z = scale;

    ImDrawList* drawlist = dl;
    health = std::clamp(health, 0, 100);

    // Health-only: skip shield hex when there is no armor to show.
    if (max_shield <= 0 && shield <= 0) {
        const int bar_width = static_cast<int>(std::round(105.f * Z));
        ImVec2 h1(x - bar_width / 2, y - 4.f * Z);
        ImVec2 h2(h1.x - 5.f * Z, h1.y - 8.f * Z);
        ImVec2 h3m(h2.x + static_cast<float>(bar_width), h2.y);
        ImVec2 h4m(h1.x + static_cast<float>(bar_width), h1.y);
        drawlist->AddQuadFilled(h1, h2, h3m, h4m, ImColor(10, 10, 30, 60));
        DrawHealthBarSegmentedGradient(
            drawlist, h1, h2, h3m, h4m, static_cast<float>(bar_width), health);

        auto drawOverlayLine = [&](const char* txt, float yTop, float fontSize, bool blackFill) {
            if (!txt || !txt[0])
                return;
            ImFont* font = ImGui::GetFont();
            const float fsDraw = fontSize * ots;
            ImVec2 textSize = font ? font->CalcTextSizeA(fsDraw, FLT_MAX, 0.f, txt) : ImGui::CalcTextSize(txt);
            ImVec2 p(x - textSize.x * 0.5f, yTop);
            ImU32 fg = blackFill ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
            if (overlayTextColor) {
                fg = ImGui::ColorConvertFloat4ToU32(ImVec4(
                    std::clamp(overlayTextColor->x, 0.f, 1.f), std::clamp(overlayTextColor->y, 0.f, 1.f),
                    std::clamp(overlayTextColor->z, 0.f, 1.f), std::clamp(overlayTextColor->w, 0.f, 1.f)));
            }
            if (font)
                drawlist->AddText(font, fsDraw, p, fg, txt);
            else
                drawlist->AddText(p, fg, txt);
        };

        if (overlayLine1 && overlayLine1[0]) {
            const bool twoLines = overlayLine2 && overlayLine2[0];
            const float fs1 = twoLines ? std::max(8.f, 10.5f * Z) : std::max(6.5f, 7.8f * Z);
            ImFont* fontOv = ImGui::GetFont();
            const float fs1Draw = fs1 * ots;
            ImVec2 sz1 = fontOv ? fontOv->CalcTextSizeA(fs1Draw, FLT_MAX, 0.f, overlayLine1) : ImGui::CalcTextSize(overlayLine1);
            float y1 = twoLines
                ? h2.y + 2.f * Z
                : (h1.y + h2.y) * 0.5f - (overlay1BlackCenteredInHp ? sz1.y * 0.5f : fs1Draw * 0.32f);
            drawOverlayLine(overlayLine1, y1, fs1, overlay1BlackCenteredInHp && !twoLines);
            if (twoLines) {
                const float fs2 = std::max(8.f, 10.5f * Z);
                drawOverlayLine(overlayLine2, y1 + sz1.y + 1.5f * Z, fs2, false);
            }
        }
        return;
    }

        int bg_offset = static_cast<int>(std::round(3.f * Z));
        int bar_width = static_cast<int>(std::round(105.f * Z)); //158
        // 4steps...2*3=6
        // 38*4=152 152+6 = 158
        // 5steps...2*4=8
        // 30*5=150 150+8 = 158
        float max_health = 100.0f;//100
        float shield_step = 25.0f; //25

        float shield_25 = 19.4f * Z; //30
        int steps = 5;


        ImVec2 bg1(x - bar_width / 2 - bg_offset, y);
        ImVec2 bg2(bg1.x - 10.f * Z, bg1.y - 16.f * Z);
        ImVec2 bg3(bg2.x + 5.f * Z, bg2.y - 7.f * Z);
        ImVec2 bg4(bg3.x + bar_width + bg_offset, bg3.y);
        ImVec2 bg5(bg4.x + 11.f * Z, bg4.y + 18.f * Z);
        ImVec2 bg6(x + bar_width / 2 + bg_offset, y);
        {
            const ImVec2 hexPts[6] = { bg1, bg2, bg3, bg4, bg5, bg6 };
            drawlist->AddConvexPolyFilled(hexPts, 6, IM_COL32(0, 0, 0, 120));
        }


        ImVec2 h1(bg1.x + 3.f * Z, bg1.y - 4.f * Z);
        ImVec2 h2(h1.x - 5.f * Z, h1.y - 8.f * Z);
        ImVec2 h3m(h2.x + static_cast<float>(bar_width), h2.y);
        ImVec2 h4m(h1.x + static_cast<float>(bar_width), h1.y);
        drawlist->AddQuadFilled(h1, h2, h3m, h4m, ImColor(10, 10, 30, 60));
        DrawHealthBarSegmentedGradient(drawlist, h1, h2, h3m, h4m, static_cast<float>(bar_width), health);


        ImColor shieldCracked(97, 97, 97);
        ImColor shieldCrackedDark(67, 67, 67);

        ImColor shieldCol;
        ImColor shieldColDark; // reserved for inner shadow styling
        if (max_shield == 50) { //white
            shieldCol = ImColor(247, 247, 247);
            shieldColDark = ImColor(164, 164, 164);
        }
        else if (max_shield == 75) { //blue
            shieldCol = ImColor(39, 178, 255);
            shieldColDark = ImColor(27, 120, 210);
        }
        else if (max_shield == 100) {
            // Purple vs gold both cap at 100 in-game; use armorType==1 for gold when callers have tier info.
            if (armorType == 1) {
                shieldCol = ImColor(255, 255, 79);
                shieldColDark = ImColor(218, 175, 49);
            } else {
                shieldCol = ImColor(206, 59, 255);
                shieldColDark = ImColor(136, 36, 220);
            }
        }
        else if (max_shield == 125) { //red
            shieldCol = ImColor(219, 2, 2);
            shieldColDark = ImColor(219, 2, 2);
        }
        else {
            shieldCol = ImColor(247, 247, 247);
            shieldColDark = ImColor(164, 164, 164);
        }
        int shield_tmp = shield;
        int shield1 = 0;
        int shield2 = 0;
        int shield3 = 0;
        int shield4 = 0;
        int shield5 = 0;
        if (shield_tmp > 25) {
            shield1 = 25;
            shield_tmp -= 25;
            if (shield_tmp > 25) {
                shield2 = 25;
                shield_tmp -= 25;
                if (shield_tmp > 25) {
                    shield3 = 25;
                    shield_tmp -= 25;
                    if (shield_tmp > 25) {
                        shield4 = 25;
                        shield_tmp -= 25;
                        shield5 = shield_tmp;
                    }
                    else {
                        shield4 = shield_tmp;
                    }
                }
                else {
                    shield3 = shield_tmp;
                }
            }
            else {
                shield2 = shield_tmp;
            }
        }
        else {
            shield1 = shield_tmp;
        }
        ImVec2 s1(h2.x - 1.f * Z, h2.y - 2.f * Z);
        ImVec2 s2(s1.x - 3.f * Z, s1.y - 5.f * Z);
        ImVec2 s3(s2.x + shield1 / shield_step * shield_25, s2.y);
        ImVec2 s4(s1.x + shield1 / shield_step * shield_25, s1.y);
        ImVec2 s3m(s2.x + shield_25, s2.y);
        ImVec2 s4m(s1.x + shield_25, s1.y);

        ImVec2 ss1(s4m.x + 2.f * Z, s1.y);
        ImVec2 ss2(s3m.x + 2.f * Z, s2.y);
        ImVec2 ss3(ss2.x + shield2 / shield_step * shield_25, s2.y);
        ImVec2 ss4(ss1.x + shield2 / shield_step * shield_25, s1.y);
        ImVec2 ss3m(ss2.x + shield_25, s2.y);
        ImVec2 ss4m(ss1.x + shield_25, s1.y);

        ImVec2 sss1(ss4m.x + 2.f * Z, s1.y);
        ImVec2 sss2(ss3m.x + 2.f * Z, s2.y);
        ImVec2 sss3(sss2.x + shield3 / shield_step * shield_25, s2.y);
        ImVec2 sss4(sss1.x + shield3 / shield_step * shield_25, s1.y);
        ImVec2 sss3m(sss2.x + shield_25, s2.y);
        ImVec2 sss4m(sss1.x + shield_25, s1.y);

        ImVec2 ssss1(sss4m.x + 2, s1.y);
        ImVec2 ssss2(sss3m.x + 2, s2.y);
        ImVec2 ssss3(ssss2.x + shield4 / shield_step * shield_25, s2.y);
        ImVec2 ssss4(ssss1.x + shield4 / shield_step * shield_25, s1.y);
        ImVec2 ssss3m(ssss2.x + shield_25, s2.y);
        ImVec2 ssss4m(ssss1.x + shield_25, s1.y);

        ImVec2 sssss1(ssss4m.x + 2.f * Z, s1.y);
        ImVec2 sssss2(ssss3m.x + 2.f * Z, s2.y);
        ImVec2 sssss3(sssss2.x + shield5 / shield_step * shield_25, s2.y);
        ImVec2 sssss4(sssss1.x + shield5 / shield_step * shield_25, s1.y);
        ImVec2 sssss3m(sssss2.x + shield_25, s2.y);
        ImVec2 sssss4m(sssss1.x + shield_25, s1.y);
        if (max_shield == 50) {
            if (shield <= 25) {
                if (shield < 25) {
                    drawlist->AddQuadFilled(s1, s2, s3m, s4m, shieldCracked);
                    drawlist->AddQuadFilled(ss1, ss2, ss3m, ss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);

            }
            else if (shield <= 50) {
                drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);
                if (shield != 50) {
                    drawlist->AddQuadFilled(ss1, ss2, ss3m, ss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(ss1, ss2, ss3, ss4, shieldCol);
            }
        }
        else if (max_shield == 75) {
            if (shield <= 25) {
                if (shield < 25) {
                    drawlist->AddQuadFilled(s1, s2, s3m, s4m, shieldCracked);
                    drawlist->AddQuadFilled(ss1, ss2, ss3m, ss4m, shieldCracked);
                    drawlist->AddQuadFilled(sss1, sss2, sss3m, sss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);

            }
            else if (shield <= 50) {
                drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);
                if (shield < 50) {
                    drawlist->AddQuadFilled(ss1, ss2, ss3m, ss4m, shieldCracked);
                    drawlist->AddQuadFilled(sss1, sss2, sss3m, sss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(ss1, ss2, ss3, ss4, shieldCol);
            }
            else if (shield <= 75) {
                drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);
                drawlist->AddQuadFilled(ss1, ss2, ss3, ss4, shieldCol);
                if (shield < 75) {
                    drawlist->AddQuadFilled(sss1, sss2, sss3m, sss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(sss1, sss2, sss3, sss4, shieldCol);
            }
        }
        else if (max_shield == 100) {
            if (shield <= 25) {
                if (shield < 25) {
                    drawlist->AddQuadFilled(s1, s2, s3m, s4m, shieldCracked);
                    drawlist->AddQuadFilled(ss1, ss2, ss3m, ss4m, shieldCracked);
                    drawlist->AddQuadFilled(sss1, sss2, sss3m, sss4m, shieldCracked);
                    drawlist->AddQuadFilled(ssss1, ssss2, ssss3m, ssss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);

            }
            else if (shield <= 50) {
                drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);
                if (shield < 50) {
                    drawlist->AddQuadFilled(ss1, ss2, ss3m, ss4m, shieldCracked);
                    drawlist->AddQuadFilled(sss1, sss2, sss3m, sss4m, shieldCracked);
                    drawlist->AddQuadFilled(ssss1, ssss2, ssss3m, ssss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(ss1, ss2, ss3, ss4, shieldCol);
            }
            else if (shield <= 75) {
                drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);
                drawlist->AddQuadFilled(ss1, ss2, ss3, ss4, shieldCol);
                if (shield < 75) {
                    drawlist->AddQuadFilled(sss1, sss2, sss3m, sss4m, shieldCracked);
                    drawlist->AddQuadFilled(ssss1, ssss2, ssss3m, ssss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(sss1, sss2, sss3, sss4, shieldCol);
            }
            else if (shield <= 100) {
                drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);
                drawlist->AddQuadFilled(ss1, ss2, ss3, ss4, shieldCol);
                drawlist->AddQuadFilled(sss1, sss2, sss3, sss4, shieldCol);
                if (shield < 100) {
                    drawlist->AddQuadFilled(ssss1, ssss2, ssss3m, ssss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(ssss1, ssss2, ssss3, ssss4, shieldCol);
            }
        }
        else if (max_shield == 125) {
            if (shield <= 25) {
                if (shield < 25) {
                    drawlist->AddQuadFilled(s1, s2, s3m, s4m, shieldCracked);
                    drawlist->AddQuadFilled(ss1, ss2, ss3m, ss4m, shieldCracked);
                    drawlist->AddQuadFilled(sss1, sss2, sss3m, sss4m, shieldCracked);
                    drawlist->AddQuadFilled(ssss1, ssss2, ssss3m, ssss4m, shieldCracked);
                    drawlist->AddQuadFilled(sssss1, sssss2, sssss3m, sssss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);

            }
            else if (shield <= 50) {
                drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);
                if (shield < 50) {
                    drawlist->AddQuadFilled(ss1, ss2, ss3m, ss4m, shieldCracked);
                    drawlist->AddQuadFilled(sss1, sss2, sss3m, sss4m, shieldCracked);
                    drawlist->AddQuadFilled(ssss1, ssss2, ssss3m, ssss4m, shieldCracked);
                    drawlist->AddQuadFilled(sssss1, sssss2, sssss3m, sssss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(ss1, ss2, ss3, ss4, shieldCol);
            }
            else if (shield <= 75) {
                drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);
                drawlist->AddQuadFilled(ss1, ss2, ss3, ss4, shieldCol);
                if (shield < 75) {
                    drawlist->AddQuadFilled(sss1, sss2, sss3m, sss4m, shieldCracked);
                    drawlist->AddQuadFilled(ssss1, ssss2, ssss3m, ssss4m, shieldCracked);
                    drawlist->AddQuadFilled(sssss1, sssss2, sssss3m, sssss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(sss1, sss2, sss3, sss4, shieldCol);
            }
            else if (shield <= 100) {
                drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);
                drawlist->AddQuadFilled(ss1, ss2, ss3, ss4, shieldCol);
                drawlist->AddQuadFilled(sss1, sss2, sss3, sss4, shieldCol);
                if (shield < 100) {
                    drawlist->AddQuadFilled(ssss1, ssss2, ssss3m, ssss4m, shieldCracked);
                    drawlist->AddQuadFilled(sssss1, sssss2, sssss3m, sssss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(ssss1, ssss2, ssss3, ssss4, shieldCol);
            }
            else if (shield <= 125) {
                drawlist->AddQuadFilled(s1, s2, s3, s4, shieldCol);
                drawlist->AddQuadFilled(ss1, ss2, ss3, ss4, shieldCol);
                drawlist->AddQuadFilled(sss1, sss2, sss3, sss4, shieldCol);
                drawlist->AddQuadFilled(ssss1, ssss2, ssss3, ssss4, shieldCol);
                if (shield < 125) {
                    drawlist->AddQuadFilled(sssss1, sssss2, sssss3m, sssss4m, shieldCracked);
                }
                if (shield != 0)
                    drawlist->AddQuadFilled(sssss1, sssss2, sssss3, sssss4, shieldCol);
            }
        }

    auto drawOverlayLine = [&](const char* txt, float yTop, float fontSize, bool blackFill, bool outlineLight) {
        if (!txt || !txt[0])
            return;
        ImFont* font = ImGui::GetFont();
        const float fsDraw = fontSize * ots;
        ImVec2 textSize = font ? font->CalcTextSizeA(fsDraw, FLT_MAX, 0.f, txt) : ImGui::CalcTextSize(txt);
        ImVec2 p(x - textSize.x * 0.5f, yTop);
        ImU32 fg;
        if (overlayTextColor) {
            fg = ImGui::ColorConvertFloat4ToU32(ImVec4(
                std::clamp(overlayTextColor->x, 0.f, 1.f), std::clamp(overlayTextColor->y, 0.f, 1.f),
                std::clamp(overlayTextColor->z, 0.f, 1.f), std::clamp(overlayTextColor->w, 0.f, 1.f)));
        } else
            fg = blackFill ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
        (void)outlineLight;
        if (font) {
            drawlist->AddText(font, fsDraw, p, fg, txt);
        } else {
            drawlist->AddText(p, fg, txt);
        }
    };

    if (overlayLine1 && overlayLine1[0]) {
        const bool twoLines = overlayLine2 && overlayLine2[0];
        const float fs1 = twoLines ? std::max(8.f, 10.5f * Z) : std::max(6.5f, 7.8f * Z);
        ImFont* fontOv = ImGui::GetFont();
        const float fs1Draw = fs1 * ots;
        ImVec2 sz1 = fontOv ? fontOv->CalcTextSizeA(fs1Draw, FLT_MAX, 0.f, overlayLine1) : ImGui::CalcTextSize(overlayLine1);
        float y1;
        if (twoLines)
            y1 = bg3.y + 2.f * Z;
        else if (overlay1BlackCenteredInHp)
            y1 = (h1.y + h2.y) * 0.5f - sz1.y * 0.5f;
        else
            y1 = (h1.y + h2.y) * 0.5f - fs1Draw * 0.32f;
        const bool blk = overlay1BlackCenteredInHp && !twoLines;
        drawOverlayLine(overlayLine1, y1, fs1, blk, blk);
        if (twoLines) {
            const float fs2 = std::max(8.f, 10.5f * Z);
            const float y2 = y1 + sz1.y + 1.5f * Z;
            drawOverlayLine(overlayLine2, y2, fs2, false, false);
        }
    }
}

} // namespace Visuals
