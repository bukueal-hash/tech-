#include "CrashHandler.h"
#include "AgentLog.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

// ── VEH crash handler ──────────────────────────────────────────────────────
// Logs a single NDJSON line to kArcVerifyPath with:
//   exception code, flags, fault address, RIP/RSP,
//   general-purpose registers RAX..R15 (x64 only),
//   then re-raises so Windows crash dialog / WER still fires.

#ifdef _WIN64

static const char* ExceptionName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:     return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW:       return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:  return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:   return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:   return "FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_OVERFLOW:         return "FLT_OVERFLOW";
    case EXCEPTION_FLT_UNDERFLOW:        return "FLT_UNDERFLOW";
    case EXCEPTION_GUARD_PAGE:           return "GUARD_PAGE";
    case STATUS_HEAP_CORRUPTION:         return "HEAP_CORRUPTION";
    case 0xE06D7363:                     return "CPP_EXCEPTION";   // MSVC C++ throw
    default:                             return "UNKNOWN";
    }
}

// SEH-safe stack read (must be separate function — no C++ objects in __try scope)
static bool ReadStackQword(uintptr_t* stack, int idx, uintptr_t& out)
{
    __try {
        out = stack[idx];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

LONG CALLBACK ArcCrashHandler(EXCEPTION_POINTERS* ex)
{
    if (!ex || !ex->ExceptionRecord || !ex->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD* er = ex->ExceptionRecord;
    const CONTEXT* ctx = ex->ContextRecord;

    // Format timestamp
    const auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Build NDJSON line
    std::ofstream f(kArcVerifyPath, std::ios::app);
    if (!f)
        return EXCEPTION_CONTINUE_SEARCH;

    f << "{\"sessionId\":\"c190fb\",\"runId\":\"crash\","
      << "\"hypothesisId\":\"X\","
      << "\"location\":\"CrashHandler.cpp\","
      << "\"message\":\"" << ExceptionName(er->ExceptionCode) << "\","
      << "\"data\":{"
      << "\"code\":" << er->ExceptionCode
      << ",\"flags\":" << er->ExceptionFlags
      << ",\"addr\":\"0x" << std::hex << reinterpret_cast<uintptr_t>(er->ExceptionAddress) << std::dec << "\""
      << ",\"rip\":\"0x" << std::hex << ctx->Rip << std::dec << "\""
      << ",\"rsp\":\"0x" << std::hex << ctx->Rsp << std::dec << "\""
      << ",\"rax\":\"0x" << std::hex << ctx->Rax << std::dec << "\""
      << ",\"rbx\":\"0x" << std::hex << ctx->Rbx << std::dec << "\""
      << ",\"rcx\":\"0x" << std::hex << ctx->Rcx << std::dec << "\""
      << ",\"rdx\":\"0x" << std::hex << ctx->Rdx << std::dec << "\""
      << ",\"rsi\":\"0x" << std::hex << ctx->Rsi << std::dec << "\""
      << ",\"rdi\":\"0x" << std::hex << ctx->Rdi << std::dec << "\""
      << ",\"r8\":\"0x"  << std::hex << ctx->R8  << std::dec << "\""
      << ",\"r9\":\"0x"  << std::hex << ctx->R9  << std::dec << "\""
      << ",\"r10\":\"0x" << std::hex << ctx->R10 << std::dec << "\""
      << ",\"r11\":\"0x" << std::hex << ctx->R11 << std::dec << "\""
      << ",\"r12\":\"0x" << std::hex << ctx->R12 << std::dec << "\""
      << ",\"r13\":\"0x" << std::hex << ctx->R13 << std::dec << "\""
      << ",\"r14\":\"0x" << std::hex << ctx->R14 << std::dec << "\""
      << ",\"r15\":\"0x" << std::hex << ctx->R15 << std::dec << "\""
      << "},\"timestamp\":" << ts << "}\n";

    // Also dump a small stack snapshot (8 qwords from RSP)
    f << "{\"sessionId\":\"c190fb\",\"runId\":\"crash\","
      << "\"hypothesisId\":\"X\","
      << "\"location\":\"CrashHandler.cpp\","
      << "\"message\":\"stack_trace\","
      << "\"data\":{"
      << "\"rsp\":\"0x" << std::hex << ctx->Rsp << std::dec << "\"";

    {
        uintptr_t* stack = reinterpret_cast<uintptr_t*>(ctx->Rsp);
        for (int i = 0; i < 8; ++i) {
            uintptr_t val = 0;
            if (!ReadStackQword(stack, i, val))
                break;
            f << ",\"s" << i << "\":\"0x" << std::hex << val << std::dec << "\"";
        }
    }

    f << "},\"timestamp\":" << ts << "}\n";
    f.flush();

    // Continue search — let Windows crash dialog / WER fire
    return EXCEPTION_CONTINUE_SEARCH;
}

#else
// 32-bit fallback — stub (this project is x64-only)
LONG CALLBACK ArcCrashHandler(EXCEPTION_POINTERS*) { return EXCEPTION_CONTINUE_SEARCH; }
#endif

void InstallCrashHandler()
{
    AddVectoredExceptionHandler(0 /* first handler */, ArcCrashHandler);
}
