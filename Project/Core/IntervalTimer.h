#pragma once

#include <chrono>
#include <cstdint>

// Streck-style pseudo-interrupt: poll elapsed time inside a worker loop.
// Returns true once per interval, then resets the base clock.
class IntervalTimer {
public:
    explicit IntervalTimer(uint32_t intervalMs)
        : m_intervalMs(intervalMs)
    {
        m_base = std::chrono::steady_clock::now();
    }

    bool fire()
    {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - m_base);
        if (elapsed.count() < static_cast<int64_t>(m_intervalMs))
            return false;
        m_base = now;
        return true;
    }

    void reset()
    {
        m_base = std::chrono::steady_clock::now();
    }

    uint32_t intervalMs() const { return m_intervalMs; }

private:
    uint32_t m_intervalMs = 0;
    std::chrono::steady_clock::time_point m_base{};
};
