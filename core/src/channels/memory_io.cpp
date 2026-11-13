// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MemoryBank files: JSON (the saved file and an export), CSV (spreadsheets, other programs), the import of simple "frequency,name" lists and of
// an SDR++ Frequency Manager export, and the atomic save. The format is documented in memory_bank.h and in app/memory/README.md.
#include "dect2/channels/memory_bank.h"
#include "dect2/common/u8file.h"
#include "memory_json.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dect2 {

namespace {
using mj::Value;

std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string num(double v) { char b[40]; snprintf(b, sizeof b, "%.10g", v); return b; }

// ---- JSON out
void jstr(std::string& o, const char* k, const std::string& v) { if (!v.empty()) o += std::string(",\"") + k + "\":" + mj::quote(v); }
void jnum(std::string& o, const char* k, double v) { if (v != 0) o += std::string(",\"") + k + "\":" + num(v); }
void jbool(std::string& o, const char* k, bool v) { if (v) o += std::string(",\"") + k + "\":true"; }

std::string entryJson(const Memory& m) {
    std::string o = "{\"freqHz\":" + std::to_string((unsigned long long)m.freqHz);
    jstr(o, "name", m.name); jstr(o, "mode", m.mode); jstr(o, "subId", m.subId);
    if (m.family >= 0) o += ",\"family\":" + std::to_string(m.family);
    jnum(o, "stdMode", m.stdMode); jnum(o, "sampleRateHz", m.sampleRateHz); jnum(o, "bandwidthHz", m.bandwidthHz);
    jstr(o, "radio", m.radio);
    if (m.hasGain) o += ",\"gain\":{\"lna\":" + std::to_string(m.lna) + ",\"vga\":" + std::to_string(m.vga) + ",\"amp\":" + (m.amp ? "true" : "false") + ",\"db\":" + num(m.gainDb) + "}";
    if (!m.options.empty()) {
        o += ",\"options\":{";
        bool first = true;
        for (const auto& kv : m.options) { if (!first) o += ","; first = false; o += mj::quote(kv.first) + ":" + mj::quote(kv.second); }
        o += "}";
    }
    jstr(o, "group", m.group); jstr(o, "tags", m.tags); jstr(o, "notes", m.notes);
    jnum(o, "colour", m.colour); jnum(o, "lastUsed", (double)m.lastUsed); jnum(o, "created", (double)m.created);
    jbool(o, "favourite", m.favourite); jbool(o, "scan", m.scan); jnum(o, "dwellSec", m.dwellSec);
    return o + "}";
}

// ---- JSON in
std::string asText(const Value* v) {
    if (!v) return "";
    if (v->kind == Value::Str) return v->str;
    if (v->kind == Value::Num) return num(v->num);
    if (v->kind == Value::Bool) return v->b ? "true" : "false";
    return "";
}
double asNum(const Value* v, double def = 0) {
    if (!v) return def;
    if (v->kind == Value::Num) return std::isfinite(v->num) ? v->num : def;
    if (v->kind == Value::Str) { char* e = nullptr; const double d = strtod(v->str.c_str(), &e); return e != v->str.c_str() && std::isfinite(d) ? d : def; }
    return def;
}
bool asBool(const Value* v) { return v && (v->kind == Value::Bool ? v->b : v->kind == Value::Num ? v->num != 0 : v->kind == Value::Str && (v->str == "1" || lower(v->str) == "true")); }

// one entry; false when it has no usable frequency
bool entryFrom(const Value& e, Memory& m, std::string& why) {
    if (e.kind != Value::Obj) { why = "not an object"; return false; }
    const double hz = asNum(e.get("freqHz"), -1);
    if (!(hz >= 1 && hz < 1e13)) { why = "no usable freqHz"; return false; }
    m.freqHz = (uint64_t)(hz + 0.5);
    m.name = asText(e.get("name")); m.mode = asText(e.get("mode")); m.subId = asText(e.get("subId"));
    m.family = (int)asNum(e.get("family"), -1); m.stdMode = (int)asNum(e.get("stdMode"));
    m.sampleRateHz = std::max(0.0, asNum(e.get("sampleRateHz"))); m.bandwidthHz = std::max(0.0, asNum(e.get("bandwidthHz")));
    m.radio = asText(e.get("radio"));
    if (const Value* g = e.get("gain"); g && g->kind == Value::Obj) {
        m.hasGain = true;
        m.lna = (int)asNum(g->get("lna")); m.vga = (int)asNum(g->get("vga")); m.amp = asBool(g->get("amp")); m.gainDb = asNum(g->get("db"));
    }
    if (const Value* o = e.get("options"); o && o->kind == Value::Obj) for (const auto& kv : o->obj) m.options[kv.first] = asText(&kv.second);
    m.group = asText(e.get("group")); m.tags = asText(e.get("tags")); m.notes = asText(e.get("notes"));
    m.colour = (uint32_t)std::max(0.0, std::min(16777215.0, asNum(e.get("colour"))));
    m.lastUsed = (int64_t)asNum(e.get("lastUsed")); m.created = (int64_t)asNum(e.get("created"));
    m.favourite = asBool(e.get("favourite")); m.scan = asBool(e.get("scan")); m.dwellSec = std::max(0.0, asNum(e.get("dwellSec")));
    return true;
}

// SDR++ Frequency Manager: {"lists":{"<list>":{"bookmarks":{"<name>":{"frequency":Hz,"bandwidth":Hz,"mode":n}}}}}. The mode number is that of
// SDR++'s own demodulators, which OnAir's modes do not map to one-to-one, so only the frequency, bandwidth, name and list are taken over.
size_t fromSdrpp(const Value& root, std::vector<Memory>& out, MemoryLoadReport& rep) {
    const Value* lists = root.get("lists");
    if (!lists || lists->kind != Value::Obj) return 0;
    size_t n = 0;
    for (const auto& l : lists->obj) {
        const Value* bm = l.second.get("bookmarks");
        if (!bm || bm->kind != Value::Obj) continue;
        for (const auto& b : bm->obj) {
            const double f = asNum(b.second.get("frequency"), -1);
            if (!(f >= 1 && f < 1e13)) { rep.skipped++; rep.log.push_back("skipped SDR++ bookmark \"" + b.first + "\": no frequency"); continue; }
            Memory m; m.name = b.first; m.freqHz = (uint64_t)(f + 0.5); m.group = l.first; m.bandwidthHz = std::max(0.0, asNum(b.second.get("bandwidth")));
            out.push_back(std::move(m)); n++;
        }
    }
    return n;
}

// ---- CSV
std::string csvCell(const std::string& s) {
    const std::string v = u8valid(s);
    if (v.find_first_of(",;\"\r\n") == std::string::npos && (v.empty() || (v.front() != ' ' && v.back() != ' '))) return v;
    std::string o = "\"";
    for (char c : v) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}
std::string optEnc(const std::string& s) {   // key=value;key=value with % escapes for the three characters the layout uses
    std::string o;
    for (char c : s) { if (c == '%' || c == ';' || c == '=') { char b[4]; snprintf(b, sizeof b, "%%%02X", (unsigned char)c); o += b; } else o += c; }
    return o;
}
std::string optDec(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else o += s[i];
    }
    return o;
}

