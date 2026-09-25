// Player-ESP miss ledger suite (diagnostics for missing player ESP).
//
// The ledger answers "which player is missing and WHY": every pipeline gate
// that drops a player records a per-actor reason. These tests pin the pure
// behavior — reason naming, per-reason throttle vs. always-count semantics,
// open-miss lifecycle (a miss opens a row, a frame selection closes it), and
// the NDJSON line formats consumed by the debug log.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/PlayerEspMissLog.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

// Sink collector: replaces the default file tap so tests never touch disk.
struct SinkCapture {
    std::shared_ptr<std::vector<std::string>> lines =
        std::make_shared<std::vector<std::string>>();
    PlayerEspMiss::MissLedger::Sink Bind()
    {
        auto captured = lines;
        return [captured](const std::string& s) { captured->push_back(s); };
    }
};

// MissLedger owns a mutex (non-movable) — heap-allocate the fixture ledger.
std::unique_ptr<PlayerEspMiss::MissLedger> MakeLedger(SinkCapture& cap)
{
    auto ledger = std::make_unique<PlayerEspMiss::MissLedger>();
    ledger->SetSink(cap.Bind());
    return ledger;
}

} // namespace

TEST_CASE("ReasonName names every reason uniquely")
{
    std::vector<std::string> names;
    for (size_t i = 1; i < PlayerEspMiss::kReasonCount; ++i) {
        const char* n = PlayerEspMiss::ReasonName(
            static_cast<PlayerEspMiss::Reason>(i));
        CHECK(std::string(n) != "none");
        names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    CHECK(std::unique(names.begin(), names.end()) == names.end());
}

TEST_CASE("NoteAt counts every miss but throttles emissions per reason")
{
    SinkCapture cap;
    auto ledger = MakeLedger(cap);

    // Three rapid misses at the same gate: all counted, one emitted.
    CHECK(ledger->NoteAt(1000, 0xABC, "Rex", 12.5f,
        PlayerEspMiss::Reason::FrameNotDrawing));
    CHECK_FALSE(ledger->NoteAt(1100, 0xABC, "Rex", 12.5f,
        PlayerEspMiss::Reason::FrameNotDrawing));
    CHECK_FALSE(ledger->NoteAt(1900, 0xABC, "Rex", 12.5f,
        PlayerEspMiss::Reason::FrameNotDrawing));
    CHECK(cap.lines->size() == 1);

    // Same actor, DIFFERENT reason: independent throttle slot, emits.
    CHECK(ledger->NoteAt(1200, 0xABC, "Rex", 12.5f,
        PlayerEspMiss::Reason::FramePosition));
    CHECK(cap.lines->size() == 2);

    // The window is measured from the last EMISSION (t=1000), so t=3200
    // emits again even though the throttled attempts at 1100/1900 ran in
    // between — sustained misses keep producing one line per window.
    CHECK(ledger->NoteAt(3200, 0xABC, "Rex", 13.f,
        PlayerEspMiss::Reason::FrameNotDrawing));
    CHECK(cap.lines->size() == 3);

    // 5 notes total (4 not-drawing + 1 position), all counted.
    const auto rows = ledger->OpenMisses(3200, 60000, 8);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].total == 5);
    CHECK(rows[0].ReasonCount(PlayerEspMiss::Reason::FrameNotDrawing) == 4);
    CHECK(rows[0].ReasonCount(PlayerEspMiss::Reason::FramePosition) == 1);
    CHECK(rows[0].TopReason() == PlayerEspMiss::Reason::FrameNotDrawing);
}

TEST_CASE("NoteAt rejects non-actors and Reason::None")
{
    SinkCapture cap;
    auto ledger = MakeLedger(cap);
    CHECK_FALSE(ledger->NoteAt(1000, 0, "Nobody", 5.f,
        PlayerEspMiss::Reason::FrameDistance));
    CHECK_FALSE(ledger->NoteAt(1000, 0x123, "Nobody", 5.f,
        PlayerEspMiss::Reason::None));
    CHECK(ledger->Size() == 0);
    CHECK(cap.lines->empty());
}

TEST_CASE("A miss opens a row and a frame selection closes it")
{
    SinkCapture cap;
    auto ledger = MakeLedger(cap);
    constexpr auto R = PlayerEspMiss::Reason::FrameNotDrawing;

    ledger->NoteAt(1000, 0x1, "Ana", 20.f, R);
    auto open = ledger->OpenMisses(1500, 60000, 8);
    REQUIRE(open.size() == 1);

    // Selected into a frame: row no longer counts as open...
    ledger->NoteSelectedAt(2000, 0x1, "Ana");
    CHECK(ledger->OpenMisses(2500, 60000, 8).empty());

    // ...but a fresh miss re-opens it (selectedMs < lastMs).
    ledger->NoteAt(3000, 0x1, "Ana", 21.f, R);
    open = ledger->OpenMisses(3500, 60000, 8);
    REQUIRE(open.size() == 1);
    CHECK(open[0].total == 2);
}

