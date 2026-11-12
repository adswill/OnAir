// The parts of the debug report that need no window: the processor feature list (cpu_features.h) from known register values, and the
// 1 MB log limit (log_tail.h).
#include "dect2/common/cpu_features.h"
#include "dect2/common/log_tail.h"
#include <cstdarg>
#include <cstdio>
#include <string>

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

static bool has(const std::vector<dect2::cpu::Feature>& fs, const char* name, bool cpu, bool os) {
    for (const auto& f : fs) if (f.name == name) return f.cpu == cpu && (!cpu || f.os == os);
    return false;
}

int main() {
    using namespace dect2::cpu;
    {   // a Skylake-X like processor with an OS that saves AVX-512 state
        X86Regs r;
        r.maxLeaf = 0x16; r.maxExt = 0x80000008u;
        r.l1ecx = (1u << 0) | (1u << 9) | (1u << 12) | (1u << 19) | (1u << 20) | (1u << 23) | (1u << 27) | (1u << 28) | (1u << 29);
        r.l1edx = (1u << 23) | (1u << 25) | (1u << 26);
        r.l7ebx = (1u << 3) | (1u << 5) | (1u << 8) | (1u << 16) | (1u << 17) | (1u << 28) | (1u << 30) | (1u << 31);
        r.xcr0 = 0xe7;
        const auto f = decodeX86(r);
        if (!has(f, "AVX2", true, true)) fail("AVX2 with OS support");
        if (!has(f, "AVX-512F", true, true)) fail("AVX-512F with OS support");
        if (!has(f, "FMA", true, true)) fail("FMA");
        if (!has(f, "AMX-TILE", false, false)) fail("no AMX");
        const std::string line = featureLine(f);
        if (line.find("AVX-512BW") == std::string::npos || line.find("cpu only") != std::string::npos) fail("feature line: %s", line.c_str());
    }
    {   // the same processor, but the OS (or a hypervisor) does not save the AVX-512 registers
        X86Regs r;
        r.maxLeaf = 0x16;
        r.l1ecx = (1u << 27) | (1u << 28);
        r.l7ebx = (1u << 5) | (1u << 16);
        r.xcr0 = 0x7;
        const auto f = decodeX86(r);
        if (!has(f, "AVX2", true, true)) fail("AVX2 usable with xcr0=7");
        if (!has(f, "AVX-512F", true, false)) fail("AVX-512F must be cpu only with xcr0=7");
        if (featureLine(f).find("AVX-512F(cpu only)") == std::string::npos) fail("cpu only mark missing: %s", featureLine(f).c_str());
    }
    {   // AVX with OSXSAVE off: not usable
        X86Regs r;
        r.maxLeaf = 1;
        r.l1ecx = 1u << 28;
        if (!has(decodeX86(r), "AVX", true, false)) fail("AVX without OSXSAVE");
    }
    {   // a maximum leaf below 7: leaf 7 bits are not read even if set
        X86Regs r;
        r.maxLeaf = 5;
        r.l7ebx = 1u << 5;
        if (!has(decodeX86(r), "AVX2", false, false)) fail("leaf 7 used beyond the maximum leaf");
    }
#ifdef DECT2_CPUID
    if (!has(decodeX86(readX86()), "SSE2", true, true)) fail("this x86-64 machine must report SSE2");
#endif

    {   // the log limit
        std::string log;
        for (int i = 0; i < 40000; i++) log += "line " + std::to_string(i) + " of the test log, with some words to make it longer\n";   // ~3 MB
        const std::string t = dect2::logTail(log, dect2::kReportLogBytes);
        if (t.size() > dect2::kReportLogBytes) fail("tail is %zu bytes", t.size());
        if (t.size() < dect2::kReportLogBytes - 200) fail("tail wastes room: %zu bytes", t.size());
        if (t.rfind("[", 0) != 0 || t.find(" earlier lines left out]\n") == std::string::npos) fail("no left-out line");
        const size_t first = t.find('\n') + 1;
        if (t.compare(first, 5, "line ") != 0) fail("the tail does not start at a whole line");
        if (t.size() < 20 || t.compare(t.size() - 20, 20, log, log.size() - 20, 20) != 0) fail("the newest line is not last");
        if (dect2::logTail("short\n") != "short\n") fail("a short log must stay as it is");
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
