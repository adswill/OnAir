// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Frequency list formats (CHIRP, SDR++, SDR#, plain CSV), the import preview and merge rules, "My lists" and the built-in lists. See memory_formats.h.
#include "dect2/channels/memory_formats.h"
#include "dect2/channels/freq_db.h"
#include "dect2/common/xmlmini.h"
#include "memory_json.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

namespace dect2 {

namespace {
using mj::Value;

std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) { const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n"); return b == std::string::npos ? "" : s.substr(b, e - b + 1); }
std::string colKey(const std::string& s) { std::string o; for (char c : trim(s)) if (c != ' ' && c != '_' && c != '-' && c != '(' && c != ')') o += (char)tolower((unsigned char)c); return o; }
std::string num(double v, int dec = 6) {
    char b[48];
    snprintf(b, sizeof b, "%.*f", dec, v);
    for (char* p = b; *p; p++) if (*p == ',') *p = '.';
    std::string s = b;
    if (s.find('.') != std::string::npos) { while (!s.empty() && s.back() == '0') s.pop_back(); if (!s.empty() && s.back() == '.') s.pop_back(); }
    return s.empty() || s == "-0" ? "0" : s;
}
std::string csvCell(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos && (s.empty() || (s.front() != ' ' && s.back() != ' '))) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}
std::string xmlEsc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;";
        else if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') continue;
        else o += (char)c;
    }
    return o;
}

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

char delimOf(const std::string& text) {
    const size_t nl = text.find_first_of("\r\n");
    const std::string first = text.substr(0, nl);
    return std::count(first.begin(), first.end(), ';') > std::count(first.begin(), first.end(), ',') ? ';' : ',';
}

// ---- the demodulator names: one table for SDR++ (mode numbers), SDR# (DetectorType) and CHIRP (Mode); the neutral key is stored in options["demod"]
struct Demod { const char* key; int sdrpp; const char* sharp; const char* chirp; };
const Demod kDemods[] = {{"nfm", 0, "NFM", "NFM"}, {"wfm", 1, "WFM", "WFM"}, {"am", 2, "AM", "AM"}, {"dsb", 3, "DSB", "DSB"}, {"usb", 4, "USB", "USB"},
                         {"cw", 5, "CW", "CW"}, {"lsb", 6, "LSB", "LSB"}, {"raw", 7, "RAW", "RAW"}, {"fm", 0, "NFM", "FM"}};
const Demod* demodByKey(const std::string& k) { for (const Demod& d : kDemods) if (k == d.key) return &d; return nullptr; }
std::string demodFromSharp(const std::string& s) { const std::string l = lower(trim(s)); for (const Demod& d : kDemods) if (l == lower(d.sharp)) return d.key; return l.empty() ? "" : l; }
std::string demodFromChirp(const std::string& s) { const std::string l = lower(trim(s)); for (const Demod& d : kDemods) if (l == lower(d.chirp)) return d.key; return l; }
std::string demodOf(const Memory& m) {
    auto it = m.options.find("demod");
    if (it != m.options.end() && !it->second.empty()) return it->second;
    if (m.mode == "fm") return m.bandwidthHz >= 100000 ? "wfm" : "nfm";
    if (m.mode == "am") return "am";
    return "";
}

// the neutral demodulator key goes to options; FM flavours also say mode "fm" (the mode id the memory bank recalls)
void setDemod(Memory& m, const std::string& key) {
    m.options["demod"] = key;
    if (key == "nfm" || key == "wfm" || key == "fm") m.mode = "fm";
}

uint64_t hzFromMhzText(const std::string& s, bool& ok) {
    char* e = nullptr;
    std::string x = trim(s);
    for (char& c : x) if (c == ',') c = '.';
    const double d = strtod(x.c_str(), &e);
    ok = e != x.c_str() && d > 0 && d < 1e7;
    return ok ? (uint64_t)llround(d * 1e6) : 0;
}

