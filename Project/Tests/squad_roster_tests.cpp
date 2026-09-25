// Squad roster suite - SteamID64 gating + squad tag formatting.
//
// The permanent id must only ever be a real Steam account id (the payload
// dword layout is the proof), and the squad tag must stay empty rather than
// inventing "S0" for unknown membership.

#include "tests_main.hpp"

#include "doctest/doctest.h"

#include "Core/SquadRoster.hpp"

TEST_CASE("PlausibleSteamId accepts Steam individual accounts")
{
    // 0x01100001 universe/type/instance + account id 12345678
    CHECK(SquadRoster::PlausibleSteamId(0x0110000100BC614Eull));
    CHECK(SquadRoster::PlausibleSteamId(0x0110000100000001ull));
}

TEST_CASE("PlausibleSteamId rejects everything else")
{
    CHECK_FALSE(SquadRoster::PlausibleSteamId(0));                      // unread
    CHECK_FALSE(SquadRoster::PlausibleSteamId(0x0110000100000000ull));  // zero account
    CHECK_FALSE(SquadRoster::PlausibleSteamId(0x0110000200BC614Eull));  // wrong instance
    CHECK_FALSE(SquadRoster::PlausibleSteamId(0x0003000000100001ull));  // console/other layout
    CHECK_FALSE(SquadRoster::PlausibleSteamId(0x4141414141414141ull));  // ASCII garbage
}

TEST_CASE("SteamLabel is the full decimal id")
{
    // 76561197960265728 (Steam id base) + account 12345678
    CHECK(SquadRoster::SteamLabel(0x0110000100BC614Eull) == "76561197972611406");
}

TEST_CASE("SquadTag covers real indices and hides unknowns")
{
    CHECK(SquadRoster::SquadTag(1) == "S1");
    CHECK(SquadRoster::SquadTag(12) == "S12");
    CHECK(SquadRoster::SquadTag(0) == "");
    CHECK(SquadRoster::SquadTag(-3) == "");
}

TEST_CASE("ParseDecimalId accepts clean decimal account ids")
{
    const uint8_t id[] = { '7', '6', '5', '6', '1', '1', '9', '8' };
    CHECK(SquadRoster::ParseDecimalId(id, 8) == 76561198ull);
    const uint8_t pad[] = { '0', '0', '1', '2', '3' };
    CHECK(SquadRoster::ParseDecimalId(pad, 5) == 123ull);
}

TEST_CASE("ParseDecimalId rejects anything that is not a plain number")
{
    const uint8_t letters[] = { '1', '2', 'a', '4', '5' };
    CHECK(SquadRoster::ParseDecimalId(letters, 5) == 0);
    const uint8_t zero[] = { '0', '0', '0', '0', '0' };
    CHECK(SquadRoster::ParseDecimalId(zero, 5) == 0);
    const uint8_t shortId[] = { '1', '2', '3', '4' };
    CHECK(SquadRoster::ParseDecimalId(shortId, 4) == 0);   // < 5 digits
    const uint8_t longId[21] = { '1' };
    CHECK(SquadRoster::ParseDecimalId(longId, 21) == 0);   // > 20 digits
    CHECK(SquadRoster::ParseDecimalId(nullptr, 8) == 0);
    // overflow guard: 20 nines wraps a uint64 -> refused, not truncated
    const uint8_t nines[20] = { '9', '9', '9', '9', '9', '9', '9', '9', '9', '9',
                                '9', '9', '9', '9', '9', '9', '9', '9', '9', '9' };
    CHECK(SquadRoster::ParseDecimalId(nines, 20) == 0);
}

TEST_CASE("StatusTag joins only the flags that are set")
{
    CHECK(SquadRoster::StatusTag(false, false, false) == "");
    CHECK(SquadRoster::StatusTag(true, false, false) == "[Bot]");
    CHECK(SquadRoster::StatusTag(false, true, false) == "[Spec]");
    CHECK(SquadRoster::StatusTag(false, false, true) == "[Done]");
    CHECK(SquadRoster::StatusTag(true, true, true) == "[Bot] [Spec] [Done]");
}
