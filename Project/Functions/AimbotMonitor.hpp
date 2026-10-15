#pragma once
#include <cstdint>
#include <array>
#include <chrono>
#include <cmath>
#include "../Core/Engine.h"
#include "../Core/Vector.hpp"

namespace AimbotMonitor {

// Frame timing metrics
struct FrameMetrics {
    double dmaReadMs = 0.0;        // DMA memory read latency
    double predictionMs = 0.0;     // Prediction calculation time
    double aimCalcMs = 0.0;        // Aim calculation time
    double totalFrameMs = 0.0;     // Total frame time
    uint32_t frameCount = 0;
    double avgDmaMs = 0.0;
    double maxDmaMs = 0.0;
};

// Target state snapshot
struct TargetState {
    uintptr_t targetPtr = 0;
    const char* targetName = "None";
    float distance = 0.0f;
    float health = 0.0f;
    float maxHealth = 0.0f;
    bool isVisible = false;
    bool isOccluded = false;
    Vector3 position = {};
    Vector3 velocity = {};
    Vector3 predictedPosition = {};
    float predictionError = 0.0f;  // Distance between predicted vs actual next frame
    uint32_t updatesSinceAcquire = 0;
};

// Aim correction deltas
struct AimDeltas {
    float pitchDelta = 0.0f;       // Rotation adjustment this frame
    float yawDelta = 0.0f;
    float rollDelta = 0.0f;
    float totalRotationDelta = 0.0f;  // Magnitude of adjustment
    float smoothingFactor = 1.0f;     // Current smoothing multiplier
    float lagCompensation = 1.0f;     // Lag compensation factor
    float jitterAmount = 0.0f;        // Detected jitter in aim
    const char* activeSmoothing = "none";

    // ── Why it shakes ─────────────────────────────────────────────────────
    // A standard deviation cannot say WHY the loop rings. These are the
    // control loop's own numbers, carried per tick so a shake can be
    // attributed from the trace instead of guessed at.
    //
    // The flip/overshoot/switch counters in Aimbot.cpp are WINDOWED: they are
    // only cleared inside the debug_aim_shake print block, so with that flag
    // off they accumulate forever. The Aimbot tick therefore delta-accumulates
    // them into the per-lock totals below, which are unambiguous either way.
    int flipsX = 0;             // per-lock error sign crossovers, X (x = axis-major)
    int flipsY = 0;             // per-lock error sign crossovers, Y (y = axis-major)
    int overshootEvents = 0;    // per-lock adaptive-gain overshoot detections
    int lockSwitches = 0;       // per-lock target switches
    float dampX = 0.0f;         // oscillation damper engaged, 0..kOscDampMax
    float dampY = 0.0f;
    float tickMs = 0.0f;        // measured inter-tick time (a stall overshoots)
    float pxPerMouse = 0.0f;    // learned mouse strength (over-strong = ring)
    float closeFrac = 0.0f;     // fraction of error closed this tick
    float errorPx = 0.0f;       // screen error at command time
    float velMag = 0.0f;        // target speed: movers need lead, not less pull
    float cmdX = 0.0f;          // counts actually sent, X
    float cmdY = 0.0f;
};

// Bone selection and tracking
struct BoneTracking {
    const char* currentBone = "head";
    const char* fallbackBone = "neck";
    uint32_t boneIndex = 0;
    float boneDistance = 0.0f;
    Vector3 boneWorldPos = {};
    Vector3 boneScreenPos = {};
    bool boneVisible = false;
    float distanceToBone = 0.0f;
};

// Detection and visibility data
struct DetectionMetrics {
    bool raycastValid = false;
    bool lineOfSightClear = false;
    float occlusionPercentage = 0.0f;  // % of body occluded
    uint32_t occludingActors = 0;
    const char* occludedBy = "none";
    bool behindCover = false;
    float wallPenetrationDistance = 0.0f;
    bool inFOV = false;
};

// Aimbot state machine
enum class AimbotState {
    Idle,
    Searching,
    Acquired,
    Tracking,
    Correcting,
    Locked,
    Disabled
};

// Main monitor data structure
struct AimbotTelemetry {
    // State
    AimbotState state = AimbotState::Idle;
    const char* stateStr = "Idle";
    uint64_t stateFrameCount = 0;
    
    // Target
    TargetState target;
    
    // Aim
    AimDeltas aimDeltas;
    Vector3 currentRotation = {};
    Vector3 targetRotation = {};
    Vector3 rotationError = {};
    
    // Bone
    BoneTracking boneData;
    
    // Detection
    DetectionMetrics detection;
    
    // Performance
    FrameMetrics frameMetrics;
    
    // History for jitter detection
    std::array<float, 16> pitchHistory = {};     // Last 16 frames
    std::array<float, 16> yawHistory = {};
    uint32_t historyIndex = 0;
    // How many of those 16 slots are real samples. Without this the jitter
    // deviation averaged the zero-initialised slots too, understating the
    // shake exactly while a fresh lock rings hardest.
    uint32_t historyFilled = 0;
    float peakJitter = 0.0f;   // worst jitter seen in the current lock
    
    // Statistics
    uint32_t totalTargetsScanned = 0;
    uint32_t targetsAcquired = 0;
    uint32_t framesTracking = 0;
    uint32_t framesLocked = 0;
    double averageDistance = 0.0;
    double maxDistance = 0.0;
    double minDistance = 999999.0;
};

// Global telemetry instance. NOT thread_local — the aim thread writes it and
// the PAINT thread reads it for AimbotDebugPanel; thread_local made the panel
// read its own zeroed copy forever ("monitor shows no action while aiming").
extern AimbotTelemetry g_telemetry;

// Update functions (call from aimbot code)
void UpdateTargetState(const TargetState& state);
void UpdateAimDeltas(const AimDeltas& deltas);
void UpdateBoneTracking(const BoneTracking& bone);
void UpdateDetectionMetrics(const DetectionMetrics& detection);
void UpdateFrameMetrics(const FrameMetrics& metrics);
void SetAimbotState(AimbotState newState);
void AddRotationToHistory(float pitch, float yaw);
// Drop the jitter window and peak. Called on every target change: samples from
// the previous engagement describe a target we are no longer aiming at, and
// they made the jitter readout lag a real lock change by up to 16 ticks.
void ResetJitterHistory();

// Calculate derived metrics
float CalculateJitter();  // Std dev of recent rotations
float CalculatePredictionAccuracy();  // How close prediction was
float CalculateResponseLag();  // Frames between input and result

// Diagnostic output
void LogTelemetry();  // Console output
void WriteTelemetryToFile();  // File logging
const char* GetStateString(AimbotState state);

}  // namespace AimbotMonitor