// ---- SDR++
void parseSdrpp(const Value& root, std::vector<Memory>& out, MemoryLoadReport& rep) {
    const Value* lists = root.get("lists");
    if (!lists || lists->kind != Value::Obj) return;
    for (const auto& l : lists->obj) {
        const Value* bm = l.second.get("bookmarks");
        if (!bm || bm->kind != Value::Obj) continue;
        for (const auto& b : bm->obj) {
            const Value* f = b.second.get("frequency");
            const double hz = f && f->kind == Value::Num ? f->num : -1;
            if (!(hz >= 1 && hz < 1e13)) { rep.skipped++; rep.log.push_back("skipped SDR++ bookmark \"" + b.first + "\": no frequency"); continue; }
            Memory m; m.name = b.first; m.freqHz = (uint64_t)(hz + 0.5); m.group = l.first;
            const Value* bw = b.second.get("bandwidth");
            if (bw && bw->kind == Value::Num && bw->num > 0) m.bandwidthHz = bw->num;
            const Value* md = b.second.get("mode");
            if (md && md->kind == Value::Num) for (const Demod& d : kDemods) if (d.sdrpp == (int)md->num) { setDemod(m, d.key); break; }
            out.push_back(std::move(m));
        }
    }
}

std::string writeSdrpp(const std::vector<Memory>& v) {
    std::vector<std::string> names;
    std::map<std::string, std::vector<const Memory*>> by;
    for (const Memory& m : v) { const std::string g = m.group.empty() ? "OnAir" : m.group; if (!by.count(g)) names.push_back(g); by[g].push_back(&m); }
    std::string o = "{\n  \"lists\": {";
    for (size_t li = 0; li < names.size(); li++) {
        o += li ? ",\n    " : "\n    ";
        o += mj::quote(names[li]) + ": {\n      \"showOnWaterfall\": true,\n      \"bookmarks\": {";
        std::map<std::string, int> used;
        const auto& items = by[names[li]];
        for (size_t i = 0; i < items.size(); i++) {
            const Memory& m = *items[i];
            std::string nm = m.name.empty() ? memoryFreqMhzText(m.freqHz) + " MHz" : m.name;
            const int n = used[nm]++;
            if (n) nm += " (" + std::to_string(n + 1) + ")";   // the bookmark names are the object keys: they must be unique inside a list
            const Demod* d = demodByKey(demodOf(m));
            o += i ? ",\n        " : "\n        ";
            o += mj::quote(nm) + ": {\"frequency\": " + num((double)m.freqHz, 0) + ", \"bandwidth\": " + num(m.bandwidthHz > 0 ? m.bandwidthHz : 12500, 0) + ", \"mode\": " + std::to_string(d ? d->sdrpp : 0) + "}";
        }
        o += "\n      }\n    }";
    }
    return o + "\n  },\n  \"selectedList\": " + mj::quote(names.empty() ? "General" : names[0]) + "\n}\n";
}

// ---- SDR#
void parseSharp(const std::string& text, std::vector<Memory>& out, MemoryLoadReport& rep, const std::string& defGroup) {
    auto root = parseXml(text);
    if (!root) { rep.ok = false; rep.error = "not valid XML"; return; }
    size_t n = 0;
    for (const auto& e : root->all("MemoryEntry")) {
        n++;
        auto get = [&](const char* k) { auto c = e->first(k); return c ? trim(c->text) : std::string(); };
        const std::string f = get("Frequency");
        char* end = nullptr;
        const double hz = strtod(f.c_str(), &end);
        if (f.empty() || end == f.c_str() || !(hz >= 1)) { rep.skipped++; rep.log.push_back("skipped SDR# entry " + std::to_string(n) + ": no frequency"); continue; }
        Memory m; m.freqHz = (uint64_t)(hz + 0.5); m.name = get("Name"); m.group = get("GroupName"); m.notes = get("Notes");
        if (m.group.empty()) m.group = defGroup;
        const std::string fav = lower(get("IsFavourite"));
        m.favourite = fav == "true" || fav == "1";
        const double bw = atof(get("FilterBandwidth").c_str());
        if (bw > 0) m.bandwidthHz = bw;
        const std::string dm = demodFromSharp(get("DetectorType"));
        if (!dm.empty()) setDemod(m, dm);
        const double shift = atof(get("Shift").c_str());
        if (shift != 0) m.options["offsetHz"] = num(shift, 0);
        out.push_back(std::move(m));
    }
}

