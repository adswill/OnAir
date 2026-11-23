// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Android codec import (docs/CODECS.md, "Codecs on Android"): the file-name allowlist and the ELF / ABI check that decide whether a library the
// user picked may be copied into the app's codecs folder.
#include "dect2/codec/codec_import.h"
#include <cstdio>
#include <cstring>
#include <string>

using namespace dect2::codec;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)

// A minimal ELF header: class 1 (32 bit) or 2 (64 bit), data 1 (little endian) or 2, type, machine
static std::string elf(int cls, int data, int type, int machine, size_t size = 64) {
    std::string h(size, '\0');
    if (size >= 20) {
        h[0] = 0x7F; h[1] = 'E'; h[2] = 'L'; h[3] = 'F'; h[4] = (char)cls; h[5] = (char)data; h[6] = 1;
        h[16] = (char)(type & 255); h[17] = (char)(type >> 8); h[18] = (char)(machine & 255); h[19] = (char)(machine >> 8);
    }
    return h;
}
static ImportCheck chk(const std::string& name, const std::string& bytes) {
    return checkCodecImport(name, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

int main() {
    const std::string arm64 = elf(2, 1, 3, 183);

    // ELF and ABI
    CHECK(elfCheck(reinterpret_cast<const unsigned char*>(arm64.data()), arm64.size()).ok);
    CHECK(elfCheck(reinterpret_cast<const unsigned char*>(arm64.data()), arm64.size()).abi == "arm64-v8a");
    const std::string x64 = elf(2, 1, 3, 62), arm32 = elf(1, 1, 3, 40), x86 = elf(1, 1, 3, 3), be = elf(2, 2, 3, 183), exe = elf(2, 1, 2, 183);
    auto e = [](const std::string& s) { return elfCheck(reinterpret_cast<const unsigned char*>(s.data()), s.size()); };
    CHECK(!e(x64).ok && e(x64).abi == "x86_64" && e(x64).problem.find("arm64-v8a") != std::string::npos);
    CHECK(!e(arm32).ok && e(arm32).abi == "armeabi-v7a");
    CHECK(!e(x86).ok && e(x86).abi == "x86");
    CHECK(!e(be).ok);
    CHECK(!e(exe).ok);                           // an executable, not a shared library
    CHECK(!e(std::string("MZ......")).ok);       // a Windows file
    CHECK(!e(std::string("PK\3\4zipzip")).ok);
    CHECK(!e(std::string()).ok);
    CHECK(!e(elf(2, 1, 3, 183, 10)).ok);         // too short for a header

    // the allowlist: names the core looks for
    for (const char* n : {"libonair_dra.so", "libmbe.so", "libmbe.so.1", "libmbe-neo.so", "libcodec2.so", "libfaad.so", "libfaad.so.2", "libfaad_hdc.so",
                          "libfdk-aac.so", "libfdk-aac.so.2", "libmp3lame.so", "libavcodec.so", "libavcodec.so.61", "libavutil.so", "onair-codec-ambe2.so", "LIBMBE.SO"}) {
        const ImportCheck c = chk(n, arm64);
        if (!c.ok) printf("  refused %s: %s\n", n, c.problem.c_str());
        CHECK(c.ok);
        CHECK(!c.id.empty() && !c.name.empty() && c.file == n && c.abi == "arm64-v8a");
    }
    CHECK(chk("libonair_dra.so", arm64).id == "dra");
    CHECK(chk("libmbe.so", arm64).id == "mbe");
    CHECK(chk("libfaad_hdc.so", arm64).id == "hdc");
    CHECK(chk("onair-codec-tetra.so", arm64).id == "codecplugins");
    // not on the list: other libraries, radio drivers (they have their own path), wrong extension
    for (const char* n : {"libevil.so", "libc.so", "libusb-1.0.so", "librtlsdr.so", "libuhd.so", "libmbe.dll", "libmbe.dylib", "libmbe", "libmbe.so.zip", "libmbe.sox", "mbe.so", ".so", ""}) {
        const ImportCheck c = chk(n, arm64);
        CHECK(!c.ok);
        CHECK(!c.problem.empty());
    }
    // names that try to leave the folder
    for (const char* n : {"../libmbe.so", "a/libmbe.so", "a\\libmbe.so", "/data/libmbe.so", "..", "libmbe.so/.."}) CHECK(!chk(n, arm64).ok);
    CHECK(!chk(std::string("libmbe.so\0x", 11), arm64).ok);
    CHECK(!chk(std::string(300, 'a') + ".so", arm64).ok);
    // right name, wrong content
    CHECK(!chk("libmbe.so", x64).ok);
    CHECK(!chk("libmbe.so", elf(2, 1, 3, 183, 8)).ok);
    CHECK(chk("libmbe.so", x64).problem.find("x86_64") != std::string::npos);

    // the list shown in Settings > Codecs: every codec, no radio driver
    const std::vector<ImportableCodec> list = importableCodecs();
    bool dra = false, mbe = false, drv = false;
    for (const auto& c : list) {
        if (c.id == "dra") dra = true;
        if (c.id == "mbe") mbe = true;
        if (c.id == "rtlsdr" || c.id == "uhd" || c.id == "hamlib") drv = true;
        CHECK(!c.name.empty() && !c.usedFor.empty() && !c.files.empty());
    }
    CHECK(dra && mbe && !drv);
    CHECK(list.size() >= 8);

    if (fails) { printf("%d failed\n", fails); return 1; }
    printf("codec_import ok\n");
    return 0;
}