// rows of cells; quotes may hold the delimiter, doubled quotes and line breaks
std::vector<std::vector<std::string>> csvRows(const std::string& t, char delim) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string cell;
    bool q = false, any = false;
    size_t i = t.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    auto endRow = [&]() { row.push_back(cell); cell.clear(); if (any || row.size() > 1 || !row[0].empty()) rows.push_back(row); row.clear(); any = false; };
    for (; i < t.size(); i++) {
        const char c = t[i];
        if (q) {
            if (c == '"') { if (i + 1 < t.size() && t[i + 1] == '"') { cell += '"'; i++; } else q = false; }
            else cell += c;
        } else if (c == '"' && cell.find_first_not_of(" \t") == std::string::npos) { q = true; any = true; cell.clear(); }
        else if (c == delim) { row.push_back(cell); cell.clear(); any = true; }
        else if (c == '\n' || c == '\r') { if (c == '\r' && i + 1 < t.size() && t[i + 1] == '\n') i++; endRow(); }
        else cell += c;
    }
    if (!cell.empty() || !row.empty() || any) endRow();
    return rows;
}

std::string trim(const std::string& s) { const size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t"); return b == std::string::npos ? "" : s.substr(b, e - b + 1); }
std::string colKey(const std::string& s) { std::string o; for (char c : trim(s)) if (c != ' ' && c != '_' && c != '-') o += (char)tolower((unsigned char)c); return o; }

bool fromCsv(const std::string& text, std::vector<Memory>& out, MemoryLoadReport& rep, const std::string& defGroup) {
    const size_t nl = text.find_first_of("\r\n");
    const std::string first = text.substr(0, nl);
    const char delim = std::count(first.begin(), first.end(), ';') > std::count(first.begin(), first.end(), ',') ? ';' : ',';
    const auto rows = csvRows(text, delim);
    if (rows.empty()) return false;
    // a header names the columns; without one the lines are "frequency,name" (or "name,frequency")
    std::map<std::string, size_t> col;
    bool header = false;
    {
        uint64_t hz;
        bool anyFreq = false;
        for (const std::string& c : rows[0]) anyFreq |= memoryParseFreq(c, hz) && !c.empty() && isdigit((unsigned char)trim(c)[0]);
        for (size_t i = 0; i < rows[0].size(); i++) col.emplace(colKey(rows[0][i]), i);
        header = !anyFreq && (col.count("frequencymhz") || col.count("frequency") || col.count("freq") || col.count("frequencyhz") || col.count("freqhz") || col.count("mhz"));
    }
    auto at = [&](const std::vector<std::string>& r, std::initializer_list<const char*> names) -> const std::string* {
        for (const char* n : names) { auto it = col.find(n); if (it != col.end() && it->second < r.size()) return &r[it->second]; }
        return nullptr;
    };
    for (size_t ri = header ? 1 : 0; ri < rows.size(); ri++) {
        const auto& r = rows[ri];
        Memory m;
        bool ok = false;
        if (!header) {
            uint64_t hz = 0;
            size_t fi = 0;
            if (r.size() >= 1 && memoryParseFreq(r[0], hz)) ok = true;
            else if (r.size() >= 2 && memoryParseFreq(r[1], hz)) { ok = true; fi = 1; }
            if (ok) { m.freqHz = hz; m.name = trim(r.size() > 1 ? r[1 - fi] : ""); }
        } else {
            uint64_t hz = 0;
            if (const std::string* f = at(r, {"frequencymhz", "frequency", "freq", "mhz"})) {
                // an explicit MHz column is MHz whatever the size; the generic "frequency" follows memoryParseFreq's rule
                const bool mhzCol = col.count("frequencymhz") || col.count("mhz");
                if (mhzCol && !f->empty() && f->find_first_not_of("0123456789.,- ") == std::string::npos) { char* e; std::string x = *f; for (char& c : x) if (c == ',') c = '.'; const double d = strtod(x.c_str(), &e); if (d > 0) { hz = (uint64_t)(d * 1e6 + 0.5); ok = true; } }
                else ok = memoryParseFreq(*f, hz);
            } else if (const std::string* f2 = at(r, {"frequencyhz", "freqhz", "hz"})) { char* e; const double d = strtod(f2->c_str(), &e); if (d >= 1) { hz = (uint64_t)(d + 0.5); ok = true; } }
            if (ok) {
                m.freqHz = hz;
                auto s = [&](std::initializer_list<const char*> n) { const std::string* p = at(r, n); return p ? *p : std::string(); };
                auto d = [&](std::initializer_list<const char*> n) { const std::string* p = at(r, n); return p && !p->empty() ? strtod(p->c_str(), nullptr) : 0.0; };
                auto b = [&](std::initializer_list<const char*> n) { const std::string v = lower(trim(s(n))); return v == "1" || v == "true" || v == "yes" || v == "x" || v == "*"; };
                m.name = s({"name", "label", "description", "title"}); m.mode = s({"mode", "modeid"}); m.subId = s({"subid", "submode"});
                const std::string fam = s({"family"}); m.family = fam.empty() ? -1 : atoi(fam.c_str());
                m.stdMode = (int)d({"std", "stdmode"}); m.sampleRateHz = d({"samplerate", "samplerates", "samplerateHz", "sampleratehz"});
                m.bandwidthHz = d({"bandwidthhz", "bandwidth", "bw"});
                m.radio = s({"radio", "device"});
                const std::string lna = s({"lna"}), vga = s({"vga"}), gdb = s({"gaindb", "gain"}), amp = s({"amp"});
                if (!lna.empty() || !vga.empty() || !gdb.empty() || !amp.empty()) { m.hasGain = true; m.lna = atoi(lna.c_str()); m.vga = atoi(vga.c_str()); m.amp = b({"amp"}); m.gainDb = gdb.empty() ? 0 : strtod(gdb.c_str(), nullptr); }
                m.group = s({"group"}); m.tags = s({"tags", "tag"}); m.notes = s({"notes", "comment", "comments"});
                const std::string co = trim(s({"colour", "color"}));
                if (!co.empty()) m.colour = (uint32_t)(strtoul(co[0] == '#' ? co.c_str() + 1 : co.c_str(), nullptr, 16) & 0xFFFFFF);
                m.lastUsed = (int64_t)d({"lastused"}); m.created = (int64_t)d({"created"});
                m.favourite = b({"favourite", "favorite", "fav"}); m.scan = b({"scan"}); m.dwellSec = d({"dwells", "dwell", "dwellsec"});
                const std::string opt = s({"options"});
                size_t i = 0;
                while (i < opt.size()) {
                    size_t j = opt.find(';', i);
                    if (j == std::string::npos) j = opt.size();
                    const std::string kv = opt.substr(i, j - i);
                    const size_t eq = kv.find('=');
                    if (eq != std::string::npos && eq > 0) m.options[optDec(kv.substr(0, eq))] = optDec(kv.substr(eq + 1));
                    i = j + 1;
                }
            }
        }
        if (!ok) { rep.skipped++; rep.log.push_back("skipped CSV line " + std::to_string(ri + 1) + ": no frequency"); continue; }
        if (m.group.empty()) m.group = defGroup;
        out.push_back(std::move(m));
    }
    return true;
}

bool readAll(const std::string& path, std::string& s, std::string& err) {
    std::ifstream f(u8file(path), std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    s = ss.str();
    return true;
}
bool writeAtomic(const std::string& path, const std::string& data, std::string* err) {
    const std::filesystem::path p = u8file(path), tmp = u8file(path + ".tmp");
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot write " + path + ".tmp"; return false; }
        f.write(data.data(), (std::streamsize)data.size());
        f.flush();
        if (!f) { if (err) *err = "write failed: " + path + ".tmp"; std::filesystem::remove(tmp, ec); return false; }
    }
    std::filesystem::rename(tmp, p, ec);   // replaces an existing file
    if (ec) { if (err) *err = "cannot replace " + path + ": " + ec.message(); std::filesystem::remove(tmp, ec); return false; }
    return true;
}
} // namespace

