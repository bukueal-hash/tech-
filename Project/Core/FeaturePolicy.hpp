#pragma once

#include <cstdint>

namespace FeaturePolicy {

// Player ESP diagnostics: identify the gate that excluded the most entries.
struct FrameGateCounts {
    int frameNotDrawing = 0;
    int frameNotInitialized = 0;
    int frameAllyHidden = 0;
    int frameDistance = 0;
    int framePosition = 0;
};

inline const char* DominantFrameGate(const FrameGateCounts& counts, int& count)
{
    const struct { const char* name; int n; } gates[] = {
        { "frame_position", counts.framePosition },
        { "frame_ally_hidden", counts.frameAllyHidden },
        { "frame_not_initialized", counts.frameNotInitialized },
        { "frame_distance", counts.frameDistance },
        { "frame_not_drawing", counts.frameNotDrawing },
    };
    const char* best = nullptr;
    count = 0;
    for (const auto& gate : gates) {
        if (gate.n > count) {
            best = gate.name;
            count = gate.n;
        }
    }
    return best;
}

namespace Staleness {
inline constexpr uint64_t HealthMs = 500;
}

// Shared KMBox click-test state: do not allow a second press before release.
class PressReleaseState {
public:
    static constexpr int kMaxReleaseAttempts = 3;

    bool BeginPress()
    {
        if (m_pressOutstanding)
            return false;
        m_pressOutstanding = true;
        return true;
    }

    int BeginRelease()
    {
        if (!m_pressOutstanding)
            return 0;
        m_pressOutstanding = false;
        return kMaxReleaseAttempts;
    }

    bool PressOutstanding() const { return m_pressOutstanding; }

    void NoteReleaseUndeliverable()
    {
        ++m_undeliverable;
        m_pressOutstanding = true;
    }

    uint32_t UndeliverableReleases() const { return m_undeliverable; }

private:
    bool m_pressOutstanding = false;
    uint32_t m_undeliverable = 0;
};

} // namespace FeaturePolicy
