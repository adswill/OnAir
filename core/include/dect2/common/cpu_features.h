// The instruction set extensions of this processor, for the debug report: what the CPU has, and for the wide registers (AVX, AVX-512, AMX)
// whether the operating system turned them on (XGETBV): a CPU flag alone does not show a hypervisor or OS that switched them off.
// decodeX86() is pure (fixed register values in, a list out) so the test can feed it known processors; readX86() asks this CPU.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#if (defined(__x86_64__) || defined(__i386__)) && defined(__GNUC__)
#include <cpuid.h>
#define DECT2_CPUID 1
#endif

namespace dect2::cpu {

struct X86Regs {
    uint32_t maxLeaf = 0, maxExt = 0;
    uint32_t l1ecx = 0, l1edx = 0;                    // leaf 1
    uint32_t l7ebx = 0, l7ecx = 0, l7edx = 0;         // leaf 7, sub-leaf 0
    uint32_t l71eax = 0;                              // leaf 7, sub-leaf 1
    uint32_t e1ecx = 0, e1edx = 0;                    // leaf 0x80000001
    uint64_t xcr0 = 0;                                // XGETBV(0), 0 when OSXSAVE is off
    bool hypervisor() const { return (l1ecx >> 31) & 1; }
};

struct Feature {
    std::string name;
    bool cpu = false;   // the processor has it
    bool os = true;     // the operating system lets programs use it (only AVX, AVX-512 and AMX can be off)
};

inline std::vector<Feature> decodeX86(const X86Regs& r) {
    const bool osxsave = (r.l1ecx >> 27) & 1;
    const bool osAvx = osxsave && (r.xcr0 & 0x6) == 0x6;           // SSE and AVX state
    const bool osAvx512 = osAvx && (r.xcr0 & 0xe0) == 0xe0;         // opmask, ZMM_Hi256, Hi16_ZMM
    const bool osAmx = osxsave && (r.xcr0 & 0x60000) == 0x60000;    // XTILECFG, XTILEDATA
    struct Bit { const char* name; uint32_t reg; int bit; int need; };   // need: 0 none, 1 AVX, 2 AVX-512, 3 AMX
    const bool l7 = r.maxLeaf >= 7, e1 = r.maxExt >= 0x80000001u;
    const uint32_t b7 = l7 ? r.l7ebx : 0, c7 = l7 ? r.l7ecx : 0, d7 = l7 ? r.l7edx : 0, a71 = l7 ? r.l71eax : 0, ce = e1 ? r.e1ecx : 0;
    const Bit bits[] = {
        {"MMX", r.l1edx, 23, 0}, {"SSE", r.l1edx, 25, 0}, {"SSE2", r.l1edx, 26, 0}, {"SSE3", r.l1ecx, 0, 0},
        {"SSSE3", r.l1ecx, 9, 0}, {"SSE4.1", r.l1ecx, 19, 0}, {"SSE4.2", r.l1ecx, 20, 0}, {"SSE4a", ce, 6, 0},
        {"POPCNT", r.l1ecx, 23, 0}, {"LZCNT", ce, 5, 0}, {"MOVBE", r.l1ecx, 22, 0}, {"PCLMULQDQ", r.l1ecx, 1, 0},
        {"AES-NI", r.l1ecx, 25, 0}, {"SHA", b7, 29, 0}, {"RDRAND", r.l1ecx, 30, 0}, {"RDSEED", b7, 18, 0},
        {"BMI1", b7, 3, 0}, {"BMI2", b7, 8, 0}, {"ADX", b7, 19, 0}, {"GFNI", c7, 8, 0},
        {"AVX", r.l1ecx, 28, 1}, {"F16C", r.l1ecx, 29, 1}, {"FMA", r.l1ecx, 12, 1}, {"FMA4", ce, 16, 1},
        {"AVX2", b7, 5, 1}, {"AVX-VNNI", a71, 4, 1}, {"VAES", c7, 9, 1}, {"VPCLMULQDQ", c7, 10, 1},
        {"AVX-512F", b7, 16, 2}, {"AVX-512CD", b7, 28, 2}, {"AVX-512BW", b7, 30, 2}, {"AVX-512DQ", b7, 17, 2},
        {"AVX-512VL", b7, 31, 2}, {"AVX-512IFMA", b7, 21, 2}, {"AVX-512VBMI", c7, 1, 2}, {"AVX-512VBMI2", c7, 6, 2},
        {"AVX-512VNNI", c7, 11, 2}, {"AVX-512BITALG", c7, 12, 2}, {"AVX-512VPOPCNTDQ", c7, 14, 2},
        {"AVX-512BF16", a71, 5, 2}, {"AVX-512FP16", d7, 23, 2},
        {"AMX-TILE", d7, 24, 3}, {"AMX-INT8", d7, 25, 3}, {"AMX-BF16", d7, 22, 3},
    };
    std::vector<Feature> out;
    for (const Bit& b : bits) {
        Feature f;
        f.name = b.name;
        f.cpu = (b.reg >> b.bit) & 1;
        f.os = b.need == 0 || (b.need == 1 && osAvx) || (b.need == 2 && osAvx512) || (b.need == 3 && osAmx);
        out.push_back(f);
    }
    return out;
}

// "SSE2 AVX2 AVX-512F(cpu only) ...": the features the CPU has; "(cpu only)" where the OS has them switched off
inline std::string featureLine(const std::vector<Feature>& fs) {
    std::string s;
    for (const Feature& f : fs) {
        if (!f.cpu) continue;
        if (!s.empty()) s += ' ';
        s += f.name;
        if (!f.os) s += "(cpu only)";
    }
    return s.empty() ? std::string("none") : s;
}

#ifdef DECT2_CPUID
inline X86Regs readX86() {
    X86Regs r;
    unsigned a = 0, b = 0, c = 0, d = 0;
    r.maxLeaf = __get_cpuid_max(0, nullptr);
    r.maxExt = __get_cpuid_max(0x80000000u, nullptr);
    if (r.maxLeaf >= 1 && __get_cpuid(1, &a, &b, &c, &d)) { r.l1ecx = c; r.l1edx = d; }
    if (r.maxLeaf >= 7) {
        __cpuid_count(7, 0, a, b, c, d);
        r.l7ebx = b; r.l7ecx = c; r.l7edx = d;
        if (a >= 1) { __cpuid_count(7, 1, a, b, c, d); r.l71eax = a; }
    }
    if (r.maxExt >= 0x80000001u && __get_cpuid(0x80000001u, &a, &b, &c, &d)) { r.e1ecx = c; r.e1edx = d; }
    if ((r.l1ecx >> 27) & 1) {   // OSXSAVE: XGETBV is allowed
        uint32_t lo = 0, hi = 0;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        r.xcr0 = ((uint64_t)hi << 32) | lo;
    }
    return r;
}
#endif

}  // namespace dect2::cpu
