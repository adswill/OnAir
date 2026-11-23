// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
// Importing a codec library on Android (Settings > Codecs, docs/CODECS.md "Codecs on Android"). OnAir ships no patented codec: the user builds
// or gets one, picks the .so (or a .zip of them) and the app copies it into its private codecs folder, where the same loaders as on the desktop
// find it. This is the part that decides, from the file name and the first bytes, whether a file may be copied at all:
//  - the name must be one the core looks for (the list of optional_libs.cpp: codecs and plug-ins, no radio drivers), as a plain file name;
//  - the file must be a 64-bit little-endian ELF shared library for arm64-v8a (the only ABI the app is built for).
// Nothing is loaded, downloaded or run here.
#include <cstddef>
#include <string>
#include <vector>

namespace dect2::codec {

struct ElfInfo {
    bool ok = false;           // an arm64-v8a ELF shared library
    std::string abi;           // "arm64-v8a", "armeabi-v7a", "x86_64", "x86", "riscv64" or "" when it is not an ELF file
    std::string problem;       // plain words when !ok
};
// Checks the first bytes (at least 20 are needed) of a file.
ElfInfo elfCheck(const unsigned char* data, size_t size);

struct ImportCheck {
    bool ok = false;
    std::string id;            // "dra", "mbe", "codecplugins" ... (the optional_libs.cpp ids)
    std::string name;          // "DRA+ add-on (CDR sound)"
    std::string usedFor;       // what needs it
    std::string file;          // the file name to store under (the same as the input, never a path)
    std::string abi;
    std::string problem;       // plain words when !ok
};
// The name part alone (ok, id, name, usedFor, file set when the name is acceptable).
ImportCheck checkCodecName(const std::string& fileName);
// `head` is the start of the file (a few hundred bytes are plenty).
ImportCheck checkCodecImport(const std::string& fileName, const unsigned char* head, size_t size);

struct ImportableCodec {
    std::string id, name, usedFor, files, source;   // files: the names accepted, comma separated
};
// Every codec and plug-in that can be imported on Android, in the order of the Settings > Codecs list.
std::vector<ImportableCodec> importableCodecs();

} // namespace dect2::codec
