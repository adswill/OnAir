// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/codec/codec_import.h"
#include "dect2/codec/optional_libs.h"
#include <cctype>

namespace dect2::codec {

namespace {

std::string lowerOf(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }

std::vector<std::string> splitComma(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == ',') { if (!cur.empty()) v.push_back(cur); cur.clear(); }
        else if (c != ' ') cur += c;
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}

// ends in .so or .so.<digits>[.<digits>...]: what a shared library on Android is called
bool soSuffix(const std::string& lower) {
    const size_t p = lower.rfind(".so");
    if (p == std::string::npos || p == 0) return false;
    size_t i = p + 3;
    if (i == lower.size()) return true;
    while (i < lower.size()) {
        if (lower[i] != '.') return false;
        ++i;
        const size_t s = i;
        while (i < lower.size() && std::isdigit((unsigned char)lower[i])) ++i;
        if (i == s) return false;
    }
    return true;
}

unsigned rd16(const unsigned char* p) { return p[0] | (p[1] << 8); }

struct Extra { const char* pattern; const char* id; };
// FFmpeg's other half: the add-on needs libavutil next to libavcodec
const Extra kExtra[] = {{"libavutil.so*", "ffmpeg"}};

} // namespace

ElfInfo elfCheck(const unsigned char* d, size_t n) {
    ElfInfo r;
    if (!d || n < 20 || d[0] != 0x7F || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') {
        r.problem = "this is not an Android library (it is not an ELF file)";
        return r;
    }
    const int cls = d[4], data = d[5];
    const unsigned type = data == 1 ? rd16(d + 16) : 0, machine = data == 1 ? rd16(d + 18) : 0;
    if (data != 1) { r.problem = "this library is big-endian; Android phones need a little-endian arm64-v8a library"; return r; }
    switch (machine) {
        case 183: r.abi = "arm64-v8a"; break;
        case 62: r.abi = "x86_64"; break;
        case 40: r.abi = "armeabi-v7a"; break;
        case 3: r.abi = "x86"; break;
        case 243: r.abi = "riscv64"; break;
        default: r.abi = "unknown"; break;
    }
    if (r.abi != "arm64-v8a" || cls != 2) {
        r.problem = "this library is built for " + r.abi + "; OnAir on Android needs arm64-v8a (build it with the NDK for arm64-v8a)";
        return r;
    }
    if (type != 3) { r.problem = "this is not a shared library (.so) but an executable or object file"; return r; }
    r.ok = true;
    return r;
}

ImportCheck checkCodecName(const std::string& fileName) {
    ImportCheck r;
    r.file = fileName;
    if (fileName.empty() || fileName.size() > 120) { r.problem = "the file name is empty or too long"; return r; }
    for (char c : fileName) {
        if (c == '/' || c == '\\' || c == 0 || (unsigned char)c < 32) { r.problem = "the file name must be a plain name, without a folder"; return r; }
    }
    if (fileName == "." || fileName == ".." || fileName[0] == '.') { r.problem = "the file name must be a plain name, without a folder"; return r; }
    const std::string low = lowerOf(fileName);
    if (!soSuffix(low)) { r.problem = "only .so libraries can be imported on Android"; return r; }

    for (const OptionalLib& o : optionalCodecDefs()) {
        for (const std::string& p : splitComma(o.files)) {
            if (optionalNameMatches(p, fileName)) { r.id = o.id; r.name = o.name; r.usedFor = o.usedFor; break; }
        }
        if (!r.id.empty()) break;
    }
    if (r.id.empty()) {
        for (const Extra& x : kExtra) {
            if (!optionalNameMatches(x.pattern, fileName)) continue;
            for (const OptionalLib& o : optionalCodecDefs()) if (o.id == x.id) { r.id = o.id; r.name = o.name; r.usedFor = o.usedFor; }
            break;
        }
    }
    if (r.id.empty()) {
        r.problem = "OnAir does not use a library with this name; see Settings > Codecs for the names it accepts";
        return r;
    }
    r.ok = true;
    return r;
}

ImportCheck checkCodecImport(const std::string& fileName, const unsigned char* head, size_t size) {
    ImportCheck r = checkCodecName(fileName);
    if (!r.ok) return r;
    r.ok = false;
    const ElfInfo e = elfCheck(head, size);
    r.abi = e.abi;
    if (!e.ok) { r.problem = e.problem; r.id.clear(); r.name.clear(); r.usedFor.clear(); return r; }
    r.ok = true;
    return r;
}

std::vector<ImportableCodec> importableCodecs() {
    std::vector<ImportableCodec> v;
    for (const OptionalLib& o : optionalCodecDefs()) {
        ImportableCodec c;
        c.id = o.id; c.name = o.name; c.usedFor = o.usedFor; c.files = o.files; c.source = o.source;
        if (c.id == "ffmpeg") c.files += ", libavutil.so*";
        v.push_back(std::move(c));
    }
    return v;
}

} // namespace dect2::codec
