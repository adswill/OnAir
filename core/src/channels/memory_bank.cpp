// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MemoryBank: add / update / remove / reorder / search / sort, duplicate detection and the recall order. The files are in memory_io.cpp.
#include "dect2/channels/memory_bank.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace dect2 {

namespace {
std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
bool hasWord(const std::string& list, const std::string& word) {   // a comma separated list holds word (case-insensitive)
    const std::string w = lower(word);
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i);
        if (j == std::string::npos) j = list.size();
        std::string t = list.substr(i, j - i);
        const size_t b = t.find_first_not_of(" \t"), e = t.find_last_not_of(" \t");
        t = b == std::string::npos ? "" : t.substr(b, e - b + 1);
        if (!t.empty() && lower(t) == w) return true;
        i = j + 1;
    }
    return false;
}
} // namespace

std::string memoryFreqMhzText(uint64_t hz) {
    char b[48];
    snprintf(b, sizeof b, "%llu.%06llu", (unsigned long long)(hz / 1000000), (unsigned long long)(hz % 1000000));
    std::string s = b;
    const size_t dot = s.find('.');
    while (s.size() - dot - 1 > 3 && s.back() == '0') s.pop_back();   // keep three decimals at least
    return s;
}

// accepts a decimal comma, a unit (MHz, kHz, Hz, GHz) and a bare number: below 30000 it is MHz (up to 30 GHz), from 30000 up it is Hz
bool memoryParseFreq(const std::string& text, uint64_t& hz) {
    std::string s;
    for (char c : text) s += c == ',' ? '.' : c;
    const size_t b = s.find_first_not_of(" \t\"");
    if (b == std::string::npos) return false;
    char* end = nullptr;
    const double v = strtod(s.c_str() + b, &end);
    if (end == s.c_str() + b || !(v > 0) || v > 1e13) return false;
    std::string unit = lower(end);
    unit.erase(std::remove_if(unit.begin(), unit.end(), [](unsigned char c) { return isspace(c) || c == '"'; }), unit.end());
    double hzv;
    if (unit == "ghz") hzv = v * 1e9;
    else if (unit == "mhz" || unit == "m") hzv = v * 1e6;
    else if (unit == "khz" || unit == "k") hzv = v * 1e3;
    else if (unit == "hz") hzv = v;
    else if (unit.empty()) hzv = v < 30000 ? v * 1e6 : v;
    else return false;
    if (!(hzv >= 1 && hzv < 1e13)) return false;
    hz = (uint64_t)(hzv + 0.5);
    return true;
}

const Memory* MemoryBank::find(uint64_t id) const { const int i = indexOf(id); return i < 0 ? nullptr : &v_[(size_t)i]; }
Memory* MemoryBank::find(uint64_t id) { const int i = indexOf(id); return i < 0 ? nullptr : &v_[(size_t)i]; }
int MemoryBank::indexOf(uint64_t id) const {
    for (size_t i = 0; i < v_.size(); i++) if (v_[i].id == id) return (int)i;
    return -1;
}

const Memory* MemoryBank::findDuplicate(uint64_t freqHz, const std::string& mode, uint64_t exceptId) const {
    for (const Memory& m : v_) if (m.id != exceptId && m.freqHz == freqHz && m.mode == mode) return &m;
    return nullptr;
}

MemoryAddResult MemoryBank::add(Memory m, DupPolicy dup, int64_t now) {
    MemoryAddResult r;
    if (dup != DupPolicy::Keep) {
        if (const Memory* d = findDuplicate(m.freqHz, m.mode)) {
            r.what = MemoryAdd::Duplicate; r.id = d->id; r.index = (size_t)indexOf(d->id);
            if (dup == DupPolicy::Update) update(d->id, std::move(m));
            return r;
        }
    }
    if (v_.size() >= kMemoryCap) { r.what = MemoryAdd::Full; return r; }
    m.id = nextId_++;
    if (!m.created) m.created = now ? now : (int64_t)time(nullptr);
    v_.push_back(std::move(m));
    r.id = v_.back().id; r.index = v_.size() - 1;
    return r;
}

bool MemoryBank::update(uint64_t id, Memory m) {
    Memory* d = find(id);
    if (!d) return false;
    m.id = id;
    m.created = d->created;
    *d = std::move(m);
    return true;
}

bool MemoryBank::remove(uint64_t id, Memory* removed, size_t* pos) {
    const int i = indexOf(id);
    if (i < 0) return false;
    if (removed) *removed = v_[(size_t)i];
    if (pos) *pos = (size_t)i;
    v_.erase(v_.begin() + i);
    return true;
}

void MemoryBank::restore(Memory m, size_t pos) {
    if (v_.size() >= kMemoryCap || indexOf(m.id) >= 0) return;
    if (m.id >= nextId_) nextId_ = m.id + 1;
    v_.insert(v_.begin() + (long)std::min(pos, v_.size()), std::move(m));
}

