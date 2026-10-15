#pragma once
#include "../../Functions/AimbotMonitor.hpp"

namespace OverlayUI {

class AimbotDebugPanel {
public:
    static void Render();
    static void ToggleVisibility() { visible = !visible; }
    static bool IsVisible() { return visible; }
    
private:
    static bool visible;
    static float panelOpacity;
    
    static void RenderStateSection();
    static void RenderTargetSection();
    static void RenderAimSection();
    static void RenderJitterSection();
    static void RenderPerformanceSection();
    static void RenderDetectionSection();
    static void RenderBoneSection();
    static void RenderHistoryGraphs();
};

}  // namespace OverlayUI
