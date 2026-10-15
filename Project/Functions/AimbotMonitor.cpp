#include "AimbotMonitor.hpp"
#include <algorithm>
#include <mutex>
#include <numeric>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>

namespace AimbotMonitor {

AimbotTelemetry g_telemetry;

// Writers take this lock; the paint-thread panel reads unlocked (debug POD
// display — a torn read can only garble a number, never crash).
static std::mutex g_telMutex;

namespace {

/** Not the zeroed default and not a nonsense magnitude. */
bool IsPlausible(const Vector3& p) {
    const double mag = std::fabs(p.x) + std::fabs(p.y) + std::fabs(p.z);
    return mag > 1.0 && mag < 1.0e9;
}

// A stopped target, a missed read or a fresh lock are not prediction failures;
// only a bounded miss is a measurement. Beyond this the sample is re-anchored
// instead of reported as a huge error.
constexpr double kMaxPredictionErrorCm = 2000.0;  // 20 m

/**
 * Distance between the PREVIOUS tick's prediction and this tick's actual
 * position, in metres.
 *
 * This is the only layer that can measure it: the aim tick deliberately keeps
 * no per-target history, so without remembering the last prediction here the
 * "prediction error" readout had nothing to compare and simply printed 0.000 m
 * forever — including while a lead was being applied every tick.
 */
float MeasurePredictionError(const TargetState& state) {
    static uintptr_t s_key = 0;
    static Vector3 s_predicted{};
    static bool s_havePrev = false;

    float error = 0.f;
    if (state.targetPtr != 0 && state.targetPtr == s_key && s_havePrev
        && IsPlausible(state.position)) {
        const double dx = state.position.x - s_predicted.x;
        const double dy = state.position.y - s_predicted.y;
        const double dz = state.position.z - s_predicted.z;
        const double cm = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (cm <= kMaxPredictionErrorCm)
            error = static_cast<float>(cm / 100.0);
    }

    s_key = state.targetPtr;
    s_havePrev = IsPlausible(state.predictedPosition);
    s_predicted = state.predictedPosition;
    return error;
}

}  // namespace

void UpdateTargetState(const TargetState& state) {
    std::lock_guard<std::mutex> lk(g_telMutex);
    TargetState measured = state;
    measured.predictionError = MeasurePredictionError(state);
    g_telemetry.target = measured;
    g_telemetry.totalTargetsScanned++;
    
    if (state.targetPtr) {
        g_telemetry.targetsAcquired++;
        g_telemetry.averageDistance = (g_telemetry.averageDistance + state.distance) / 2.0;
        g_telemetry.maxDistance = (std::max)(g_telemetry.maxDistance, (double)state.distance);
        g_telemetry.minDistance = (std::min)(g_telemetry.minDistance, (double)state.distance);
    }
}

void UpdateAimDeltas(const AimDeltas& deltas) {
    std::lock_guard<std::mutex> lk(g_telMutex);
    g_telemetry.aimDeltas = deltas;
    g_telemetry.aimDeltas.totalRotationDelta = static_cast<float>(std::sqrt(
        deltas.pitchDelta * deltas.pitchDelta + 
        deltas.yawDelta * deltas.yawDelta
    ));
}

void UpdateBoneTracking(const BoneTracking& bone) {
    std::lock_guard<std::mutex> lk(g_telMutex);
    g_telemetry.boneData = bone;
}

void UpdateDetectionMetrics(const DetectionMetrics& detection) {
    std::lock_guard<std::mutex> lk(g_telMutex);
    g_telemetry.detection = detection;
}

void UpdateFrameMetrics(const FrameMetrics& metrics) {
    std::lock_guard<std::mutex> lk(g_telMutex);
    g_telemetry.frameMetrics = metrics;
    
    // Update rolling averages
    static std::array<double, 32> dmaHistory = {};
    static uint32_t dmaIndex = 0;
    dmaHistory[dmaIndex % 32] = metrics.dmaReadMs;
    dmaIndex++;
    
    double sum = 0;
    double maxVal = 0;
    for (double val : dmaHistory) {
        sum += val;
        maxVal = (std::max)(maxVal, val);
    }
    g_telemetry.frameMetrics.avgDmaMs = sum / 32.0;
    g_telemetry.frameMetrics.maxDmaMs = maxVal;
}

void SetAimbotState(AimbotState newState) {
    std::lock_guard<std::mutex> lk(g_telMutex);
    if (newState != g_telemetry.state) {
        g_telemetry.state = newState;
        g_telemetry.stateFrameCount = 0;
        
        switch (newState) {
            case AimbotState::Idle:
                g_telemetry.stateStr = "Idle";
                break;
            case AimbotState::Searching:
                g_telemetry.stateStr = "Searching";
                break;
            case AimbotState::Acquired:
                g_telemetry.stateStr = "Acquired";
                break;
            case AimbotState::Tracking:
                g_telemetry.stateStr = "Tracking";
                g_telemetry.framesTracking = 0;
                break;
            case AimbotState::Correcting:
                g_telemetry.stateStr = "Correcting";
                break;
            case AimbotState::Locked:
                g_telemetry.stateStr = "Locked";
                g_telemetry.framesLocked = 0;
                break;
            case AimbotState::Disabled:
                g_telemetry.stateStr = "Disabled";
                break;
        }
    }
    g_telemetry.stateFrameCount++;
    
    if (newState == AimbotState::Tracking) g_telemetry.framesTracking++;
    if (newState == AimbotState::Locked) g_telemetry.framesLocked++;
}

void AddRotationToHistory(float pitch, float yaw) {
    std::lock_guard<std::mutex> lk(g_telMutex);
    g_telemetry.pitchHistory[g_telemetry.historyIndex] = pitch;
    g_telemetry.yawHistory[g_telemetry.historyIndex] = yaw;
    g_telemetry.historyIndex = (g_telemetry.historyIndex + 1) % 16;
    if (g_telemetry.historyFilled < 16)
        ++g_telemetry.historyFilled;
    // Track the worst deviation of the lock so a transient burst is still
    // visible after the 16-sample window has moved past it.
    const float j = CalculateJitter();
    if (j > g_telemetry.peakJitter)
        g_telemetry.peakJitter = j;
}

void ResetJitterHistory() {
    std::lock_guard<std::mutex> lk(g_telMutex);
    g_telemetry.pitchHistory.fill(0.f);
    g_telemetry.yawHistory.fill(0.f);
    g_telemetry.historyIndex = 0;
    g_telemetry.historyFilled = 0;
    g_telemetry.peakJitter = 0.f;
}

float CalculateJitter() {
    // Standard deviation of recent pitch/yaw commands — over the FILLED slots
    // only. Averaging all 16 while the ring is still filling dragged the mean
    // toward the zero-initialised slots and understated the shake exactly when
    // a fresh lock rings hardest, so the readout improved as the fight went on
    // whether or not the aim actually settled.
    const uint32_t n = (std::min)(g_telemetry.historyFilled, 16u);
    if (n < 2)
        return 0.f;

    double pitchMean = 0.0;
    double yawMean = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        pitchMean += g_telemetry.pitchHistory[i];
        yawMean += g_telemetry.yawHistory[i];
    }
    pitchMean /= n;
    yawMean /= n;

