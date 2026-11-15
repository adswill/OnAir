// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tetra/tetra_names.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace dect2 {
namespace {

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string norm(const std::string& s) { std::string o; for (unsigned char c : s) if (std::isalnum(c)) o += (char)std::tolower(c); return o; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
bool parseId(const std::string& s0, uint32_t& v) {
    const std::string s = trim(s0);
    if (s.empty() || s.size() > 10) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    const unsigned long long x = std::strtoull(s.c_str(), nullptr, 10);
    if (x > 0xFFFFFFull * 256 + 255 || x == 0) return false;   // ISSI / GSSI are 24 bit; 0 is no id
    if (x > 0xFFFFFFull) return false;
    v = (uint32_t)x; return true;
}
bool parseOpt(const std::string& s0, int lo, int hi, int& v) {   // empty = -1 (any); false: not a number in range
    const std::string s = trim(s0);
    if (s.empty()) { v = -1; return true; }
    if (s.size() > 5) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    const int x = std::atoi(s.c_str());
    if (x < lo || x > hi) return false;
    v = x; return true;
}
bool kindFromText(const std::string& t, TetraIdKind& k, bool& any) {
    const std::string n = norm(t);
    any = false;
    if (n == "issi" || n == "ssi" || n == "terminal" || n == "radio" || n == "unit") { k = TetraIdKind::Issi; return true; }
    if (n == "gssi" || n == "talkgroup" || n == "tg" || n == "group") { k = TetraIdKind::Gssi; return true; }
    if (n.empty() || n == "any" || n == "all") { any = true; return true; }
    return false;
}
std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n\r;\t") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += "\"\""; else if (c != '\r' && c != '\n') o += c; else o += ' '; }
    return o + "\"";
}
bool nextRecord(const std::string& t, size_t& p, char sep, std::vector<std::string>& f) {
    f.clear();
    if (p >= t.size()) return false;
    std::string cur; bool q = false, any = false;
    while (p < t.size()) {
        const char c = t[p++];
        if (q) {
            if (c == '"') { if (p < t.size() && t[p] == '"') { cur += '"'; p++; } else q = false; }
            else cur += c;
        } else if (c == '"') { q = true; any = true; }
        else if (c == sep) { f.push_back(cur); cur.clear(); any = true; }
        else if (c == '\n') break;
        else if (c == '\r') {}
        else { cur += c; any = true; }
    }
    if (any || !cur.empty()) f.push_back(cur);
    return true;
}
std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') { s = s.substr(1, s.size() - 2); }
    return trim(s);
}

// One headerless line: [ISSI|GSSI][ :=,;tab space ] id  sep  name [sep notes]
bool parseLine(const std::string& line0, TetraIdKind defKind, TetraNameEntry& e) {
    const std::string line = trim(line0);
    if (line.empty() || line[0] == '#') return false;
    size_t p = 0;
    e = TetraNameEntry(); e.kind = defKind;
    // an optional type word first
    size_t w = 0; while (w < line.size() && std::isalpha((unsigned char)line[w])) w++;
    if (w > 0) {
        TetraIdKind k; bool any = false;
        if (!kindFromText(line.substr(0, w), k, any) || any) return false;   // a word that is not a type: not an id line
        e.kind = k; p = w;
        while (p < line.size() && (std::isspace((unsigned char)line[p]) || line[p] == ':' || line[p] == '=' || line[p] == ',' || line[p] == ';')) p++;
    }
    size_t d = p; while (d < line.size() && std::isdigit((unsigned char)line[d])) d++;
    if (d == p || !parseId(line.substr(p, d - p), e.id)) return false;
    size_t q = d;
    while (q < line.size() && (line[q] == ' ' || line[q] == '\t')) q++;
    char sep = 0;
    if (q < line.size() && (line[q] == ';' || line[q] == ',' || line[q] == '=' || line[q] == ':' || line[q] == '\t')) { sep = line[q]; q++; }
    else if (q == d) return false;   // digits directly followed by something that is no separator ("12ab")
    std::string rest = line.substr(q);
    if (sep == '=' || sep == ':') { e.name = unquote(rest); return !e.name.empty(); }
    std::vector<std::string> f; size_t pos = 0;
    const char s2 = sep ? sep : ' ';
    if (s2 == ' ') { f.push_back(rest); }
    else { std::string r = rest + "\n"; while (nextRecord(r, pos, s2, f) && f.empty()) {} }
    if (f.empty()) return false;
    e.name = trim(f[0]);
    if (f.size() > 1) e.notes = trim(f[1]);
    return !e.name.empty();
}

}  // namespace

