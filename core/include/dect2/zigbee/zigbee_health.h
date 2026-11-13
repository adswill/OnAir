// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Zigbee / Thread home network health (3.9, docs/SMART5-iot-3.9.md item 14, docs/SMART5-UI-3.9.md): feeds the frames of the one PAN the user
// marked as theirs into dect2/zwave/homenet_health.h. An 802.15.4 acknowledgement carries only the sequence number (no addresses, no PAN), so it
// is matched to the last frame of that PAN that asked for one with the same number within the last second. Only the MAC header is used, so
// encrypted networks work as well. Retries are frames with the same sequence number to the same destination within 3 s.
#pragma once
#include "dect2/zigbee/zigbee_tel.h"
#include "dect2/zwave/homenet_health.h"
#include <algorithm>
#include <deque>
#include <string>

namespace dect2 {

class ZigbeeHealthFeed {
public:
    explicit ZigbeeHealthFeed(int pan = -1) : pan_(pan) {}
    int pan() const { return pan_; }
    // one decoded frame, in time order
    void add(const ZbFrame& f) {
        if (pan_ < 0 || !f.fcsOk) return;
        while (!asks_.empty() && f.sec - asks_.front().t > 1.0) asks_.pop_front();
        HomeNetFrame h; h.t = f.sec; h.seq = f.seq; h.rssi = f.rssi;
        if (f.type == "Ack") {
            auto q = std::find_if(asks_.rbegin(), asks_.rend(), [&](const Ask& a) { return a.seq == f.seq; });
            if (q == asks_.rend()) return;   // the acknowledgement of another network's frame
            h.isAck = true; h.src = q->dst; h.dst = q->src;
        } else {
            if ((int)f.pan != pan_ || f.src.empty()) return;
            h.src = f.src; h.dst = f.dst; h.ackReq = f.ackReq;
            if (f.ackReq && f.seq >= 0) asks_.push_back({f.sec, f.src, f.dst, f.seq});
        }
        h_.add(h);
        last_ = std::max(last_, f.sec);
    }
    // the receiver's frame log (newest first): only the frames newer than the last one fed
    void addLog(const std::vector<ZbFrame>& log) {
        const double was = last_;
        for (auto it = log.rbegin(); it != log.rend(); ++it) if (it->sec > was) add(*it);
        h_.tick(log.empty() ? last_ : std::max(last_, log.front().sec));
    }
    void tick(double now) { h_.tick(now); }
    const HomeNetHealth& health() const { return h_; }

private:
    struct Ask { double t; std::string src, dst; int seq; };
    int pan_;
    double last_ = -1e18;
    std::deque<Ask> asks_;
    HomeNetHealth h_;
};

}  // namespace dect2
