// Crash reports. The handlers do as little as they can: the text is built in a fixed buffer and written with plain system calls,
// because the heap and the C library may be what is broken. The binaries are stripped, so a report names module+offset and the
// image base; those can be looked up against the matching symbols later.
#include "dect2/crash_report.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#else
#include <fcntl.h>
#include <unistd.h>
#if defined(__has_include) && __has_include(<execinfo.h>)
#include <execinfo.h>
#define DECT2_HAVE_BACKTRACE 1
#endif
#endif

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "?"
#endif

namespace fs = std::filesystem;

namespace dect2::crash {

namespace {

// Paths are UTF-8 strings outside; the file system wants the native form.
fs::path toPath(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
std::string fromPath(const fs::path& p) { const std::u8string u = p.u8string(); return std::string(reinterpret_cast<const char*>(u.data()), u.size()); }

// A text buffer that never allocates.
struct Out {
    char* p; size_t n, cap;
    void put(const char* s) { while (*s && n + 1 < cap) p[n++] = *s++; }
    void hex(uint64_t v) {
        put("0x");
        char d[16]; int k = 0;
        do { d[k++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
        while (k && n + 1 < cap) p[n++] = d[--k];
    }
    void dec(long long v) {
        if (v < 0) { put("-"); v = -v; }
        char d[24]; int k = 0;
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k && n + 1 < cap) p[n++] = d[--k];
    }
};

char gVersion[32] = ONAIR_VERSION;
bool gPrevCrashed = false;
std::string gPrevReport;

#ifdef _WIN32

wchar_t gReportW[1024], gDumpW[1024];
char gBuf[8192];
volatile LONG gBusy = 0;

void putAddr(Out& o, uintptr_t a) {   // module+offset, or the bare address when no module holds it
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m) && m) {
        char path[260];
        DWORD n = GetModuleFileNameA(m, path, sizeof path);
        if (n == 0 || n >= sizeof path) n = 0;
        path[n] = 0;
        const char* base = path;
        for (DWORD i = 0; i < n; i++) if (path[i] == '\\' || path[i] == '/') base = path + i + 1;
        o.put(base); o.put("+"); o.hex(a - (uintptr_t)m);
    } else o.hex(a);
}

void writeAll(HANDLE h, const Out& o) {
    DWORD done = 0;
    WriteFile(h, o.p, (DWORD)o.n, &done, nullptr);
}

void report(EXCEPTION_POINTERS* ep, const char* what) {
    if (InterlockedExchange(&gBusy, 1)) return;   // a second fault inside the handler: give up quietly
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    HANDLE h = CreateFileW(gReportW, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        Out o{gBuf, 0, sizeof gBuf};
        o.put("OnAir "); o.put(gVersion); o.put(" crashed: exception "); o.hex(r->ExceptionCode);
        o.put(", address "); putAddr(o, (uintptr_t)r->ExceptionAddress);
        if (what) { o.put(" ["); o.put(what); o.put("]"); }
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
            o.put(r->ExceptionInformation[0] == 0 ? ", reading " : r->ExceptionInformation[0] == 1 ? ", writing " : ", executing ");
            o.hex(r->ExceptionInformation[1]);
        }
        o.put("\nimage base of OnAir.exe: "); o.hex((uintptr_t)GetModuleHandleW(nullptr));
        o.put("\n");
        writeAll(h, o);   // the header first: it is kept even if walking the stack fails

        o.n = 0;
        o.put("stack:\n");
#if defined(__x86_64__) || defined(_M_X64)
        CONTEXT ctx = *ep->ContextRecord;   // walk with the unwind tables, from the faulting instruction
        for (int i = 0; i < 48 && ctx.Rip; i++) {
            o.put("  "); putAddr(o, (uintptr_t)ctx.Rip); o.put("\n");
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
            if (fn) {
                PVOID handlerData = nullptr; DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &frame, nullptr);
            } else {   // a leaf function: the return address is on top of the stack
                ctx.Rip = *(DWORD64*)ctx.Rsp; ctx.Rsp += 8;
            }
        }
#else
        void* frames[48];
        const USHORT n = RtlCaptureStackBackTrace(0, 48, frames, nullptr);
        for (USHORT i = 0; i < n; i++) { o.put("  "); putAddr(o, (uintptr_t)frames[i]); o.put("\n"); }
#endif
        writeAll(h, o);
        CloseHandle(h);
    }
    HANDLE d = CreateFileW(gDumpW, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (d != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId(); mei.ExceptionPointers = ep; mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), d, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(d);
    }
}

