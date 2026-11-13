// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/plugin/manifest.h"
#include "dect2/automation/json_mini.h"
#include <cmath>
#include <cstdlib>

namespace dect2::plugin {
namespace aj = automation::json;

const char* permissionName(uint32_t bit) {
    switch (bit) {
    case PermNetwork: return "network";
    case PermFilesystem: return "filesystem";
    case PermAudioOut: return "audio-out";
    case PermRecord: return "record";
    case PermSpawn: return "spawn-process";
    default: return "";
    }
}
std::vector<std::string> permissionNames(uint32_t mask) {
    std::vector<std::string> v;
    for (uint32_t b = 1; b <= PermSpawn; b <<= 1) if (mask & b) v.push_back(permissionName(b));
    return v;
}

bool validId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (size_t i = 0; i < id.size(); i++) {
        const char c = id[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (i > 0 && (c == '-' || c == '_' || c == '.'));
        if (!ok) return false;
    }
    return id.find("..") == std::string::npos;
}

static bool validModeId(const std::string& id) {
    if (id.empty() || id.size() > 32) return false;
    for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}

int compareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& s) {
        std::vector<long> v;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '-' || s[i] == '+') break;
            long n = 0; bool any = false;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9' && n < 100000000) { n = n * 10 + (s[i] - '0'); i++; any = true; }
            v.push_back(any ? n : 0);
            if (i < s.size() && s[i] == '.') i++; else if (i < s.size() && s[i] != '-' && s[i] != '+') { while (i < s.size() && s[i] != '.' && s[i] != '-' && s[i] != '+') i++; if (i < s.size() && s[i] == '.') i++; }
        }
        return v;
    };
    auto x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

static bool validVersion(const std::string& v) {
    if (v.empty() || v.size() > 32 || v[0] < '0' || v[0] > '9') return false;
    for (char c : v) if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) return false;
    return true;
}

static bool validFileName(const std::string& f) {
    if (f.empty() || f.size() > 100 || f[0] == '.') return false;
    for (char c : f) if (c == '/' || c == '\\' || c == ':' || (unsigned char)c < 32) return false;
    return f.find("..") == std::string::npos;
}

static bool cleanText(const std::string& s, size_t maxLen) {
    if (s.size() > maxLen) return false;
    for (unsigned char c : s) if (c < 32 && c != '\n' && c != '\t') return false;
    return true;
}

static bool numField(const aj::Value& o, const char* k, double& dst, double lo, double hi, std::string& err) {
    const aj::Value* v = o.get(k);
    if (!v) return true;
    if (v->type != aj::Value::Num || !std::isfinite(v->n) || v->n < lo || v->n > hi) { err = std::string("\"") + k + "\" must be a number from " + std::to_string((long long)lo) + " to " + std::to_string((long long)hi); return false; }
    dst = v->n;
    return true;
}

bool versionSpecMatches(const std::string& spec, const std::string& version) {
    if (spec == "*") return true;
    if (spec.compare(0, 2, "<=") == 0) return compareVersions(version, spec.substr(2)) <= 0;
    return !spec.empty() && compareVersions(version, spec) == 0;
}

std::string Manifest::libraryFile() const {
#if defined(__APPLE__)
    return libMac;
#elif defined(_WIN32)
    return libWindows;
#else
    return libLinux;
#endif
}

