#pragma once
// Squads & identity (feature: squads & identity).
//
// Pure helpers between the DMA reads (UEmbarkPlatformIdComponent ->
// FUniqueNetIdRepl -> FUniqueNetId payload, and the UEmbarkSquad* membership
// on PioneerPlayerState) and the ESP label. A SteamID64 is only surfaced when
// it really is a Steam account id, so garbage reads can never mint a "permanent
// id".

#include <cstdint>
#include <string>

namespace SquadRoster {

// Steam individual accounts carry universe 1 / type 1 / instance 1 in the high
// dword (0x01100001) and a non-zero account id in the low dword. Everything
// else (a flaky read, an inline TVariant alternative, console FString ids) is
// not surfaced as a permanent id.
inline bool PlausibleSteamId(uint64_t id)
{
    return (id >> 32) == 0x01100001ull && (id & 0xFFFFFFFFull) != 0;
}

// Permanent-id label: the full decimal SteamID64 (stable across raids).
inline std::string SteamLabel(uint64_t steamId64)
{
    return std::to_string(steamId64);
}

// Squad tag for the label: "S1", "S2", ... Empty for unknown membership.
inline std::string SquadTag(int squadIdx)
{
    if (squadIdx < 1)
        return "";
    return "S" + std::to_string(squadIdx);
}

// Platform identity fallback (UniqueNetIdRepl::REPL_BYTES_*): the inline
// FAccountId alternative replicates as plain decimal digits (the drop:
// "console -> FString of decimal digits"). Accepts only a clean 5-20 digit
// number that fits a uint64 - anything else is an unresolved id, never a
// garbled "permanent id".
inline uint64_t ParseDecimalId(const uint8_t* bytes, int len)
{
    if (!bytes || len < 5 || len > 20)
        return 0;
    uint64_t v = 0;
    for (int i = 0; i < len; ++i) {
        const uint8_t c = bytes[i];
        if (c < '0' || c > '9')
            return 0;
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (v > ((~0ull) - digit) / 10)
            return 0;
        v = v * 10 + digit;
    }
    return v != 0 ? v : 0;
}

// Player-state status tags (PlayerState bits): "[Bot] [Spec] [Done]".
// [Bot] = bIsABot, [Spec] = bIsSpectator, [Done] = bFinishedRound.
// Empty when no flag is set so quiet players keep clean labels.
inline std::string StatusTag(bool bot, bool spectator, bool finishedRound)
{
    std::string out;
    if (bot)
        out += "[Bot]";
    if (spectator) {
        if (!out.empty())
            out += " ";
        out += "[Spec]";
    }
    if (finishedRound) {
        if (!out.empty())
            out += " ";
        out += "[Done]";
    }
    return out;
}

} // namespace SquadRoster
