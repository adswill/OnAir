// Debug dump (core/src/engine/debug_dump.cpp): a dump of a running process with named threads is written and read back. The file must
// be a minidump with every thread (registers and the stack around its stack pointer), the thread names, and the test program itself in
// the module list with the build id of the file on disk, so a debugger finds the right symbols. Linux x86-64 and Windows write dumps;
// elsewhere the test checks that write() says it cannot.
#include "dect2/engine/debug_dump.h"
#include "dect2/common/thread_name.h"
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
static int failures = 0;

static void fail(const char* fmt, ...) {
    std::string b(512, '\0');
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b.data(), b.size(), fmt, ap);
    va_end(ap);
    std::printf("FAIL: %s\n", b.c_str());
    failures++;
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Rd {
    const std::string& d;
    bool ok = true;
    uint64_t get(size_t off, int n) {
        if (off + (size_t)n > d.size()) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < n; i++) v |= (uint64_t)(uint8_t)d[off + (size_t)i] << (8 * i);
        return v;
    }
    std::string str(size_t rva) {   // MINIDUMP_STRING, ASCII part only
        const uint64_t len = get(rva, 4);
        std::string s;
        for (uint64_t i = 0; i < len; i += 2) s.push_back((char)get(rva + 4 + i, 2));
        return s;
    }
};

#if defined(__linux__) && defined(__x86_64__)
// the GNU build id of an ELF file, read from its section of notes on disk
static std::string fileBuildId(const std::string& elf) {
    std::string d = elf;
    Rd r{d};
    if (r.get(0, 4) != 0x464c457fu) return "";
    const uint64_t shoff = r.get(0x28, 8), shentsize = r.get(0x3a, 2), shnum = r.get(0x3c, 2);
    for (uint64_t i = 0; i < shnum; i++) {
        const uint64_t sh = shoff + i * shentsize;
        if (r.get(sh + 4, 4) != 7) continue;   // SHT_NOTE
        uint64_t n = r.get(sh + 0x18, 8);
        const uint64_t end = n + r.get(sh + 0x20, 8);
        while (n + 12 <= end) {
            const uint64_t namesz = r.get(n, 4), descsz = r.get(n + 4, 4), type = r.get(n + 8, 4);
            const uint64_t desc = n + 12 + ((namesz + 3) & ~3ull);
            if (type == 3 && namesz == 4) return d.substr(desc, descsz);
            n = desc + ((descsz + 3) & ~3ull);
        }
    }
    return "";
}
#endif

