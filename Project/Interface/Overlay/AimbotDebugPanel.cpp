#include "AimbotDebugPanel.hpp"
#include "../../Core/FeaturePolicy.hpp"
#include "../../Core/ReadBudget.hpp"
#include "../../Core/AimCalibration.hpp"
#include "../Utils/UpgradeOptions.hpp"
#include "../../ThirdParty/ImGui/imgui.h"
#include <chrono>
#include <cmath>

namespace OverlayUI {

bool AimbotDebugPanel::visible = false;
float AimbotDebugPanel::panelOpacity = 0.85f;

void AimbotDebugPanel::Render() {
    if (!visible) return;
    
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(600, 900), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(panelOpacity);
    
    if (ImGui::Begin("Aimbot Debug Monitor", &visible, 
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        
        // Opacity slider
        ImGui::SliderFloat("Panel Opacity", &panelOpacity, 0.3f, 1.0f);
        ImGui::Separator();
        
        // Sections
        if (ImGui::CollapsingHeader("State", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderStateSection();
        }
        if (ImGui::CollapsingHeader("Target", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderTargetSection();
        }
        if (ImGui::CollapsingHeader("Aim Corrections", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderAimSection();
        }
        if (ImGui::CollapsingHeader("Jitter Analysis", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderJitterSection();
        }
        if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderPerformanceSection();
        }
        if (ImGui::CollapsingHeader("Detection", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderDetectionSection();
        }
        if (ImGui::CollapsingHeader("Bone Tracking", ImGuiTreeNodeFlags_DefaultOpen)) {
            RenderBoneSection();
        }
        if (ImGui::CollapsingHeader("History Graphs")) {
            RenderHistoryGraphs();
        }
        
        ImGui::End();
    }
}

void AimbotDebugPanel::RenderStateSection() {
    using namespace AimbotMonitor;
    
    ImGui::Text("State: ");
    ImGui::SameLine();
    
    // Color code state
    ImVec4 stateColor = ImVec4(0.7f, 0.7f, 0.7f, 1.0f);  // Gray
    switch (g_telemetry.state) {
        case AimbotState::Idle:
            stateColor = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
            break;
        case AimbotState::Searching:
            stateColor = ImVec4(1.0f, 1.0f, 0.0f, 1.0f);  // Yellow
            break;
        case AimbotState::Acquired:
            stateColor = ImVec4(0.0f, 1.0f, 1.0f, 1.0f);  // Cyan
            break;
        case AimbotState::Tracking:
            stateColor = ImVec4(0.0f, 1.0f, 0.0f, 1.0f);  // Green
            break;
        case AimbotState::Correcting:
            stateColor = ImVec4(1.0f, 0.5f, 0.0f, 1.0f);  // Orange
            break;
        case AimbotState::Locked:
            stateColor = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);  // Red
            break;
        case AimbotState::Disabled:
            stateColor = ImVec4(0.3f, 0.3f, 0.3f, 1.0f);  // Dark gray
            break;
    }
    
    ImGui::TextColored(stateColor, "%s", g_telemetry.stateStr);
    ImGui::Text("Frame in State: %llu", g_telemetry.stateFrameCount);
    ImGui::Text("Frames Tracking: %u | Frames Locked: %u", 
                g_telemetry.framesTracking, g_telemetry.framesLocked);
}

void AimbotDebugPanel::RenderTargetSection() {
    using namespace AimbotMonitor;
    
    ImGui::Text("Target: %s", g_telemetry.target.targetName);
    ImGui::Text("Distance: %.2f m", g_telemetry.target.distance);
    ImGui::Text("Health: %.1f / %.1f", g_telemetry.target.health, g_telemetry.target.maxHealth);
    
    ImGui::Checkbox("Visible", (bool*)&g_telemetry.target.isVisible);
    ImGui::SameLine();
    ImGui::Checkbox("Occluded", (bool*)&g_telemetry.target.isOccluded);
    
    ImGui::Text("Position: [%.2f, %.2f, %.2f]",
                g_telemetry.target.position.x,
                g_telemetry.target.position.y,
                g_telemetry.target.position.z);
    
    ImGui::Text("Velocity: [%.2f, %.2f, %.2f] cm/s",
                g_telemetry.target.velocity.x,
                g_telemetry.target.velocity.y,
                g_telemetry.target.velocity.z);
    
    ImGui::Text("Predicted: [%.2f, %.2f, %.2f]",
                g_telemetry.target.predictedPosition.x,
                g_telemetry.target.predictedPosition.y,
                g_telemetry.target.predictedPosition.z);
    
    ImGui::TextColored(
        ImVec4(1.0f, g_telemetry.target.predictionError < 0.5f ? 1.0f : 0.0f, 0.0f, 1.0f),
        "Prediction Error: %.3f m", g_telemetry.target.predictionError);
}

void AimbotDebugPanel::RenderAimSection() {
    using namespace AimbotMonitor;
    
    ImGui::Text("Pitch Delta: %.6f°", g_telemetry.aimDeltas.pitchDelta);
    ImGui::Text("Yaw Delta: %.6f°", g_telemetry.aimDeltas.yawDelta);
    ImGui::Text("Roll Delta: %.6f°", g_telemetry.aimDeltas.rollDelta);
    ImGui::Text("Total Rotation: %.6f°", g_telemetry.aimDeltas.totalRotationDelta);
    
    ImGui::Separator();
    ImGui::Text("Smoothing Factor: %.3f", g_telemetry.aimDeltas.smoothingFactor);
    ImGui::Text("Lag Compensation: %.3f", g_telemetry.aimDeltas.lagCompensation);
    ImGui::Text("Active Smoothing: %s", g_telemetry.aimDeltas.activeSmoothing);
    
    ImGui::Separator();
    ImGui::Text("Current Rotation:");
    ImGui::Text("  X: %.2f° | Y: %.2f° | Z: %.2f°",
                g_telemetry.currentRotation.x,
                g_telemetry.currentRotation.y,
                g_telemetry.currentRotation.z);
    
    ImGui::Text("Target Rotation:");
    ImGui::Text("  X: %.2f° | Y: %.2f° | Z: %.2f°",
                g_telemetry.targetRotation.x,
                g_telemetry.targetRotation.y,
                g_telemetry.targetRotation.z);
    
    ImGui::Text("Rotation Error:");
    ImGui::Text("  X: %.2f° | Y: %.2f° | Z: %.2f°",
                g_telemetry.rotationError.x,
                g_telemetry.rotationError.y,
                g_telemetry.rotationError.z);
}

void AimbotDebugPanel::RenderJitterSection() {
    using namespace AimbotMonitor;
    
    float jitter = CalculateJitter();
    
    ImGui::TextColored(
        ImVec4(1.0f, (std::max)(0.0f, 1.0f - jitter * 0.5f), 0.0f, 1.0f),
        "Jitter Amount: %.6f°", jitter);
    
    ImGui::Text("Detected Jitter: %.6f°", g_telemetry.aimDeltas.jitterAmount);
    ImGui::Text("Peak This Lock: %.6f°  (window %u/16 samples)",
        g_telemetry.peakJitter, g_telemetry.historyFilled);

    // ── Why it shakes ─────────────────────────────────────────────────
    // The deviation above says how much; these say what the loop was doing
    // while it happened. Per-lock totals, so a lock reset reads as 0/0.
    ImGui::Separator();
    ImGui::Text("Flips X/Y: %d / %d   Overshoot: %d   Switches: %d",
        g_telemetry.aimDeltas.flipsX, g_telemetry.aimDeltas.flipsY,
        g_telemetry.aimDeltas.overshootEvents, g_telemetry.aimDeltas.lockSwitches);
    ImGui::Text("Damp X/Y: %.3f / %.3f",
        g_telemetry.aimDeltas.dampX, g_telemetry.aimDeltas.dampY);
    ImGui::Text("Tick: %.2f ms   PxPerMouse: %.3f   CloseFrac: %.3f",
        g_telemetry.aimDeltas.tickMs, g_telemetry.aimDeltas.pxPerMouse,
        g_telemetry.aimDeltas.closeFrac);
    ImGui::Text("Error: %.1f px   TargetVel: %.0f cm/s   Cmd: %.1f,%.1f",
        g_telemetry.aimDeltas.errorPx, g_telemetry.aimDeltas.velMag,
        g_telemetry.aimDeltas.cmdX, g_telemetry.aimDeltas.cmdY);

    ImGui::Separator();
    ImGui::Text("Pitch History (last 16 frames):");
    ImGui::PlotLines("##pitch", g_telemetry.pitchHistory.data(), 16, 0, nullptr, -10.0f, 10.0f, ImVec2(500, 100));
    
    ImGui::Text("Yaw History (last 16 frames):");
    ImGui::PlotLines("##yaw", g_telemetry.yawHistory.data(), 16, 0, nullptr, -10.0f, 10.0f, ImVec2(500, 100));
}

void AimbotDebugPanel::RenderPerformanceSection() {
    using namespace AimbotMonitor;
    
    float responseLag = CalculateResponseLag();
    float fps = g_telemetry.frameMetrics.totalFrameMs > 0 ?
        static_cast<float>(1000.0 / g_telemetry.frameMetrics.totalFrameMs) : 0.0f;
    
    ImGui::Text("DMA Read: %.3f ms", g_telemetry.frameMetrics.dmaReadMs);
    ImGui::Text("  Avg: %.3f ms | Max: %.3f ms", 
                g_telemetry.frameMetrics.avgDmaMs,
                g_telemetry.frameMetrics.maxDmaMs);
    
    ImGui::Text("Prediction: %.3f ms", g_telemetry.frameMetrics.predictionMs);
    ImGui::Text("Aim Calc: %.3f ms", g_telemetry.frameMetrics.aimCalcMs);
    ImGui::Text("Total Frame: %.3f ms (%.1f FPS)", 
                g_telemetry.frameMetrics.totalFrameMs, fps);
    
    ImGui::TextColored(
        ImVec4(1.0f, (std::max)(0.0f, 1.0f - responseLag * 0.1f), 0.0f, 1.0f),
        "Response Lag: %.3f ms", responseLag);
    
    ImGui::Text("Frame Count: %u", g_telemetry.frameMetrics.frameCount);

    // ── DMA read ledger ─────────────────────────────────────────────────────
    // The stopwatch above wraps ONE frame on ONE thread, which is why it sits at
    // 0.000 ms while the scan, retain, position and aim workers all read at once.
    // This breaks the same work down by who issued it and what it was for, per
    // pass, so contention is a number instead of an impression.
    {
        const ReadBudget::Snapshot rb = ReadBudget::Get(4, 5);
        ImGui::Separator();
        ImGui::Text("DMA ledger (per pass, %llu passes)",
            static_cast<unsigned long long>(rb.passes));
        ImGui::Text("  Reads: %llu  (pull %llu / scatter %llu)",
            static_cast<unsigned long long>(rb.passReads),
            static_cast<unsigned long long>(rb.passPull),
            static_cast<unsigned long long>(rb.passScatter));
        ImGui::Text("  Time: %.3f ms   Bytes: %llu",
            rb.passUs / 1000.0,
            static_cast<unsigned long long>(rb.passBytes));

        if (rb.requesters.empty()) {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.f),
                "  no reads recorded yet");
        } else {
            ImGui::Text("  By requester:");
            for (const ReadBudget::Row& row : rb.requesters) {
                ImGui::Text("    %-14s %8llu reads %7.2f ms", row.name.c_str(),
                    static_cast<unsigned long long>(row.reads), row.us / 1000.0);
            }
        }
        if (!rb.purposes.empty()) {
            ImGui::Text("  By purpose:");
            for (const ReadBudget::Row& row : rb.purposes) {
                ImGui::Text("    %-14s %8llu reads %7.2f ms", row.name.c_str(),
                    static_cast<unsigned long long>(row.reads), row.us / 1000.0);
            }
        }
    }
}

void AimbotDebugPanel::RenderDetectionSection() {
    using namespace AimbotMonitor;
    
    ImGui::Text("Raycast Valid: %s", g_telemetry.detection.raycastValid ? "YES" : "NO");
    ImGui::Text("Line of Sight: %s", g_telemetry.detection.lineOfSightClear ? "CLEAR" : "BLOCKED");
    
    ImGui::ProgressBar(g_telemetry.detection.occlusionPercentage / 100.0f, 
                      ImVec2(-1, 0), "Occlusion");
    
    ImGui::Text("Occluding Actors: %u", g_telemetry.detection.occludingActors);
    ImGui::Text("Occluded By: %s", g_telemetry.detection.occludedBy);
    ImGui::Text("Behind Cover: %s", g_telemetry.detection.behindCover ? "YES" : "NO");
    ImGui::Text("Wall Penetration: %.2f m", g_telemetry.detection.wallPenetrationDistance);
    ImGui::Text("In FOV: %s", g_telemetry.detection.inFOV ? "YES" : "NO");

    // Phase 2: why the assist path did not act on the last tick. "not_refused"
    // means the pass either acted or never ran; every other value names a gate.
    const FeaturePolicy::AimRefusal refusal = FeaturePolicy::CurrentRefusal();
    ImGui::TextColored(
        FeaturePolicy::AimRefusalBlocked(refusal)
            ? ImVec4(1.0f, 0.72f, 0.25f, 1.0f) : ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
        "Assist refusal: %s", FeaturePolicy::AimRefusalName(refusal));

    // T4.1: the calibration in force, per axis, read through the same accessor
    // the aim path uses. Shown next to the refusal because a refused calibration
    // is the one case a user has to diagnose quickly.
    const AimCalibration::Profile calP = UpgradeOptions::CalibrationProfile();
    const auto capLabel = [](const AimCalibration::Profile& p) {
        if (!AimCalibration::CapUsable(p))
            return "unusable";
        return p.maxCountsPerTick > 0.f ? "set" : "deferred";
    };
    ImGui::TextColored(
        AimCalibration::Validate(calP).ok ? ImVec4(0.75f, 0.85f, 0.95f, 1.0f)
                                         : ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
        "Calibration (shared)  X %.2f  Y %.2f  curve %.2f  cap %s",
        calP.countsPerPixel.x, calP.countsPerPixel.y, calP.curve, capLabel(calP));
    if (UpgradeOptions::calibrationRejections > 0) {
        // Refused, not clamped: the line was ignored and the previous value kept.
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
            "Calibration lines refused: %d (last: %s)",
            UpgradeOptions::calibrationRejections,
            UpgradeOptions::lastCalibrationRejection.key);
    }

    // T4.3: the aim point as the aim path published it, provenance included. An
    // inferred point and a plotted one are labelled differently on purpose: a user
    // reading an estimate as a confirmed point is the mistake this prevents, so the
    // panel never infers one from the other.
    const FeaturePolicy::AimPointDebug point = FeaturePolicy::CurrentAimPointDebug();
    const FeaturePolicy::AimPointConfidence confidence =
        static_cast<FeaturePolicy::AimPointConfidence>(point.confidence);
    ImGui::TextColored(
        confidence == FeaturePolicy::AimPointConfidence::Confirmed
            ? ImVec4(0.55f, 0.85f, 0.55f, 1.0f) : ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
        "Aim point: %s%s  age %.0f ms  "
        "target %llu (%s)",
        FeaturePolicy::AimPointConfidenceName(confidence),
        point.validated ? "" : ", unvalidated",
        point.ageMs,
        static_cast<unsigned long long>(point.targetKey),
        point.isRobot ? "ARC" : "player");
    ImGui::Text("Aim error %.1f,%.1f px -> cmd %.2f,%.2f counts",
        point.errorPxX, point.errorPxY, point.cmdX, point.cmdY);

    // T4.6: the candidate accounting behind the tick that just ran -- what was
    // examined, what was admitted, and the reasons the rest were dropped, in the
    // same vocabulary as the refusal above.
    uint64_t gateStampMs = 0;
    const FeaturePolicy::TargetGateCounts counts =
        FeaturePolicy::CurrentTargetGateCounts(&gateStampMs);
    const uint64_t panelNowMs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t accountingAgeMs =
        (gateStampMs != 0 && panelNowMs >= gateStampMs) ? panelNowMs - gateStampMs : 0;

    // Whether the aim path is even running, said before its results are read. A
    // pass that is deliberately paused (menu open) and a pass that ran and
    // examined nothing are both "zero candidates", and only one of them is worth
    // investigating.
    const FeaturePolicy::AimPassSnapshot pass = FeaturePolicy::CurrentAimPass();
    const uint64_t passAgeMs = (pass.lastTickMs != 0 && panelNowMs >= pass.lastTickMs)
        ? panelNowMs - pass.lastTickMs : 0;
    switch (pass.state) {
    case FeaturePolicy::AimPassState::Running:
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
            "Aim pass: running (%.1fk ticks, last %llu ms ago)",
            static_cast<double>(pass.ticks) / 1000.0,
            static_cast<unsigned long long>(passAgeMs));
        break;
    case FeaturePolicy::AimPassState::SuspendedMenu:
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
            "Aim pass: paused - menu open (not a fault; close the menu to aim)");
        break;
    case FeaturePolicy::AimPassState::NoRaid:
        ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
            "Aim pass: idle - no resolved raid");
        break;
    case FeaturePolicy::AimPassState::NotStarted:
    default:
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "Aim pass: not started");
        break;
    }
    ImGui::Text("Strict gate: player %s / ARC %s   admitted %u of last tick (%llu ms ago)",
        UpgradeOptions::playerStrictGate ? "on" : "off",
        UpgradeOptions::arcStrictGate ? "on" : "off",
        static_cast<unsigned>(counts.admitted),
        static_cast<unsigned long long>(accountingAgeMs));
    if (counts.considered > 0) {
        ImGui::Text("Candidates: %s",
            FeaturePolicy::TargetGateSummaryLine(counts).c_str());
    } else if (pass.state != FeaturePolicy::AimPassState::Running) {
        // No count exists because no pass ran. Saying "none examined" here would
        // blame the candidate set for a state the candidate set has nothing to do
        // with.
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "Candidates: not counted - the aim pass is not running (%s)",
            FeaturePolicy::AimPassStateName(pass.state));
    } else {
        // The pass ran and examined nothing: the cone, the range or the pool.
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
            "Candidates: none examined on the last aim tick (%llu ms ago)",
            static_cast<unsigned long long>(accountingAgeMs));
    }

    // Phase 4 evidence: WHY the last tick's candidates were refused, rather than
    // only how many. The trace separates "the sampler is late" from "there was no
    // root to sample", and names the strict gate's own decision on every candidate
    // that reached it.
    {
        const FeaturePolicy::AimGateTrace trace = FeaturePolicy::CurrentAimGateTrace();
        if (trace.motionStale > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f), "Admission: %s",
                FeaturePolicy::MotionStaleSummary(trace).c_str());
        }
        // The pool's own latency, shown even with nothing refused: "0 stale"
        // behind a 600 ms snapshot is a healthy sampler under a slow frame, and
        // it is a different situation from "0 stale" behind a 20 ms one. The
        // samples were NOT charged this (FeaturePolicy::SnapshotSampleAgeMs).
        if (trace.snapshotLatencyMs > 0) {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                "Pool: frame snapshot %llu ms old, not charged to samples",
                static_cast<unsigned long long>(trace.snapshotLatencyMs));
        }
        if (trace.strictEvaluated > 0) {
            ImGui::Text("Strict decision: %s",
                FeaturePolicy::StrictDecisionSummary(trace).c_str());
        } else if (counts.considered > 0) {
            // Candidates existed and none survived admission, so the strict gate
            // had nothing to decide. Reporting zero decisions without saying that
            // is how a gate gets blamed for an admission problem.
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                "Strict decision: none - no candidate survived admission");
        }
    }
}

