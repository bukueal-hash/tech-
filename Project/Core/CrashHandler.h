#pragma once
// ── VEH crash handler ───────────────────────────────────────────────────────
// Installs a vectored exception handler that logs the crash context (exception
// code, fault address, registers, stack trace) to kArcVerifyPath as NDJSON,
// then returns EXCEPTION_CONTINUE_SEARCH so the Windows crash dialog still
// appears.  No cleanup — the process is about to die anyway.

#include <windows.h>

LONG CALLBACK ArcCrashHandler(EXCEPTION_POINTERS* ex);
void InstallCrashHandler();
