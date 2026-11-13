// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RDS-TMC decoder, see rds_tmc.h.
#include "dect2/fm/rds_tmc.h"
#include <algorithm>
#include <cstdio>

namespace dect2 {
namespace {

struct Ev { int code; bool longLasting; const char* text; };
// The frequent events of ISO 14819-2 (code, longer-lasting nature, English text; wording after the public OSM TMC event list).
// The full list has about 1550 entries.
const Ev kEvents[] = {
    {1, false, "traffic problem"},
    {2, false, "queuing traffic. Danger of stationary traffic"},
    {12, false, "accident, traffic being directed around accident area"},
    {16, false, "closed, rescue and recovery work in progress"},
    {24, true, "bridge closed"},
    {25, true, "tunnel closed"},
    {26, false, "bridge blocked"},
    {27, false, "tunnel blocked"},
    {28, false, "road closed intermittently"},
    {41, true, "overtaking lane closed"},
    {42, false, "overtaking lane blocked"},
    {51, true, "roadworks, overtaking lane closed"},
    {52, true, "roadworks on the hard shoulder"},
    {80, true, "heavy traffic has to be expected"},
    {91, false, "delays for cars"},
    {101, false, "stationary traffic"},
    {107, false, "stationary traffic expected"},
    {108, false, "queuing traffic"},
    {114, false, "queuing traffic expected"},
    {115, false, "slow traffic"},
    {121, false, "slow traffic expected"},
    {122, false, "heavy traffic"},
    {123, false, "heavy traffic expected"},
    {127, false, "traffic congestion cleared"},
    {130, false, "danger of stationary traffic"},
    {132, false, "danger of queuing traffic"},
    {138, false, "queuing traffic. Approach with care"},
    {139, false, "queuing traffic around a bend in the road"},
    {140, false, "queuing traffic over the crest of a hill"},
    {201, false, "accident"},
    {202, false, "serious accident"},
    {203, false, "multi-vehicle accident"},
    {204, false, "accident involving heavy lorr(y/ies)"},
    {205, false, "accident involving hazardous materials"},
    {206, false, "fuel spillage accident"},
    {207, false, "chemical spillage accident"},
    {208, false, "vehicles slowing to look at accident"},
    {209, false, "accident in the opposing lanes"},
    {211, false, "broken down vehicle"},
    {212, false, "broken down heavy lorr(y/ies)"},
    {213, false, "vehicle fire"},
    {215, false, "accident. Stationary traffic"},
    {222, false, "accident. Queuing traffic"},
    {229, false, "accident. Slow traffic"},
    {240, false, "road closed due to accident"},
    {333, false, "accident cleared"},
    {336, false, "oil spillage accident"},
    {337, false, "overturned vehicle"},
    {344, false, "accident investigation work"},
    {345, false, "secondary accident"},
    {397, false, "rescue and recovery work in progress"},
    {401, true, "closed"},
    {402, false, "blocked"},
    {403, true, "closed for heavy vehicles"},
    {406, true, "entry slip road closed"},
    {407, true, "exit slip road closed"},
    {408, true, "slip roads closed"},
    {466, true, "slip roads reopened"},
    {467, false, "reopened"},
    {469, true, "closed ahead"},
    {470, false, "blocked ahead"},
    {471, true, "entry slip road closed"},
    {474, true, "exit slip road closed"},
    {478, true, "connecting carriageway closed"},
    {500, true, "lane closed"},
    {501, true, "right lane closed"},
    {502, true, "centre lane closed"},
    {503, true, "left lane closed"},
    {504, true, "hard shoulder closed"},
    {505, true, "two lanes closed"},
    {506, true, "three lanes closed"},
    {507, false, "right lane blocked"},
    {508, false, "centre lane blocked"},
    {509, false, "left lane blocked"},
    {510, false, "hard shoulder blocked"},
    {514, true, "carriageway reduced to one lane"},
    {515, true, "carriageway reduced to two lanes"},
    {516, true, "carriageway reduced to three lanes"},
    {517, true, "contraflow"},
    {518, true, "narrow lanes"},
    {520, false, "lane blocked"},
    {624, true, "lane closures removed"},
    {631, false, "road cleared"},
    {634, true, "all carriageways reopened"},
    {641, true, "one lane closed"},
    {646, false, "one lane blocked"},
    {664, true, "carriageway closed"},
    {665, true, "both directions closed"},
    {701, true, "roadworks"},
    {702, true, "major roadworks"},
    {703, false, "maintenance work"},
    {704, true, "resurfacing work"},
    {707, true, "bridge maintenance work"},
    {735, true, "closed due to roadworks"},
    {800, true, "roadworks cleared"},
    {802, true, "long-term roadworks"},
    {804, false, "slow moving maintenance vehicles"},
    {900, false, "flooding expected"},
    {903, false, "spillage on the road"},
    {904, false, "storm damage"},
    {907, true, "flooding"},
    {909, false, "flash floods"},
    {911, true, "avalanches"},
    {913, true, "rockfalls"},
    {916, true, "road surface in poor condition"},
    {921, false, "serious fire"},
    {922, false, "animals on roadway"},
    {925, false, "blocked by storm damage"},
    {926, false, "blocked by fallen trees"},
    {938, true, "closed due to flooding"},
    {1002, false, "danger of aquaplaning"},
    {1003, true, "slippery road"},
    {1004, true, "mud on road"},
    {1005, true, "leaves on road"},
    {1006, true, "ice"},
    {1007, true, "danger of ice"},
    {1008, true, "black ice"},
    {1009, false, "freezing rain"},
    {1012, true, "snow on the road"},
    {1013, true, "packed snow"},
    {1019, false, "slippery road due to frost"},
    {1020, true, "road blocked by snow"},
    {1044, true, "oil on road"},
    {1060, true, "icy patches on bridges"},
    {1067, false, "large animals on roadway"},
    {1084, false, "house fire"},
    {1085, true, "forest fire"},
    {1101, false, "heavy snowfall"},
    {1104, false, "snowfall"},
    {1106, false, "hail"},
    {1108, false, "thunderstorms"},
    {1109, false, "heavy rain"},
    {1112, false, "rain"},
    {1205, true, "strong winds"},
    {1210, true, "crosswinds"},
    {1301, false, "dense fog"},
    {1304, false, "fog"},
    {1308, false, "freezing fog"},
    {1318, false, "visibility reduced"},
    {1323, false, "blowing snow"},
    {1326, false, "sandstorms"},
    {1332, true, "smog alert"},
    {1450, true, "international sports meeting"},
    {1461, false, "marathon"},
    {1468, false, "funfair"},
    {1475, true, "strike"},
    {1477, true, "police checkpoint"},
    {1498, false, "police activity ongoing"},
    {1513, false, "demonstration"},
    {1515, false, "security alert"},
    {1601, false, "delays"},
    {1602, false, "delays up to 15 minutes"},
    {1603, false, "delays up to 30 minutes"},
    {1604, false, "delays up to one hour"},
    {1605, false, "delays up to two hours"},
    {1606, false, "delays of several hours"},
    {1607, false, "delays expected"},
    {1608, false, "long delays"},
    {1631, true, "very long delays"},
    {1680, true, "delays have to be expected"},
    {1731, false, "abnormal load, danger"},
    {1734, false, "slow vehicle, danger"},
    {1738, false, "military convoy, danger"},
    {1801, false, "lane control signs not working"},
    {1903, false, "car park full"},
    {1904, false, "all car parks full"},
    {1971, false, "police directing traffic"},
    {1990, false, "car park closed"},
};

// Duration (dynamic events) or persistence (longer-lasting ones) of a single-group message, and the duration field of a multi-group one.
const char* const kDurD[8] = {"", "15 minutes", "30 minutes", "1 hour", "2 hours", "3 hours", "4 hours", "rest of the day"};
const char* const kDurL[8] = {"", "a few hours", "until tonight", "rest of the day", "until tomorrow", "a few days", "a week", "a long time"};
const double kSecD[8] = {3600, 900, 1800, 3600, 7200, 10800, 14400, 43200};
const double kSecL[8] = {86400, 10800, 28800, 43200, 86400, 259200, 604800, 1209600};

const int kLabelBits[16] = {3, 3, 5, 5, 5, 8, 8, 8, 8, 11, 16, 16, 16, 16, 0, 0};
}

std::string TmcDecoder::eventText(int code, bool* longLasting) {
    for (const Ev& e : kEvents) if (e.code == code) { if (longLasting) *longLasting = e.longLasting; return e.text; }
    if (longLasting) *longLasting = false;
    return "event " + std::to_string(code);
}

bool TmcDecoder::eventKnown(int code) {
    for (const Ev& e : kEvents) if (e.code == code) return true;
    return false;
}

std::string TmcDecoder::durationText(int code, bool longLasting) {
    if (code < 1 || code > 7) return "";
    return (longLasting ? kDurL : kDurD)[code];
}

void TmcDecoder::onGroup3A(uint16_t b, uint16_t c, uint16_t d) {
    if (d != 0xCD46 && d != 0xCD47) return;
    if ((b & 31) != 16) return;   // the application sits in group 8A (group type 8, version A)
    sys_.seen = true; sys_.aid = d;
    const int variant = c >> 14;
    if (variant == 0) { sys_.ltn = (c >> 6) & 63; sys_.afi = (c >> 5) & 1; sys_.mode = (c >> 4) & 1; }
    else if (variant == 1) sys_.sid = (c >> 6) & 63;
}

void TmcDecoder::onGroup8A(uint16_t b, uint16_t c, uint16_t d, double now) {
    if (!sys_.seen) return;
    const bool tuning = (b >> 4) & 1, single = (b >> 3) & 1; const int dp = b & 7;
    if (tuning) return;   // tuning information (service names, other networks): not a traffic message
    if (single) {
        TmcMessage m;
        m.diversion = (c >> 15) & 1; m.negative = (c >> 14) & 1; m.extent = (c >> 11) & 7; m.event = c & 0x7FF; m.location = d;
        m.durationCode = dp;
        bool ll = false;
        m.text = eventText(m.event, &ll);
        if (dp) m.durationText = (ll ? kDurL : kDurD)[dp];
        m.expires = (ll ? kSecL : kSecD)[dp];   // seconds for now, made absolute in store()
        if (m.event == 0 || m.location == 0) return;
        store(m, now);
        return;
    }
    const int ci = b & 7;
    Pending& p = pend_[ci];
    if ((c >> 15) & 1) {   // first group
        p = Pending();
        p.open = true;
        p.m.multiGroup = true;
        p.m.negative = (c >> 14) & 1; p.m.extent = (c >> 11) & 7; p.m.event = c & 0x7FF; p.m.location = d;
        p.m.text = eventText(p.m.event, &p.longLasting);
        if (p.m.event == 0 || p.m.location == 0) p = Pending();
        return;
    }
    if (!p.open) return;   // the first group was lost: the message cannot be completed
    const bool second = (c >> 14) & 1;
    const int gsi = (c >> 12) & 3;
    if (second) {
        if (p.remaining != -1) { p = Pending(); return; }
        p.remaining = gsi;
    } else {
        if (p.remaining < 1 || gsi != p.remaining - 1) { p = Pending(); return; }   // a group lost or out of order
        p.remaining = gsi;
    }
    // 28 bits of free format: 12 from block C, 16 from block D
    for (int i = 11; i >= 0; i--) p.bits.push_back((c >> i) & 1);
    for (int i = 15; i >= 0; i--) p.bits.push_back((d >> i) & 1);
    if (p.remaining == 0) { finish(p, now); p = Pending(); }
}

void TmcDecoder::finish(Pending& p, double now) {
    TmcMessage m = p.m;
    size_t pos = 0;
    auto take = [&](int n) { int v = 0; for (int i = 0; i < n; i++) v = v << 1 | p.bits[pos++]; return v; };
    int dur = 0;
    while (pos + 4 <= p.bits.size()) {
        const int label = take(4);
        const int n = kLabelBits[label];
        if (label >= 14 || pos + (size_t)n > p.bits.size()) break;   // separator or reserved: the rest is padding
        const int v = take(n);
        if (label == 2 || label == 3 || label == 9 || label == 10 || label == 11) m.details.push_back({label, v});
        char buf[64];
        switch (label) {
        case 0: dur = v; break;
        case 2: snprintf(buf, sizeof buf, "length of route affected %d km", v <= 10 ? v : v <= 15 ? 10 + (v - 10) * 2 : 20 + (v - 15) * 5); m.more.push_back(buf); break;
        case 3: snprintf(buf, sizeof buf, "speed limit %d km/h", v * 5); m.more.push_back(buf); break;
        case 9: if (v) m.more.push_back(eventText(v)); break;
        case 10: snprintf(buf, sizeof buf, "diversion via location %d", v); m.more.push_back(buf); m.diversion = true; break;
        case 11: snprintf(buf, sizeof buf, "destination location %d", v); m.more.push_back(buf); break;
        default: break;   // control codes, quantifiers, supplementary information, start/stop times: not shown
        }
    }
    m.durationCode = dur;
    if (dur) m.durationText = (p.longLasting ? kDurL : kDurD)[dur];
    m.expires = (p.longLasting ? kSecL : kSecD)[dur];
    store(m, now);
}

void TmcDecoder::store(TmcMessage m, double now) {
    const double span = m.expires;   // the duration in seconds, set by the caller
    for (TmcMessage& o : msgs_) {
        if (o.event == m.event && o.location == m.location && o.negative == m.negative && o.extent == m.extent && o.diversion == m.diversion &&
            o.durationCode == m.durationCode && o.more == m.more) {
            o.count++; o.lastSeen = now; o.expires = now + span;   // a repeat: kept once, its time runs from the last reception
            return;
        }
    }
    m.received = m.lastSeen = now; m.expires = now + span;
    msgs_.push_back(m);
    if (msgs_.size() > kMaxMessages) msgs_.erase(msgs_.begin());
}

std::vector<TmcMessage> TmcDecoder::messages(double now) const {
    std::vector<TmcMessage> out;
    for (auto it = msgs_.rbegin(); it != msgs_.rend(); ++it) if (it->expires > now) out.push_back(*it);
    return out;
}

} // namespace dect2