    double pitchVar = 0;
    double yawVar = 0;
    for (uint32_t i = 0; i < n; ++i) {
        double pDiff = g_telemetry.pitchHistory[i] - pitchMean;
        double yDiff = g_telemetry.yawHistory[i] - yawMean;
        pitchVar += pDiff * pDiff;
        yawVar += yDiff * yDiff;
    }

    double jitter = std::sqrt((pitchVar + yawVar) / (2.0 * static_cast<double>(n)));
    return static_cast<float>(jitter);
}

float CalculatePredictionAccuracy() {
    return g_telemetry.target.predictionError;
}

float CalculateResponseLag() {
    // Estimated latency based on frame metrics
    return static_cast<float>(g_telemetry.frameMetrics.dmaReadMs + 
                              g_telemetry.frameMetrics.predictionMs + 
                              g_telemetry.frameMetrics.aimCalcMs);
}

const char* GetStateString(AimbotState state) {
    switch (state) {
        case AimbotState::Idle: return "Idle";
        case AimbotState::Searching: return "Searching";
        case AimbotState::Acquired: return "Acquired";
        case AimbotState::Tracking: return "Tracking";
        case AimbotState::Correcting: return "Correcting";
        case AimbotState::Locked: return "Locked";
        case AimbotState::Disabled: return "Disabled";
        default: return "Unknown";
    }
}

