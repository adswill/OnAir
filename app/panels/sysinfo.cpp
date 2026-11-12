// This computer for the debug report (sysinfo.h). Every part says "unknown" rather than fail: a report from an odd system is still useful.
#include "sysinfo.h"
#include "../app.h"
#include "../gfx/gfx.h"
#include "dect2/common/cpu_features.h"
#include "dect2/dsp/gpu_ldpc.h"
#include "dect2/dsp/simd.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/auxv.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "?"
#endif

namespace {

std::string fmt(const char* f, ...) {
    std::string b(512, '\0');
    va_list ap;
    va_start(ap, f);
    const int n = std::vsnprintf(b.data(), b.size(), f, ap);
    va_end(ap);
    b.resize(n < 0 ? 0 : std::min<size_t>((size_t)n, b.size() - 1));
    return b;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

[[maybe_unused]] std::string readText(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return trim(std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()));
}

// "key: value" lines of a /proc file
[[maybe_unused]] std::string procField(const std::string& path, const std::string& key) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (line.compare(0, key.size(), key) != 0) continue;
        const size_t c = line.find(':');
        if (c != std::string::npos) return trim(line.substr(c + 1));
    }
    return "";
}

#if defined(__APPLE__)
std::string sysctlText(const char* name) {
    size_t n = 0;
    if (sysctlbyname(name, nullptr, &n, nullptr, 0) != 0 || n == 0) return "";
    std::string s(n, '\0');
    if (sysctlbyname(name, s.data(), &n, nullptr, 0) != 0) return "";
    return trim(std::string(s.c_str()));
}
long long sysctlNum(const char* name) {
    long long v = 0;
    size_t n = sizeof v;
    if (sysctlbyname(name, &v, &n, nullptr, 0) != 0) return -1;
    return v;
}
#endif

std::string mib(double bytes) { return fmt("%.0f MiB", bytes / 1048576.0); }

std::string archName() {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86-64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "ARM64";
#elif defined(__arm__)
    return "ARM";
#else
    return "other";
#endif
}

std::string compilerName() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#else
    return "unknown";
#endif
}

// the SIMD line: x86 from CPUID (with the OS check), ARM from the system
std::string simdLine() {
#ifdef DECT2_CPUID
    return dect2::cpu::featureLine(dect2::cpu::decodeX86(dect2::cpu::readX86()));
#elif defined(__linux__) && defined(__aarch64__)
    const unsigned long h = getauxval(AT_HWCAP), h2 = getauxval(AT_HWCAP2);
    struct B { const char* n; unsigned long w; int bit; };
    const B bits[] = {{"FP", h, 0}, {"ASIMD(NEON)", h, 1}, {"AES", h, 3}, {"PMULL", h, 4}, {"SHA1", h, 5}, {"SHA2", h, 6},
                      {"CRC32", h, 7}, {"ATOMICS", h, 8}, {"FPHP", h, 9}, {"ASIMDHP", h, 10}, {"ASIMDRDM", h, 12},
                      {"ASIMDDP(dotprod)", h, 20}, {"SVE", h, 22}, {"SVE2", h2, 1}, {"I8MM", h2, 13}, {"BF16", h2, 14}};
    std::string s;
    for (const B& b : bits) if ((b.w >> b.bit) & 1) s += std::string(s.empty() ? "" : " ") + b.n;
    return s.empty() ? std::string("none") : s;
#elif defined(__APPLE__) && defined(__aarch64__)
    std::string s = "NEON";
    for (const char* f : {"FEAT_DotProd", "FEAT_I8MM", "FEAT_BF16", "FEAT_FP16", "FEAT_SME", "FEAT_SME2"})
        if (sysctlNum((std::string("hw.optional.arm.") + f).c_str()) == 1) s += std::string(" ") + f;
    return s;
#else
    return "unknown";
#endif
}

std::string cpuDetails() {
    std::string s;
#ifdef DECT2_CPUID
    unsigned a = 0, b = 0, c = 0, d = 0;
    std::string vendor;
    if (__get_cpuid(0, &a, &b, &c, &d)) {
        for (unsigned r : {b, d, c}) for (int i = 0; i < 4; i++) vendor.push_back((char)((r >> (8 * i)) & 0xff));
    }
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        const unsigned fam = ((a >> 8) & 0xf) + (((a >> 8) & 0xf) == 0xf ? ((a >> 20) & 0xff) : 0);
        const unsigned model = ((a >> 4) & 0xf) | ((((a >> 8) & 0xf) == 6 || ((a >> 8) & 0xf) == 0xf) ? ((a >> 16) & 0xf) << 4 : 0);
        s += fmt("  Vendor: %s, family %u, model %u, stepping %u\n", vendor.c_str(), fam, model, a & 0xf);
    }
    s += std::string("  Running in a virtual machine: ") + (dect2::cpu::readX86().hypervisor() ? "yes (hypervisor bit set)" : "no") + "\n";
