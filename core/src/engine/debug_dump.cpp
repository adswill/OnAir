// Debug dump (debug_dump.h). Linux x86-64 writes the minidump itself; the layout is the public one of Windows minidumps, with the
// Linux streams Breakpad defined (ELF build ids in the module list, /proc/self/maps, /proc/self/status, the auxiliary vector), so
// common tools read it. Everything below runs without the heap and without locks: the same code writes the dump from the crash handler.
#include "dect2/engine/debug_dump.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
#define DECT2_DUMP_LINUX 1
#include <cerrno>
#include <csignal>
#include <cpuid.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <ucontext.h>
#include <unistd.h>
#elif defined(_WIN32)
#define DECT2_DUMP_WINDOWS 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <thread>
#endif

namespace dect2::dump {

#if defined(DECT2_DUMP_LINUX)

namespace {

// ------------------------------------------------------------------ limits (all memory is static or mapped once in prepare())

constexpr int kMaxThreads = 512;
constexpr size_t kCtxSize = 0x4d0;              // the AMD64 CONTEXT of a minidump
constexpr uint64_t kStackCap = 512 * 1024;      // per thread, from the stack pointer up
constexpr uint64_t kStackTotal = 48ull << 20;   // all threads together
constexpr size_t kMapsCap = 4u << 20;           // /proc/self/maps text
constexpr int kMaxMaps = 16384;
constexpr int kMaxModules = 1024;
constexpr size_t kStatusCap = 16 * 1024;
constexpr size_t kAuxvCap = 4096;
constexpr int kAnswerMs = 1000;                 // how long the writer waits for the threads (normal dump)
constexpr int kAnswerMsCrash = 500;             // the same from the crash handler

struct Slot {
    std::atomic<int> tid{0};
    std::atomic<int> state{0};   // 0 asked, 1 answered (ctx filled), 2 gone
    std::array<uint8_t, kCtxSize> ctx{};
    uint64_t sp = 0;
    std::array<char, 20> name{};
    uint64_t lo = 0, len = 0;    // the stack memory written
};

struct MapEntry { uint64_t start, end, offset; uint32_t pathOff, pathLen; bool readable; };
struct Module { uint64_t base, end; uint32_t pathOff, pathLen; std::array<uint8_t, 64> id; uint32_t idLen; };

Slot gSlots[kMaxThreads];
int gNSlots = 0;
bool gAnswered[kMaxThreads];
MapEntry gMaps[kMaxMaps];
int gNMaps = 0;
Module gMods[kMaxModules];
int gNMods = 0;
char* gMapsText = nullptr;   // mapped in prepare()
size_t gMapsLen = 0;
std::array<char, kStatusCap> gStatus{};
size_t gStatusLen = 0;
std::array<uint8_t, kAuxvCap> gAuxv{};
size_t gAuxvLen = 0;
std::array<uint8_t, 65536> gOutBuf{};
std::array<char, 8192> gDents{};

std::atomic<int> gGen{0};       // the number of the dump being written
std::atomic<int> gActive{0};    // gGen while threads may answer, else 0
std::atomic<int> gRelease{0};   // set to gGen when the stacks are written: the threads go on
std::atomic_flag gBusy = ATOMIC_FLAG_INIT;
int gSig = 0;
bool gPrepared = false;

// what the system info stream says, read once in prepare()
uint32_t gCpuVendor[3] = {0, 0, 0}, gCpuVersion = 0, gCpuFeatures = 0, gCpuAmdExt = 0;
uint32_t gNumCpu = 1;
std::array<char, 400> gUname{};
uint32_t gOsMajor = 0, gOsMinor = 0, gOsBuild = 0;

int myTid() { return (int)syscall(SYS_gettid); }

size_t cLen(const char* s, size_t max) {   // the length of a C string, at most max
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

void sleepMs(int ms) {
    struct timespec ts{0, (long)ms * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

long nowMs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// read a whole small file with plain system calls
size_t readFile(const char* path, char* buf, size_t cap) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    size_t n = 0;
    while (n < cap) {
        const ssize_t r = read(fd, buf + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    return n;
}

// a short text built in a fixed array
struct Txt {
    std::array<char, 96> b{};
    size_t n = 0;
    void put(const char* s) { while (*s && n + 1 < b.size()) b[n++] = *s++; b[n] = 0; }
    void dec(uint64_t v) {
        std::array<char, 24> d{};
        int k = 0;
        do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (k && n + 1 < b.size()) b[n++] = d[--k];
        b[n] = 0;
    }
};

uint64_t hexNum(const char*& p, const char* e) {
    uint64_t v = 0;
    while (p < e) {
        const char c = *p;
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = v * 16 + (uint64_t)d;
        p++;
    }
    return v;
}

// ------------------------------------------------------------------ registers

void put64(std::array<uint8_t, kCtxSize>& c, size_t off, uint64_t v) { for (int i = 0; i < 8; i++) c[off + i] = (uint8_t)(v >> (8 * i)); }
void put32(std::array<uint8_t, kCtxSize>& c, size_t off, uint32_t v) { for (int i = 0; i < 4; i++) c[off + i] = (uint8_t)(v >> (8 * i)); }
void put16(std::array<uint8_t, kCtxSize>& c, size_t off, uint32_t v) { c[off] = (uint8_t)v; c[off + 1] = (uint8_t)(v >> 8); }

// a Linux ucontext_t into the AMD64 CONTEXT layout of a minidump
void fillContext(std::array<uint8_t, kCtxSize>& c, const ucontext_t* uc) {
    c.fill(0);
    const greg_t* g = uc->uc_mcontext.gregs;
    put32(c, 0x30, 0x0010000Fu);   // CONTEXT_AMD64 | CONTROL | INTEGER | SEGMENTS | FLOATING_POINT
    const uint64_t seg = (uint64_t)g[REG_CSGSFS];
    put16(c, 0x38, (uint32_t)(seg & 0xffff));                       // cs
    put16(c, 0x3e, (uint32_t)((seg >> 32) & 0xffff));               // fs
    put16(c, 0x40, (uint32_t)((seg >> 16) & 0xffff));               // gs
    const uint32_t ss = (uint32_t)((seg >> 48) & 0xffff);
    put16(c, 0x42, ss ? ss : 0x2b);                                 // ss (older kernels leave it out: the user-mode value)
    put32(c, 0x44, (uint32_t)g[REG_EFL]);
    const int order[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                           REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    for (int i = 0; i < 16; i++) put64(c, 0x78 + 8 * (size_t)i, (uint64_t)g[order[i]]);
    put64(c, 0xf8, (uint64_t)g[REG_RIP]);
    if (uc->uc_mcontext.fpregs) {   // the FXSAVE area: x87, SSE registers and MXCSR
        const auto* fp = reinterpret_cast<const uint8_t*>(uc->uc_mcontext.fpregs);
        std::copy_n(fp, 512, c.begin() + 0x100);
        put32(c, 0x34, uc->uc_mcontext.fpregs->mxcsr);
    }
}

// a thread answers here: its registers, then it waits until the writer has copied its stack
void onAsk(int, siginfo_t*, void* uc) {
    const int savedErrno = errno;
    const int gen = gActive.load(std::memory_order_acquire);
    if (gen != 0) {
        const int me = myTid();
        for (int i = 0; i < gNSlots; i++) {
            Slot& s = gSlots[i];
            if (s.tid.load(std::memory_order_relaxed) != me || s.state.load(std::memory_order_acquire) != 0) continue;
            const auto* u = static_cast<const ucontext_t*>(uc);
            fillContext(s.ctx, u);
            s.sp = (uint64_t)u->uc_mcontext.gregs[REG_RSP];
            s.state.store(1, std::memory_order_release);
            for (int k = 0; k < 10000 && gRelease.load(std::memory_order_acquire) != gen; k++) sleepMs(1);   // at most 10 s
            break;
        }
    }
    errno = savedErrno;
}

// ------------------------------------------------------------------ memory map, threads, modules

void readMaps() {
    gNMaps = 0;
    gMapsLen = gMapsText ? readFile("/proc/self/maps", gMapsText, kMapsCap) : 0;
    const char* p = gMapsText;
    const char* end = gMapsText + gMapsLen;
    while (p && p < end && gNMaps < kMaxMaps) {
        const char* eol = p;
        while (eol < end && *eol != '\n') eol++;
        MapEntry m{};
        const char* q = p;
        m.start = hexNum(q, eol);
        if (q < eol && *q == '-') q++;
        m.end = hexNum(q, eol);
        while (q < eol && *q == ' ') q++;
        m.readable = q < eol && *q == 'r';
        while (q < eol && *q != ' ') q++;   // permissions
        while (q < eol && *q == ' ') q++;
        m.offset = hexNum(q, eol);
        for (int field = 0; field < 2; field++) {   // device, inode
            while (q < eol && *q == ' ') q++;
            while (q < eol && *q != ' ') q++;
        }
        while (q < eol && *q == ' ') q++;
        m.pathOff = (uint32_t)(q - gMapsText);
        m.pathLen = (uint32_t)(eol - q);
        if (m.end > m.start) gMaps[gNMaps++] = m;
        p = eol + 1;
    }
}

const MapEntry* mapAt(uint64_t a) {
    int lo = 0, hi = gNMaps - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (a < gMaps[mid].start) hi = mid - 1;
        else if (a >= gMaps[mid].end) lo = mid + 1;
        else return &gMaps[mid];
    }
    return nullptr;
}

// Can this address be read? /proc/self/maps does not say for every page: newer glibc puts the guard page of a thread stack inside the
// stack's mapping (MADV_GUARD_INSTALL), so two stacks can look like one readable range. process_vm_readv on our own process fails with
// EFAULT there instead of crashing. Where the call is not allowed (some containers), the map is trusted.
bool pageReadable(uint64_t a) {
    char b = 0;
    struct iovec local{&b, 1};
    struct iovec remote{reinterpret_cast<void*>(a), 1};
    const int savedErrno = errno;
    const long r = syscall(SYS_process_vm_readv, (long)getpid(), &local, 1L, &remote, 1L, 0L);
    const bool fault = r != 1 && errno == EFAULT;
    errno = savedErrno;
    return !fault;
}

bool readableRange(uint64_t a, uint64_t n) {
    while (n > 0) {
        const MapEntry* m = mapAt(a);
        if (!m || !m->readable) return false;
        const uint64_t take = std::min(n, m->end - a);
        a += take;
        n -= take;
    }
    return true;
}

bool pathIs(const MapEntry& m, const char* prefix) {
    const size_t k = cLen(prefix, 64);
    return m.pathLen >= k && std::equal(prefix, prefix + k, gMapsText + m.pathOff);
}

bool samePath(const MapEntry& a, const MapEntry& b) {
    return a.pathLen == b.pathLen && std::equal(gMapsText + a.pathOff, gMapsText + a.pathOff + a.pathLen, gMapsText + b.pathOff);
}

uint64_t rd(uint64_t a, int bytes) {   // little-endian read of mapped memory (the caller checked the range)
    const auto* p = reinterpret_cast<const uint8_t*>(a);
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

// the GNU build id of an ELF file mapped at base, read from its program headers in memory
void readBuildId(Module& mod) {
    mod.idLen = 0;
    const uint64_t base = mod.base;
    if (!readableRange(base, 64) || rd(base, 4) != 0x464c457fu || rd(base + 4, 1) != 2) return;   // \x7fELF, 64-bit
    const uint64_t phoff = rd(base + 0x20, 8), phentsize = rd(base + 0x36, 2), phnum = rd(base + 0x38, 2);
    if (phentsize < 56 || phnum == 0 || phnum > 256 || !readableRange(base + phoff, phentsize * phnum)) return;
    uint64_t bias = base;
    for (uint64_t i = 0; i < phnum; i++) {   // the first PT_LOAD: load address = base - its page-aligned p_vaddr
        const uint64_t ph = base + phoff + i * phentsize;
        if (rd(ph, 4) == 1) { bias = base - (rd(ph + 0x10, 8) & ~0xfffull); break; }
    }
    for (uint64_t i = 0; i < phnum && mod.idLen == 0; i++) {
        const uint64_t ph = base + phoff + i * phentsize;
        if (rd(ph, 4) != 4) continue;   // PT_NOTE
        uint64_t n = bias + rd(ph + 0x10, 8);
        const uint64_t nEnd = n + rd(ph + 0x28, 8);
        if (nEnd < n || nEnd - n > (1u << 20) || !readableRange(n, nEnd - n)) continue;
        while (n + 12 <= nEnd) {
            const uint64_t namesz = rd(n, 4), descsz = rd(n + 4, 4), type = rd(n + 8, 4);
            const uint64_t name = n + 12, desc = name + ((namesz + 3) & ~3ull);
            const uint64_t next = desc + ((descsz + 3) & ~3ull);
            if (next > nEnd) break;
            if (type == 3 && namesz == 4 && rd(name, 4) == 0x00554e47u && descsz > 0 && descsz <= mod.id.size()) {   // NT_GNU_BUILD_ID, "GNU"
                std::copy_n(reinterpret_cast<const uint8_t*>(desc), (size_t)descsz, mod.id.begin());
                mod.idLen = (uint32_t)descsz;
                break;
            }
            n = next;
        }
    }
}

void findModules() {
    gNMods = 0;
    for (int i = 0; i < gNMaps && gNMods < kMaxModules; i++) {
        const MapEntry& m = gMaps[i];
        if (m.pathLen == 0 || gMapsText[m.pathOff] != '/' || m.offset != 0 || !m.readable) continue;
        if (pathIs(m, "/dev/") || pathIs(m, "/memfd:") || pathIs(m, "/SYSV")) continue;
        if (rd(m.start, 4) != 0x464c457fu) continue;   // not an ELF file (a mapped data file)
        Module mod{};
        mod.base = m.start;
        mod.end = m.end;
        mod.pathOff = m.pathOff;
        mod.pathLen = m.pathLen;
        for (int j = i + 1; j < gNMaps && samePath(gMaps[j], m); j++) mod.end = gMaps[j].end;   // the file's later segments
        readBuildId(mod);
        gMods[gNMods++] = mod;
    }
}

void listThreads() {
    gNSlots = 0;
    const int dfd = open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd < 0) return;
    for (;;) {
        const long n = syscall(SYS_getdents64, dfd, gDents.data(), gDents.size());
        if (n <= 0) break;
        for (long off = 0; off + 19 < n;) {
            const auto* rec = reinterpret_cast<const uint8_t*>(gDents.data()) + off;
            const uint32_t reclen = (uint32_t)rec[16] | ((uint32_t)rec[17] << 8);
            if (reclen == 0) break;
            const char* nm = reinterpret_cast<const char*>(rec + 19);
            int tid = 0;
            bool num = *nm != 0;
            for (const char* c = nm; *c; c++) { if (*c < '0' || *c > '9') { num = false; break; } tid = tid * 10 + (*c - '0'); }
            if (num && gNSlots < kMaxThreads) {
                Slot& s = gSlots[gNSlots++];
                s.tid.store(tid, std::memory_order_relaxed);
                s.state.store(0, std::memory_order_relaxed);
                s.sp = 0; s.lo = 0; s.len = 0;
                s.name.fill(0);
                Txt path;
                path.put("/proc/self/task/"); path.dec((uint64_t)tid); path.put("/comm");
                const size_t k = readFile(path.b.data(), s.name.data(), s.name.size() - 1);
                for (size_t c = 0; c < k; c++) if (s.name[c] == '\n') s.name[c] = 0;
            }
            off += reclen;
        }
    }
    close(dfd);
}

// the stack memory to keep for a thread: from just below its stack pointer up to the end of its stack mapping, capped
void pickStack(Slot& s, uint64_t& budget) {
    s.lo = s.len = 0;
    if (!s.sp || budget == 0) return;
    const MapEntry* m = mapAt(s.sp);
    if (!m || !m->readable) {   // a stack overflow: the pointer is in the guard page; keep the stack above it
        m = nullptr;
        for (int i = 0; i < gNMaps; i++) if (gMaps[i].start > s.sp && gMaps[i].start - s.sp <= 65536 && gMaps[i].readable) { m = &gMaps[i]; break; }
        if (!m) return;
    }
    const uint64_t lo = std::max(m->start, s.sp > 128 ? s.sp - 128 : s.sp);   // 128: the red zone under the stack pointer
    uint64_t hi = std::min(m->end, lo + std::min(kStackCap, budget));
    for (uint64_t p = lo & ~0xfffull; p < hi; p += 4096) {   // stop at the first page that cannot be read (a guard page)
        if (!pageReadable(std::max(p, lo))) { hi = std::max(p, lo); break; }
    }
    if (hi <= lo) return;
    s.lo = lo;
    s.len = hi - lo;
    budget -= s.len;
}

// ------------------------------------------------------------------ the file

// Every part is written twice: once to count (positions), then for real. Each pass records where a block lands, so the second pass
// can point at blocks that come later in the file.
struct W {
    int fd = -1;
    bool real = false;
    bool ok = true;
    uint64_t pos = 0;
    size_t n = 0;
    void flush() {
        size_t done = 0;
        while (real && ok && done < n) {
            const ssize_t r = ::write(fd, gOutBuf.data() + done, n - done);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) { ok = false; break; }
            done += (size_t)r;
        }
        n = 0;
    }
    void bytes(const void* p, size_t len) {
        pos += len;
        if (!real) return;
        const auto* b = static_cast<const uint8_t*>(p);
        if (len >= 4096) {   // a stack: straight from memory
            flush();
            size_t done = 0;
            while (ok && done < len) {
                const ssize_t r = ::write(fd, b + done, len - done);
                if (r < 0 && errno == EINTR) continue;
                if (r <= 0) { ok = false; break; }
                done += (size_t)r;
            }
            return;
        }
        while (len > 0) {
            if (n == gOutBuf.size()) flush();
            const size_t take = std::min(len, gOutBuf.size() - n);
            std::copy_n(b, take, gOutBuf.begin() + (ptrdiff_t)n);
            n += take; b += take; len -= take;
        }
    }
    void u8(uint32_t v) { const uint8_t x = (uint8_t)v; bytes(&x, 1); }
    void u16(uint32_t v) { u8(v & 255); u8((v >> 8) & 255); }
    void u32(uint32_t v) { u16(v & 0xffff); u16(v >> 16); }
    void u64(uint64_t v) { u32((uint32_t)v); u32((uint32_t)(v >> 32)); }
    void zero(size_t k) { while (k--) u8(0); }
    void align(uint64_t a) { while (pos % a) u8(0); }
};

enum : uint32_t {
    kThreadList = 3, kModuleList = 4, kMemoryList = 5, kException = 6, kSystemInfo = 7, kMiscInfo = 15, kThreadNames = 24,
    kLinuxProcStatus = 0x47670004, kLinuxAuxv = 0x47670008, kLinuxMaps = 0x47670009,
    kOnAirNotes = 0x4f4e0001,   // OnAir's own text: the threads that did not answer
};
constexpr int kStreams = 11;

struct Layout {
    uint32_t streamRva[kStreams] = {}, streamSize[kStreams] = {};
    uint32_t osString = 0;
    uint32_t threadName[kMaxThreads] = {}, ctx[kMaxThreads] = {}, stack[kMaxThreads] = {};
    uint32_t modName[kMaxModules] = {}, modCv[kMaxModules] = {};
};
Layout gL;

void resetLayout() {   // no Layout{} temporary: this also runs on the crash handler's small stack
    std::fill(std::begin(gL.streamRva), std::end(gL.streamRva), 0u);
    std::fill(std::begin(gL.streamSize), std::end(gL.streamSize), 0u);
    gL.osString = 0;
    for (uint32_t* a : {gL.threadName, gL.ctx, gL.stack}) std::fill(a, a + kMaxThreads, 0u);
    for (uint32_t* a : {gL.modName, gL.modCv}) std::fill(a, a + kMaxModules, 0u);
}

// one code point of UTF-8 text at s[i]; anything odd becomes '?'. Returns the bytes it took.
size_t nextCp(const char* s, size_t n, size_t i, uint32_t& cp) {
    const uint8_t c = (uint8_t)s[i];
    size_t k = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (k == 0 || i + k > n) { cp = c < 0x80 ? c : '?'; return 1; }
    cp = k == 1 ? c : k == 2 ? (c & 0x1fu) : k == 3 ? (c & 0x0fu) : (c & 0x07u);
    for (size_t j = 1; j < k; j++) cp = (cp << 6) | ((uint8_t)s[i + j] & 0x3fu);
    if (cp > 0x10ffff) cp = '?';
    return k;
}

// a MINIDUMP_STRING: byte length, UTF-16 text, a 0
void putString(W& w, const char* s, size_t n) {
    size_t units = 0;
    uint32_t cp = 0;
    for (size_t i = 0; i < n;) { i += nextCp(s, n, i, cp); units += cp >= 0x10000 ? 2 : 1; }
    w.u32((uint32_t)(units * 2));
    for (size_t i = 0; i < n;) {
        i += nextCp(s, n, i, cp);
        if (cp >= 0x10000) { cp -= 0x10000; w.u16(0xD800 + (cp >> 10)); w.u16(0xDC00 + (cp & 0x3ff)); }
        else w.u16(cp);
    }
    w.u16(0);
    w.align(4);
}

void beginStream(W& w, int k) { w.align(8); gL.streamRva[k] = (uint32_t)w.pos; }
void endStream(W& w, int k) { gL.streamSize[k] = (uint32_t)(w.pos - gL.streamRva[k]); }

// one full pass over the file
void emit(W& w, int crashTid, int sig, const siginfo_t* si) {
    const uint32_t types[kStreams] = {kSystemInfo, kMiscInfo, kThreadList, kThreadNames, kModuleList, kMemoryList, sig ? kException : 0u,   // 0: unused
                                      kLinuxMaps, kLinuxProcStatus, kLinuxAuxv, kOnAirNotes};
    // header
    w.u32(0x504d444d); w.u32(0xa793); w.u32(kStreams); w.u32(32); w.u32(0); w.u32((uint32_t)time(nullptr)); w.u64(0);
    for (int k = 0; k < kStreams; k++) { w.u32(types[k]); w.u32(gL.streamSize[k]); w.u32(gL.streamRva[k]); }

    int nThreads = 0;
    for (int i = 0; i < gNSlots; i++) if (gAnswered[i]) nThreads++;

    beginStream(w, 0);   // system info
    w.u16(9);            // AMD64
    w.u16((gCpuVersion >> 8) & 0xf);
    w.u16(((gCpuVersion >> 4) & 0xf) << 8 | (gCpuVersion & 0xf));
    w.u8(gNumCpu > 255 ? 255 : gNumCpu); w.u8(0);
    w.u32(gOsMajor); w.u32(gOsMinor); w.u32(gOsBuild);
    w.u32(0x8201);       // Linux (Breakpad's platform id)
    w.u32(gL.osString); w.u16(0); w.u16(0);
    for (uint32_t v : gCpuVendor) w.u32(v);
    w.u32(gCpuVersion); w.u32(gCpuFeatures); w.u32(gCpuAmdExt);
    gL.osString = (uint32_t)w.pos;
    putString(w, gUname.data(), cLen(gUname.data(), gUname.size()));
    endStream(w, 0);

    beginStream(w, 1);   // misc info: the process id
    w.u32(24); w.u32(1); w.u32((uint32_t)getpid()); w.u32(0); w.u32(0); w.u32(0);
    endStream(w, 1);

    beginStream(w, 2);   // threads
    w.u32((uint32_t)nThreads);
    for (int i = 0; i < gNSlots; i++) {
        if (!gAnswered[i]) continue;
        const Slot& s = gSlots[i];
        w.u32((uint32_t)s.tid.load(std::memory_order_relaxed)); w.u32(0); w.u32(0); w.u32(0); w.u64(0);
        w.u64(s.lo); w.u32((uint32_t)s.len); w.u32(s.len ? gL.stack[i] : 0);
        w.u32((uint32_t)kCtxSize); w.u32(gL.ctx[i]);
    }
    endStream(w, 2);

    beginStream(w, 3);   // thread names
    w.u32((uint32_t)gNSlots);
    for (int i = 0; i < gNSlots; i++) { w.u32((uint32_t)gSlots[i].tid.load(std::memory_order_relaxed)); w.u64(gL.threadName[i]); }
    endStream(w, 3);

    beginStream(w, 4);   // modules
    w.u32((uint32_t)gNMods);
    for (int j = 0; j < gNMods; j++) {
        const Module& m = gMods[j];
        w.u64(m.base); w.u32((uint32_t)std::min<uint64_t>(m.end - m.base, 0xffffffffu)); w.u32(0); w.u32(0); w.u32(gL.modName[j]);
        w.zero(52);                                            // VS_FIXEDFILEINFO
        w.u32(m.idLen ? 4 + m.idLen : 0); w.u32(m.idLen ? gL.modCv[j] : 0);   // CodeView record: 'BpEL' + build id
        w.u32(0); w.u32(0); w.u64(0); w.u64(0);
    }
    endStream(w, 4);

    beginStream(w, 5);   // memory: the stacks
    int nMem = 0;
    for (int i = 0; i < gNSlots; i++) if (gAnswered[i] && gSlots[i].len) nMem++;
    w.u32((uint32_t)nMem);
    for (int i = 0; i < gNSlots; i++) {
        if (!gAnswered[i] || !gSlots[i].len) continue;
        w.u64(gSlots[i].lo); w.u32((uint32_t)gSlots[i].len); w.u32(gL.stack[i]);
    }
    endStream(w, 5);

    beginStream(w, 6);   // exception: only after a crash (an empty stream otherwise)
    if (sig) {
        int ci = -1;
        for (int i = 0; i < gNSlots; i++) if (gAnswered[i] && gSlots[i].tid.load(std::memory_order_relaxed) == crashTid) ci = i;
        w.u32((uint32_t)crashTid); w.u32(0);
        w.u32((uint32_t)sig); w.u32(si ? (uint32_t)si->si_code : 0u); w.u64(0);
        w.u64(si ? (uint64_t)(uintptr_t)si->si_addr : 0); w.u32(0); w.u32(0);
        w.zero(15 * 8);
        w.u32(ci >= 0 ? (uint32_t)kCtxSize : 0u); w.u32(ci >= 0 ? gL.ctx[ci] : 0u);
    }
    endStream(w, 6);

    beginStream(w, 7); w.bytes(gMapsText, gMapsLen); endStream(w, 7);
    beginStream(w, 8); w.bytes(gStatus.data(), gStatusLen); endStream(w, 8);
    beginStream(w, 9); w.bytes(gAuxv.data(), gAuxvLen); endStream(w, 9);

    beginStream(w, 10);   // OnAir notes
    for (int i = 0; i < gNSlots; i++) {
        if (gAnswered[i]) continue;
        Txt t;
        t.put("no answer: thread "); t.dec((uint64_t)gSlots[i].tid.load(std::memory_order_relaxed)); t.put(" ");
        t.put(gSlots[i].name.data()); t.put("\n");
        w.bytes(t.b.data(), t.n);
    }
    endStream(w, 10);

    // the blocks the streams point at
    for (int i = 0; i < gNSlots; i++) {
        w.align(4);
        gL.threadName[i] = (uint32_t)w.pos;
        putString(w, gSlots[i].name.data(), cLen(gSlots[i].name.data(), gSlots[i].name.size()));
    }
    for (int j = 0; j < gNMods; j++) {
        w.align(4);
        gL.modName[j] = (uint32_t)w.pos;
        putString(w, gMapsText + gMods[j].pathOff, gMods[j].pathLen);
        if (gMods[j].idLen) {
            gL.modCv[j] = (uint32_t)w.pos;
            w.u32(0x4c457042);   // 'BpEL'
            w.bytes(gMods[j].id.data(), gMods[j].idLen);
        }
    }
    for (int i = 0; i < gNSlots; i++) {
        if (!gAnswered[i]) continue;
        w.align(16);
        gL.ctx[i] = (uint32_t)w.pos;
        w.bytes(gSlots[i].ctx.data(), kCtxSize);
    }
    for (int i = 0; i < gNSlots; i++) {
        if (!gAnswered[i] || !gSlots[i].len) continue;
        w.align(16);
        gL.stack[i] = (uint32_t)w.pos;
        w.bytes(reinterpret_cast<const void*>(gSlots[i].lo), (size_t)gSlots[i].len);
    }
    w.flush();
}

// the whole dump; self is the calling thread's registers
bool collect(int fd, const ucontext_t* self, int sig, const siginfo_t* si, int waitMs) {
    const int me = myTid();
    const int gen = gGen.fetch_add(1, std::memory_order_acq_rel) + 1;
    readMaps();
    gStatusLen = readFile("/proc/self/status", gStatus.data(), gStatus.size());
    gAuxvLen = readFile("/proc/self/auxv", reinterpret_cast<char*>(gAuxv.data()), gAuxv.size());
    listThreads();
    gRelease.store(0, std::memory_order_release);
    gActive.store(gen, std::memory_order_release);
    const pid_t pid = getpid();
    for (int i = 0; i < gNSlots; i++) {
        Slot& s = gSlots[i];
        const int tid = s.tid.load(std::memory_order_relaxed);
        if (tid == me) {
            fillContext(s.ctx, self);
            s.sp = (uint64_t)self->uc_mcontext.gregs[REG_RSP];
            s.state.store(1, std::memory_order_release);
        } else if (syscall(SYS_tgkill, pid, tid, gSig) != 0) s.state.store(2, std::memory_order_release);
    }
    const long until = nowMs() + waitMs;
    for (;;) {
        bool all = true;
        for (int i = 0; i < gNSlots; i++) if (gSlots[i].state.load(std::memory_order_acquire) == 0) { all = false; break; }
        if (all || nowMs() > until) break;
        sleepMs(1);
    }
    for (int i = 0; i < gNSlots; i++) gAnswered[i] = gSlots[i].state.load(std::memory_order_acquire) == 1;
    uint64_t budget = kStackTotal;
    for (int i = 0; i < gNSlots; i++) if (gAnswered[i] && gSlots[i].tid.load(std::memory_order_relaxed) == me) pickStack(gSlots[i], budget);   // the caller's first
    for (int i = 0; i < gNSlots; i++) if (gAnswered[i] && gSlots[i].tid.load(std::memory_order_relaxed) != me) pickStack(gSlots[i], budget);
    findModules();
    resetLayout();
    W count;
    emit(count, me, sig, si);   // positions
    W out;
    out.fd = fd;
    out.real = true;
    emit(out, me, sig, si);
    gActive.store(0, std::memory_order_release);
    gRelease.store(gen, std::memory_order_release);   // the threads go on
    return out.ok;
}

void parseRelease(const char* r) {   // "6.8.0-45-generic" -> 6, 8, 0
    uint32_t v[3] = {0, 0, 0};
    for (int k = 0; k < 3 && *r; k++) {
        while (*r >= '0' && *r <= '9') v[k] = v[k] * 10 + (uint32_t)(*r++ - '0');
        if (*r == '.') r++; else break;
    }
    gOsMajor = v[0]; gOsMinor = v[1]; gOsBuild = v[2];
}

}  // namespace

bool supported() { return true; }

void prepare() {
    if (gPrepared) return;
    gPrepared = true;
    void* m = mmap(nullptr, kMapsCap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    gMapsText = m == MAP_FAILED ? nullptr : static_cast<char*>(m);
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(0, &a, &b, &c, &d)) { gCpuVendor[0] = b; gCpuVendor[1] = d; gCpuVendor[2] = c; }
    if (__get_cpuid(1, &a, &b, &c, &d)) { gCpuVersion = a; gCpuFeatures = d; }
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) gCpuAmdExt = c;
    const long n = sysconf(_SC_NPROCESSORS_ONLN);
    gNumCpu = n > 0 ? (uint32_t)n : 1;
    struct utsname u{};
    if (uname(&u) == 0) {
        std::string s = std::string(u.sysname) + " " + u.release + " " + u.version + " " + u.machine;
        if (s.size() >= gUname.size()) s.resize(gUname.size() - 1);
        std::copy_n(s.begin(), s.size(), gUname.begin());
        gUname[s.size()] = 0;
        parseRelease(u.release);
    }
    gSig = SIGRTMAX - 3;
    struct sigaction sa{};
    sa.sa_sigaction = onAsk;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(gSig, &sa, nullptr);
}

bool write(const std::string& path, std::string& err) {
    prepare();
    if (!gMapsText) { err = "no memory for the dump"; return false; }
    if (gBusy.test_and_set(std::memory_order_acquire)) { err = "a dump is already being written"; return false; }
    const std::filesystem::path p(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    const int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) { gBusy.clear(std::memory_order_release); err = "cannot create " + path + ": " + std::strerror(errno); return false; }
    ucontext_t uc{};
    getcontext(&uc);
    const bool ok = collect(fd, &uc, 0, nullptr, kAnswerMs);
    close(fd);
    gBusy.clear(std::memory_order_release);
    if (!ok) err = "writing the dump failed (disk full?)";
    return ok;
}

bool writeFromSignal(int fd, int sig, const void* si, const void* uctx) {
    if (!gPrepared || !gMapsText || !uctx || fd < 0) return false;
    if (gBusy.test_and_set(std::memory_order_acquire)) return false;   // a dump was running when it crashed: no second one
    const bool ok = collect(fd, static_cast<const ucontext_t*>(uctx), sig, static_cast<const siginfo_t*>(si), kAnswerMsCrash);
    gBusy.clear(std::memory_order_release);
    return ok;
}

#elif defined(DECT2_DUMP_WINDOWS)

bool supported() { return true; }
void prepare() {}

bool write(const std::string& path, std::string& err) {
    const std::filesystem::path p(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
    HANDLE h = CreateFileW(p.wstring().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot create " + path; return false; }
    BOOL ok = FALSE;
    // from a thread of its own: a thread cannot take a reliable picture of its own stack, and the caller then shows as a normal thread
    std::thread t([&] {
        ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
                               (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo), nullptr, nullptr, nullptr);
    });
    t.join();
    CloseHandle(h);
    if (!ok) err = "MiniDumpWriteDump failed";
    return ok != FALSE;
}

#else

bool supported() { return false; }
void prepare() {}
bool write(const std::string&, std::string& err) {
    err = "a stack dump is not available on this system yet (Linux x86-64 and Windows only)";
    return false;
}
#ifndef _WIN32
bool writeFromSignal(int, int, const void*, const void*) { return false; }
#endif

#endif

}  // namespace dect2::dump