void LogTelemetry() {
    // Calculate metrics
    float jitter = CalculateJitter();
    float responseLag = CalculateResponseLag();
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════╗\n");
    printf("║                    AIMBOT TELEMETRY MONITOR                    ║\n");
    printf("╚════════════════════════════════════════════════════════════════╝\n");
    
    // State
    printf("\n[STATE] %s (frame %llu)\n", g_telemetry.stateStr, g_telemetry.stateFrameCount);
    
    // Target
    printf("\n[TARGET]\n");
    printf("  Name: %s\n", g_telemetry.target.targetName);
    printf("  Distance: %.2f m\n", g_telemetry.target.distance);
    printf("  Health: %.1f / %.1f (%.0f%%)\n", 
           g_telemetry.target.health, 
           g_telemetry.target.maxHealth,
           (g_telemetry.target.maxHealth > 0) ? 
               (g_telemetry.target.health / g_telemetry.target.maxHealth * 100.0f) : 0.0f);
    printf("  Visible: %s | Occluded: %s\n", 
           g_telemetry.target.isVisible ? "YES" : "NO",
           g_telemetry.target.isOccluded ? "YES" : "NO");
    printf("  Position: [%.2f, %.2f, %.2f]\n", 
           g_telemetry.target.position.x, 
           g_telemetry.target.position.y, 
           g_telemetry.target.position.z);
    printf("  Velocity: [%.2f, %.2f, %.2f] cm/s\n",
           g_telemetry.target.velocity.x,
           g_telemetry.target.velocity.y,
           g_telemetry.target.velocity.z);
    printf("  Predicted Pos: [%.2f, %.2f, %.2f]\n",
           g_telemetry.target.predictedPosition.x,
           g_telemetry.target.predictedPosition.y,
           g_telemetry.target.predictedPosition.z);
    printf("  Prediction Error: %.3f m\n", g_telemetry.target.predictionError);
    
    // Aim Corrections
    printf("\n[AIM CORRECTIONS]\n");
    printf("  Pitch Delta: %.4f°\n", g_telemetry.aimDeltas.pitchDelta);
    printf("  Yaw Delta: %.4f°\n", g_telemetry.aimDeltas.yawDelta);
    printf("  Roll Delta: %.4f°\n", g_telemetry.aimDeltas.rollDelta);
    printf("  Total Rotation: %.4f°\n", g_telemetry.aimDeltas.totalRotationDelta);
    printf("  Smoothing Factor: %.3f\n", g_telemetry.aimDeltas.smoothingFactor);
    printf("  Lag Compensation: %.3f\n", g_telemetry.aimDeltas.lagCompensation);
    printf("  Active Smoothing: %s\n", g_telemetry.aimDeltas.activeSmoothing);
    
    // Jitter Analysis
    printf("\n[JITTER ANALYSIS]\n");
    printf("  Current Jitter: %.4f°\n", jitter);
    printf("  Detected Jitter: %.4f°\n", g_telemetry.aimDeltas.jitterAmount);
    printf("  Pitch History: ");
    for (int i = 0; i < 8; ++i) {
        printf("%.2f ", g_telemetry.pitchHistory[i]);
    }
    printf("\n  Yaw History:   ");
    for (int i = 0; i < 8; ++i) {
        printf("%.2f ", g_telemetry.yawHistory[i]);
    }
    printf("\n");
    
    // Rotations
    printf("\n[ROTATION STATE]\n");
    printf("  Current: P=%.2f° Y=%.2f° R=%.2f°\n",
           g_telemetry.currentRotation.x,
           g_telemetry.currentRotation.y,
           g_telemetry.currentRotation.z);
    printf("  Target:  P=%.2f° Y=%.2f° R=%.2f°\n",
           g_telemetry.targetRotation.x,
           g_telemetry.targetRotation.y,
           g_telemetry.targetRotation.z);
    printf("  Error:   P=%.2f° Y=%.2f° R=%.2f°\n",
           g_telemetry.rotationError.x,
           g_telemetry.rotationError.y,
           g_telemetry.rotationError.z);
    
    // Bone Tracking
    printf("\n[BONE TRACKING]\n");
    printf("  Current Bone: %s\n", g_telemetry.boneData.currentBone);
    printf("  Fallback Bone: %s\n", g_telemetry.boneData.fallbackBone);
    printf("  Bone Distance: %.2f m\n", g_telemetry.boneData.boneDistance);
    printf("  Bone Visible: %s\n", g_telemetry.boneData.boneVisible ? "YES" : "NO");
    printf("  Bone World Pos: [%.2f, %.2f, %.2f]\n",
           g_telemetry.boneData.boneWorldPos.x,
           g_telemetry.boneData.boneWorldPos.y,
           g_telemetry.boneData.boneWorldPos.z);
    
    // Detection
    printf("\n[DETECTION]\n");
    printf("  Raycast Valid: %s\n", g_telemetry.detection.raycastValid ? "YES" : "NO");
    printf("  Line of Sight: %s\n", g_telemetry.detection.lineOfSightClear ? "CLEAR" : "BLOCKED");
    printf("  Occlusion: %.1f%%\n", g_telemetry.detection.occlusionPercentage);
    printf("  Occluding Actors: %u\n", g_telemetry.detection.occludingActors);
    printf("  Occluded By: %s\n", g_telemetry.detection.occludedBy);
    printf("  Behind Cover: %s\n", g_telemetry.detection.behindCover ? "YES" : "NO");
    printf("  Wall Penetration: %.2f m\n", g_telemetry.detection.wallPenetrationDistance);
    printf("  In FOV: %s\n", g_telemetry.detection.inFOV ? "YES" : "NO");
    
    // Performance
    printf("\n[PERFORMANCE]\n");
    printf("  DMA Read: %.3f ms (avg: %.3f, max: %.3f)\n",
           g_telemetry.frameMetrics.dmaReadMs,
           g_telemetry.frameMetrics.avgDmaMs,
           g_telemetry.frameMetrics.maxDmaMs);
    printf("  Prediction: %.3f ms\n", g_telemetry.frameMetrics.predictionMs);
    printf("  Aim Calc: %.3f ms\n", g_telemetry.frameMetrics.aimCalcMs);
    printf("  Total Frame: %.3f ms (%.1f FPS)\n",
           g_telemetry.frameMetrics.totalFrameMs,
           1000.0 / (g_telemetry.frameMetrics.totalFrameMs > 0 ? 
                    g_telemetry.frameMetrics.totalFrameMs : 1.0));
    printf("  Response Lag: %.3f ms\n", responseLag);
    printf("  Frame Count: %u\n", g_telemetry.frameMetrics.frameCount);
    
    // Statistics
    printf("\n[STATISTICS]\n");
    printf("  Targets Scanned: %u\n", g_telemetry.totalTargetsScanned);
    printf("  Targets Acquired: %u\n", g_telemetry.targetsAcquired);
    printf("  Frames Tracking: %u\n", g_telemetry.framesTracking);
    printf("  Frames Locked: %u\n", g_telemetry.framesLocked);
    printf("  Avg Distance: %.2f m\n", g_telemetry.averageDistance);
    printf("  Distance Range: %.2f - %.2f m\n", g_telemetry.minDistance, g_telemetry.maxDistance);
    
    printf("\n╚════════════════════════════════════════════════════════════════╝\n\n");
}

