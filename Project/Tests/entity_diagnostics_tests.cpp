#include "tests_main.hpp"
#include "Core/EntityDiagnostics.hpp"

#include <limits>

#pragma warning(push)
#pragma warning(disable : 5285)
#include "doctest/doctest.h"
#pragma warning(pop)

TEST_CASE("Entity diagnostics escapes names and reports invalid state")
{
    CHECK(EntityDiagnostics::JsonEscape("crate\"\\line\n") == "crate\\\"\\\\line\\n");
    CHECK(EntityDiagnostics::IsPlausiblePosition(10000.f, 20000.f, 30000.f));
    CHECK_FALSE(EntityDiagnostics::IsPlausiblePosition(0.f, 0.f, 0.f));
    CHECK_FALSE(EntityDiagnostics::IsPlausiblePosition(
        std::numeric_limits<float>::quiet_NaN(), 1.f, 1.f));

    const uint32_t issues = EntityDiagnostics::BuildIssueFlags(
        false, false, false, true, -1.f, 0.f, 6000);
    CHECK((issues & (1u << 0)) != 0); // missing name
    CHECK((issues & (1u << 1)) != 0); // invalid position
    CHECK((issues & (1u << 2)) != 0); // uninitialized position
    CHECK((issues & (1u << 3)) != 0); // drawing invalid position
    CHECK((issues & (1u << 4)) != 0); // invalid health
    CHECK((issues & (1u << 5)) != 0); // stale position
}

TEST_CASE("Entity diagnostics emits parseable summary and entity lines")
{
    EntityDiagnostics::CategorySummary players{"player", 3, 1, 2};
    const std::string summary = EntityDiagnostics::FormatSummary(
        "live", "world=1,playerEsp=1", players,
        {"bot", 2, 1, 0}, {"container", 5, 4, 1},
        {"item", 7, 6, 0}, 42);
    CHECK(summary.find("\"message\":\"entity_summary\"") != std::string::npos);
    CHECK(summary.find("\"players\":{\"total\":3,\"drawing\":1,\"issues\":2}") != std::string::npos);
    CHECK(summary.find("\"generation\":42") != std::string::npos);
    CHECK(summary.back() == '\n');

    EntityDiagnostics::EntitySample sample;
    sample.kind = "bot";
    sample.actorKey = 0x1234;
    sample.name = "Wasp\"";
    sample.x = 10000.f;
    sample.y = 20000.f;
    sample.z = 30000.f;
    sample.positionInitialized = true;
    sample.drawing = true;
    sample.issueFlags = 0;
    const std::string entity = EntityDiagnostics::FormatEntity(sample);
    CHECK(entity.find("\"kind\":\"bot\"") != std::string::npos);
    CHECK(entity.find("\"actorKey\":\"0x1234\"") != std::string::npos);
    CHECK(entity.find("Wasp\\\"") != std::string::npos);
    CHECK(entity.find("\"severity\":\"INFO\"") != std::string::npos);
}