std::string writeSharp(const std::vector<Memory>& v) {
    std::string o = "<?xml version=\"1.0\"?>\n<ArrayOfMemoryEntry xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">\n";
    for (const Memory& m : v) {
        const Demod* d = demodByKey(demodOf(m));
        auto off = m.options.find("offsetHz");
        o += "  <MemoryEntry>\n    <IsFavourite>" + std::string(m.favourite ? "true" : "false") + "</IsFavourite>\n    <Name>" + xmlEsc(m.name) + "</Name>\n    <Frequency>" +
             std::to_string((unsigned long long)m.freqHz) + "</Frequency>\n    <DetectorType>" + (d ? d->sharp : "NFM") + "</DetectorType>\n    <Shift>" +
             (off != m.options.end() ? num(atof(off->second.c_str()), 0) : "0") + "</Shift>\n    <FilterBandwidth>" + num(m.bandwidthHz > 0 ? m.bandwidthHz : 12500, 0) +
             "</FilterBandwidth>\n    <GroupName>" + xmlEsc(m.group) + "</GroupName>\n    <Notes>" + xmlEsc(m.notes) + "</Notes>\n  </MemoryEntry>\n";
    }
    return o + "</ArrayOfMemoryEntry>\n";
}

// ---- plain CSV (SdrCsv)
const char* kSdrCsvHeader = "Group,Name,Frequency (Hz),Mode,Bandwidth (Hz),Comment";

std::string writeSdrCsv(const std::vector<Memory>& v) {
    std::string o = std::string(kSdrCsvHeader) + "\r\n";
    for (const Memory& m : v) {
        const std::string dm = demodOf(m);
        const std::vector<std::string> c = {m.group, m.name, std::to_string((unsigned long long)m.freqHz), dm.empty() ? m.mode : dm, m.bandwidthHz > 0 ? num(m.bandwidthHz, 0) : "", m.notes};
        for (size_t i = 0; i < c.size(); i++) { if (i) o += ','; o += csvCell(c[i]); }
        o += "\r\n";
    }
    return o;
}

void parseSdrCsv(const std::string& text, std::vector<Memory>& out, MemoryLoadReport& rep, const std::string& defGroup) {
    const auto rows = csvRows(text, delimOf(text));
    if (rows.empty()) return;
    std::map<std::string, size_t> col;
    for (size_t i = 0; i < rows[0].size(); i++) col.emplace(colKey(rows[0][i]), i);
    auto at = [&](const std::vector<std::string>& r, std::initializer_list<const char*> names) -> const std::string* {
        for (const char* n : names) { auto it = col.find(n); if (it != col.end() && it->second < r.size()) return &r[it->second]; }
        return nullptr;
    };
    for (size_t ri = 1; ri < rows.size(); ri++) {
        const auto& r = rows[ri];
        Memory m;
        bool ok = false;
        if (const std::string* f = at(r, {"frequencyhz", "freqhz", "hz"})) { const double d = atof(f->c_str()); if (d >= 1) { m.freqHz = (uint64_t)(d + 0.5); ok = true; } }
        else if (const std::string* f2 = at(r, {"frequencymhz", "freqmhz", "mhz"})) m.freqHz = hzFromMhzText(*f2, ok);
        else if (const std::string* f3 = at(r, {"frequency", "freq"})) ok = memoryParseFreq(*f3, m.freqHz);
        if (!ok) { rep.skipped++; rep.log.push_back("skipped CSV line " + std::to_string(ri + 1) + ": no frequency"); continue; }
        auto s = [&](std::initializer_list<const char*> n) { const std::string* p = at(r, n); return p ? trim(*p) : std::string(); };
        m.name = s({"name", "label", "title"});
        m.group = s({"group", "groupname", "list", "category"});
        if (m.group.empty()) m.group = defGroup;
        m.notes = s({"comment", "comments", "notes", "description"});
        const std::string md = s({"mode", "modulation", "demod", "detector"});
        if (!md.empty()) setDemod(m, demodFromSharp(md));
        std::string bw = s({"bandwidthhz", "bandwidth", "filterbandwidth", "bw"});
        for (char& c : bw) if (c == ',') c = '.';   // decimal comma
        if (!bw.empty()) { const double b = atof(bw.c_str()); if (b > 0) m.bandwidthHz = b < 1000 ? b * 1000 : b; }   // a bare 12.5 is kHz
        out.push_back(std::move(m));
    }
}

// ---- CHIRP
const char* kChirpHeader = "Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,DtcsPolarity,RxDtcsCode,CrossMode,Mode,TStep,Skip,Power,Comment,URCALL,RPT1CALL,RPT2CALL,DVCODE";

