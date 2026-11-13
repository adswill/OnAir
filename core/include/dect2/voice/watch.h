// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The watch list of the digital voice modes (DMR, P25, NXDN, TETRA, dPMR, D-STAR, Yaesu, M17): follow a talkgroup, a radio ID or a callsign across
// every mode at once. Each entry can alert when a call of it starts (desktop notification, optional sound) and can ask the auto-record rules to
// save its calls. Everything is OFF by default: a new entry only lists, the alert and record boxes start unticked.
//
// It reads the modes' voice monitors (voice_monitor.h), so the same entry works for every mode and nothing here touches a decoder. Matching is the
// monitor's IdList: numbers, ranges "100-199", callsigns, "LY*" prefixes; a call matches when its target or its source does. Encrypted calls are
// listed and can alert (the fact that a call happened), never recorded (the record rules and the monitor refuse them).
// The window (app/voice/watch_ui.cpp) shows the list, the history and delivers the alerts; poll() is the only thing it has to call.
#pragma once
#include "dect2/session/record_rules.h"
#include "dect2/session/voice_bridge.h"
#include "dect2/voice/voice_monitor.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dect2::voice {

struct WatchEntry {
    uint32_t id = 0;
    std::string mode;            // monitor name ("dmr", "p25" ...); empty: every voice mode
    std::string match;           // IdList text: "91", "100-199", "LY1ABC", "LY*"
    std::string name;            // shown instead of the match when set
    bool alert = false;          // notify when a call of it starts
    bool sound = false;          // and beep (only with alert; never when the program is muted)
    bool record = false;         // keep an auto-record rule for it
    uint32_t ruleId = 0;         // the record rule made for it (0: none)
    std::string title() const { return name.empty() ? match : name; }
};

struct WatchEvent {
    uint32_t entry = 0;
    std::string mode;
    uint64_t callId = 0;         // CallEntry::id of that mode's monitor
    int64_t wall = 0;            // Unix time the call was first seen
    std::string text;            // "DMR TG 91 from 3120123 LY1ABC"
    bool emergency = false, encrypted = false;
};

class Watch {
public:
    static constexpr size_t kMaxEvents = 500;