const char* tetraIdKindName(TetraIdKind k) { return k == TetraIdKind::Gssi ? "GSSI" : "ISSI"; }

size_t TetraNames::import(const std::string& textIn, TetraIdKind defKind, size_t* skipped) {
    size_t added = 0, skip = 0;
    std::string text = textIn;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.erase(0, 3);
    // is there a header row (the first row that is not blank or a comment)?
    size_t ls = 0; std::string first;
    while (ls < text.size()) {
        size_t le = text.find('\n', ls); if (le == std::string::npos) le = text.size();
        const std::string l = trim(text.substr(ls, le - ls)); ls = le + 1;
        if (!l.empty() && l[0] != '#') { first = l; break; }
    }
    char sep = ','; size_t best = 0;
    for (char c : {',', ';', '\t'}) { const size_t n = (size_t)std::count(first.begin(), first.end(), c); if (n > best) { best = n; sep = c; } }
    int cType = -1, cId = -1, cName = -1, cNotes = -1, cMcc = -1, cMnc = -1, cCc = -1, cSite = -1;
    bool header = false;
    {
        std::vector<std::string> f; size_t p = 0; std::string one = first + "\n";
        if (nextRecord(one, p, sep, f)) {
            bool idCol = false, nameCol = false;
            for (size_t i = 0; i < f.size(); i++) {
                const std::string n = norm(f[i]);
                if (n == "type" || n == "kind") cType = (int)i;
                else if (n == "id" || n == "issi" || n == "gssi" || n == "ssi" || n == "number") { if (cId < 0) { cId = (int)i; idCol = true; } }
                else if (n == "name" || n == "label" || n == "alias" || n == "alphatag") { if (cName < 0) { cName = (int)i; nameCol = true; } }
                else if (n == "notes" || n == "note" || n == "comment" || n == "description") cNotes = (int)i;
                else if (n == "mcc") cMcc = (int)i;
                else if (n == "mnc") cMnc = (int)i;
                else if (n == "colourcode" || n == "colorcode" || n == "cc") cCc = (int)i;
                else if (n == "site" || n == "location" || n == "cell") cSite = (int)i;
            }
            header = idCol && nameCol;
        }
    }
    size_t p = 0; std::vector<std::string> f; bool firstRow = true;
    if (header) {
        while (nextRecord(text, p, sep, f)) {
            { bool blank = true; for (const auto& c : f) if (!trim(c).empty()) { blank = false; break; } if (blank) continue; }
            if (!f.empty() && !f[0].empty() && trim(f[0])[0] == '#') continue;
            if (firstRow) { firstRow = false; continue; }   // the header
            auto at = [&](int i) { return i >= 0 && (size_t)i < f.size() ? trim(f[(size_t)i]) : std::string(); };
            TetraNameEntry e; e.kind = defKind;
            if (cType >= 0) { bool any = false; TetraIdKind k = defKind; if (!kindFromText(at(cType), k, any)) { skip++; continue; } if (!any) e.kind = k; }
            if (!parseId(at(cId), e.id)) { skip++; continue; }
            e.name = at(cName); e.notes = at(cNotes); e.site = at(cSite);
            if (e.name.empty() || !parseOpt(at(cMcc), 0, 999, e.mcc) || !parseOpt(at(cMnc), 0, 9999, e.mnc) || !parseOpt(at(cCc), 0, 63, e.colourCode)) { skip++; continue; }
            if ((e.mcc < 0) != (e.mnc < 0)) { skip++; continue; }   // a network needs both
            m_[{(int)e.kind, e.id, e.mcc, e.mnc}] = e; added++;
        }
    } else {
        std::istringstream in(text); std::string line;
        while (std::getline(in, line)) {
            if (trim(line).empty() || trim(line)[0] == '#') continue;
            TetraNameEntry e;
            if (!parseLine(line, defKind, e)) { skip++; continue; }
            m_[{(int)e.kind, e.id, -1, -1}] = e; added++;
        }
    }
    if (skipped) *skipped = skip;
    return added;
}

