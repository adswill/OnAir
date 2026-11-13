// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Trunking: the talkgroup list (names, tags, priority, lockout, scan lists, CSV) and the call log (CSV export).
#include "dect2/p25/trunk.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace dect2 {
namespace trunk {

namespace {

std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}

// one CSV record starting at pos; returns false at the end. Handles quotes, "" escapes and CR / LF / CRLF.
bool csvRecord(const std::string& t, size_t& pos, std::vector<std::string>& f) {
    f.clear();
    if (pos >= t.size()) return false;
    std::string cur;
    bool q = false, any = false;
    while (pos < t.size()) {
        const char c = t[pos++];
        any = true;
        if (q) {
            if (c == '"') { if (pos < t.size() && t[pos] == '"') { cur += '"'; pos++; } else q = false; }
            else cur += c;
        } else if (c == '"' && cur.empty()) q = true;
        else if (c == ',') { f.push_back(cur); cur.clear(); }
        else if (c == '\r' || c == '\n') {
            if (c == '\r' && pos < t.size() && t[pos] == '\n') pos++;
            f.push_back(cur);
            return true;
        } else cur += c;
    }
    if (any) f.push_back(cur);
    return any;
}

std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}

bool parseInt(const std::string& s, int base, int& out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    char* e = nullptr;
    const long v = strtol(t.c_str(), &e, base);
    if (*e || v < 0 || v > 0xFFFFFF) return false;
    out = (int)v;
    return true;
}

bool truthy(const std::string& s) { const std::string t = lower(trim(s)); return t == "1" || t == "y" || t == "yes" || t == "true" || t == "x"; }

enum Col { cDec, cHex, cAlpha, cMode, cDesc, cTag, cCat, cPrio, cLock, cList, cListen, cCount };

int colOf(const std::string& h) {
    const std::string s = lower(trim(h));
    if (s == "decimal" || s == "dec" || s == "tgid" || s == "talkgroup" || s == "tg" || s == "id") return cDec;
    if (s == "hex") return cHex;
    if (s == "alpha tag" || s == "alpha" || s == "alphatag" || s == "name") return cAlpha;
    if (s == "mode") return cMode;
    if (s == "description" || s == "desc") return cDesc;
    if (s == "tag" || s == "service tag") return cTag;
    if (s == "category" || s == "group" || s == "agency" || s == "agency/category") return cCat;
    if (s == "priority" || s == "prio") return cPrio;
    if (s == "lockout" || s == "lockedout" || s == "avoid") return cLock;
    if (s == "list" || s == "scan list" || s == "scanlist") return cList;
    if (s == "listen") return cListen;
    return -1;
}

} // namespace

TgEntry* TalkgroupList::find(int id) { for (auto& e : v_) if (e.id == id) return &e; return nullptr; }
const TgEntry* TalkgroupList::find(int id) const { for (const auto& e : v_) if (e.id == id) return &e; return nullptr; }
TgEntry& TalkgroupList::upsert(int id) {
    if (TgEntry* e = find(id)) return *e;
    TgEntry n;
    n.id = id;
    v_.push_back(n);
    return v_.back();
}
bool TalkgroupList::remove(int id) {
    auto it = std::remove_if(v_.begin(), v_.end(), [&](const TgEntry& e) { return e.id == id; });
    const bool had = it != v_.end();
    v_.erase(it, v_.end());
    return had;
}

void TalkgroupList::enableList(const std::string& name, bool on) { enabled_[name] = on; }
bool TalkgroupList::listEnabled(const std::string& name) const { auto it = enabled_.find(name); return it != enabled_.end() && it->second; }
std::vector<std::string> TalkgroupList::lists() const {
    std::vector<std::string> o;
    for (const auto& e : v_) if (!e.list.empty() && std::find(o.begin(), o.end(), e.list) == o.end()) o.push_back(e.list);
    std::sort(o.begin(), o.end());
    return o;
}

TalkgroupList::Decision TalkgroupList::decide(int tg) const {
    Decision d;
    const TgEntry* e = find(tg);
    if (e) {
        d.priority = e->priority;
        if (e->lockout) { d.follow = false; d.reason = "locked out"; return d; }
    }
    if (scanOnly_) {
        bool anyList = false;
        for (const auto& kv : enabled_) anyList = anyList || kv.second;
        if (!e || !e->listen) { d.follow = false; d.reason = "not in the scan list"; return d; }
        if (anyList && (e->list.empty() || !listEnabled(e->list))) { d.follow = false; d.reason = "its list is off"; return d; }
    }
    return d;
}