LONG WINAPI onException(EXCEPTION_POINTERS* ep) {
    report(ep, nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void onAbort(int) {   // abort() and std::terminate do not raise an exception
    CONTEXT c;
    RtlCaptureContext(&c);
    EXCEPTION_RECORD r{};
    r.ExceptionCode = 0x40000015;   // STATUS_FATAL_APP_EXIT
    r.ExceptionAddress = __builtin_return_address(0);
    EXCEPTION_POINTERS ep{&r, &c};
    report(&ep, "abort");
    TerminateProcess(GetCurrentProcess(), 3);
}

void arm(const fs::path& txt, const fs::path& dmp) {
    wcsncpy(gReportW, txt.wstring().c_str(), 1023);
    wcsncpy(gDumpW, dmp.wstring().c_str(), 1023);
    SetUnhandledExceptionFilter(onException);
    signal(SIGABRT, onAbort);
}

#else

char gReport[1024];
char gAlt[65536];   // a stack of its own, so a stack overflow can still be reported

#ifdef DECT2_HAVE_BACKTRACE
const char* sigName(int s) {
    switch (s) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "SIGABRT";
    }
    return "signal";
}

void onSignal(int sig, siginfo_t* si, void*) {
    char buf[256];
    Out o{buf, 0, sizeof buf};
    o.put("OnAir "); o.put(gVersion); o.put(" crashed: signal "); o.dec(sig);
    o.put(" ("); o.put(sigName(sig)); o.put("), address "); o.hex((uintptr_t)(si ? si->si_addr : nullptr));
    o.put("\nstack:\n");
    void* frames[64];
    const int n = backtrace(frames, 64);
    int fd = gReport[0] ? open(gReport, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    if (fd >= 0) {
        (void)!write(fd, buf, o.n);
        backtrace_symbols_fd(frames, n, fd);
        close(fd);
    }
    (void)!write(STDERR_FILENO, buf, o.n);   // and in the log, which on these systems is standard error
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    raise(sig);   // the handler was reset when it started (SA_RESETHAND): this ends the process the usual way
}
#endif

void arm(const fs::path& txt) {
    const std::string s = fromPath(txt);
    snprintf(gReport, sizeof gReport, "%s", s.c_str());
#ifdef DECT2_HAVE_BACKTRACE
    void* warm[2];
    backtrace(warm, 2);   // the first call loads code: do it now, not inside the handler
    stack_t st{};
    st.ss_sp = gAlt; st.ss_size = sizeof gAlt;
    sigaltstack(&st, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    for (int s2 : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(s2, &sa, nullptr);
#endif
}

#endif

}

std::string logDir() {
#ifdef _WIN32
    wchar_t w[1024];
    const DWORD n = GetEnvironmentVariableW(L"APPDATA", w, 1024);
    if (n == 0 || n >= 1024) return ".";
    return fromPath(fs::path(w) / L"OnAir");
#else
    const char* h = getenv("HOME");
    const std::string home = h && *h ? h : ".";
#ifdef __APPLE__
    return home + "/Library/Logs/OnAir";
#else
    return home + "/.config/onair";
#endif
#endif
}

void install(const char* name, const char* dir) {
    const std::string d = dir ? dir : logDir();
    const std::string base = name && *name ? name : "crash";
    std::error_code ec;
    fs::create_directories(toPath(d), ec);
    const fs::path txt = toPath(d) / (base + ".txt");
    const fs::path stamp = toPath(d) / (base + ".started");
    gPrevCrashed = false;
    gPrevReport.clear();
    // a report newer than the last start means that run ended in a crash
    if (fs::exists(txt, ec)) {
        std::error_code e1, e2;
        const auto ct = fs::last_write_time(txt, e1);
        const auto st = fs::last_write_time(stamp, e2);
        // >=: a crash right after the start can get the same file time as the start mark (the clock of some file systems, the Docker
        // volumes of the Linux CI among them, is coarser than that)
        if (!e1 && (e2 || ct >= st)) { gPrevCrashed = true; gPrevReport = fromPath(txt); }
    }
    {   // mark this start
        std::ofstream f(stamp, std::ios::trunc);
        f << ONAIR_VERSION << "\n";
    }
    {   // and make sure the mark is newer than an old report, so that the next start does not report it again
        std::error_code e1, e2;
        const auto ct = fs::last_write_time(txt, e1);
        const auto st = fs::last_write_time(stamp, e2);
        if (!e1 && !e2 && st <= ct) fs::last_write_time(stamp, ct + std::chrono::milliseconds(1), e2);
    }
#ifdef _WIN32
    arm(txt, toPath(d) / (base + ".dmp"));
#else
    arm(txt);
#endif
}

bool lastRunCrashed(std::string& reportPath) {
    if (gPrevCrashed) reportPath = gPrevReport;
    return gPrevCrashed;
}

void rotateLog(const std::string& dir) {
    const fs::path cur = toPath(dir) / "onair.log", prev = toPath(dir) / "onair-prev.log";
    std::error_code ec;
    if (!fs::exists(cur, ec)) return;
    fs::remove(prev, ec);
    fs::rename(cur, prev, ec);
}

#ifdef _WIN32
void startLog() {
    if (!_wgetenv(L"APPDATA")) return;
    const std::string d = logDir();
    std::error_code ec;
    fs::create_directories(toPath(d), ec);
    rotateLog(d);
    FILE* unused = _wfreopen((toPath(d) / "onair.log").c_str(), L"w", stderr);
    (void)unused;
}
#endif

}