    // ---- the list
    uint32_t add(WatchEntry e) {
        std::lock_guard<std::mutex> lk(mu_);
        e.id = ++nextId_;
        trim(e);
        if (e.match.empty()) return 0;
        list_.push_back(e);
        gen_++;
        return e.id;
    }
    void update(const WatchEntry& in) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& e : list_) if (e.id == in.id) { const uint32_t rule = e.ruleId; e = in; trim(e); e.ruleId = rule; gen_++; }
    }
    void remove(uint32_t id) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = list_.begin(); it != list_.end(); ++it) if (it->id == id) { orphan_.push_back(it->ruleId); list_.erase(it); gen_++; break; }
    }
    std::vector<WatchEntry> list() const { std::lock_guard<std::mutex> lk(mu_); return list_; }
    uint64_t gen() const { std::lock_guard<std::mutex> lk(mu_); return gen_; }

    // ---- alerts and history. Call often (every frame is fine); returns the calls seen for the first time since the last call.
    std::vector<WatchEvent> poll(int64_t wallNow = 0) {
        if (!wallNow) wallNow = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::vector<WatchEntry> list = this->list();
        std::vector<WatchEvent> fresh;
        if (list.empty()) return fresh;
        std::vector<IdList> ids;
        for (const auto& e : list) ids.emplace_back(e.match);
        for (const std::string& mode : monitorModes()) {
            const std::vector<CallEntry> calls = monitor(mode).calls();
            for (const CallEntry& c : calls) {
                for (size_t i = 0; i < list.size(); i++) {
                    if (!list[i].mode.empty() && list[i].mode != mode) continue;
                    if (!ids[i].match(c.info)) continue;
                    std::lock_guard<std::mutex> lk(mu_);
                    if (!seen_.insert({list[i].id, mode, c.id}).second) continue;   // one event per entry and call
                    if (seen_.size() > 4000) seen_.erase(seen_.begin());
                    WatchEvent ev;
                    ev.entry = list[i].id; ev.mode = mode; ev.callId = c.id; ev.wall = c.wallStart ? c.wallStart : wallNow;
                    ev.emergency = c.info.emergency; ev.encrypted = c.info.encrypted;
                    ev.text = describe(mode, c);
                    events_.push_back(ev);
                    if (events_.size() > kMaxEvents) events_.pop_front();
                    fresh.push_back(ev);
                }
            }
        }
        return fresh;
    }
    std::vector<WatchEvent> events() const { std::lock_guard<std::mutex> lk(mu_); return std::vector<WatchEvent>(events_.begin(), events_.end()); }   // oldest first
    void clearEvents() { std::lock_guard<std::mutex> lk(mu_); events_.clear(); seen_.clear(); }

    // the history of an entry as the monitors keep it (calls going on and ended), newest first, with durations and recordings
    std::vector<std::pair<std::string, CallEntry>> history(uint32_t entryId, size_t max = 200) const {
        std::vector<std::pair<std::string, CallEntry>> out;
        WatchEntry we; bool found = false;
        { std::lock_guard<std::mutex> lk(mu_); for (const auto& e : list_) if (e.id == entryId) { we = e; found = true; } }
        if (!found) return out;
        const IdList ids(we.match);
        for (const std::string& mode : monitorModes()) {
            if (!we.mode.empty() && we.mode != mode) continue;
            Monitor& mon = monitor(mode);
            for (const CallEntry& c : mon.calls()) if (ids.match(c.info)) out.emplace_back(mode, c);   // still going on
            for (const CallEntry& c : mon.history(std::string(), 500)) if (ids.match(c.info)) out.emplace_back(mode, c);
        }
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second.wallStart > b.second.wallStart; });
        if (out.size() > max) out.resize(max);
        return out;
    }

    // ---- recording: make the record rules agree with the "record" boxes. Turning a box on adds a rule and switches the master switch on
    // (the user ticked it); turning it off, or removing the entry, removes only the rule this list made.
    void syncRecord(sess::RecordRules& rr) {
        std::vector<uint32_t> gone;
        std::vector<WatchEntry> list;
        { std::lock_guard<std::mutex> lk(mu_); gone.swap(orphan_); list = list_; }
        for (uint32_t r : gone) if (r) rr.remove(r);
        for (auto& e : list) {
            if (e.record && !e.ruleId) {
                sess::RecordRule r;
                const int m = e.mode.empty() ? 0 : sess::voicebridge::modeNumber(e.mode);
                r.mode = (uint16_t)(m > 0 ? m : 0);
                r.field = sess::RecField::Any; r.match = e.match; r.name = "Watch: " + e.title(); r.maxSeconds = 3600;
                const uint32_t id = rr.add(r);
                rr.setEnabled(true);
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& x : list_) if (x.id == e.id) x.ruleId = id;
                gen_++;
            } else if (!e.record && e.ruleId) {
                rr.remove(e.ruleId);
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& x : list_) if (x.id == e.id) x.ruleId = 0;
                gen_++;
            }
        }
    }

    // ---- saving (one line per entry: mode, match, name, alert, sound, record, rule id; tab separated)
    std::string saveText() const {
        std::lock_guard<std::mutex> lk(mu_);
        std::string o;
        for (const auto& e : list_) o += e.mode + "\t" + e.match + "\t" + e.name + "\t" + (e.alert ? "1" : "0") + "\t" + (e.sound ? "1" : "0") + "\t" + (e.record ? "1" : "0") + "\t" + std::to_string(e.ruleId) + "\n";
        return o;
    }
    void loadText(const std::string& text) {
        std::lock_guard<std::mutex> lk(mu_);
        list_.clear();
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            std::vector<std::string> f; size_t s = 0;
            for (;;) { const size_t t = line.find('\t', s); f.push_back(line.substr(s, t == std::string::npos ? t : t - s)); if (t == std::string::npos) break; s = t + 1; }
            if (f.size() < 6 || f[1].empty()) continue;
            WatchEntry e; e.id = ++nextId_; e.mode = f[0]; e.match = f[1]; e.name = f[2]; e.alert = f[3] == "1"; e.sound = f[4] == "1"; e.record = f[5] == "1";
            if (f.size() > 6) e.ruleId = (uint32_t)std::strtoul(f[6].c_str(), nullptr, 10);
            list_.push_back(e);
        }
        gen_++;
    }

private:
    static void clean(std::string& s) { for (auto& c : s) if (c == '\t' || c == '\n' || c == '\r') c = ' '; while (!s.empty() && s.back() == ' ') s.pop_back(); size_t i = 0; while (i < s.size() && s[i] == ' ') i++; s.erase(0, i); if (s.size() > 96) s.resize(96); }
    static void trim(WatchEntry& e) { clean(e.match); clean(e.name); clean(e.mode); if (!e.alert) e.sound = false; }
    static std::string describe(const std::string& mode, const CallEntry& c) {
        std::string t = sess::voicebridge::modeName(mode) ? sess::voicebridge::modeName(mode) : mode;
        t += " " + targetText(c.info);
        if (!c.label.empty()) t += " " + c.label;
        const std::string src = sourceText(c.info);
        if (!src.empty() && src != "0") t += " from " + src + (c.srcLabel.empty() || c.srcLabel == src ? "" : " " + c.srcLabel);
        return t;
    }
    mutable std::mutex mu_;
    std::vector<WatchEntry> list_;
    std::deque<WatchEvent> events_;
    std::set<std::tuple<uint32_t, std::string, uint64_t>> seen_;
    std::vector<uint32_t> orphan_;
    uint32_t nextId_ = 0;
    uint64_t gen_ = 1;
};

inline Watch& watch() { static Watch w; return w; }

} // namespace dect2::voice