void parseChirp(const std::string& text, std::vector<Memory>& out, MemoryLoadReport& rep, const std::string& defGroup) {
    const auto rows = csvRows(text, ',');
    if (rows.empty()) return;
    std::map<std::string, size_t> col;
    for (size_t i = 0; i < rows[0].size(); i++) col.emplace(colKey(rows[0][i]), i);
    auto cell = [&](const std::vector<std::string>& r, const char* k) { auto it = col.find(k); return it != col.end() && it->second < r.size() ? trim(r[it->second]) : std::string(); };
    for (size_t ri = 1; ri < rows.size(); ri++) {
        const auto& r = rows[ri];
        bool ok = false;
        const uint64_t hz = hzFromMhzText(cell(r, "frequency"), ok);
        if (!ok) { rep.skipped++; rep.log.push_back("skipped CHIRP line " + std::to_string(ri + 1) + ": no frequency"); continue; }
        Memory m; m.freqHz = hz; m.name = cell(r, "name"); m.notes = cell(r, "comment"); m.group = defGroup; m.mode = "fm";
        const std::string dm = demodFromChirp(cell(r, "mode"));
        if (!dm.empty()) setDemod(m, dm);
        const std::string dup = cell(r, "duplex");
        if (!dup.empty()) m.options["duplex"] = dup;
        bool offOk = false;
        const uint64_t off = hzFromMhzText(cell(r, "offset"), offOk);
        if (offOk && off > 0) m.options["offsetHz"] = std::to_string((unsigned long long)off);
        const std::string tone = cell(r, "tone");
        if (!tone.empty()) m.options["tone"] = tone;
        const std::string rt = cell(r, "rtonefreq"), ct = cell(r, "ctonefreq"), dtcs = cell(r, "dtcscode"), ts = cell(r, "tstep");
        if (!rt.empty()) m.options["rToneHz"] = rt;
        if (!ct.empty()) m.options["cToneHz"] = ct;
        if (!dtcs.empty()) m.options["dtcs"] = dtcs;
        if (!ts.empty()) m.options["tstepKHz"] = ts;
        const std::string loc = cell(r, "location");
        if (!loc.empty()) m.options["location"] = loc;
        m.scan = cell(r, "skip").empty();   // "S" = skipped by the radio's scan
        out.push_back(std::move(m));
    }
}

std::string writeChirp(const std::vector<Memory>& v) {
    std::string o = std::string(kChirpHeader) + "\r\n";
    int loc = 0;
    for (const Memory& m : v) {
        auto opt = [&](const char* k, const char* def = "") { auto it = m.options.find(k); return it != m.options.end() ? it->second : std::string(def); };
        std::string duplex = opt("duplex");
        const uint64_t offHz = (uint64_t)atoll(opt("offsetHz").c_str());
        const std::string dm = demodOf(m);
        const Demod* d = demodByKey(dm);
        std::string tone = opt("tone");
        const std::string rt = opt("rToneHz", "88.5"), ct = opt("cToneHz", "88.5");
        const std::vector<std::string> c = {std::to_string(loc++), m.name.substr(0, 16), memoryFreqMhzText(m.freqHz), duplex, num(offHz / 1e6, 6), tone, rt, ct,
                                            opt("dtcs", "023"), "NN", "023", "Tone->Tone", d ? d->chirp : "FM", opt("tstepKHz", "5.00"), "", "5.0W",
                                            m.notes, "", "", "", ""};
        std::string line;
        for (size_t i = 0; i < c.size(); i++) { if (i) line += ','; line += csvCell(c[i]); }
        o += line + "\r\n";
    }
    return o;
}

// ---- the OnAir formats go through a scratch bank (the reader lives there)
std::vector<Memory> viaBank(const std::string& text, bool json, MemoryLoadReport& rep, const std::string& defGroup) {
    MemoryBank b;
    rep = json ? b.fromJson(text) : b.importText(text, DupPolicy::Keep, defGroup);
    std::vector<Memory> v = b.all();
    for (Memory& m : v) { m.id = 0; if (m.group.empty()) m.group = defGroup; }
    return v;
}

bool sameKey(const Memory& a, const Memory& b) { return a.freqHz == b.freqHz && a.mode == b.mode; }
}  // namespace

