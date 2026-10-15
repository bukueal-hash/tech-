#include "CrashHandler.h"
#include "AgentLog.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>

// ── VEH crash handler ──────────────────────────────────────────────────────
// Appends one NDJSON line to kArcVerifyPath with the exception code, flags,
// fault address, RIP/RSP and the general-purpose registers RAX..R15 (x64), plus
// a small stack snapshot, then re-raises so the Windows crash dialog / WER
// still fires.
//
// Everything on this path is a preallocated stack buffer written with
// WriteFile. That is not an optimisation: this handler runs for the very faults
// that make CRT I/O unreliable —
//
//   * STATUS_HEAP_CORRUPTION — an ofstream (heap allocation for its buffer and
//     the streambuf) faults again inside the handler;
//   * EXCEPTION_STACK_OVERFLOW — the ofstream call can be entered with almost
//     no stack left to run it.
//
// The previous version used std::ofstream + iostream formatting and std::chrono
// on both, so a crash in either of those two cases produced no report at all,
// silently. The report is the whole point of the handler, so it may not
// allocate.

#ifdef _WIN64

// Informational first-chance exceptions. Every thread that names itself raises
// kThreadNameException, and OutputDebugString raises kDebugPrintException; both
// are non-fatal. Logging them as crashes buried the real faults in the log (all
// six "crash" pairs were code 0x406D1388 with rdi = the name magic).
constexpr DWORD kThreadNameException = 0x406D1388u;  // MS_VC_EXCEPTION
constexpr DWORD kDebugPrintException = 0x40010006u;  // DBG_PRINTEXCEPTION_C

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
    case kThreadNameException:           return "THREAD_NAME";    // MS_VC_EXCEPTION
    case kDebugPrintException:           return "DEBUG_PRINT";    // DBG_PRINTEXCEPTION_C
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

namespace {

// Fixed-capacity line builder. Trivial type (no destructor, no allocation), so
// it stays safe to use on a thread that is faulting and cannot unwind.
struct LineBuilder {
    char s[4096]{};
    unsigned n = 0;

    void add(const char* piece)
    {
        while (piece && *piece && n < sizeof(s) - 1)
            s[n++] = *piece++;
    }

    void fmt(const char* f, ...)
    {
        va_list args;
        va_start(args, f);
        char tmp[192]{};
        const int w = _vsnprintf_s(tmp, sizeof(tmp), _TRUNCATE, f, args);
        va_end(args);
        if (w > 0)
            add(tmp);
    }

    // Written as a code point so no escape has to survive on this path.
    void newline()
    {
        if (n < sizeof(s) - 1)
            s[n++] = static_cast<char>(10);
    }
};

// CreateFileW/WriteFile only — no CRT heap, no locale, no stream state.
void WriteLine(const LineBuilder& b)
{
    if (b.n == 0)
        return;

    HANDLE h = CreateFileW(kArcVerifyPathW, FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    DWORD written = 0;
    WriteFile(h, b.s, static_cast<DWORD>(b.n), &written, nullptr);
    CloseHandle(h);
}

}  // namespace

LONG CALLBACK ArcCrashHandler(EXCEPTION_POINTERS* ex)
{
    if (!ex || !ex->ExceptionRecord || !ex->ContextRecord)
        return EXCEPTION_CONTINUE_SEARCH;

    const EXCEPTION_RECORD* er = ex->ExceptionRecord;
    const CONTEXT* ctx = ex->ContextRecord;

    if (er->ExceptionCode == kThreadNameException ||
        er->ExceptionCode == kDebugPrintException)
        return EXCEPTION_CONTINUE_SEARCH;   // informational — not a fault

    LineBuilder b;

    b.add(R"({"location":"CrashHandler.cpp","message":")");
    b.add(ExceptionName(er->ExceptionCode));
    b.fmt(R"(","data":{"code":%lu,"flags":%lu)",
        static_cast<unsigned long>(er->ExceptionCode),
        static_cast<unsigned long>(er->ExceptionFlags));
    b.fmt(R"(,"addr":"0x%llx)",
        static_cast<unsigned long long>(
            reinterpret_cast<uintptr_t>(er->ExceptionAddress)));
    b.fmt(R"(,"rip":"0x%llx","rsp":"0x%llx)",
        static_cast<unsigned long long>(ctx->Rip),
        static_cast<unsigned long long>(ctx->Rsp));
    b.fmt(R"(,"rax":"0x%llx","rbx":"0x%llx","rcx":"0x%llx","rdx":"0x%llx)",
        static_cast<unsigned long long>(ctx->Rax),
        static_cast<unsigned long long>(ctx->Rbx),
        static_cast<unsigned long long>(ctx->Rcx),
        static_cast<unsigned long long>(ctx->Rdx));
    b.fmt(R"(,"rsi":"0x%llx","rdi":"0x%llx","r8":"0x%llx","r9":"0x%llx)",
        static_cast<unsigned long long>(ctx->Rsi),
        static_cast<unsigned long long>(ctx->Rdi),
        static_cast<unsigned long long>(ctx->R8),
        static_cast<unsigned long long>(ctx->R9));
    b.fmt(R"(,"r10":"0x%llx","r11":"0x%llx","r12":"0x%llx","r13":"0x%llx)",
        static_cast<unsigned long long>(ctx->R10),
        static_cast<unsigned long long>(ctx->R11),
        static_cast<unsigned long long>(ctx->R12),
        static_cast<unsigned long long>(ctx->R13));
    b.fmt(R"(,"r14":"0x%llx","r15":"0x%llx)",
        static_cast<unsigned long long>(ctx->R14),
        static_cast<unsigned long long>(ctx->R15));

    // Small stack snapshot (8 qwords from RSP).
    b.add(R"(},"stack":[)");
    bool firstQword = true;
    uintptr_t* stack = reinterpret_cast<uintptr_t*>(ctx->Rsp);
    for (int i = 0; i < 8; ++i) {
        uintptr_t val = 0;
        if (!ReadStackQword(stack, i, val))
            break;
        b.fmt(R"(%s"s%d":"0x%llx")", firstQword ? "" : ",", i,
            static_cast<unsigned long long>(val));
        firstQword = false;
    }
    b.add(R"(])");

    // Wall clock without std::chrono (which also may not be enterable here).
    SYSTEMTIME st{};
    GetLocalTime(&st);
    b.fmt(R"(,"time":"%04u-%02u-%02uT%02u:%02u:%02u.%03u")",
        static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
        static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
        static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
        static_cast<unsigned>(st.wMilliseconds));

    b.add(R"(})");
    b.newline();
    WriteLine(b);

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