void AimbotDebugPanel::RenderBoneSection() {
    using namespace AimbotMonitor;
    
    ImGui::Text("Current Bone: %s", g_telemetry.boneData.currentBone);
    ImGui::Text("Fallback Bone: %s", g_telemetry.boneData.fallbackBone);
    ImGui::Text("Bone Index: %u", g_telemetry.boneData.boneIndex);
    ImGui::Text("Bone Distance: %.2f m", g_telemetry.boneData.boneDistance);
    ImGui::Text("Bone Visible: %s", g_telemetry.boneData.boneVisible ? "YES" : "NO");
    
    ImGui::Text("World Position: [%.2f, %.2f, %.2f]",
                g_telemetry.boneData.boneWorldPos.x,
                g_telemetry.boneData.boneWorldPos.y,
                g_telemetry.boneData.boneWorldPos.z);
    
    ImGui::Text("Screen Position: [%.2f, %.2f]",
                g_telemetry.boneData.boneScreenPos.x,
                g_telemetry.boneData.boneScreenPos.y);
}

void AimbotDebugPanel::RenderHistoryGraphs() {
    using namespace AimbotMonitor;
    
    ImGui::Text("DMA Read Latency History:");
    static std::array<float, 32> dmaHistory = {};
    ImGui::PlotLines("##dma", dmaHistory.data(), 32, 0, nullptr, 0.0f, 10.0f, ImVec2(500, 100));
    
    ImGui::Text("Frame Time History:");
    static std::array<float, 32> frameHistory = {};
    ImGui::PlotLines("##frame", frameHistory.data(), 32, 0, nullptr, 0.0f, 50.0f, ImVec2(500, 100));
    
    ImGui::Text("Jitter History:");
    ImGui::PlotLines("##jitter", g_telemetry.pitchHistory.data(), 16, 0, nullptr, -5.0f, 5.0f, ImVec2(500, 100));
}

}  // namespace OverlayUI
