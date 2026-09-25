#pragma once

// Shared test setup — kept as the home for common test helpers (precompiled
// headers, shared fakes) as the suite grows.
// The adopted SDK (game offsets + decode routines) is included here so every
// test build compiles it: the suite stays in lockstep with each SDK drop.
#include "Core/SDK.hpp"

// The fake memory store for decode suites lives in Tests/fake_mem.hpp (it needs
// SteamDecrypt.hpp, which only the decode suites pull in).