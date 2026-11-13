// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The scanner (View > Scanner, HIGHLY EXPERIMENTAL): the state machine that steps through a scan list (or a band) and stops on activity.
// No ImGui, no engine: the window (scanner_ui.cpp) feeds it the time and whether the channel is active, and does the tuning it asks for.
// tests/engine/test_scanner.cpp drives it with a fake activity source.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace scanlist {

struct Entry {
    double mhz = 100;
    std::string mode;          // a mode key (scanner_ui.cpp: the protocol id, the mode's id, or dvb / atsc / dab / atsc3 / isdbt / fm)
    std::string name;          // optional
    double dwellSec = 2;       // how long it listens for activity
    bool lockout = false;      // skipped
    bool priority = false;     // checked every Machine::prioritySec seconds
};

struct Band { double startMhz = 118, stopMhz = 137, stepKhz = 25; std::string mode = "airband"; double dwellSec = 2; };

struct Target { double mhz = 0; std::string mode, name; double dwellSec = 2; int entry = -1; bool priority = false; };   // entry: index in the list, -1 a band step

class Machine {
public:
    enum State { Idle, Dwell, Hold, Hang };
    std::vector<Entry> list;
    Band band;
    bool useBand = false;           // scan the band instead of the list
    double hangSec = 3;             // after activity ends, stay this long before moving on
    double holdMaxSec = 0;          // longest stay on one active channel (0: as long as it stays active)
    double prioritySec = 5;         // the priority entry is checked this often (0: never)
    double settleSec = 0.3;         // activity is not believed for this long after the receiver is ready

    State state() const { return st_; }
    const Target& target() const { return cur_; }
    bool running() const { return st_ != Idle; }
    bool hitStarted() { const bool h = hit_; hit_ = false; return h; }   // true once each time it stops on activity
    std::vector<double> bandLockouts;   // band steps locked out

    // Start: returns false when there is nothing to scan. The caller tunes to target().
    bool start(double now) {
        pos_ = -1; lastPrio_ = now; resume_ = -1;
        if (!advanceTo(nextPos(), now)) { st_ = Idle; return false; }
        return true;
    }
    void stop() { st_ = Idle; }

    // Once a frame. ready: the receiver is delivering on the new channel; active: activity measured there.
    // Returns true when the caller must tune to target().
    bool tick(double now, bool ready, bool active) {
        if (st_ == Idle) return false;
        if (ready && readyAt_ < 0) readyAt_ = now;
        const bool believed = readyAt_ >= 0 && now - readyAt_ >= settleSec;
        const bool act = believed && active;
        switch (st_) {
        case Dwell:
            if (act) { st_ = Hold; hit_ = true; lastAct_ = now; holdStart_ = now; return false; }
            if (readyAt_ >= 0 && now - readyAt_ >= settleSec + cur_.dwellSec) return advance(now);
            if (readyAt_ < 0 && now - tunedAt_ >= 10) return advance(now);   // the receiver never came up here
            return false;
        case Hold:
            if (act) { lastAct_ = now; if (holdMaxSec > 0 && now - holdStart_ >= holdMaxSec) return advance(now); return priorityDue(now) ? goPriority(now, true) : false; }
            st_ = Hang; return false;
        case Hang:
            if (act) { st_ = Hold; lastAct_ = now; if (holdMaxSec > 0 && now - holdStart_ >= holdMaxSec) return advance(now); return false; }
            if (now - lastAct_ >= hangSec) return advance(now);
            return false;
        default: return false;
        }
    }
    // Skip: move on now. Lockout: lock out the current channel and move on. Both return true (tune to target()) unless nothing is left.
    bool skip(double now) { if (st_ == Idle) return false; resume_ = -1; return advance(now); }
    bool lockout(double now) {
        if (st_ == Idle) return false;
        if (cur_.entry >= 0 && cur_.entry < (int)list.size()) list[cur_.entry].lockout = true;
        else if (cur_.entry < 0) bandLockouts.push_back(cur_.mhz);
        resume_ = -1;
        return advance(now);
    }

    int bandSteps() const {
        if (band.stepKhz <= 0 || band.stopMhz < band.startMhz) return 0;
        return (int)((band.stopMhz - band.startMhz) * 1000 / band.stepKhz + 1e-6) + 1;
    }

private:
    State st_ = Idle;
    Target cur_;
    int pos_ = -1, resume_ = -1;
    double tunedAt_ = 0, readyAt_ = -1, lastAct_ = 0, lastPrio_ = 0, holdStart_ = 0;
    bool hit_ = false;

