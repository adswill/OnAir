// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Home network health (3.9, docs/SMART5-iot-3.9.md) for the user's own Z-Wave home ID or Zigbee PAN: per node, the frames it sent, its
// retransmissions (the same source, destination and sequence number again within 3 s), the frames that asked for an acknowledgement and
// got none within 1 s, the signal level now against its first hour, and silence against the node's own learned interval.
// Z-Wave frames carry the sequence number and the ack request, so all of it works there; the Zigbee frame list here carries neither, so
// Zigbee nodes get the level trend and silence only. Only the network the user picks is followed.
// Pure logic, no clock.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct HomeNetFrame {
    double t = 0;
    std::string src, dst;        // node ids as text
    int seq = -1;                // -1: not known
    bool ackReq = false;
    bool isAck = false;          // an acknowledgement frame (src acknowledges dst's frame)
    float rssi = -200;
};

struct HomeNetNode {
    std::string id;
    uint64_t frames = 0, retries = 0, ackAsked = 0, ackMissing = 0;
    double firstT = 0, lastT = 0;
    double intervalSec = 0;      // learned, 0 until 3 gaps
    double rssiFirst = -200, rssiNow = -200;   // mean of the first hour, and smoothed now
    bool silent = false, weakening = false, lossy = false;
    std::string text;
    double retryPct() const { return frames ? 100.0 * retries / frames : 0; }
    double missPct() const { return ackAsked ? 100.0 * ackMissing / ackAsked : 0; }
};

class HomeNetHealth {
public:
    double retryWindow = 3, ackWindow = 1;
    double weakenDb = 6;          // level now below the first hour by this much
    double lossyPct = 10;         // retries or missing acks above this share (after 20 frames)

    void add(const HomeNetFrame& f) {
        expireAcks(f.t);
        if (f.isAck) {   // f.src acknowledges a frame from f.dst
            for (auto it = pend_.begin(); it != pend_.end(); ++it)
                if (it->src == f.dst && it->dst == f.src && (f.seq < 0 || it->seq < 0 || it->seq == f.seq)) { pend_.erase(it); break; }
        }
        HomeNetNode& n = nodes_[f.src];
        if (n.frames == 0) { n.id = f.src; n.firstT = f.t; }
        else if (!f.isAck) {
            const double gap = f.t - n.lastT;
            if (gap >= 2) { n.intervalSec = gaps_[f.src] == 0 ? gap : n.intervalSec + (std::min(gap, 4 * n.intervalSec + 1) - n.intervalSec) * 0.2; gaps_[f.src]++; }
        }
        n.frames++; n.lastT = f.t; n.silent = false;
        if (f.rssi > -199) {
            n.rssiNow = n.rssiNow < -199 ? f.rssi : n.rssiNow + (f.rssi - n.rssiNow) * 0.1;
            if (f.t - n.firstT <= 3600) { auto& a = first_[f.src]; a.first += f.rssi; a.second++; n.rssiFirst = a.first / a.second; }
        }
        if (!f.isAck && f.seq >= 0) {
            auto& r = recent_[f.src];
            for (auto& x : r) if (x.dst == f.dst && x.seq == f.seq && f.t - x.t <= retryWindow) { n.retries++; break; }
            r.push_back({f.t, f.dst, f.seq});
            while (!r.empty() && f.t - r.front().t > retryWindow) r.pop_front();
        }
        if (f.ackReq && !f.isAck) {
            bool dup = false; for (auto& p : pend_) if (p.src == f.src && p.dst == f.dst && p.seq == f.seq) { dup = true; p.t = f.t; }   // a retry replaces the wait
            if (!dup) { n.ackAsked++; pend_.push_back({f.t, f.src, f.dst, f.seq}); }
        }
        judge(n, f.t);
    }
    void tick(double now) {
        expireAcks(now);
        for (auto& [k, n] : nodes_) { n.silent = n.intervalSec > 0 && gaps_[k] >= 3 && now - n.lastT > std::max(3 * n.intervalSec, n.intervalSec + 300); judge(n, now); }
    }
    const std::map<std::string, HomeNetNode>& nodes() const { return nodes_; }

private:
    struct Pend { double t; std::string src, dst; int seq; };
    struct Rec { double t; std::string dst; int seq; };
    std::map<std::string, HomeNetNode> nodes_;
    std::map<std::string, int> gaps_;
    std::map<std::string, std::pair<double, int>> first_;
    std::map<std::string, std::deque<Rec>> recent_;
    std::deque<Pend> pend_;
    void expireAcks(double now) {
        while (!pend_.empty() && now - pend_.front().t > ackWindow) {
            auto it = nodes_.find(pend_.front().src); if (it != nodes_.end()) it->second.ackMissing++;
            pend_.pop_front();
        }
    }
    void judge(HomeNetNode& n, double now) {
        n.weakening = n.rssiFirst > -199 && now - n.firstT > 3600 && n.rssiFirst - n.rssiNow > weakenDb;
        n.lossy = n.frames >= 20 && (n.retryPct() > lossyPct || (n.ackAsked >= 10 && n.missPct() > lossyPct));
        char b[200]; b[0] = 0;
        if (n.silent) snprintf(b, sizeof b, "node %s silent for %.0f min (it usually sends every %.0f min)", n.id.c_str(), (now - n.lastT) / 60, n.intervalSec / 60);
        else if (n.lossy) snprintf(b, sizeof b, "node %s: %.0f %% retransmitted, %.0f %% not acknowledged", n.id.c_str(), n.retryPct(), n.missPct());
        else if (n.weakening) snprintf(b, sizeof b, "node %s: %.1f dB weaker than in its first hour", n.id.c_str(), n.rssiFirst - n.rssiNow);
        n.text = b;
    }
};

} // namespace dect2