#endif
#if defined(__linux__)
    const std::string mc = procField("/proc/cpuinfo", "microcode");
    if (!mc.empty()) s += "  Microcode: " + mc + "\n";
    const std::string maxKhz = readText("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
    if (!maxKhz.empty()) s += fmt("  Maximum clock: %.0f MHz\n", std::atof(maxKhz.c_str()) / 1000.0);
    const std::string gov = readText("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (!gov.empty()) s += "  Frequency governor: " + gov + "\n";
    std::string caches;
    for (int i = 0; i < 8; i++) {
        const std::string p = "/sys/devices/system/cpu/cpu0/cache/index" + std::to_string(i) + "/";
        const std::string lvl = readText(p + "level"), type = readText(p + "type"), size = readText(p + "size");
        if (lvl.empty()) break;
        caches += (caches.empty() ? "" : ", ") + std::string("L") + lvl + (type == "Data" ? "d" : type == "Instruction" ? "i" : "") + " " + size;
    }
    if (!caches.empty()) s += "  Caches (per core or shared): " + caches + "\n";
#elif defined(__APPLE__)
    const long long freq = sysctlNum("hw.cpufrequency_max");
    if (freq > 0) s += fmt("  Maximum clock: %lld MHz\n", freq / 1000000);
    const long long l2 = sysctlNum("hw.l2cachesize"), l3 = sysctlNum("hw.l3cachesize");
    if (l2 > 0) s += fmt("  Caches: L2 %lld KiB", l2 / 1024) + (l3 > 0 ? fmt(", L3 %lld KiB", l3 / 1024) : std::string()) + "\n";
    const long long p = sysctlNum("hw.perflevel0.physicalcpu"), e = sysctlNum("hw.perflevel1.physicalcpu");
    if (p > 0) s += fmt("  Cores: %lld performance, %lld efficiency\n", p, e > 0 ? e : 0);
#elif defined(_WIN32)
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    s += fmt("  Processors (Windows): %lu\n", (unsigned long)si.dwNumberOfProcessors);
#endif
    return s;
}

std::string memoryLine() {
#if defined(__linux__)
    return "  Memory: " + procField("/proc/meminfo", "MemTotal") + " total, " + procField("/proc/meminfo", "MemAvailable") + " available, swap " +
           procField("/proc/meminfo", "SwapTotal") + " (" + procField("/proc/meminfo", "SwapFree") + " free)\n" +
           "  OnAir process: " + procField("/proc/self/status", "VmRSS") + " resident, " + procField("/proc/self/status", "Threads") + " threads\n";
#elif defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof m;
    if (!GlobalMemoryStatusEx(&m)) return "  Memory: unknown\n";
    return "  Memory: " + mib((double)m.ullTotalPhys) + " total, " + mib((double)m.ullAvailPhys) + " available, page file " + mib((double)m.ullTotalPageFile) + "\n";
#elif defined(__APPLE__)
    const long long b = sysctlNum("hw.memsize");
    return b > 0 ? "  Memory: " + mib((double)b) + " total\n" : std::string("  Memory: unknown\n");
#else
    return "  Memory: unknown\n";
#endif
}

std::string glString(unsigned id) {
    typedef const unsigned char* (*GetString)(unsigned);
    if (!glfwGetCurrentContext()) return "";
    auto gs = (GetString)glfwGetProcAddress("glGetString");
    const unsigned char* r = gs ? gs(id) : nullptr;
    return r ? std::string((const char*)r) : std::string();
}

std::string graphicsDetails() {
    std::string s;
    const std::string vendor = glString(0x1F00), renderer = glString(0x1F01), version = glString(0x1F02), glsl = glString(0x8B8C);
    if (!renderer.empty()) {
        s += "  OpenGL vendor: " + vendor + "\n  OpenGL renderer: " + renderer + "\n  OpenGL version: " + version + "\n  GLSL version: " + glsl + "\n";
    } else {
#ifdef __APPLE__
        s += "  Drawing: Metal\n";
#else
        s += "  OpenGL: no context\n";
#endif
    }
    s += std::string("  Drawing level: ") + gfx::levelName() + "\n";
    const dect2::GpuLdpc& g = dect2::GpuLdpc::instance();
    s += std::string("  GPU decoder: ") + (g.available() ? g.deviceName() : "not available");
#ifdef _WIN32
    if (g.available()) s += fmt(" (%zu MiB video memory)", g.dedicatedMemoryMb());
#endif
    s += "\n";
#if defined(__linux__)
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
        const std::string n = e.path().filename().string();
        if (n.rfind("card", 0) != 0 || n.find('-') != std::string::npos) continue;
        const std::filesystem::path dev = e.path() / "device";
        std::error_code e2;
        const std::string driver = std::filesystem::read_symlink(dev / "driver", e2).filename().string();
        s += "  " + n + ": PCI " + readText((dev / "vendor").string()) + ":" + readText((dev / "device").string()) + ", driver " + (driver.empty() ? "?" : driver) + "\n";
    }
#endif
    int count = 0;
    GLFWmonitor** mons = glfwGetMonitors(&count);
    for (int i = 0; i < count && mons; i++) {
        const GLFWvidmode* m = glfwGetVideoMode(mons[i]);
        float sx = 1, sy = 1;
        glfwGetMonitorContentScale(mons[i], &sx, &sy);
        const char* name = glfwGetMonitorName(mons[i]);
        s += fmt("  Screen %d: %s, %dx%d at %d Hz, scale %.2f\n", i + 1, name ? name : "?", m ? m->width : 0, m ? m->height : 0, m ? m->refreshRate : 0, sx);
    }
    return s;
}