TEST_CASE("OpenMisses ages rows out and bounds the result")
{
    SinkCapture cap;
    auto ledger = MakeLedger(cap);
    constexpr auto R = PlayerEspMiss::Reason::EvictPosInvalid;

    ledger->NoteAt(1000, 0x1, "A", 1.f, R);
    ledger->NoteAt(1000, 0x2, "B", 2.f, R);
    ledger->NoteAt(1000, 0x3, "C", 3.f, R);

    // maxAgeMs window excludes stale rows.
    CHECK(ledger->OpenMisses(1000 + 30001, 30000, 8).empty());
    CHECK(ledger->OpenMisses(2000, 30000, 8).size() == 3);

    // Bounding: maxRows truncates.
    CHECK(ledger->OpenMisses(2000, 30000, 2).size() == 2);
}

TEST_CASE("Ledger evicts the oldest row at capacity")
{
    SinkCapture cap;
    auto ledger = MakeLedger(cap);
    constexpr auto R = PlayerEspMiss::Reason::FrameDistance;
    constexpr size_t capN = PlayerEspMiss::MissLedger::kMaxTrackedActors;

    for (size_t i = 0; i < capN; ++i)
        ledger->NoteAt(1000 + i, 0x1000 + i, "P", 1.f, R);
    CHECK(ledger->Size() == capN);

    // Actor 0x1000 is the oldest (lastMs = 1000); one more pushes it out.
    ledger->NoteAt(99999, 0xFFFF, "New", 1.f, R);
    CHECK(ledger->Size() == capN);
    CHECK(ledger->OpenMisses(99999, 600000, capN + 1).size() == capN);
}

TEST_CASE("FormatMissJson emits one NDJSON line per miss event")
{
    PlayerEspMiss::ActorMiss row;
    row.actorKey = 0x7FF;
    row.name = "Vex \"II\"";
    row.distance = 42.25f;
    row.lastMs = 5000;
    row.total = 7;
    row.byReason[static_cast<size_t>(PlayerEspMiss::Reason::FrameDistance)] = 7;

    const std::string line = PlayerEspMiss::FormatMissJson(
        row, PlayerEspMiss::Reason::FrameDistance, 6000, 1700000000123);

    CHECK(line.find("\"message\":\"player_miss\"") != std::string::npos);
    CHECK(line.find("\"actor\":\"0x7FF\"") != std::string::npos);
    CHECK(line.find("\"name\":\"Vex \\\"II\\\"\"") != std::string::npos);
    CHECK(line.find("\"reason\":\"frame_distance\"") != std::string::npos);
    CHECK(line.find("\"count\":7") != std::string::npos);
    CHECK(line.find("\"total\":7") != std::string::npos);
    CHECK(line.find("\"ageMs\":1000") != std::string::npos);
    CHECK(line.find("\"timestamp\":1700000000123") != std::string::npos);
    // Balanced braces: valid single-line JSON shape.
    CHECK(line.front() == '{');
    CHECK(line.back() == '}');
}

TEST_CASE("FormatSummaryLine aggregates open misses with top reasons")
{
    SinkCapture cap;
    auto ledger = MakeLedger(cap);
    ledger->NoteAt(1000, 0x1, "Ana", 20.f,
        PlayerEspMiss::Reason::FrameNotDrawing);
    ledger->NoteAt(1000, 0x1, "Ana", 20.f,
        PlayerEspMiss::Reason::FrameDistance);
    ledger->NoteAt(1050, 0x2, "Bo", 55.f,
        PlayerEspMiss::Reason::EvictPsLost);

    const std::string line = ledger->FormatSummaryLine(1500, 16, 30000);
    CHECK(line.find("\"message\":\"player_miss_summary\"") != std::string::npos);
    CHECK(line.find("\"open\":2") != std::string::npos);
    CHECK(line.find("\"name\":\"Ana\"") != std::string::npos);
    CHECK(line.find("\"name\":\"Bo\"") != std::string::npos);
    CHECK(line.find("evict_ps_lost") != std::string::npos);

    const std::string console = ledger->FormatConsoleSummary(1500, 8);
    CHECK(console.find("open=2") != std::string::npos);
    CHECK(console.find("Ana") != std::string::npos);
}

TEST_CASE("SummaryTryLock reports open misses without blocking")
{
    SinkCapture cap;
    auto ledger = MakeLedger(cap);
    ledger->NoteAt(1000, 0x1, "Ana", 20.f,
        PlayerEspMiss::Reason::FrameGateReject);

    std::string summary;
    CHECK(ledger->SummaryTryLock(summary, 1500, 4));
    CHECK(summary.find("open=1") != std::string::npos);
    CHECK(summary.find("frame_gate_reject") != std::string::npos);

    // Empty ledger → empty summary (overlay renders "none").
    PlayerEspMiss::MissLedger empty;
    CHECK(empty.SummaryTryLock(summary, 1500, 4));
    CHECK(summary.empty());
}