const std::vector<std::string>& memoryCsvColumns() {
    static const std::vector<std::string> c = {"name", "frequency_mhz", "mode", "sub_id", "family", "std", "sample_rate_hz", "bandwidth_hz", "radio", "lna", "vga", "amp",
                                               "gain_db", "group", "tags", "notes", "colour", "last_used", "created", "favourite", "scan", "dwell_s", "options"};
    return c;
}

std::string MemoryBank::toJson() const {
    std::string o = "{\"format\":\"onair-memories\",\"version\":" + std::to_string(kMemoryFileVersion) + ",\"memories\":[";
    for (size_t i = 0; i < v_.size(); i++) { o += i ? ",\n" : "\n"; o += entryJson(v_[i]); }
    return o + "\n]}\n";
}

std::string MemoryBank::toCsv() const {
    std::string o;
    const auto& cols = memoryCsvColumns();
    for (size_t i = 0; i < cols.size(); i++) { if (i) o += ','; o += cols[i]; }
    o += "\r\n";
    for (const Memory& m : v_) {
        std::string opt;
        for (const auto& kv : m.options) { if (!opt.empty()) opt += ';'; opt += optEnc(kv.first) + "=" + optEnc(kv.second); }
        char col[16] = "";
        if (m.colour) snprintf(col, sizeof col, "#%06X", m.colour);
        const std::vector<std::string> cells = {m.name, memoryFreqMhzText(m.freqHz), m.mode, m.subId, m.family >= 0 ? std::to_string(m.family) : "", m.stdMode ? std::to_string(m.stdMode) : "",
            m.sampleRateHz ? num(m.sampleRateHz) : "", m.bandwidthHz ? num(m.bandwidthHz) : "", m.radio, m.hasGain ? std::to_string(m.lna) : "", m.hasGain ? std::to_string(m.vga) : "",
            m.hasGain ? (m.amp ? "1" : "0") : "", m.hasGain && m.gainDb != 0 ? num(m.gainDb) : "", m.group, m.tags, m.notes, col, m.lastUsed ? std::to_string((long long)m.lastUsed) : "",
            m.created ? std::to_string((long long)m.created) : "", m.favourite ? "1" : "", m.scan ? "1" : "", m.dwellSec ? num(m.dwellSec) : "", opt};
        for (size_t i = 0; i < cells.size(); i++) { if (i) o += ','; o += csvCell(cells[i]); }
        o += "\r\n";
    }
    return o;
}

