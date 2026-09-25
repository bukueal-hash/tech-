#pragma once
// Activity feed (feature #10).
//
// A tiny newest-first ring between the scanner-side event detection (container
// opened transitions, player down/up transitions) and the feed panel. Pure
// storage + formatting - the DMA side decides WHAT happened, this only keeps
// the timeline, deduped and time-limited.
//
// Note on the EmbarkServerEvents structs (PlayerExtractionStarted,
// ExtractionTimerStarted, ...): those are transient dispatch objects handed to
// listeners at the moment of the event. DMA reads persistent memory only, so
// by the time a scanner polls, the dispatch has already happened and the
// object is gone - there is no readable "events fired" list. The feed
// therefore reflects persistent interaction state (which is readable) instead.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace ActivityFeed {

struct Entry {
    uint64_t stampMs = 0;
    uint64_t key = 0;      // source actor - the dedupe identity
    char text[64]{};
};

class Feed {
public:
    static constexpr int kCapacity = 6;

    /** Append (newest first). An identical (key, text) within dedupeMs of the
     *  current newest is dropped so one slow scanner cannot spam duplicates. */
    void Push(uint64_t nowMs, uint64_t key, const char* text, uint64_t dedupeMs = 8000)
    {
        if (!text || !text[0])
            return;
        if (m_count > 0
            && m_entries[0].key == key
            && std::strncmp(m_entries[0].text, text, sizeof(m_entries[0].text)) == 0
            && nowMs >= m_entries[0].stampMs
            && (nowMs - m_entries[0].stampMs) <= dedupeMs)
            return;
        int last = m_count;
        if (last > kCapacity - 1)
            last = kCapacity - 1;
        for (int i = last; i > 0; --i)
            m_entries[i] = m_entries[i - 1];
        m_entries[0].stampMs = nowMs;
        m_entries[0].key = key;
        // Bounded manual copy - no deprecated CRT surface.
        std::size_t i = 0;
        for (; i + 1 < sizeof(m_entries[0].text) && text[i] != '\0'; ++i)
            m_entries[0].text[i] = text[i];
        m_entries[0].text[i] = '\0';
        if (m_count < kCapacity)
            ++m_count;
    }

    /** Drop entries older than ttlMs (entries are newest-first, so the tail
     *  goes in one cut). */
    void Expire(uint64_t nowMs, uint64_t ttlMs)
    {
        for (int i = 0; i < m_count; ++i) {
            if (nowMs >= m_entries[i].stampMs
                && (nowMs - m_entries[i].stampMs) > ttlMs) {
                m_count = i;
                return;
            }
        }
    }

    int Count() const { return m_count; }
    const Entry& At(int i) const { return m_entries[i]; }

private:
    Entry m_entries[kCapacity]{};
    int m_count = 0;
};

// "now" (first 5s), "12s", "2m" - short age text for the feed rows.
inline std::string AgeText(uint64_t nowMs, uint64_t stampMs)
{
    if (stampMs == 0 || nowMs < stampMs)
        return "";
    const uint64_t seconds = (nowMs - stampMs) / 1000;
    if (seconds < 5)
        return "now";
    if (seconds < 60)
        return std::to_string(seconds) + "s";
    return std::to_string(seconds / 60) + "m";
}

} // namespace ActivityFeed
