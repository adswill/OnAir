// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The architecture of a plug-in file and OnAir's, and the plain-words text when they differ (core/include/dect2/codec/codec_arch.h).
#include "dect2/codec/codec_arch.h"
#include <cstdio>
#include <cstring>

using namespace dect2::codec;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; std::printf("FAIL %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void le32(unsigned char* p, uint32_t v) { p[0] = v & 255; p[1] = v >> 8 & 255; p[2] = v >> 16 & 255; p[3] = v >> 24; }
static void be32(unsigned char* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16 & 255; p[2] = v >> 8 & 255; p[3] = v & 255; }

int main() {
    unsigned char b[128] = {};
    le32(b, 0xFEEDFACF); le32(b + 4, 0x0100000C);
    CHECK(machoArchOf(b, 32) == "arm64", "thin arm64: %s", machoArchOf(b, 32).c_str());
    le32(b + 4, 0x01000007);
    CHECK(machoArchOf(b, 32) == "x86_64", "thin x86_64");
    le32(b + 4, 0x12345);
    CHECK(machoArchOf(b, 32) == "unknown", "an unknown CPU");
    std::memset(b, 0, sizeof b);
    be32(b, 0xCAFEBABE); be32(b + 4, 2); be32(b + 8, 0x01000007); be32(b + 28, 0x0100000C);
    CHECK(machoArchOf(b, sizeof b) == "universal (x86_64, arm64)", "universal: %s", machoArchOf(b, sizeof b).c_str());
    be32(b + 4, 52);   // a Java class file starts with CAFEBABE too, then its version
    CHECK(machoArchOf(b, sizeof b).empty(), "a Java class is not a plug-in");
    const unsigned char elf[] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 0};
    CHECK(machoArchOf(elf, sizeof elf).empty(), "ELF is not Mach-O");
    CHECK(machoArchOf(b, 3).empty(), "too short");
    CHECK(machoArchOfFile("/no/such/file").empty(), "a missing file");

    const std::string e = "dlopen(/x/onair-codec-ambe2.dylib, 0x0006): tried: '/x/onair-codec-ambe2.dylib' (mach-o file, but is an incompatible architecture (have 'arm64', need 'x86_64'))";
    const std::string m = archMismatchMessage(e);
    CHECK(m.find("built for arm64 but OnAir is running as x86_64") != std::string::npos, "the message names both: %s", m.c_str());
    CHECK(m.find("Apple Silicon build of OnAir") != std::string::npos, "and points to the Apple Silicon build");
    CHECK(m.find("-arch arm64 -arch x86_64") != std::string::npos, "and the universal build");
    const std::string m2 = archMismatchMessage("(mach-o file, but is an incompatible architecture (have 'x86_64', need 'arm64'))");
    CHECK(m2.find("built for x86_64 but OnAir is running as arm64") != std::string::npos && m2.find("Apple Silicon build") == std::string::npos, "the other way round: %s", m2.c_str());
    CHECK(archMismatchMessage("image not found").empty(), "other errors keep their own text");
    CHECK(rosettaNotice().find("Rosetta") != std::string::npos && rosettaNotice().find("arm64") != std::string::npos, "the Rosetta notice");
    CHECK(std::strlen(processArch()) > 0, "OnAir's own architecture");
    std::printf(fails ? "FAILED\n" : "codec_arch ok (running as %s%s)\n", processArch(), runningUnderRosetta() ? ", under Rosetta" : "");
    return fails ? 1 : 0;
}