int main() {
    const fs::path dir = fs::temp_directory_path() / ("onair_dump_test_" + std::to_string((long long)std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const fs::path dmp = dir / "dump.dmp";

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    static const char* const kNames[3] = {"onair-dumptest0", "onair-dumptest1", "onair-dumptest2"};
    for (int i = 0; i < 3; i++)
        threads.push_back(dect2::namedThread(kNames[i], [&stop] {
            while (!stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    std::string err;
    const bool ok = dect2::dump::write(dmp.string(), err);
    stop = true;
    for (auto& t : threads) t.join();

    if (!dect2::dump::supported()) {
        if (ok || err.empty()) fail("an unsupported system must say why it wrote no dump");
        std::printf("debug dump: not supported here (%s)\n", err.c_str());
        fs::remove_all(dir);
        return failures ? 1 : 0;
    }
    if (!ok) { fail("write() failed: %s", err.c_str()); fs::remove_all(dir); return 1; }

    if (const char* keep = std::getenv("ONAIR_DUMP_TEST_KEEP")) {   // a copy for trying the analysis script (tools/debug_dump)
        std::error_code ec;
        fs::copy_file(dmp, fs::path(keep), fs::copy_options::overwrite_existing, ec);
    }
    const std::string d = slurp(dmp);
    Rd r{d};
    if (r.get(0, 4) != 0x504d444du) fail("no MDMP signature");
    const uint64_t nStreams = r.get(8, 4), dirRva = r.get(12, 4);
    uint64_t threadList = 0, threadNames = 0, moduleList = 0, memoryList = 0;
    for (uint64_t k = 0; k < nStreams; k++) {
        const uint64_t e = dirRva + 12 * k, type = r.get(e, 4), rva = r.get(e + 8, 4);
        if (type == 3) threadList = rva;
        if (type == 24) threadNames = rva;
        if (type == 4) moduleList = rva;
        if (type == 5) memoryList = rva;
    }
    if (!threadList || !moduleList) fail("thread list or module list missing");
    if (!memoryList) fail("memory list missing");

    // threads: at least ours and the three helpers, each with registers and a stack around its stack pointer
    const uint64_t nThreads = r.get(threadList, 4);
    if (nThreads < 4) fail("only %llu threads in the dump", (unsigned long long)nThreads);
    for (uint64_t i = 0; i < nThreads; i++) {
        const uint64_t t = threadList + 4 + 48 * i;
        const uint64_t tid = r.get(t, 4), stackStart = r.get(t + 24, 8), stackLen = r.get(t + 32, 4), stackRva = r.get(t + 36, 4);
        const uint64_t ctxLen = r.get(t + 40, 4), ctxRva = r.get(t + 44, 4);
        if (ctxLen == 0 || ctxRva + ctxLen > d.size()) { fail("thread %llu has no registers", (unsigned long long)tid); continue; }
        if (stackLen == 0 || stackRva + stackLen > d.size()) { fail("thread %llu has no stack", (unsigned long long)tid); continue; }
#if defined(__x86_64__) || defined(_M_X64)
        const uint64_t sp = r.get(ctxRva + 0x98, 8), ip = r.get(ctxRva + 0xf8, 8);
        if (sp < stackStart || sp > stackStart + stackLen) fail("thread %llu: the stack pointer %llx is outside its stack %llx+%llx",
            (unsigned long long)tid, (unsigned long long)sp, (unsigned long long)stackStart, (unsigned long long)stackLen);
        if (ip == 0) fail("thread %llu: no instruction pointer", (unsigned long long)tid);
#endif
    }

    // thread names (Linux writes them; Windows dumps have them only on newer systems)
#if defined(__linux__)
    int named = 0;
    if (!threadNames) fail("thread names missing");
    else {
        const uint64_t n = r.get(threadNames, 4);
        for (uint64_t i = 0; i < n; i++) {
            const std::string name = r.str(r.get(threadNames + 4 + 12 * i + 4, 8));
            if (name.rfind("onair-dumptest", 0) == 0) named++;
        }
    }
    if (named != 3) fail("found %d of the 3 named threads", named);
#endif

    // modules: the test program itself, with the build id of the file
    const uint64_t nMods = r.get(moduleList, 4);
    bool self = false;
#if defined(__linux__) && defined(__x86_64__)
    const std::string exe = fs::read_symlink("/proc/self/exe").string();
    const std::string want = fileBuildId(slurp(exe));
    if (want.empty()) fail("the test program has no build id (link with --build-id)");
    for (uint64_t j = 0; j < nMods; j++) {
        const uint64_t m = moduleList + 4 + 108 * j;
        if (r.str(r.get(m + 20, 4)) != exe) continue;
        self = true;
        const uint64_t cvLen = r.get(m + 76, 4), cvRva = r.get(m + 80, 4);
        if (cvLen < 5 || r.get(cvRva, 4) != 0x4c457042u) { fail("the test program has no ELF build id record"); break; }
        if (d.substr(cvRva + 4, cvLen - 4) != want) fail("the build id in the dump differs from the file's");
    }
#else
    self = nMods > 0;
#endif
    if (!self) fail("the test program is not in the module list");
    if (!r.ok) fail("the dump is cut short");

    std::printf("debug dump: %llu threads, %llu modules, %zu bytes\n", (unsigned long long)nThreads, (unsigned long long)nMods, d.size());
    fs::remove_all(dir);
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
