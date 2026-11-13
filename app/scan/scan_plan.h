// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The merged Scanner view's pure logic: the memory table rows, which of them a scan covers (all / a group / a range / the starred), the scan list
// built from them, next-channel choice, dwell, hold, resume delay and priority revisit, hit bookkeeping and the tone text. No ImGui, no engine:
// tests/engine/test_scan_plan.cpp drives it. The channel-by-channel state machine itself is scanlist::Machine (scan_list.h); this header decides
// WHAT it is given and what is written back after a hit or a lockout.
#pragma once
#include "scan_list.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace scanplan {

// One row of the table. The scan fields live in a memory's options map (see readRow / writeRow), so the memory files need no new columns.
struct Row {
    double mhz = 0;
    std::string mode, name, group, tone;   // tone: "88.5 Hz", "D023N" or empty
    double dwellSec = 0;                    // 0: the scan's default
    bool lockout = false, priority = false, star = false;
    int hits = 0;
    long long lastHit = 0;                  // unix time, 0: never
};

enum class Scope { All, Group, Range, Starred, Fast };   // Fast: the FFT fast scan (fast_scan_ui.cpp) has its own list; no memory row is stepped through
struct Params {
    Scope scope = Scope::All;
    std::string group;                      // Scope::Group
    double loMhz = 0, hiMhz = 0;            // Scope::Range
    bool skipLocked = true;                 // false: locked-out rows are scanned too
    bool priorityCheck = true;              // false: the priority row is just an ordinary row
    double defaultDwell = 2;
};

inline bool inScope(const Row& r, const Params& p) {
    if (!(r.mhz > 0)) return false;
    if (p.skipLocked && r.lockout) return false;
    switch (p.scope) {
    case Scope::All: return true;
    case Scope::Group: return r.group == p.group;
    case Scope::Range: return r.mhz >= p.loMhz - 1e-9 && r.mhz <= p.hiMhz + 1e-9;
    case Scope::Starred: return r.star;
    case Scope::Fast: return false;
    }
    return false;
}

// Row indexes the scan covers, in table order.
inline std::vector<int> pick(const std::vector<Row>& rows, const Params& p) {
    std::vector<int> v;
    for (int i = 0; i < (int)rows.size(); i++) if (inScope(rows[i], p)) v.push_back(i);
    return v;
}

inline double dwellFor(const Row& r, double def) { return r.dwellSec > 0 ? r.dwellSec : (def > 0 ? def : 2); }

// The machine's list for the rows picked. rowOf[k] = the table row of list entry k. At most one priority entry (the first), none when priorityCheck is off.
inline std::vector<scanlist::Entry> toEntries(const std::vector<Row>& rows, const Params& p, std::vector<int>& rowOf) {
    std::vector<scanlist::Entry> out;
    rowOf.clear();
    bool havePrio = false;
    for (int i : pick(rows, p)) {
        const Row& r = rows[i];
        scanlist::Entry e;
        e.mhz = r.mhz; e.mode = r.mode; e.name = r.name; e.dwellSec = dwellFor(r, p.defaultDwell);
        e.lockout = false;                     // locked-out rows are already out when skipLocked; scanned when not
        e.priority = p.priorityCheck && r.priority && !havePrio;
        havePrio = havePrio || e.priority;
        out.push_back(e); rowOf.push_back(i);
    }
    return out;
}

// The next position after pos in a picked order (wraps), skipping rows the caller marks locked; -1 when every one is locked. pos -1 starts at the top.
inline int nextChannel(const std::vector<Row>& rows, const std::vector<int>& order, int pos, bool skipLocked) {
    const int n = (int)order.size();
    for (int k = 1; k <= n; k++) {
        const int p = ((pos < 0 ? -1 : pos) + k) % n;
        if (!skipLocked || !rows[order[p]].lockout) return p;
    }
    return -1;
}