void WriteTelemetryToFile() {
    static std::ofstream telemetryLog("AimbotTelemetry.log", std::ios::app);
    
    if (!telemetryLog.is_open()) {
        return;
    }
    
    float jitter = CalculateJitter();
    float responseLag = CalculateResponseLag();
    
    std::ostringstream oss;
    oss << "[" << g_telemetry.frameMetrics.frameCount << "] "
        << "State=" << g_telemetry.stateStr << " "
        << "Target=" << g_telemetry.target.targetName << " "
        << "Dist=" << g_telemetry.target.distance << "m "
        << "Health=" << g_telemetry.target.health << "/" << g_telemetry.target.maxHealth << " "
        << "PitchDelta=" << g_telemetry.aimDeltas.pitchDelta << "° "
        << "YawDelta=" << g_telemetry.aimDeltas.yawDelta << "° "
        << "Jitter=" << jitter << "° "
        << "PeakJitter=" << g_telemetry.peakJitter << "° "
        << "PredError=" << g_telemetry.target.predictionError << "m "
        << "Lag=" << responseLag << "ms "
        << "DMARd=" << g_telemetry.frameMetrics.dmaReadMs << "ms "
        << "FrameTime=" << g_telemetry.frameMetrics.totalFrameMs << "ms "
        << "Occluded=" << g_telemetry.target.isOccluded << " "
        << "Visible=" << g_telemetry.target.isVisible << " "
        // ── shake trace: the loop numbers, one line per write, so a ring can
        // be attributed after the fact instead of re-run.
        << "Flips=" << g_telemetry.aimDeltas.flipsX << "," << g_telemetry.aimDeltas.flipsY << " "
        << "Overshoot=" << g_telemetry.aimDeltas.overshootEvents << " "
        << "Switches=" << g_telemetry.aimDeltas.lockSwitches << " "
        << "Damp=" << g_telemetry.aimDeltas.dampX << "," << g_telemetry.aimDeltas.dampY << " "
        << "TickMs=" << g_telemetry.aimDeltas.tickMs << " "
        << "PxPerMouse=" << g_telemetry.aimDeltas.pxPerMouse << " "
        << "CloseFrac=" << g_telemetry.aimDeltas.closeFrac << " "
        << "ErrPx=" << g_telemetry.aimDeltas.errorPx << " "
        << "Vel=" << g_telemetry.aimDeltas.velMag << " "
        << "Cmd=" << g_telemetry.aimDeltas.cmdX << "," << g_telemetry.aimDeltas.cmdY << "\n";
    
    telemetryLog << oss.str();
    telemetryLog.flush();
}

}  // namespace AimbotMonitor