bool MemoryBank::move(size_t from, size_t to) {
    if (from >= v_.size() || to >= v_.size()) return false;
    if (from == to) return true;
    Memory m = std::move(v_[from]);
    v_.erase(v_.begin() + (long)from);
    v_.insert(v_.begin() + (long)to, std::move(m));
    return true;
}

void MemoryBank::touch(uint64_t id, int64_t now) { if (Memory* m = find(id)) m->lastUsed = now ? now : (int64_t)time(nullptr); }

std::vector<size_t> MemoryBank::view(const MemoryQuery& q, MemorySort sort, bool desc, bool favFirst) const {
    std::vector<size_t> out;
    const std::string text = lower(q.text), mode = lower(q.mode), group = lower(q.group);
    // a typed frequency ("162.4", "162,4") matches the start of a memory's MHz digits; without a point it means the whole MHz ("162" finds 162.xxx)
    std::string typed;
    for (char c : text) typed += c == ',' ? '.' : c;
    const bool freqText = !typed.empty() && typed.find_first_not_of("0123456789.") == std::string::npos;
    if (freqText && typed.find('.') == std::string::npos) typed += '.';
    for (size_t i = 0; i < v_.size(); i++) {
        const Memory& m = v_[i];
        if (q.favouritesOnly && !m.favourite) continue;
        if (q.loHz && m.freqHz < q.loHz) continue;
        if (q.hiHz && m.freqHz > q.hiHz) continue;
        if (!q.tag.empty() && !hasWord(m.tags, q.tag)) continue;
        if (!mode.empty() && lower(m.mode) != mode) continue;
        if (!group.empty() && lower(m.group) != group) continue;
        if (!text.empty()) {
            bool hit = lower(m.name).find(text) != std::string::npos || lower(m.notes).find(text) != std::string::npos ||
                       lower(m.tags).find(text) != std::string::npos || lower(m.group).find(text) != std::string::npos ||
                       lower(m.mode).find(text) != std::string::npos;
            if (!hit && freqText) hit = memoryFreqMhzText(m.freqHz).compare(0, typed.size(), typed) == 0;
            if (!hit) continue;
        }
        out.push_back(i);
    }
    if (sort != MemorySort::Manual) {
        auto key = [&](const Memory& m) { return sort == MemorySort::Name ? lower(m.name) : sort == MemorySort::Group ? lower(m.group) : lower(m.mode); };
        std::stable_sort(out.begin(), out.end(), [&](size_t a, size_t b) {
            const Memory &x = v_[a], &y = v_[b];
            if (sort == MemorySort::Frequency) return desc ? x.freqHz > y.freqHz : x.freqHz < y.freqHz;
            if (sort == MemorySort::LastUsed) return desc ? x.lastUsed < y.lastUsed : x.lastUsed > y.lastUsed;   // newest first unless reversed
            const std::string kx = key(x), ky = key(y);
            if (kx != ky) return desc ? kx > ky : kx < ky;
            return x.freqHz < y.freqHz;
        });
    }
    if (favFirst) std::stable_partition(out.begin(), out.end(), [&](size_t i) { return v_[i].favourite; });
    return out;
}

std::vector<std::string> MemoryBank::groups() const {
    std::vector<std::string> g;
    for (const Memory& m : v_) if (!m.group.empty() && std::find(g.begin(), g.end(), m.group) == g.end()) g.push_back(m.group);
    return g;
}

std::vector<size_t> MemoryBank::recent(size_t n) const {
    std::vector<size_t> r;
    for (size_t i = 0; i < v_.size(); i++) if (v_[i].lastUsed > 0) r.push_back(i);
    std::stable_sort(r.begin(), r.end(), [&](size_t a, size_t b) { return v_[a].lastUsed > v_[b].lastUsed; });
    if (r.size() > n) r.resize(n);
    return r;
}

void MemoryBank::sortInPlace(MemorySort sort, bool desc) {
    const std::vector<size_t> o = view(MemoryQuery(), sort, desc);
    std::vector<Memory> n;
    n.reserve(v_.size());
    for (size_t i : o) n.push_back(std::move(v_[i]));
    v_ = std::move(n);
}

RecallResult memoryRecall(MemoryHost& h, const Memory& m, bool withMode) {
    RecallResult r;
    if (withMode && !m.mode.empty()) {
        if (!h.hasMode(m.mode)) { r.note = "this mode is not in this build of OnAir: " + m.mode; return r; }
        std::string note;
        if (!h.setMode(m, note)) { r.note = note; return r; }
        r.modeChanged = true;
        if (!note.empty()) r.note = note;
        for (const auto& kv : m.options) h.setOption(kv.first, kv.second);
        if (m.hasGain) h.setGains(m);
        if (m.sampleRateHz > 0) h.setSampleRate(m.sampleRateHz);
    }
    h.setFrequencyHz(m.freqHz);
    r.ok = true;
    return r;
}

} // namespace dect2
