// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/common/tg_dir.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace dect2 {
namespace {

std::string norm(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += (char)std::tolower(c);
    return o;
}
std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
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
bool parseId(const std::string& s0, uint32_t& v) {
    const std::string s = trim(s0);
    if (s.empty() || s.size() > 10) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    const unsigned long long x = std::strtoull(s.c_str(), nullptr, 10);
    if (x > 0xFFFFFFFFull) return false;
    v = (uint32_t)x; return true;
}
std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n\r;\t") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += "\"\""; else if (c != '\r') o += c; }
    return o + "\"";
}
bool kindFromText(const std::string& t, TgKind& k, bool& any) {
    const std::string n = norm(t);
    any = false;
    if (n == "dmr") { k = TgKind::Dmr; return true; }
    if (n == "tetra") { k = TgKind::Tetra; return true; }
    if (n == "p25") { k = TgKind::P25; return true; }
    if (n == "any" || n.empty() || n == "all") { any = true; return true; }
    return false;
}

}  // namespace

const char* tgKindName(TgKind k) { return k == TgKind::Tetra ? "TETRA" : k == TgKind::P25 ? "P25" : "DMR"; }

size_t TgDir::import(const std::string& csvIn, TgKind panelKind, size_t* skipped) {
    size_t added = 0, skip = 0;
    std::string csv = csvIn;
    if (csv.size() >= 3 && (unsigned char)csv[0] == 0xEF && (unsigned char)csv[1] == 0xBB && (unsigned char)csv[2] == 0xBF) csv.erase(0, 3);
    const size_t nl = csv.find('\n');
    const std::string first = csv.substr(0, nl);
    char sep = ','; size_t best = 0;
    for (char c : {',', ';', '\t'}) { const size_t n = (size_t)std::count(first.begin(), first.end(), c); if (n > best) { best = n; sep = c; } }
    int cKind = -1, cId = 0, cName = 1, cGroup = 2, cNote = 3, cDesc = -1, cCat = -1;
    bool rr = false;   // RadioReference-style names: Alpha Tag is the name, Tag the group
    size_t p = 0; std::vector<std::string> f; bool firstRow = true;
    uint32_t tmp;
    while (nextRecord(csv, p, sep, f)) {
        { bool blank = true; for (const auto& c : f) if (!trim(c).empty()) { blank = false; break; } if (blank) continue; }
        if (!f.empty() && !f[0].empty() && f[0][0] == '#') continue;
        if (firstRow) {
            firstRow = false;
            bool header = false;
            for (const auto& c : f) if (!trim(c).empty() && !parseId(c, tmp)) { header = true; break; }
            if (header && !parseId(f[0], tmp)) {
                cKind = cId = cName = cGroup = cNote = cDesc = cCat = -1;
                int cAlpha = -1, cTag = -1, cName2 = -1;
                for (size_t i = 0; i < f.size(); i++) {
                    const std::string n = norm(f[i]);
                    if (n == "kind" || n == "system" || n == "type" || n == "network") cKind = (int)i;
                    else if (n == "id" || n == "decimal" || n == "dec" || n == "tgid" || n == "talkgroup" || n == "tg" || n == "gssi" || n == "groupid") { if (cId < 0) cId = (int)i; }
                    else if (n == "alphatag" || n == "alpha") { cAlpha = (int)i; rr = true; }
                    else if (n == "name" || n == "label") cName2 = (int)i;
                    else if (n == "description" || n == "descr") cDesc = (int)i;
                    else if (n == "tag") cTag = (int)i;
                    else if (n == "group") cGroup = (int)i;
                    else if (n == "category") cCat = (int)i;
                    else if (n == "note" || n == "notes" || n == "comment") cNote = (int)i;
                }
                cName = cAlpha >= 0 ? cAlpha : cName2 >= 0 ? cName2 : cDesc;
                if (cGroup < 0) cGroup = cTag >= 0 ? cTag : cCat;
                if (cId < 0 || cName < 0) { if (skipped) *skipped = 1; return 0; }
                if (cNote < 0 && cDesc >= 0 && cDesc != cName) cNote = cDesc;
                continue;
            }
        }
        auto at = [&](int i) { return i >= 0 && (size_t)i < f.size() ? trim(f[(size_t)i]) : std::string(); };
        TgDirEntry e; e.kind = panelKind;
        if (cKind >= 0) {
            bool any = false; TgKind k = panelKind;
            if (!kindFromText(at(cKind), k, any)) { skip++; continue; }
            if (!any) e.kind = k;
        }
        if (!parseId(at(cId), e.id)) { skip++; continue; }
        e.name = at(cName); e.group = at(cGroup); e.note = at(cNote);
        if (e.name.empty() && cDesc >= 0) e.name = at(cDesc);
        if (e.name.empty()) { skip++; continue; }
        m_[{(int)e.kind, e.id}] = e; added++;
    }
    (void)rr;
    if (skipped) *skipped = skip;
    return added;
}

std::string TgDir::exportCsv(bool allKinds, TgKind only) const {
    std::string o = "Kind,Id,Name,Group,Note\n";
    for (const auto& kv : m_) {
        if (!allKinds && kv.second.kind != only) continue;
        const TgDirEntry& e = kv.second;
        o += std::string(tgKindName(e.kind)) + "," + std::to_string(e.id) + "," + csvField(e.name) + "," + csvField(e.group) + "," + csvField(e.note) + "\n";
    }
    return o;
}

bool TgDir::find(TgKind k, uint32_t id, TgDirEntry& out) const {
    auto it = m_.find({(int)k, id});
    if (it == m_.end()) return false;
    out = it->second; return true;
}
std::string TgDir::label(TgKind k, uint32_t id) const { TgDirEntry e; return find(k, id, e) ? e.name : std::string(); }
void TgDir::set(const TgDirEntry& e) { if (trim(e.name).empty()) m_.erase({(int)e.kind, e.id}); else m_[{(int)e.kind, e.id}] = e; }
bool TgDir::erase(TgKind k, uint32_t id) { return m_.erase({(int)k, id}) > 0; }
size_t TgDir::count(TgKind k) const { size_t n = 0; for (const auto& kv : m_) if (kv.second.kind == k) n++; return n; }

std::vector<TgDirEntry> TgDir::entries(TgKind k, const std::string& filter) const {
    std::vector<std::string> words;
    { std::istringstream ws(lower(filter)); std::string w; while (ws >> w) words.push_back(w); }
    std::vector<TgDirEntry> v;
    for (const auto& kv : m_) {
        if (kv.second.kind != k) continue;
        const TgDirEntry& e = kv.second;
        const std::string hay = std::to_string(e.id) + " " + lower(e.name) + " " + lower(e.group) + " " + lower(e.note);
        bool ok = true;
        for (const auto& w : words) if (hay.find(w) == std::string::npos) { ok = false; break; }
        if (ok) v.push_back(e);
    }
    return v;
}

namespace {
std::mutex gMu; TgDir gDir; std::atomic<uint64_t> gRev{1};
}
std::string tgLabel(TgKind k, uint32_t id) { std::lock_guard<std::mutex> lk(gMu); return gDir.label(k, id); }
TgDir tgSnapshot() { std::lock_guard<std::mutex> lk(gMu); return gDir; }
void tgEdit(const std::function<void(TgDir&)>& f) { { std::lock_guard<std::mutex> lk(gMu); f(gDir); } gRev++; }
uint64_t tgRevision() { return gRev.load(); }

}  // namespace dect2
