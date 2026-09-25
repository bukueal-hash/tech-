// Activity feed suite - ring storage, dedupe, expiry, age text.
//
// The feed is fed by scanner passes that can repeat themselves (a container
// probe at 500ms cadence re-observes the same state): these tests pin that one
// event stays one row and that old rows leave instead of clogging the panel.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/ActivityFeed.hpp"

TEST_CASE("Push keeps newest first")
{
    ActivityFeed::Feed feed;
    feed.Push(1000, 10, "alpha");
    feed.Push(2000, 20, "beta");
    feed.Push(3000, 30, "gamma");
    CHECK(feed.Count() == 3);
    CHECK(std::string(feed.At(0).text) == "gamma");
    CHECK(std::string(feed.At(2).text) == "alpha");
}

TEST_CASE("Push dedupes repeated observations of the same event")
{
    ActivityFeed::Feed feed;
    feed.Push(1000, 10, "crate opened");
    feed.Push(1500, 10, "crate opened");   // same probe re-observing
    feed.Push(2500, 10, "crate opened");
    CHECK(feed.Count() == 1);
    // past the dedupe window the same event counts again
    feed.Push(20000, 10, "crate opened");
    CHECK(feed.Count() == 2);
}

TEST_CASE("Push caps at kCapacity, dropping the oldest")
{
    ActivityFeed::Feed feed;
    for (int i = 0; i < 10; ++i)
        feed.Push(1000 + static_cast<uint64_t>(i) * 100,
            static_cast<uint64_t>(i), "evt");
    CHECK(feed.Count() == ActivityFeed::Feed::kCapacity);
    CHECK(std::string(feed.At(0).text) == "evt");
    CHECK(feed.At(0).key == 9);
}

TEST_CASE("Expire cuts the old tail")
{
    ActivityFeed::Feed feed;
    feed.Push(1000, 1, "old");
    feed.Push(5000, 2, "mid");
    feed.Push(9000, 3, "new");
    feed.Expire(10000, 6000);   // drops anything before t=4000
    CHECK(feed.Count() == 2);
    CHECK(std::string(feed.At(0).text) == "new");
    CHECK(std::string(feed.At(1).text) == "mid");
}

TEST_CASE("AgeText shortens with age")
{
    CHECK(ActivityFeed::AgeText(1000, 1000) == "now");
    CHECK(ActivityFeed::AgeText(5000, 1000) == "now");
    CHECK(ActivityFeed::AgeText(12000, 1000) == "11s");
    CHECK(ActivityFeed::AgeText(130000, 1000) == "2m");
    CHECK(ActivityFeed::AgeText(500, 1000) == "");   // clock went backwards
}