// ---- timing, as the control bar's fields mean them
// hold: the longest stay on one active channel (0: no limit). resume delay: how long after the activity ends before moving on. dwell: how long a
// quiet channel is listened to. priority: how often the priority row is looked at.
inline bool holdExpired(double now, double heldSince, double holdMax) { return holdMax > 0 && now - heldSince >= holdMax; }
inline bool resumeDue(double now, double activityEnd, double resumeDelay) { return now - activityEnd >= resumeDelay; }
inline bool priorityDue(double now, double lastCheck, double every, bool enabled) { return enabled && every > 0 && now - lastCheck >= every; }

// What a scan does with a hit: the row's counters, last hit time and, when a tone was read, the tone (an existing tone is not replaced by an empty one).
inline void recordHit(Row& r, long long now, const std::string& tone = "") {
    r.hits++; r.lastHit = now;
    if (!tone.empty()) r.tone = tone;
}

// The frequencies of a stepped range, lo to hi inclusive, at most cap of them (a runaway range is cut, not allocated).
inline std::vector<double> rangeSteps(double loMhz, double hiMhz, double stepKhz, size_t cap = 5000) {
    std::vector<double> v;
    if (!(stepKhz > 0) || hiMhz < loMhz) return v;
    for (size_t i = 0; i < cap; i++) { const double f = loMhz + i * stepKhz / 1000.0; if (f > hiMhz + 1e-9) break; v.push_back(f); }
    return v;
}

// ---- tones
// "88.5 Hz" for a CTCSS tone, "D023N" / "D023I" for a DCS code (code as written, 23 for 023), "" for none.
inline std::string toneText(double ctcssHz, int dcsCode, bool dcsInv) {
    char b[24];
    if (ctcssHz > 0) { snprintf(b, sizeof b, "%.1f Hz", ctcssHz); return b; }
    if (dcsCode >= 0) { snprintf(b, sizeof b, "D%03d%c", dcsCode, dcsInv ? 'I' : 'N'); return b; }
    return "";
}
// the tone as a lock-in on a tone telemetry; a tone seen earlier stays until a different one is saved
inline bool toneDiffers(const Row& r, const std::string& seen) { return !seen.empty() && seen != r.tone; }

// ---- storage in a memory's options map
inline const char* const kKeyLock = "scan.lock";
inline const char* const kKeyPrio = "scan.prio";
inline const char* const kKeyHits = "scan.hits";
inline const char* const kKeyLast = "scan.last";
inline const char* const kKeyTone = "scan.tone";

inline void readRow(const std::map<std::string, std::string>& o, Row& r) {
    auto g = [&](const char* k) -> const std::string* { auto it = o.find(k); return it == o.end() ? nullptr : &it->second; };
    if (auto s = g(kKeyLock)) r.lockout = *s == "1";
    if (auto s = g(kKeyPrio)) r.priority = *s == "1";
    if (auto s = g(kKeyHits)) r.hits = std::atoi(s->c_str());
    if (auto s = g(kKeyLast)) r.lastHit = std::atoll(s->c_str());
    if (auto s = g(kKeyTone)) r.tone = *s;
}
inline void writeRow(std::map<std::string, std::string>& o, const Row& r) {
    if (r.lockout) o[kKeyLock] = "1"; else o.erase(kKeyLock);
    if (r.priority) o[kKeyPrio] = "1"; else o.erase(kKeyPrio);
    if (r.hits > 0) o[kKeyHits] = std::to_string(r.hits); else o.erase(kKeyHits);
    if (r.lastHit > 0) o[kKeyLast] = std::to_string(r.lastHit); else o.erase(kKeyLast);
    if (!r.tone.empty()) o[kKeyTone] = r.tone; else o.erase(kKeyTone);
}

// One priority row at a time: setting it clears the others.
inline void setPriority(std::vector<Row>& rows, int idx, bool on) {
    for (int i = 0; i < (int)rows.size(); i++) rows[i].priority = on && i == idx;
}

}  // namespace scanplan