bool parseManifest(const std::string& text, Manifest& out, std::string& err, const std::string& hostVersion) {
    out = Manifest{};
    if (text.size() > kMaxManifestBytes) { err = "manifest.json is larger than 64 KiB"; return false; }
    aj::Value root;
    std::string perr;
    if (!aj::parse(text, root, perr)) { err = "manifest.json is not valid JSON (" + perr + ")"; return false; }
    if (root.type != aj::Value::Obj) { err = "manifest.json must be a JSON object"; return false; }
    auto text1 = [&](const char* k, std::string& dst, size_t maxLen, bool required) {
        const aj::Value* v = root.get(k);
        if (!v) { if (required) { err = std::string("missing \"") + k + "\""; return false; } return true; }
        if (v->type != aj::Value::Str || !cleanText(v->s, maxLen)) { err = std::string("\"") + k + "\" must be text of at most " + std::to_string(maxLen) + " characters"; return false; }
        if (required && v->s.empty()) { err = std::string("\"") + k + "\" is empty"; return false; }
        dst = v->s;
        return true;
    };
    if (!text1("id", out.id, 64, true) || !text1("name", out.name, 80, true) || !text1("version", out.version, 32, true) || !text1("author", out.author, 120, true) ||
        !text1("license", out.license, 80, true) || !text1("homepage", out.homepage, 200, false) || !text1("description", out.description, 2000, false) ||
        !text1("kind", out.kind, 16, true) || !text1("minimum_onair", out.minOnAir, 32, false))
        return false;
    if (!validId(out.id)) { err = "\"id\" must be 1-64 characters of a-z 0-9 - _ . (starting with a letter or digit)"; return false; }
    if (!validVersion(out.version)) { err = "\"version\" must look like 1.2.3"; return false; }
    if (out.kind != "mode" && out.kind != "action" && out.kind != "source" && out.kind != "panel-lite") { err = "\"kind\" must be mode, action, source or panel-lite"; return false; }
    if (!out.homepage.empty() && out.homepage.compare(0, 8, "https://") != 0 && out.homepage.compare(0, 7, "http://") != 0) { err = "\"homepage\" must be an http(s) address"; return false; }
    if (!out.minOnAir.empty() && !validVersion(out.minOnAir)) { err = "\"minimum_onair\" must look like 0.2.7"; return false; }

    double abi = 0;
    if (!root.get("abi_version")) { err = "missing \"abi_version\""; return false; }
    if (!numField(root, "abi_version", abi, 1, 1000000, err)) return false;
    out.abi = (uint32_t)abi;
    if (out.abi != kAbiVersion) { err = "built for plugin ABI " + std::to_string(out.abi) + ", this OnAir speaks ABI " + std::to_string(kAbiVersion); return false; }
    if (!hostVersion.empty() && !out.minOnAir.empty() && compareVersions(hostVersion, out.minOnAir) < 0) { err = "needs OnAir " + out.minOnAir + " or newer (this is " + hostVersion + ")"; return false; }

    if (const aj::Value* lib = root.get("library")) {
        if (lib->type == aj::Value::Str) out.libMac = out.libLinux = out.libWindows = lib->s;   // one name for all (unusual, but allowed)
        else if (lib->type == aj::Value::Obj) {
            out.libMac = lib->str("macos"); out.libLinux = lib->str("linux"); out.libWindows = lib->str("windows");
        } else { err = "\"library\" must be a file name or an object with macos, linux and windows names"; return false; }
        for (const std::string* f : {&out.libMac, &out.libLinux, &out.libWindows})
            if (!f->empty() && !validFileName(*f)) { err = "\"library\" must be plain file names inside the plugin folder"; return false; }
    }

    if (const aj::Value* p = root.get("permissions")) {
        if (p->type != aj::Value::Arr || p->a.size() > 16) { err = "\"permissions\" must be a list"; return false; }
        for (const auto& e : p->a) {
            if (e.type != aj::Value::Str) { err = "\"permissions\" must be a list of names"; return false; }
            uint32_t bit = 0;
            for (uint32_t b = 1; b <= PermSpawn; b <<= 1) if (e.s == permissionName(b)) bit = b;
            if (!bit) { err = "unknown permission \"" + e.s.substr(0, 40) + "\" (network, filesystem, audio-out, record, spawn-process)"; return false; }
            out.permissions |= bit;
        }
    }

    if (const aj::Value* m = root.get("modes")) {
        if (m->type != aj::Value::Arr || m->a.size() > kMaxModes) { err = "\"modes\" must be a list of at most 16 modes"; return false; }
        for (const auto& e : m->a) {
            if (e.type != aj::Value::Obj) { err = "each mode must be an object"; return false; }
            ModeDecl d;
            d.id = e.str("id"); d.name = e.str("name"); d.group = e.str("group");
            if (!validModeId(d.id)) { err = "a mode \"id\" must be 1-32 characters of a-z 0-9 - _"; return false; }
            if (d.name.empty() || !cleanText(d.name, 60) || !cleanText(d.group, 60)) { err = "mode \"" + d.id + "\" needs a \"name\" of at most 60 characters"; return false; }
            for (const auto& o : out.modes) if (o.id == d.id) { err = "mode id \"" + d.id + "\" is used twice"; return false; }
            if (!numField(e, "default_frequency_hz", d.defaultHz, 1, 1e12, err) || !numField(e, "bandwidth_hz", d.bandwidthHz, 1, 1e10, err)) return false;
            if (const aj::Value* sr = e.get("sample_rates")) {
                if (sr->type != aj::Value::Arr || sr->a.size() > 16) { err = "\"sample_rates\" must be a short list"; return false; }
                for (const auto& r : sr->a) {
                    if (r.type != aj::Value::Num || !std::isfinite(r.n) || r.n < 1000 || r.n > 1e9) { err = "\"sample_rates\" must be numbers from 1000 to 1e9"; return false; }
                    d.sampleRates.push_back(r.n);
                }
            }
            out.modes.push_back(std::move(d));
        }
    }
    if ((out.kind == "mode" || out.kind == "source") && out.modes.empty()) { err = "a plugin of this kind needs at least one entry in \"modes\""; return false; }

    if (const aj::Value* s = root.get("settings")) {
        if (s->type != aj::Value::Arr || s->a.size() > kMaxSettings) { err = "\"settings\" must be a list of at most 64 controls"; return false; }
        for (const auto& e : s->a) {
            if (e.type != aj::Value::Obj) { err = "each setting must be an object"; return false; }
            SettingDef d;
            d.key = e.str("key"); d.label = e.str("label"); d.type = e.str("type");
            if (!validModeId(d.key)) { err = "a setting \"key\" must be 1-32 characters of a-z 0-9 - _"; return false; }
            for (const auto& o : out.settings) if (o.key == d.key) { err = "setting key \"" + d.key + "\" is used twice"; return false; }
            if (d.label.empty() || !cleanText(d.label, 80)) { err = "setting \"" + d.key + "\" needs a \"label\""; return false; }
            if (d.type != "slider" && d.type != "checkbox" && d.type != "combo" && d.type != "text") { err = "setting \"" + d.key + "\": type must be slider, checkbox, combo or text"; return false; }
            d.max = 100;
            if (!numField(e, "min", d.min, -1e12, 1e12, err) || !numField(e, "max", d.max, -1e12, 1e12, err) || !numField(e, "step", d.step, 0, 1e12, err)) return false;
            if (d.type == "slider" && !(d.min < d.max)) { err = "setting \"" + d.key + "\": min must be below max"; return false; }
            if (const aj::Value* df = e.get("default")) { if (df->type == aj::Value::Num && std::isfinite(df->n) && std::fabs(df->n) <= 1e12) d.def = df->n; else if (df->type == aj::Value::Str) { if (!cleanText(df->s, 500)) { err = "setting default too long"; return false; } d.defText = df->s; } else if (df->type == aj::Value::Bool) d.defFlag = df->b; }
            if (d.type == "combo") {
                const aj::Value* op = e.get("options");
                if (!op || op->type != aj::Value::Arr || op->a.empty() || op->a.size() > kMaxOptions) { err = "setting \"" + d.key + "\": a combo needs 1-64 \"options\""; return false; }
                for (const auto& x : op->a) { if (x.type != aj::Value::Str || !cleanText(x.s, 80)) { err = "setting \"" + d.key + "\": options are short texts"; return false; } d.options.push_back(x.s); }
                if (d.def < 0 || d.def >= (double)d.options.size()) d.def = 0;
            }
            out.settings.push_back(std::move(d));
        }
    }
    return true;
}

} // namespace dect2::plugin
