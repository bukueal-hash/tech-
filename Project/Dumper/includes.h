#pragma once
#include <stdio.h>
#include <cctype>
#include <string>
#include <vector>
#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <functional>
#include <shared_mutex>
#include <unordered_map>
#include <intrin.h>
#include <map>

#include "Driver.h"

// The app's single source of truth for layout + crypto. Offsets.h is generated
// from the SDK drop, NameCrypto.hpp holds the tested decode primitives and
// Reflection.hpp wraps both (plus steam_decrypt) into the name/class walkers.
// The dumper used to carry a private fork of every constant and drifted on each
// patch; consuming these means a patch only touches the app's own headers.
#include "Core/Offsets.h"
#include "Core/NameCrypto.hpp"
#include "Core/Reflection.hpp"

#include "utility.hxx"
#include "engine.h"
#include "helpers.hxx"
#include "structs.h"
#include "gobjects.hxx"
#include "gnames.hxx"
#include "sdk.hxx"
#include "dump.hxx"