const char* listFormatName(ListFormat f) {
    switch (f) {
        case ListFormat::OnAirJson: return "OnAir JSON"; case ListFormat::OnAirCsv: return "OnAir CSV"; case ListFormat::SdrppJson: return "SDR++ Frequency Manager JSON";
        case ListFormat::SdrSharpXml: return "SDR# frequency list (XML)"; case ListFormat::SdrCsv: return "Frequency list CSV (SDRangel / SDR#)"; case ListFormat::ChirpCsv: return "CHIRP CSV";
        default: return "Unknown";
    }
}
const char* listFormatExt(ListFormat f) {
    switch (f) { case ListFormat::OnAirJson: case ListFormat::SdrppJson: return ".json"; case ListFormat::SdrSharpXml: return ".xml"; default: return ".csv"; }
}

ListFormat detectListFormat(const std::string& text) {
    const size_t b = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    if (b == std::string::npos) return ListFormat::Unknown;
    if (text[b] == '[') return ListFormat::OnAirJson;
    if (text[b] == '{') {
        Value root;
        std::string err;
        mj::parse(text, root, err);
        if (root.kind == Value::Obj && root.get("lists")) return ListFormat::SdrppJson;
        return ListFormat::OnAirJson;
    }
    if (text[b] == '<') return text.find("MemoryEntry") != std::string::npos ? ListFormat::SdrSharpXml : ListFormat::Unknown;
    const size_t nl = text.find_first_of("\r\n", b);
    const auto head = csvRows(text.substr(b, nl == std::string::npos ? std::string::npos : nl - b), delimOf(text));
    std::map<std::string, size_t> col;
    if (!head.empty()) for (size_t i = 0; i < head[0].size(); i++) col.emplace(colKey(head[0][i]), i);
    if (col.count("location") && col.count("duplex") && col.count("frequency")) return ListFormat::ChirpCsv;
    if (col.count("frequencymhz") && (col.count("subid") || col.count("family") || col.count("favourite") || col.count("options"))) return ListFormat::OnAirCsv;
    if (col.count("frequencyhz") || col.count("freqhz") || ((col.count("frequency") || col.count("freq")) && (col.count("name") || col.count("group")))) return ListFormat::SdrCsv;
    return ListFormat::OnAirCsv;   // headerless "frequency,name" lines and the OnAir columns are read by the same reader
}

std::vector<Memory> parseList(const std::string& text, ListFormat fmt, MemoryLoadReport& rep, const std::string& defGroup) {
    rep = MemoryLoadReport();
    std::vector<Memory> out;
    if (fmt == ListFormat::Unknown) fmt = detectListFormat(text);
    switch (fmt) {
        case ListFormat::SdrppJson: {
            Value root; std::string err;
            if (!mj::parse(text, root, err)) { rep.ok = false; rep.error = "not valid JSON: " + err; return out; }
            parseSdrpp(root, out, rep);
            for (Memory& m : out) if (m.group.empty()) m.group = defGroup;
            break;
        }
        case ListFormat::SdrSharpXml: parseSharp(text, out, rep, defGroup); break;
        case ListFormat::SdrCsv: parseSdrCsv(text, out, rep, defGroup); break;
        case ListFormat::ChirpCsv: parseChirp(text, out, rep, defGroup); break;
        case ListFormat::OnAirJson: out = viaBank(text, true, rep, defGroup); break;
        default: out = viaBank(text, false, rep, defGroup); break;
    }
    rep.loaded = out.size();
    if (out.empty() && rep.ok && rep.error.empty()) { rep.ok = false; rep.error = "no frequencies found"; }
    return out;
}

std::string writeList(const std::vector<Memory>& v, ListFormat fmt) {
    switch (fmt) {
        case ListFormat::SdrppJson: return writeSdrpp(v);
        case ListFormat::SdrSharpXml: return writeSharp(v);
        case ListFormat::SdrCsv: return writeSdrCsv(v);
        case ListFormat::ChirpCsv: return writeChirp(v);
        default: {
            MemoryBank b;
            for (const Memory& m : v) b.add(m, DupPolicy::Keep);
            return fmt == ListFormat::OnAirCsv ? b.toCsv() : b.toJson();
        }
    }
}