MemoryLoadReport MemoryBank::fromJson(const std::string& text) {
    MemoryLoadReport rep;
    v_.clear();
    Value root;
    std::string err;
    const bool whole = mj::parse(text, root, err);
    if (!whole) rep.log.push_back("memories file damaged (" + err + "): reading what is before the damage");
    const Value* arr = root.kind == Value::Arr ? &root : root.get("memories");
    if (root.kind == Value::Obj) {
        const Value* ver = root.get("version");
        if (ver && asNum(ver) > kMemoryFileVersion) rep.log.push_back("memories file is from a newer version (" + num(asNum(ver)) + "): unknown fields are ignored");
    }
    if (!arr || arr->kind != Value::Arr) { rep.ok = false; rep.error = whole ? "no memories in the file" : err; return rep; }
    for (size_t i = 0; i < arr->arr.size(); i++) {
        // in a cut-off file the last entry is incomplete: its frequency may be there without the rest, so it is dropped
        if (!whole && i + 1 == arr->arr.size()) { rep.skipped++; rep.log.push_back("dropped the incomplete last entry"); break; }
        Memory m;
        std::string why;
        if (!entryFrom(arr->arr[i], m, why)) { rep.skipped++; rep.log.push_back("skipped entry " + std::to_string(i + 1) + ": " + why); continue; }
        if (v_.size() >= kMemoryCap) { rep.skipped++; rep.log.push_back("over the cap of " + std::to_string(kMemoryCap) + " memories: the rest is not loaded"); break; }
        m.id = nextId_++;
        if (!m.created) m.created = 0;
        v_.push_back(std::move(m));
        rep.loaded++;
    }
    if (!whole && rep.loaded == 0) { rep.ok = false; rep.error = err; }
    return rep;
}

