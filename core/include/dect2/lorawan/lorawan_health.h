// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LoRaWAN gateway / network health (3.9, docs/SMART5-field-3.9.md item 7), from the frames this receiver hears (LoRaWAN mode and the
// mesh mode's LoRaWAN channels). Nothing is decrypted: only the MAC header, DevAddr, FCnt and the radio settings are used.
//  - joins: join requests and join accepts per hour; a device (DevEUI) that sends joinStorm requests within 30 minutes cannot join
//    ("lorawan_join_storm": no accept reaches it, or the network refuses it); an hour with many requests and no accept at all
//    ("lorawan_no_accepts": the network server or its gateways are not answering, or the downlinks do not reach this receiver);
//  - frame counter per device: frames heard against the counter's steps, so the share lost on the way to this receiver
//    ("lorawan_loss" from lossPct with at least lossMinFrames expected), repeats (the same counter: NbTrans or confirmed retries) and
//    counter restarts (a rejoin or a reset device);
//  - spreading-factor mix: frames per SF in the last hour against the last day; "lorawan_sf_shift" when the SF11 + SF12 share doubles
//    (devices falling back to slow rates: ADR reacting to worse coverage, or a gateway gone).
// Pure logic, no clock: times are Unix seconds.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct LwHealthFrame {
    double t = 0;
    int mtype = -1;                      // 0 join request, 1 join accept, 2 unconfirmed up, 3 unconfirmed down, 4 confirmed up, 5 confirmed down, 6 rejoin
    std::string devAddr;                 // data frames: 8 hex digits
    std::string devEui;                  // join requests
    long fcnt = -1;                      // 16-bit counter of data frames, -1 none
    int sf = 0;                          // 0 FSK
    bool crcOk = true;
};

struct LwDeviceHealth {
    std::string id;
    uint32_t frames = 0, repeats = 0, lost = 0, restarts = 0, joins = 0;
    long lastFcnt = -1;
    double first = 0, last = 0;
    bool lossAlerted = false;
    double lossPct() const { const double e = frames + lost; return e > 0 ? 100.0 * lost / e : 0; }
};

class LorawanHealth {
public:
    int joinStorm = 5;
    double lossPct = 30; int lossMinFrames = 30;
    int noAcceptMin = 10;
    double alertCooldownSec = 6 * 3600;

    // one frame; findings (kind, key, text) are appended to `out`
    struct Finding { std::string kind, key, text; int severity = 0; };
    void add(const LwHealthFrame& f, std::vector<Finding>& out) {
        if (!f.crcOk) return;
        const int64_t h = (int64_t)std::floor(f.t / 3600);
        while (hours_.size() > 48 && hours_.begin()->first < h) hours_.erase(hours_.begin());
        Hour& hr = hours_[h];
        if (f.sf >= 5 && f.sf <= 12) { hr.sf[f.sf]++; }
        else hr.sf[0]++;
        if (f.mtype == 0 || f.mtype == 6) {
            hr.joinReq++;
            const std::string id = f.devEui.empty() ? "?" : f.devEui;
            auto& d = dev_[id]; if (d.id.empty()) { d.id = id; d.first = f.t; }
            d.joins++; d.last = f.t; d.frames++;
            auto& q = joinTimes_[id];
            q.push_back(f.t);
            while (!q.empty() && f.t - q.front() > 1800) q.pop_front();
            if ((int)q.size() >= joinStorm) alert(out, f.t, "lorawan_join_storm", id, 1, "LoRaWAN device " + id + " sent " + std::to_string(q.size()) + " join requests in 30 min: it cannot join");
        } else if (f.mtype == 1) {
            hr.joinAcc++;
        } else if ((f.mtype >= 2 && f.mtype <= 5) && !f.devAddr.empty()) {
            const bool up = f.mtype == 2 || f.mtype == 4;
            if (up) hr.up++; else hr.down++;
            if (up && f.fcnt >= 0) counter(f, out);
        }
        // an hour that ended with requests but no accepts
        if (h != lastHour_) {
            auto p = hours_.find(lastHour_);
            if (p != hours_.end() && (int)p->second.joinReq >= noAcceptMin && p->second.joinAcc == 0)
                alert(out, f.t, "lorawan_no_accepts", "network", 0, std::to_string(p->second.joinReq) + " LoRaWAN join requests and no join accept heard in the last hour");
            lastHour_ = h;
            sfShift(f.t, out);
        }
    }

