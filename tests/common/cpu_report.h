// Speed figures for calibrating the limits on the CI machines: every speed check also prints one line
//   CPUCAL test=<name> case="<what>" value=<v> <unit> limit<op><l> use=<u> checked=<0|1>
// use is the share of the limit taken (1.0 at the limit, above 1 fails); checked=0 where the limit is not applied here (CI, sanitizers).
// Before its first figure a test prints the machine: CPUCAL machine cpu="..." threads=<n> os=<os> ci=<0|1> slack=<ciSlack()>.
// ctest keeps the output of passing tests in its log (Testing/Temporary/LastTest.log, or --output-log): grep CPUCAL there.
#pragma once
#include "ci_slack.h"
#include <array>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)   // GCC and Clang, also MinGW (which defines _M_X64 too)
#include <cpuid.h>
#endif

// the processor's own name ("13th Gen Intel(R) Core(TM) i7-1365U", "Apple M1"); "unknown" where it has none to give
inline std::string cpuCalModel() {
    std::string s;
#if defined(__APPLE__)
    size_t n = 0;
    if (sysctlbyname("machdep.cpu.brand_string", nullptr, &n, nullptr, 0) == 0 && n > 1) {
        s.resize(n);
        if (sysctlbyname("machdep.cpu.brand_string", s.data(), &n, nullptr, 0) != 0) s.clear();
    }
#elif defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
    std::array<unsigned, 12> r{};
    for (unsigned i = 0; i < 3; i++) {
#if defined(_MSC_VER)
        std::array<int, 4> q{};
        __cpuid(q.data(), (int)(0x80000002u + i));
        for (int k = 0; k < 4; k++) r[i * 4 + k] = (unsigned)q[(size_t)k];
#else
        __get_cpuid(0x80000002u + i, &r[i * 4], &r[i * 4 + 1], &r[i * 4 + 2], &r[i * 4 + 3]);
#endif
    }
    for (unsigned v : r)
        for (int b = 0; b < 4; b++) s += (char)((v >> (8 * b)) & 0xff);
#endif
    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
    const size_t f = s.find_first_not_of(' ');
    s = f == std::string::npos ? std::string() : s.substr(f);
    return s.empty() ? std::string("unknown") : s;
}

inline const char* cpuCalOs() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "other";
#endif
}

// a sample rate as a case name: "2.4 Msps", "12.288 Msps"
inline std::string cpuCalMsps(double rate) {
    std::ostringstream o;
    o.setf(std::ios::fixed);
    o.precision(3);
    o << rate / 1e6;
    std::string s = o.str();
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s + " Msps";
}

// one figure; op '<': the value must stay below the limit, '>': above it
inline void cpuCal(const char* test, const std::string& what, double value, const char* unit, char op, double limit, bool checked = true) {
    static std::once_flag once;
    static std::mutex mu;
    std::call_once(once, [] {
        printf("CPUCAL machine cpu=\"%s\" threads=%u os=%s ci=%d slack=%.1f\n", cpuCalModel().c_str(), std::thread::hardware_concurrency(), cpuCalOs(),
               ciSlack() > 1 ? 1 : 0, ciSlack());
    });
    const double use = op == '<' ? (limit != 0 ? value / limit : 0) : (value != 0 ? limit / value : 99);
    std::lock_guard<std::mutex> lk(mu);
    printf("CPUCAL test=%s case=\"%s\" value=%.4g %s limit%c%.4g use=%.3f checked=%d\n", test, what.c_str(), value, unit, op, limit, use, checked ? 1 : 0);
    fflush(stdout);
}