std::string TetraNames::exportCsv(bool allKinds, TetraIdKind only) const {
    std::string o = "type,id,name,notes,mcc,mnc,colour_code,site\n";
    for (const auto& kv : m_) {
        const TetraNameEntry& e = kv.second;
        if (!allKinds && e.kind != only) continue;
        o += std::string(tetraIdKindName(e.kind)) + "," + std::to_string(e.id) + "," + csvField(e.name) + "," + csvField(e.notes) + "," +
             (e.mcc >= 0 ? std::to_string(e.mcc) : "") + "," + (e.mnc >= 0 ? std::to_string(e.mnc) : "") + "," +
             (e.colourCode >= 0 ? std::to_string(e.colourCode) : "") + "," + csvField(e.site) + "\n";
    }
    return o;
}

void TetraNames::set(const TetraNameEntry& e) {
    const auto key = std::make_tuple((int)e.kind, e.id, e.mcc, e.mnc);
    if (trim(e.name).empty()) m_.erase(key); else m_[key] = e;
}
bool TetraNames::erase(TetraIdKind k, uint32_t id, int mcc, int mnc) { return m_.erase(std::make_tuple((int)k, id, mcc, mnc)) > 0; }
size_t TetraNames::count(TetraIdKind k) const { size_t n = 0; for (const auto& kv : m_) if (kv.second.kind == k) n++; return n; }

bool TetraNames::find(TetraIdKind k, uint32_t id, int mcc, int mnc, TetraNameEntry& out) const {
    if (mcc >= 0 && mnc >= 0) { auto it = m_.find(std::make_tuple((int)k, id, mcc, mnc)); if (it != m_.end()) { out = it->second; return true; } }
    auto it = m_.find(std::make_tuple((int)k, id, -1, -1));
    if (it != m_.end()) { out = it->second; return true; }
    if (mcc < 0 || mnc < 0) {   // network not known: the first entry of the id
        auto lo = m_.lower_bound(std::make_tuple((int)k, id, -1, -1));
        if (lo != m_.end() && std::get<0>(lo->first) == (int)k && std::get<1>(lo->first) == id) { out = lo->second; return true; }
    }
    return false;
}
std::string TetraNames::label(TetraIdKind k, uint32_t id, int mcc, int mnc) const { TetraNameEntry e; return find(k, id, mcc, mnc, e) ? e.name : std::string(); }

std::vector<TetraNameEntry> TetraNames::entries(TetraIdKind k, const std::string& filter) const {
    std::vector<std::string> words; { std::istringstream is(lower(filter)); std::string w; while (is >> w) words.push_back(w); }
    std::vector<TetraNameEntry> v;
    for (const auto& kv : m_) {
        const TetraNameEntry& e = kv.second;
        if (e.kind != k) continue;
        const std::string hay = lower(std::to_string(e.id) + " " + e.name + " " + e.notes + " " + e.site);
        bool ok = true;
        for (const auto& w : words) if (hay.find(w) == std::string::npos) { ok = false; break; }
        if (ok) v.push_back(e);
    }
    return v;
}

namespace {
std::filesystem::path u8p(const std::string& s) { return std::filesystem::path(reinterpret_cast<const char8_t*>(s.c_str())); }
std::mutex gMu; TetraNames gBook; std::atomic<uint64_t> gRev{1};
}
std::string tetraNameLabel(TetraIdKind k, uint32_t id, int mcc, int mnc) { std::lock_guard<std::mutex> lk(gMu); return gBook.label(k, id, mcc, mnc); }
std::string tetraNameWithId(TetraIdKind k, uint32_t id, int mcc, int mnc) {
    const std::string n = tetraNameLabel(k, id, mcc, mnc);
    return n.empty() ? std::to_string(id) : std::to_string(id) + "  " + n;
}
TetraNames tetraNamesSnapshot() { std::lock_guard<std::mutex> lk(gMu); return gBook; }
void tetraNamesEdit(const std::function<void(TetraNames&)>& f) { { std::lock_guard<std::mutex> lk(gMu); f(gBook); } gRev++; }
uint64_t tetraNamesRevision() { return gRev.load(); }

bool tetraNamesLoadFile(const std::string& path, TetraNames& into, size_t* skipped) {
    std::ifstream f(u8p(path), std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf();
    into.import(ss.str(), TetraIdKind::Issi, skipped);
    return true;
}
bool tetraNamesSaveFile(const std::string& path, const TetraNames& from) {
    std::error_code ec;
    const auto p = u8p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << from.exportCsv(true);
    return (bool)f;
}

}  // namespace dect2