std::string osDetails() {
    std::string s;
#ifdef _WIN32
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    if (nt && GetProcAddress(nt, "wine_get_version")) s += "  Running under Wine\n";
#endif
#if defined(__linux__)
    const std::string session = getenv("XDG_SESSION_TYPE") ? getenv("XDG_SESSION_TYPE") : "";
    const std::string desktop = getenv("XDG_CURRENT_DESKTOP") ? getenv("XDG_CURRENT_DESKTOP") : "";
    if (!session.empty() || !desktop.empty()) s += "  Session: " + (session.empty() ? std::string("?") : session) + ", desktop " + (desktop.empty() ? std::string("?") : desktop) + "\n";
    const std::string glibc = readText("/proc/sys/kernel/osrelease");
    if (!glibc.empty()) s += "  Kernel: " + glibc + "\n";
#endif
    return s;
}

}  // namespace

std::string systemInfoSummary() {
    std::string os, cpu, gpu;
    systemInfoText(os, cpu, gpu);
    return std::string("OnAir ") + ONAIR_VERSION + " on " + os + ", " + cpu + ", " + gpu + ", SIMD: " + simdLine();
}

std::string systemInfoReport(App& a) {
    (void)a;
    std::string os, cpu, gpu;
    systemInfoText(os, cpu, gpu);
    std::string s = "OnAir system information\n\n";

    s += "OnAir\n";
    s += std::string("  Version: ") + ONAIR_VERSION + "\n";
    s += "  Architecture: " + archName() + "\n";
    s += "  Compiler: " + compilerName() + "\n";
    s += "  Built for: ";
#if defined(DECT2_NO_SIMD)
    s += "portable (no SIMD)";
#elif defined(__AVX2__)
    s += "AVX2 and FMA (the whole program)";
#elif defined(__x86_64__) || defined(_M_X64)
    s += "SSE2, with AVX2 paths chosen at run time";
#elif defined(__ARM_NEON) || defined(__aarch64__)
    s += "NEON";
#else
    s += "generic";
#endif
    s += "\n";
#if defined(__x86_64__) || defined(_M_X64)
    s += std::string("  AVX2 paths in use: ") + (dect2::cpuHasAvx2() ? "yes" : "no") + "\n";
#endif
    s += "\n";

    s += "Operating system\n";
    s += "  " + os + "\n" + osDetails() + "\n";

    s += "Processor\n";
    s += "  " + cpu + "\n";
    s += "  Architecture: " + archName() + ", " + std::to_string(std::thread::hardware_concurrency()) + " hardware threads\n";
    s += cpuDetails();
    s += "  SIMD: " + simdLine() + "\n";
    s += "  (\"(cpu only)\": the processor has it, but the operating system or a virtual machine has it switched off)\n\n";

    s += "Memory\n" + memoryLine() + "\n";

    s += "Graphics\n";
    s += "  " + gpu + "\n" + graphicsDetails();
    return s;
}
