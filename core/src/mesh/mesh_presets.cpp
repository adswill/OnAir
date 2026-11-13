// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MeshCore radio presets: the built-in table (mesh_presets_data.cpp), the api.meshcore.nz document parser and the saved-file loader.
#include "dect2/mesh/mesh_presets.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace dect2 {

std::string meshCoreRegionOf(const std::string& title) {
    std::string t;
    for (char c : title) t += (char)std::tolower((unsigned char)c);
    static const char* keys[][2] = {{"australia", "Oceania"}, {"new zealand", "Oceania"}, {"brazil", "South America"}, {"canada", "North America"},
                                    {"usa", "North America"}, {"costa rica", "North America"}, {"vietnam", "Asia"}};
    for (const auto& k : keys) if (t.find(k[0]) != std::string::npos) return k[1];
    return "Europe";
}

std::string meshCorePresetSummary(const MeshCorePresetEntry& p) {
    char b[96];
    snprintf(b, sizeof b, "%.3f MHz / %g kHz / SF%d / CR 4/%d", p.mhz, p.bwKhz, p.sf, p.cr);
    return b;
}

MeshLoraSettings meshCorePresetToLora(const MeshCorePresetEntry& p) {
    MeshLoraSettings s;
    s.name = p.title;
    s.freqHz = p.mhz * 1e6;
    s.sf = p.sf; s.bwHz = p.bwKhz * 1e3; s.cr = p.cr;
    s.syncWord = 0x12;
    s.preamble = p.sf <= 8 ? 32 : 16;
    s.ldro = meshLoraLdro(p.sf, s.bwHz);
    return s;
}

std::vector<MeshCorePresetEntry> meshCoreBuiltinPresets() {
    std::vector<MeshCorePresetEntry> v;
    for (int i = 0; i < kMeshCorePresetCount; i++) {
        const MeshCorePreset& p = kMeshCorePresets[i];
        MeshCorePresetEntry e;
        e.title = p.title; e.region = p.region; e.note = p.note; e.mhz = p.mhz; e.bwKhz = p.bwKhz; e.sf = p.sf; e.cr = p.cr;
        v.push_back(e);
    }
    return v;
}

namespace {

// A tolerant reader of just what the document holds: finds "key": and returns the string or number after it, within [from, to).
bool findValue(const std::string& s, size_t from, size_t to, const char* key, std::string& out) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k, from);
    if (p == std::string::npos || p >= to) return false;
    p = s.find(':', p + k.size());
    if (p == std::string::npos || p >= to) return false;
    p++;
    while (p < to && std::isspace((unsigned char)s[p])) p++;
    if (p >= to) return false;
    out.clear();
    if (s[p] == '"') {
        for (p++; p < to && s[p] != '"'; p++) {
            if (s[p] == '\\' && p + 1 < to) p++;
            out += s[p];
        }
        return p < to;
    }
    while (p < to && s[p] != ',' && s[p] != '}' && !std::isspace((unsigned char)s[p])) out += s[p++];
    return !out.empty();
}

bool toNumber(const std::string& v, double& d) {
    char* end = nullptr;
    d = strtod(v.c_str(), &end);
    return end && end != v.c_str() && *end == 0;
}

} // namespace

bool parseMeshCoreConfig(const std::string& json, std::vector<MeshCorePresetEntry>& out) {
    const size_t sec = json.find("\"suggested_radio_settings\"");
    if (sec == std::string::npos) return false;
    const size_t ent = json.find("\"entries\"", sec);
    if (ent == std::string::npos) return false;
    size_t p = json.find('[', ent);
    if (p == std::string::npos) return false;
    std::vector<MeshCorePresetEntry> v;
    bool closed = false;
    for (p++; p < json.size();) {
        while (p < json.size() && (std::isspace((unsigned char)json[p]) || json[p] == ',')) p++;
        if (p >= json.size()) break;
        if (json[p] == ']') { closed = true; break; }
        if (json[p] != '{') return false;
        size_t q = p + 1;
        int depth = 1;
        bool inStr = false;
        for (; q < json.size() && depth; q++) {
            const char c = json[q];
            if (inStr) { if (c == '\\') q++; else if (c == '"') inStr = false; }
            else if (c == '"') inStr = true;
            else if (c == '{') depth++;
            else if (c == '}') depth--;
        }
        if (depth) return false;   // truncated
        std::string t, f, bw, sf, cr, desc;
        double mhz, bwk, sfd, crd;
        if (findValue(json, p, q, "title", t) && findValue(json, p, q, "frequency", f) && findValue(json, p, q, "bandwidth", bw) &&
            findValue(json, p, q, "spreading_factor", sf) && findValue(json, p, q, "coding_rate", cr) &&
            toNumber(f, mhz) && toNumber(bw, bwk) && toNumber(sf, sfd) && toNumber(cr, crd) &&
            mhz >= 400 && mhz <= 1000 && bwk >= 7.8 && bwk <= 500 && sfd >= 5 && sfd <= 12 && crd >= 5 && crd <= 8 && !t.empty()) {
            MeshCorePresetEntry e;
            e.title = t; e.region = meshCoreRegionOf(t); e.mhz = mhz; e.bwKhz = bwk; e.sf = (int)sfd; e.cr = (int)crd;
            if (findValue(json, p, q, "description", desc)) {   // "... / 3B": the path-hash size
                const size_t b = desc.rfind('/');
                if (b != std::string::npos) {
                    std::string n = desc.substr(b + 1);
                    n.erase(std::remove_if(n.begin(), n.end(), [](unsigned char c) { return std::isspace(c); }), n.end());
                    if (n.size() == 2 && std::isdigit((unsigned char)n[0]) && n[1] == 'B') e.note = n;
                }
            }
            v.push_back(e);
        }
        p = q;
    }
    if (!closed || v.empty()) return false;
    out = v;
    return true;
}

bool loadMeshCorePresetFile(const std::string& path, std::vector<MeshCorePresetEntry>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    if (ss.str().size() > (4u << 20)) return false;
    return parseMeshCoreConfig(ss.str(), out);
}

std::vector<MeshCorePresetEntry> meshCorePresets(const std::string& savedFile, bool* fromFile) {
    if (fromFile) *fromFile = false;
    struct stat st;
    if (!savedFile.empty() && stat(savedFile.c_str(), &st) == 0 && (long long)st.st_mtime > kMeshCorePresetsFetched) {
        std::vector<MeshCorePresetEntry> v;
        if (loadMeshCorePresetFile(savedFile, v)) { if (fromFile) *fromFile = true; return v; }
    }
    return meshCoreBuiltinPresets();
}

std::vector<std::string> meshCoreRegions(const std::vector<MeshCorePresetEntry>& v) {
    std::vector<std::string> r;
    for (const auto& e : v) if (std::find(r.begin(), r.end(), e.region) == r.end()) r.push_back(e.region);
    return r;
}

} // namespace dect2
