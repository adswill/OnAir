// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Callsign prefix -> DXCC entity: see ham_dxcc.h.
#include "dect2/ham/ham_dxcc.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace dect2::ham {

namespace {
const char* kBuiltin =
#include "ham_dxcc_data.inc"
    ;
std::string upper(std::string s) { for (char& c : s) c = (char)std::toupper((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
bool hasDigit(const std::string& s) { for (char c : s) if (std::isdigit((unsigned char)c)) return true; return false; }
}  // namespace

const DxccDb& DxccDb::builtin() {
    static const DxccDb db = [] { DxccDb d; std::string err; d.loadCty(kBuiltin, err); return d; }();
    return db;
}

bool DxccDb::loadCty(const std::string& text, std::string& err) {
    DxccDb out;
    if (text.size() > (32u << 20)) { err = "the file is too large for a country file"; return false; }
    size_t p = (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) ? 3 : 0;
    while (p < text.size()) {
        // the header: 8 fields ending with ':'
        std::string f[8];
        int got = 0;
        while (got < 8 && p < text.size()) {
            const size_t c = text.find(':', p);
            if (c == std::string::npos) break;
            f[got++] = trim(text.substr(p, c - p));
            p = c + 1;
        }
        if (got < 8) { if (trim(text.substr(std::min(p, text.size()))).empty() || got == 0) break; err = "a country header is cut short"; return false; }
        const size_t semi = text.find(';', p);
        if (semi == std::string::npos) { err = "the prefix list of " + f[0] + " has no ';'"; return false; }
        const std::string list = text.substr(p, semi - p);
        p = semi + 1;
        DxccEntity e;
        e.name = f[0]; e.cqz = std::atoi(f[1].c_str()); e.ituz = std::atoi(f[2].c_str()); e.cont = upper(f[3]);
        e.lat = std::atof(f[4].c_str()); e.lon = -std::atof(f[5].c_str());   // cty.dat: west positive
        e.tz = std::atof(f[6].c_str());
        e.primary = upper(f[7]);
        if (!e.primary.empty() && e.primary[0] == '*') { e.primary.erase(0, 1); e.wae = true; }
        if (e.primary.empty()) { err = "the country " + f[0] + " has no primary prefix"; return false; }
        const int idx = (int)out.ents_.size();
        out.ents_.push_back(e);
        // the primary prefix always counts
        std::vector<std::string> toks = {e.primary};
        {
            size_t q = 0;
            while (q <= list.size()) {
                size_t c = list.find(',', q);
                if (c == std::string::npos) c = list.size();
                const std::string t = trim(list.substr(q, c - q));
                if (!t.empty()) toks.push_back(t);
                q = c + 1;
            }
        }
        for (std::string t : toks) {
            if (t.size() == 11 && t.compare(0, 3, "VER") == 0 && t.find_first_not_of("0123456789", 3) == std::string::npos) { out.version_ = t.substr(3); continue; }
            if (t[0] == '#') { out.ents_[(size_t)idx].adif = std::atoi(t.c_str() + 1); continue; }
            Rule r; r.ent = idx; r.cqz = e.cqz; r.ituz = e.ituz; r.cont = e.cont;
            // the overrides after the prefix: (cq) [itu] <lat/lon> {cont} ~utc~
            std::string base;
            for (size_t i = 0; i < t.size(); i++) {
                const char ch = t[i];
                auto closeOf = [&](char cl) { const size_t e2 = t.find(cl, i + 1); return e2 == std::string::npos ? t.size() : e2; };
                if (ch == '(') { const size_t e2 = closeOf(')'); r.cqz = std::atoi(t.substr(i + 1, e2 - i - 1).c_str()); i = e2; }
                else if (ch == '[') { const size_t e2 = closeOf(']'); r.ituz = std::atoi(t.substr(i + 1, e2 - i - 1).c_str()); i = e2; }
                else if (ch == '<') {
                    const size_t e2 = closeOf('>');
                    const std::string v = t.substr(i + 1, e2 - i - 1);
                    const size_t sl = v.find('/');
                    if (sl != std::string::npos) { r.hasPos = true; r.lat = std::atof(v.c_str()); r.lon = -std::atof(v.c_str() + sl + 1); }
                    i = e2;
                }
                else if (ch == '{') { const size_t e2 = closeOf('}'); r.cont = upper(t.substr(i + 1, e2 - i - 1)); i = e2; }
                else if (ch == '~') i = closeOf('~');
                else base += (char)std::toupper((unsigned char)ch);
            }
            if (base.empty()) continue;
            if (base[0] == '=') { out.exact_[base.substr(1)] = r; continue; }
            // '?': any letter (at most two per prefix, so that a garbage file cannot blow up the table)
            std::vector<std::string> ex = {base};
            int qm = 0;
            for (size_t k = 0; k < base.size(); k++) {
                if (base[k] != '?') continue;
                if (++qm > 2) { ex.clear(); break; }
                std::vector<std::string> nx;
                for (const auto& s : ex) for (char L = 'A'; L <= 'Z'; L++) { std::string s2 = s; s2[k] = L; nx.push_back(s2); }
                ex.swap(nx);
            }
            for (const auto& s : ex) { out.pre_[s] = r; out.maxLen_ = std::max(out.maxLen_, s.size()); }
        }
    }
    if (out.ents_.empty()) { err = "no countries in the file"; return false; }
    *this = std::move(out);
    err.clear();
    return true;
}

void DxccDb::fill(DxccHit& h, const Rule& r) const {
    h.e = &ents_[(size_t)r.ent]; h.cqz = r.cqz; h.ituz = r.ituz; h.cont = r.cont;
    h.lat = r.hasPos ? r.lat : h.e->lat; h.lon = r.hasPos ? r.lon : h.e->lon;
}

const DxccEntity* DxccDb::byAdif(int adif) const {
    for (const auto& e : ents_) if (e.adif == adif && adif > 0) return &e;
    return nullptr;
}

std::string prefixPart(const std::string& callIn) {
    std::vector<std::string> parts;
    {
        const std::string c = upper(trim(callIn));
        size_t p = 0;
        while (p <= c.size()) {
            size_t e = c.find('/', p);
            if (e == std::string::npos) e = c.size();
            if (e > p) parts.push_back(c.substr(p, e - p));
            p = e + 1;
        }
    }
    if (parts.empty()) return "";
    static const char* kSuffix[] = {"P", "M", "QRP", "A", "LH", "B", "R", "J", "AE", "AG", "KT", "N", "T", "X", "Q", "C"};
    std::vector<std::string> keep;
    for (const auto& s : parts) {
        if (s == "MM" || s == "AM") return "";
        bool suf = false;
        for (const char* k : kSuffix) if (s == k) suf = true;
        if (!suf || parts.size() == 1) keep.push_back(s);
    }
    if (keep.empty()) return "";
    if (keep.size() == 1) return keep[0];
    // a call and one more part: a single digit moves the call area (K1ABC/4 -> K4); otherwise the shorter part is the prefix
    std::string a = keep[0], b = keep[1];
    if (b.size() == 1 && std::isdigit((unsigned char)b[0])) {
        size_t d = 0;
        while (d < a.size() && !std::isdigit((unsigned char)a[d])) d++;
        return a.substr(0, d) + b;
    }
    if (a.size() == 1 && std::isdigit((unsigned char)a[0])) std::swap(a, b);
    if (!hasDigit(a) && hasDigit(b)) return a;   // DL/LY1ABC
    if (!hasDigit(b) && hasDigit(a)) return b;   // LY1ABC/DL
    return a.size() <= b.size() ? a : b;
}

DxccHit DxccDb::lookup(const std::string& call) const {
    DxccHit h;
    const std::string c = upper(trim(call));
    if (c.empty()) return h;
    auto ex = exact_.find(c);
    if (ex != exact_.end()) { fill(h, ex->second); h.prefix = "=" + c; return h; }
    const std::string part = prefixPart(c);
    if (part.empty()) return h;
    if (part != c) {   // W1AW/P: the exact entry of the plain call still counts
        auto ex2 = exact_.find(part);
        if (ex2 != exact_.end()) { fill(h, ex2->second); h.prefix = "=" + part; return h; }
    }
    for (size_t n = std::min(part.size(), maxLen_); n > 0; n--) {
        auto it = pre_.find(part.substr(0, n));
        if (it == pre_.end()) continue;
        fill(h, it->second); h.prefix = it->first;
        return h;
    }
    return h;
}

}  // namespace dect2::ham