ImportPreview previewImport(const MemoryBank& bank, const std::string& text, ListFormat fmt, const std::string& defGroup) {
    ImportPreview p;
    p.format = fmt == ListFormat::Unknown ? detectListFormat(text) : fmt;
    std::vector<Memory> v = parseList(text, p.format, p.rep, defGroup);
    std::vector<const Memory*> seen;   // rows of the file itself that repeat an earlier row count as duplicates too
    for (Memory& m : v) {
        ImportRow r;
        r.m = std::move(m);
        if (const Memory* d = bank.findDuplicate(r.m.freqHz, r.m.mode)) { r.duplicate = true; r.existingId = d->id; }
        p.rows.push_back(std::move(r));
    }
    for (size_t i = 0; i < p.rows.size(); i++) {
        if (!p.rows[i].duplicate) for (size_t j = 0; j < i; j++) if (!p.rows[j].duplicate && sameKey(p.rows[i].m, p.rows[j].m)) { p.rows[i].duplicate = true; p.rows[i].existingId = 0; break; }
        (p.rows[i].duplicate ? p.duplicates : p.fresh)++;
    }
    return p;
}

ImportResult applyImport(MemoryBank& bank, const ImportPreview& p, MergeRule rule) {
    ImportResult res;
    for (const ImportRow& r : p.rows) {
        if (!r.take) { res.left++; continue; }
        Memory m = r.m;
        m.id = 0;
        const Memory* ex = r.duplicate ? bank.findDuplicate(m.freqHz, m.mode) : nullptr;   // looked up again: earlier rows may have added it
        if (!ex || rule == MergeRule::KeepBoth) {
            if (bank.add(std::move(m), DupPolicy::Keep).what == MemoryAdd::Full) { res.left++; break; }
            res.added++;
            continue;
        }
        switch (rule) {
            case MergeRule::KeepExisting: res.left++; break;
            case MergeRule::Replace: {
                Memory n = m;
                n.favourite = ex->favourite || m.favourite;
                if (n.group.empty()) n.group = ex->group;
                bank.update(ex->id, n);
                res.merged++;
                break;
            }
            default: {   // FillEmpty
                Memory n = *ex;
                if (n.name.empty()) n.name = m.name;
                if (n.notes.empty()) n.notes = m.notes;
                if (n.group.empty()) n.group = m.group;
                if (n.bandwidthHz <= 0) n.bandwidthHz = m.bandwidthHz;
                if (!n.colour) n.colour = m.colour;
                if (!n.hasGain && m.hasGain) { n.hasGain = true; n.lna = m.lna; n.vga = m.vga; n.amp = m.amp; n.gainDb = m.gainDb; }
                for (const auto& kv : m.options) n.options.emplace(kv);
                if (!m.tags.empty()) {   // join without repeating a tag
                    std::string joined = n.tags;
                    size_t a = 0;
                    while (a <= m.tags.size()) {
                        size_t b = m.tags.find(',', a);
                        if (b == std::string::npos) b = m.tags.size();
                        const std::string t = trim(m.tags.substr(a, b - a));
                        const std::string have = "," + lower(joined) + ",";
                        std::string flat;
                        for (char c : have) if (c != ' ') flat += c;
                        if (!t.empty() && flat.find("," + lower(t) + ",") == std::string::npos) joined += (joined.empty() ? "" : ", ") + t;
                        a = b + 1;
                    }
                    n.tags = joined;
                }
                n.favourite = ex->favourite || m.favourite;
                bank.update(ex->id, n);
                res.merged++;
            }
        }
    }
    return res;
}