    // the share (0 .. 1) of frames per SF (index 0 = FSK) over the last `hours` hours before `now`
    std::array<double, 13> sfMix(double now, int hours) const {
        std::array<double, 13> m{}; double n = 0;
        const int64_t cur = (int64_t)std::floor(now / 3600);
        for (const auto& [h, x] : hours_) if (h > cur - hours && h <= cur) for (int i = 0; i < 13; i++) { m[i] += x.sf[i]; n += x.sf[i]; }
        if (n > 0) for (auto& v : m) v /= n;
        return m;
    }
    struct HourRow { int64_t hour; uint32_t joinReq, joinAcc, up, down; };
    std::vector<HourRow> hours() const { std::vector<HourRow> v; for (const auto& [h, x] : hours_) v.push_back({h, x.joinReq, x.joinAcc, x.up, x.down}); return v; }
    std::vector<LwDeviceHealth> devices() const {
        std::vector<LwDeviceHealth> v; for (const auto& [k, d] : dev_) v.push_back(d);
        std::sort(v.begin(), v.end(), [](const LwDeviceHealth& a, const LwDeviceHealth& b) { return a.lossPct() > b.lossPct(); });
        return v;
    }
    const LwDeviceHealth* device(const std::string& id) const { auto it = dev_.find(id); return it == dev_.end() ? nullptr : &it->second; }

private:
    struct Hour { uint32_t joinReq = 0, joinAcc = 0, up = 0, down = 0; std::array<uint32_t, 13> sf{}; };
    std::map<int64_t, Hour> hours_;
    std::map<std::string, LwDeviceHealth> dev_;
    std::map<std::string, std::deque<double>> joinTimes_;
    std::map<std::string, double> lastAlert_;
    int64_t lastHour_ = 0;

    void alert(std::vector<Finding>& out, double t, const char* kind, const std::string& key, int sev, const std::string& text) {
        const std::string k = std::string(kind) + "|" + key;
        auto it = lastAlert_.find(k);
        if (it != lastAlert_.end() && t - it->second < alertCooldownSec) return;
        lastAlert_[k] = t;
        out.push_back({kind, key, text, sev});
    }
    void counter(const LwHealthFrame& f, std::vector<Finding>& out) {
        auto& d = dev_[f.devAddr];
        if (d.id.empty()) { d.id = f.devAddr; d.first = f.t; }
        d.last = f.t;
        const long c = f.fcnt & 0xFFFF;
        if (d.lastFcnt < 0) { d.frames++; d.lastFcnt = c; return; }
        const long step = (c - d.lastFcnt + 65536) % 65536;
        if (step == 0) { d.repeats++; return; }
        if (step > 1000) { d.restarts++; d.frames++; d.lastFcnt = c; return; }   // a restart (or a long absence): not counted as loss
        d.lost += (uint32_t)(step - 1);
        d.frames++;
        d.lastFcnt = c;
        if (d.lossAlerted && d.lossPct() < lossPct * 0.5) d.lossAlerted = false;   // reported once until it gets better
        if (!d.lossAlerted && d.frames + d.lost >= (uint32_t)lossMinFrames && d.lossPct() >= lossPct) {
            d.lossAlerted = true;
            char b[160]; snprintf(b, sizeof b, "LoRaWAN device %s: %.0f %% of its uplinks not heard (%u of %u)", f.devAddr.c_str(), d.lossPct(), d.lost, d.frames + d.lost);
            alert(out, f.t, "lorawan_loss", f.devAddr, 0, b);
        }
    }
    void sfShift(double now, std::vector<Finding>& out) {
        const int64_t cur = (int64_t)std::floor(now / 3600);
        auto last = hours_.find(cur - 1);
        if (last == hours_.end()) return;
        double n1 = 0, s1 = 0, nd = 0, sd = 0;
        for (int i = 0; i < 13; i++) { n1 += last->second.sf[i]; if (i >= 11) s1 += last->second.sf[i]; }
        for (const auto& [h, x] : hours_) if (h < cur - 1 && h >= cur - 25) for (int i = 0; i < 13; i++) { nd += x.sf[i]; if (i >= 11) sd += x.sf[i]; }
        if (n1 < 20 || nd < 100) return;
        const double a = s1 / n1, b = sd / nd;
        if (a >= 2 * b && a - b >= 0.15) {
            char t[160]; snprintf(t, sizeof t, "LoRaWAN SF11/SF12 share %.0f %% in the last hour, %.0f %% before: devices falling back to slow rates", a * 100, b * 100);
            alert(out, now, "lorawan_sf_shift", "network", 0, t);
        }
    }
};

} // namespace dect2