    int count() const { return useBand ? bandSteps() : (int)list.size(); }
    double bandMhz(int i) const { return band.startMhz + i * band.stepKhz / 1000.0; }
    bool usable(int i) const {
        if (useBand) { for (double l : bandLockouts) if (std::abs(l - bandMhz(i)) < 1e-6) return false; return true; }
        return !list[i].lockout && !list[i].priority;   // the priority entry is visited on its own schedule
    }
    int prioIndex() const {
        if (useBand) return -1;
        for (int i = 0; i < (int)list.size(); i++) if (list[i].priority && !list[i].lockout) return i;
        return -1;
    }
    int nextPos() const {
        const int n = count();
        for (int k = 1; k <= n; k++) { const int i = ((pos_ < 0 ? -1 : pos_) + k) % n; if (usable(i)) return i; }
        return prioIndex() >= 0 ? -2 : -1;   // only the priority entry is left
    }
    bool priorityDue(double now) const { return prioritySec > 0 && prioIndex() >= 0 && !cur_.priority && now - lastPrio_ >= prioritySec; }
    bool goPriority(double now, bool comeBack) {
        resume_ = comeBack ? pos_ : -1;
        lastPrio_ = now;
        const Entry& e = list[prioIndex()];
        cur_ = {e.mhz, e.mode, e.name, e.dwellSec, prioIndex(), true};
        return tuned(now);
    }
    bool advanceTo(int i, double now) {
        if (i == -1) return false;
        if (i == -2) return goPriority(now, false);
        pos_ = i;
        if (useBand) cur_ = {bandMhz(i), band.mode, "", band.dwellSec, -1, false};
        else { const Entry& e = list[i]; cur_ = {e.mhz, e.mode, e.name, e.dwellSec, i, false}; }
        return tuned(now);
    }
    bool tuned(double now) { st_ = Dwell; tunedAt_ = now; readyAt_ = -1; return true; }
    bool advance(double now) {
        if (cur_.priority && resume_ >= 0 && resume_ < count() && usable(resume_)) { const int r = resume_; resume_ = -1; return advanceTo(r, now); }   // back to the held channel
        resume_ = -1;
        if (!cur_.priority && priorityDue(now)) return goPriority(now, false);
        const int i = nextPos();
        if (i == -1) { st_ = Idle; return false; }
        return advanceTo(i, now);
    }
};

// CSV: frequency_mhz,mode,name,dwell_s,lockout,priority (a header line, names in quotes when they hold a comma or a quote)
inline std::string csvField(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; if (c != '\n') o += c; }
    return o + "\"";
}
inline std::string toCsv(const std::vector<Entry>& v) {
    std::string o = "frequency_mhz,mode,name,dwell_s,lockout,priority\n";
    char b[64];
    for (const Entry& e : v) {
        snprintf(b, sizeof b, "%.6f,", e.mhz); o += b;
        o += csvField(e.mode) + "," + csvField(e.name) + ",";
        snprintf(b, sizeof b, "%.2f,%d,%d\n", e.dwellSec, e.lockout ? 1 : 0, e.priority ? 1 : 0); o += b;
    }
    return o;
}
inline std::vector<std::string> csvSplit(const std::string& line) {
    std::vector<std::string> f(1);
    bool q = false;
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (q) { if (c == '"') { if (i + 1 < line.size() && line[i + 1] == '"') { f.back() += '"'; i++; } else q = false; } else f.back() += c; }
        else if (c == '"') q = true;
        else if (c == ',') f.emplace_back();
        else if (c != '\r') f.back() += c;
    }
    return f;
}
// Lines that do not start with a frequency (the header, blanks) are skipped.
inline std::vector<Entry> fromCsv(const std::string& text) {
    std::vector<Entry> v;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        const std::vector<std::string> f = csvSplit(text.substr(p, e - p));
        p = e + 1;
        char* end = nullptr;
        const double mhz = f.empty() ? 0 : strtod(f[0].c_str(), &end);
        if (f.size() < 2 || end == f[0].c_str() || !(mhz > 0)) continue;
        Entry x;
        x.mhz = mhz; x.mode = f[1];
        if (f.size() > 2) x.name = f[2];
        if (f.size() > 3) { const double d = atof(f[3].c_str()); if (d > 0) x.dwellSec = d; }
        if (f.size() > 4) x.lockout = atoi(f[4].c_str()) != 0;
        if (f.size() > 5) x.priority = atoi(f[5].c_str()) != 0;
        v.push_back(x);
    }
    return v;
}

} // namespace scanlist