// ---- My lists
void ListSet::sync(const MemoryBank& bank) {
    const std::vector<std::string> g = bank.groups();
    for (const std::string& n : g) {
        bool have = false;
        for (const ListInfo& l : v_) have |= l.name == n;
        if (!have) v_.push_back({n, true});
    }
    if (!keepEmpty_) v_.erase(std::remove_if(v_.begin(), v_.end(), [&](const ListInfo& l) { return std::find(g.begin(), g.end(), l.name) == g.end(); }), v_.end());
}
bool ListSet::enabled(const std::string& name) const {
    if (name.empty()) return true;
    for (const ListInfo& l : v_) if (l.name == name) return l.enabled;
    return true;
}
bool ListSet::setEnabled(const std::string& name, bool on) {
    for (ListInfo& l : v_) if (l.name == name) { l.enabled = on; return true; }
    return false;
}
bool ListSet::move(size_t from, size_t to) {
    if (from >= v_.size() || to >= v_.size()) return false;
    ListInfo x = v_[from];
    v_.erase(v_.begin() + (long)from);
    v_.insert(v_.begin() + (long)to, x);
    return true;
}
bool ListSet::rename(MemoryBank& bank, const std::string& from, const std::string& to) {
    if (from.empty() || to.empty() || from == to) return false;
    size_t idx = v_.size();
    for (size_t i = 0; i < v_.size(); i++) { if (v_[i].name == to) return false; if (v_[i].name == from) idx = i; }
    for (const Memory& m : bank.all()) if (m.group == from) { Memory c = m; c.group = to; bank.update(m.id, c); }
    if (idx < v_.size()) v_[idx].name = to;
    return true;
}
std::vector<size_t> ListSet::visible(const MemoryBank& bank, const MemoryQuery& q) const {
    const std::vector<size_t> hit = bank.search(q);
    std::vector<size_t> out;
    auto rank = [&](const std::string& g) { if (g.empty()) return v_.size() + 2; for (size_t i = 0; i < v_.size(); i++) if (v_[i].name == g) return i; return v_.size() + 1; };
    for (size_t i : hit) if (enabled(bank.all()[i].group)) out.push_back(i);
    std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) { return rank(bank.all()[a].group) < rank(bank.all()[b].group); });
    return out;
}
std::vector<size_t> ListSet::quickTune(const MemoryBank& bank, size_t n) const {
    MemoryQuery q;
    q.favouritesOnly = true;
    std::vector<size_t> v = visible(bank, q);
    if (v.size() > n) v.resize(n);
    return v;
}
std::string ListSet::toJson() const {
    std::string o = "{\"format\":\"onair-lists\",\"version\":1,\"lists\":[";
    for (size_t i = 0; i < v_.size(); i++) o += std::string(i ? "," : "") + "{\"name\":" + mj::quote(v_[i].name) + ",\"enabled\":" + (v_[i].enabled ? "true" : "false") + "}";
    return o + "]}\n";
}
bool ListSet::fromJson(const std::string& text) {
    Value root; std::string err;
    mj::parse(text, root, err);
    const Value* a = root.get("lists");
    if (!a || a->kind != Value::Arr) return false;
    v_.clear();
    for (const Value& e : a->arr) {
        const Value* n = e.get("name");
        const Value* en = e.get("enabled");
        if (!n || n->kind != Value::Str || n->str.empty()) continue;
        v_.push_back({n->str, !(en && en->kind == Value::Bool && !en->b)});
    }
    return true;
}

// ---- built-in lists
const std::vector<BuiltinList>& builtinLists() {
    static const std::vector<BuiltinList> l = {
        {"Broadcast radio", "FM and AM broadcast bands and well-known services", {"broadcast"}},
        {"Airband", "Civil aviation: emergency, ATIS, air-to-air, ACARS, ADS-B", {"aviation"}},
        {"Marine", "VHF marine channels, AIS, DSC, NAVTEX", {"marine"}},
        {"Weather", "Weather radio, weather fax, weather satellites", {"weather"}},
        {"Rail", "Railway radio services", {"railway"}},
        {"Emergency services", "Public band plans of public-safety services only", {"public-safety"}},
        {"Ham band plans", "Amateur radio calling frequencies, beacons and digital modes", {"amateur"}},
        {"ISM and licence-free", "ISM bands, PMR446, CB, Wi-Fi", {"ism", "pmr", "cb", "wifi"}},
    };
    return l;
}
const std::vector<std::string>& builtinRegions() {
    static const std::vector<std::string> r = [] {
        std::vector<std::string> v = freqDistinct(builtinFrequencies(), 1);
        v.erase(std::remove(v.begin(), v.end(), std::string("GLOBAL")), v.end());
        v.insert(v.begin(), "GLOBAL");
        return v;
    }();
    return r;
}
std::vector<Memory> builtinListMemories(const std::string& listName, const std::string& region) {
    std::vector<Memory> out;
    for (const BuiltinList& l : builtinLists()) {
        if (l.name != listName) continue;
        const auto& all = builtinFrequencies();
        for (const std::string& cat : l.categories) {
            FreqQuery q;
            q.category = cat;
            q.region = region;
            for (size_t i : freqSearch(all, q)) {
                Memory m = freqToMemory(all[i]);
                m.group = l.name;
                out.push_back(std::move(m));
            }
        }
    }
    return out;
}

} // namespace dect2
