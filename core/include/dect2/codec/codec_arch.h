// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Which processor a codec plug-in and OnAir itself are built for, and plain words when they differ (Discord report 2026-10-23: the Intel
// build of OnAir under Rosetta on an M4 Mac could not load the user's arm64 plug-ins, and the dlopen text alone did not say why).
// The parsing is pure (tests/codec/test_codec_arch.cpp); the file and process helpers are thin.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace dect2::codec {

// The architecture name of a Mach-O CPU type ("arm64", "x86_64" ...), or "" when not one of the usual ones.
inline std::string machoCpuName(uint32_t cpu) {
    switch (cpu) {
    case 0x0100000Cu: return "arm64";    // CPU_TYPE_ARM64
    case 0x01000007u: return "x86_64";   // CPU_TYPE_X86_64
    case 0x0000000Cu: return "arm";
    case 0x00000007u: return "i386";
    default: return "";
    }
}

// The architectures in the first bytes of a file (at least 8; a universal file needs 8 + 20 per slice): "arm64", "x86_64",
// "universal (arm64, x86_64)", or "" when it is not a Mach-O file (an ELF .so, a Windows .dll, too short ...).
inline std::string machoArchOf(const unsigned char* b, size_t n) {
    if (n < 8) return "";
    const uint32_t le = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    const uint32_t be = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | (uint32_t)b[3];
    auto rd = [&](size_t at, bool big) -> uint32_t {
        if (at + 4 > n) return 0;
        return big ? ((uint32_t)b[at] << 24 | (uint32_t)b[at + 1] << 16 | (uint32_t)b[at + 2] << 8 | (uint32_t)b[at + 3])
                   : ((uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24);
    };
    if (le == 0xFEEDFACFu || le == 0xFEEDFACEu) { const std::string c = machoCpuName(rd(4, false)); return c.empty() ? "unknown" : c; }   // thin, little-endian
    if (be == 0xFEEDFACFu || be == 0xFEEDFACEu) { const std::string c = machoCpuName(rd(4, true)); return c.empty() ? "unknown" : c; }
    if (be == 0xCAFEBABEu || be == 0xCAFEBABFu) {   // universal: big-endian header, nfat_arch slices of 20 (or 32 for the 64-bit form) bytes
        const uint32_t count = rd(4, true);
        if (count == 0 || count > 16) return "";    // 0xCAFEBABE is also a Java class file: those have a large "count" (the version)
        const size_t step = be == 0xCAFEBABFu ? 32 : 20;
        std::string list;
        for (uint32_t i = 0; i < count; i++) {
            std::string c = machoCpuName(rd(8 + i * step, true));
            if (c.empty()) c = "unknown";
            list += (list.empty() ? "" : ", ") + c;
        }
        return "universal (" + list + ")";
    }
    return "";
}

inline std::string machoArchOfFile(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    unsigned char b[8 + 16 * 32];
    const size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    return machoArchOf(b, n);
}

// The architecture this OnAir runs as.
inline const char* processArch() {
#if defined(__aarch64__) || defined(__arm64__)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "i386";
#elif defined(__arm__)
    return "arm";
#else
    return "unknown";
#endif
}

// True when this (Intel) OnAir runs under Rosetta on an Apple Silicon Mac.
inline bool runningUnderRosetta() {
#if defined(__APPLE__) && defined(__x86_64__)
    int v = 0;
    size_t sz = sizeof v;
    return sysctlbyname("sysctl.proc_translated", &v, &sz, nullptr, 0) == 0 && v == 1;
#else
    return false;
#endif
}

inline std::string rosettaNotice() {
    return "You are running the Intel version of OnAir on an Apple Silicon Mac (Rosetta): it is slower and cannot load arm64 plug-ins. "
           "Download the Apple Silicon (arm64) build.";
}

// A loader's "incompatible architecture (have 'arm64', need 'x86_64')" in plain words; "" when the text is not about that.
inline std::string archMismatchMessage(const std::string& err) {
    const size_t h = err.find("have '"), nd = err.find("need '");
    if (err.find("incompatible architecture") == std::string::npos || h == std::string::npos || nd == std::string::npos) return "";
    const size_t h1 = err.find('\'', h + 6), n1 = err.find('\'', nd + 6);
    if (h1 == std::string::npos || n1 == std::string::npos) return "";
    const std::string have = err.substr(h + 6, h1 - h - 6), need = err.substr(nd + 6, n1 - nd - 6);
    std::string m = "This plug-in is built for " + have + " but OnAir is running as " + need + ". Build or download the " + need +
                    " version (or a universal one: -arch arm64 -arch x86_64)";
    if (need == "x86_64" && have == "arm64") m += ", or use the Apple Silicon build of OnAir";
    return m + ".";
}

}  // namespace dect2::codec
