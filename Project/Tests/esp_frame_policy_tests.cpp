#include "doctest/doctest.h"

#include "Core/EspFramePolicy.hpp"
#include "Core/BotEspPosition.hpp"

TEST_CASE("EspFramePolicy accepts a current fresh frame")
{
    EspFramePolicy::SnapshotMeta frame{
        true, 7, 1000};

    CHECK(EspFramePolicy::Check(frame, 7, 1000)
        == EspFramePolicy::Acceptance::Accepted);
    CHECK(EspFramePolicy::IsAcceptable(frame, 7, 1499));
}

TEST_CASE("EspFramePolicy rejects invalid and old-generation frames")
{
    CHECK(EspFramePolicy::Check({false, 7, 1000}, 7, 1000)
        == EspFramePolicy::Acceptance::Invalid);
    CHECK(EspFramePolicy::Check({true, 6, 1000}, 7, 1000)
        == EspFramePolicy::Acceptance::WrongGeneration);
}

TEST_CASE("EspFramePolicy rejects missing timestamps and clock skew")
{
    CHECK(EspFramePolicy::Check({true, 7, 0}, 7, 1000)
        == EspFramePolicy::Acceptance::MissingTimestamp);
    CHECK(EspFramePolicy::Check({true, 7, 1001}, 7, 1000)
        == EspFramePolicy::Acceptance::ClockSkew);
}

TEST_CASE("EspFramePolicy accepts the age boundary and rejects older frames")
{
    constexpr uint64_t maxAge = 500;
    const EspFramePolicy::SnapshotMeta frame{true, 9, 2000};

    CHECK(EspFramePolicy::Check(frame, 9, 2500, maxAge)
        == EspFramePolicy::Acceptance::Accepted);
    CHECK(EspFramePolicy::Check(frame, 9, 2501, maxAge)
        == EspFramePolicy::Acceptance::Stale);
}

TEST_CASE("EspFramePolicy default grace tolerates a slow worker publish")
{
    const EspFramePolicy::SnapshotMeta frame{true, 11, 5000};
    CHECK(EspFramePolicy::IsAcceptable(frame, 11, 5500));
    CHECK(EspFramePolicy::IsAcceptable(frame, 11, 5999));
    CHECK_FALSE(EspFramePolicy::IsAcceptable(frame, 11, 6001));
}

TEST_CASE("BotEspPosition keeps a valid root before trying center")
{
    const Vector3 root{100.f, 0.f, 0.f};
    const Vector3 center{200.f, 0.f, 0.f};
    Vector3 selected{};

    REQUIRE(BotEspPosition::Select(
        root, center, [](const Vector3& p) { return p.x > 0.f; }, selected));
    CHECK(selected.x == doctest::Approx(root.x));
}

TEST_CASE("BotEspPosition uses center when root is invalid")
{
    const Vector3 root{};
    const Vector3 center{200.f, 0.f, 0.f};
    Vector3 selected{};

    REQUIRE(BotEspPosition::Select(
        root, center, [](const Vector3& p) { return p.x > 0.f; }, selected));
    CHECK(selected.x == doctest::Approx(center.x));
}

TEST_CASE("BotEspPosition rejects when both samples are invalid")
{
    Vector3 selected{9.f, 9.f, 9.f};
    CHECK_FALSE(BotEspPosition::Select(
        Vector3{}, Vector3{},
        [](const Vector3& p) { return p.x > 0.f; }, selected));
    CHECK(selected.x == doctest::Approx(9.f));
}