int TalkgroupList::importCsv(const std::string& text, std::string* err, int* skipped, bool merge) {
    std::vector<std::string> f;
    size_t pos = 0;
    int idx[cCount];
    for (int& i : idx) i = -1;
    // a UTF-8 byte order mark
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) pos = 3;
    bool header = false;
    int taken = 0, bad = 0, line = 0;
    std::string msgs;
    std::vector<TgEntry> incoming;
    while (csvRecord(text, pos, f)) {
        line++;
        if (f.size() == 1 && trim(f[0]).empty()) continue;
        if (!header) {
            int found = 0;
            for (size_t i = 0; i < f.size(); i++) { const int c = colOf(f[i]); if (c >= 0 && idx[c] < 0) { idx[c] = (int)i; found++; } }
            if (idx[cDec] < 0 && idx[cHex] < 0) {
                // no recognisable header: the RadioReference order Decimal, Hex, Alpha Tag, Mode, Description, Tag, Category
                idx[cDec] = 0; idx[cHex] = 1; idx[cAlpha] = 2; idx[cMode] = 3; idx[cDesc] = 4; idx[cTag] = 5; idx[cCat] = 6;
                header = true;
            } else { header = true; continue; }
            (void)found;
        }
        auto get = [&](int c) { return idx[c] >= 0 && (size_t)idx[c] < f.size() ? trim(f[(size_t)idx[c]]) : std::string(); };
        int id = 0;
        bool ok = false;
        if (idx[cDec] >= 0 && !get(cDec).empty()) ok = parseInt(get(cDec), 10, id);
        else if (idx[cHex] >= 0 && !get(cHex).empty()) ok = parseInt(get(cHex), 16, id);
        if (!ok) {
            bad++;
            if (bad <= 3 && err) { char b[96]; snprintf(b, sizeof b, "line %d: no valid talkgroup number\n", line); msgs += b; }
            continue;
        }
        if (id <= 0 || id > 0xFFFF) { bad++; if (bad <= 3) { char b[96]; snprintf(b, sizeof b, "line %d: talkgroup %d is out of range\n", line, id); msgs += b; } continue; }
        TgEntry e;
        e.id = id; e.alpha = get(cAlpha); e.mode = get(cMode); e.desc = get(cDesc); e.tag = get(cTag); e.category = get(cCat); e.list = get(cList);
        int p = 0;
        if (!get(cPrio).empty() && parseInt(get(cPrio), 10, p) && p >= 0 && p <= 10) e.priority = p;
        e.lockout = truthy(get(cLock));
        e.listen = idx[cListen] < 0 || get(cListen).empty() ? true : truthy(get(cListen));
        incoming.push_back(e);
        taken++;
    }
    if (!merge) v_.clear();
    for (const TgEntry& e : incoming) {
        TgEntry& d = upsert(e.id);
        d = e;
    }
    if (err) *err = msgs;
    if (skipped) *skipped = bad;
    return taken;
}

std::string TalkgroupList::exportCsv() const {
    std::string o = "Decimal,Hex,Alpha Tag,Mode,Description,Tag,Category,Priority,Lockout,List,Listen\n";
    std::vector<const TgEntry*> s;
    for (const auto& e : v_) s.push_back(&e);
    std::sort(s.begin(), s.end(), [](const TgEntry* a, const TgEntry* b) { return a->id < b->id; });
    for (const TgEntry* e : s) {
        char h[16];
        snprintf(h, sizeof h, "%X", e->id);
        o += std::to_string(e->id) + "," + h + "," + csvField(e->alpha) + "," + csvField(e->mode) + "," + csvField(e->desc) + "," + csvField(e->tag) + "," + csvField(e->category) + "," +
             std::to_string(e->priority) + "," + (e->lockout ? "1" : "0") + "," + csvField(e->list) + "," + (e->listen ? "1" : "0") + "\n";
    }
    return o;
}

// ------------------------------------------------------------------------------------------------ call log

CallRecord* CallLog::byId(uint64_t id) {
    for (size_t i = v_.size(); i-- > 0;) if (v_[i].id == id) return &v_[i];
    return nullptr;
}
CallRecord& CallLog::add(const CallRecord& r) {
    v_.push_back(r);
    v_.back().id = next_++;
    return v_.back();
}
void CallLog::trim(size_t keep) { if (v_.size() > keep) v_.erase(v_.begin(), v_.end() - (long)keep); }

std::string CallLog::exportCsv(const TalkgroupList* names) const {
    std::string o = "Time UTC,Talkgroup,Name,Source,Duration s,Frequency MHz,Slot,Kind,Encrypted,Emergency,Followed,Note,Audio\n";
    for (const CallRecord& r : v_) {
        char t[32] = "";
        const time_t tt = (time_t)r.wallTime;
        struct tm g;
#ifdef _WIN32
        gmtime_s(&g, &tt);
#else
        gmtime_r(&tt, &g);
#endif
        strftime(t, sizeof t, "%Y-%m-%d %H:%M:%S", &g);
        const TgEntry* e = names && r.tg >= 0 ? names->find(r.tg) : nullptr;
        char fr[32] = "";
        if (r.freqHz > 0) snprintf(fr, sizeof fr, "%.5f", r.freqHz / 1e6);
        const double dur = r.active ? std::max(r.durSec, r.lastSec - r.startSec) : r.durSec;
        char d[24];
        snprintf(d, sizeof d, "%.1f", dur);
        o += std::string(t) + "," + (r.tg < 0 ? "" : std::to_string(r.tg)) + "," + csvField(e ? e->alpha : "") + "," + (r.src < 0 ? "" : std::to_string(r.src)) + "," + d + "," + fr + "," +
             (r.slot < 0 ? "" : std::to_string(r.slot + 1)) + "," + (r.dmr ? "DMR" : "P25") + (r.group ? " group" : " unit") + "," + (r.encrypted ? "encrypted" : "clear") + "," +
             (r.emergency ? "1" : "0") + "," + (r.followed ? "1" : "0") + "," + csvField(r.note) + "," + csvField(r.audioPath) + "\n";
    }
    return o;
}

} // namespace trunk
} // namespace dect2