MemoryLoadReport MemoryBank::importText(const std::string& text, DupPolicy dup, const std::string& defGroup) {
    MemoryLoadReport rep;
    std::vector<Memory> in;
    const size_t b = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (b == std::string::npos) { rep.ok = false; rep.error = "the file is empty"; return rep; }
    if (text[b] == '{' || text[b] == '[') {
        Value root;
        std::string err;
        if (!mj::parse(text, root, err)) { rep.ok = false; rep.error = "not valid JSON: " + err; return rep; }
        if (root.kind == Value::Obj && root.get("lists")) fromSdrpp(root, in, rep);
        else {
            const Value* arr = root.kind == Value::Arr ? &root : root.get("memories");
            if (!arr || arr->kind != Value::Arr) { rep.ok = false; rep.error = "no memories in the file"; return rep; }
            for (size_t i = 0; i < arr->arr.size(); i++) {
                Memory m; std::string why;
                if (entryFrom(arr->arr[i], m, why)) in.push_back(std::move(m)); else { rep.skipped++; rep.log.push_back("skipped entry " + std::to_string(i + 1) + ": " + why); }
            }
        }
        for (Memory& m : in) if (m.group.empty()) m.group = defGroup;
    } else if (!fromCsv(text, in, rep, defGroup)) { rep.ok = false; rep.error = "nothing to import"; return rep; }
    for (Memory& m : in) {
        m.id = 0;
        const MemoryAddResult r = add(std::move(m), dup);
        if (r.what == MemoryAdd::Full) { rep.skipped++; rep.log.push_back("the bank is full (" + std::to_string(kMemoryCap) + "): the rest is not imported"); break; }
        if (r.what == MemoryAdd::Duplicate) { rep.skipped++; if (dup == DupPolicy::Update) rep.loaded++; continue; }
        rep.loaded++;
    }
    if (in.empty()) { rep.ok = false; rep.error = "no frequencies found"; }
    return rep;
}

bool MemoryBank::save(const std::string& path, std::string* err) const { return writeAtomic(path, toJson(), err); }

MemoryLoadReport MemoryBank::load(const std::string& path) {
    MemoryLoadReport rep;
    v_.clear();
    std::error_code ec;
    if (!std::filesystem::exists(u8file(path), ec)) return rep;   // no file yet: an empty bank
    std::string text, err;
    if (!readAll(path, text, err)) { rep.ok = false; rep.error = err; return rep; }
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) return rep;
    rep = fromJson(text);
    if (!rep.ok) {   // keep what could not be read: the next save would otherwise replace it with an empty file
        std::filesystem::copy_file(u8file(path), u8file(path + ".corrupt"), std::filesystem::copy_options::overwrite_existing, ec);
        rep.log.push_back("the unreadable file was kept as " + path + ".corrupt");
    }
    return rep;
}

bool MemoryBank::exportFile(const std::string& path, bool csv, std::string* err) const { return writeAtomic(path, csv ? toCsv() : toJson(), err); }

MemoryLoadReport MemoryBank::importFile(const std::string& path, DupPolicy dup) {
    std::string text, err;
    if (!readAll(path, text, err)) { MemoryLoadReport r; r.ok = false; r.error = err; return r; }
    return importText(text, dup);
}

} // namespace dect2
