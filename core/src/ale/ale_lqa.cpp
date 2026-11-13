// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/ale/ale_lqa.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2 {
namespace {
int hourOf(double utc) {
    const double d = std::fmod(utc, 86400.0);
    return (int)std::floor((d < 0 ? d + 86400.0 : d) / 3600.0) % 24;
}
double snrToQuality(double snr) { return std::min(1.0, std::max(0.0, (snr + 5.0) / 30.0)) * 100.0; }   // -5 dB (0) .. 25 dB (100) in 3 kHz
}

AleLqa& AleLqa::global() { static AleLqa g; return g; }

double AleLqa::decay(double dt) const { return dt <= 0 ? 1.0 : std::pow(0.5, dt / (cfg_.halfLifeDays * 86400.0)); }

AleLqa::Bin AleLqa::decayed(const Bin& b, double now, double halfLife) {
    if (b.w <= 0 || now <= b.stamp) return b;
    const double f = std::pow(0.5, (now - b.stamp) / (halfLife * 86400.0));
    Bin o; o.w = b.w * f; o.snr = b.snr * f; o.ber = b.ber * f; o.stamp = now;
    return o;
}

void AleLqa::add(const AleLqaObs& in) {
    AleLqaObs o = in;
    for (auto& ch : o.station) if (ch == ' ' || ch == '\t' || ch == '\n') ch = '_';   // the saved text is one word per name
    if (o.station.empty() || o.station.size() > 60 || o.hz <= 0 || !std::isfinite(o.snrDb) || o.utc <= 0) return;
    if (!std::isfinite(o.ber)) o.ber = 0;
    std::lock_guard<std::mutex> lk(mu_);
    if (st_.size() >= cfg_.maxStations && st_.find(o.station) == st_.end()) {   // the station heard longest ago goes
        auto old = st_.begin();
        for (auto it = st_.begin(); it != st_.end(); ++it) if (it->second.lastUtc < old->second.lastUtc) old = it;
        st_.erase(old);
    }
    Stn& s = st_[o.station];
    s.lastUtc = std::max(s.lastUtc, o.utc);
    const long k = key(o.hz);
    if (s.ch.size() >= cfg_.maxChannels && s.ch.find(k) == s.ch.end()) {
        auto old = s.ch.begin();
        for (auto it = s.ch.begin(); it != s.ch.end(); ++it) if (it->second.lastUtc < old->second.lastUtc) old = it;
        s.ch.erase(old);
    }
    Chan& c = s.ch[k];
    c.hz = o.hz;
    Bin& b = c.bin[hourOf(o.utc)];
    b = decayed(b, o.utc, cfg_.halfLifeDays);
    const double wt = o.sounding ? 1.0 : 0.8;   // a sounding is sent for exactly this; a call is as good a measurement but its power may differ
    b.w += wt; b.snr += wt * o.snrDb; b.ber += wt * o.ber; b.stamp = std::max(b.stamp, o.utc);
    if (o.utc >= c.lastUtc) { c.lastUtc = o.utc; c.lastSnr = o.snrDb; c.lastBer = o.ber; }
    total_++;
}

AleLqaRank AleLqa::rankOf(const std::string& st, const Chan& c, double now, const std::vector<hfp::BandNow>* prop) const {
    AleLqaRank r;
    r.station = st; r.hz = c.hz; r.lastUtc = c.lastUtc;
    const int h = hourOf(now);
    // the hour of the day: this hour counts fully, the neighbours by half
    double w = 0, snr = 0, ber = 0, wHour = 0;
    for (int d = -1; d <= 1; d++) {
        const double f = d == 0 ? 1.0 : 0.5;
        const Bin b = decayed(c.bin[(h + d + 24) % 24], now, cfg_.halfLifeDays);
        w += f * b.w; snr += f * b.snr; ber += f * b.ber;
        if (d == 0) wHour = b.w;
    }
    double tod = -1, todSnr = 0, todBer = 0;
    if (w > 0.05) { todSnr = snr / w; todBer = ber / w; tod = snrToQuality(todSnr) * (1.0 - std::min(1.0, todBer * 6.0)); }
    const bool recent = c.lastUtc > 0 && now - c.lastUtc <= cfg_.recentSec && now >= c.lastUtc;
    const double recQ = snrToQuality(c.lastSnr) * (1.0 - std::min(1.0, c.lastBer * 6.0));
    double q, conf;
    if (tod >= 0 && recent) { q = 0.55 * recQ + 0.45 * tod; conf = 1.0; r.snrDb = (float)(0.55 * c.lastSnr + 0.45 * todSnr); r.ber = (float)(0.55 * c.lastBer + 0.45 * todBer); }
    else if (recent) { q = recQ; conf = 0.7; r.snrDb = c.lastSnr; r.ber = c.lastBer; }
    else if (tod >= 0) { q = tod; conf = w / (w + 2.0); r.snrDb = (float)todSnr; r.ber = (float)todBer; }
    else {   // only other hours of the day: a faint prior (propagation changes, but a channel that worked is a channel to try)
        double sw = 0, ss = 0;
        for (int k = 0; k < 24; k++) { const Bin b = decayed(c.bin[k], now, cfg_.halfLifeDays); sw += b.w; ss += b.snr; }
        q = sw > 0 ? snrToQuality(ss / sw) * 0.5 : 0; conf = sw > 0 ? 0.3 : 0; r.snrDb = sw > 0 ? (float)(ss / sw) : 0;
    }
    r.obs = w; r.recent = recent;
    double score = q * (0.4 + 0.6 * conf);
    if (prop) {
        const int band = hfp::bandOf(c.hz);
        if (band >= 0 && (size_t)band < prop->size()) {
            const hfp::BandNow& bn = (*prop)[(size_t)band];
            r.propState = bn.state;
            // a band the beacons say is open now is worth a bit more; a closed one only when something was heard on it recently
            if (bn.state == hfp::Open) score *= 1.15;
            else if (bn.state == hfp::Closed && !recent) score *= 0.8;
        }
    }
    r.score = std::min(100.0, std::max(0.0, score));
    char b[220];
    std::string t;
    if (tod >= 0) { snprintf(b, sizeof b, "usually %+.0f dB at this hour (%.0f heard)", todSnr, wHour); t = b; }
    else if (r.obs <= 0 && !recent) t = "heard at other hours only";
    if (recent) { snprintf(b, sizeof b, "%sheard %.0f min ago (%+.0f dB)", t.empty() ? "" : ", ", (now - c.lastUtc) / 60.0, (double)c.lastSnr); t += b; }
    if (r.propState == hfp::Open) t += ", band open";
    else if (r.propState == hfp::Closed) t += ", band quiet";
    r.why = t;
    return r;
}

std::vector<AleLqaRank> AleLqa::rank(const std::string& station, double now, const std::vector<hfp::BandNow>* prop) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<AleLqaRank> v;
    auto it = st_.find(station);
    if (it == st_.end()) return v;
    for (const auto& kv : it->second.ch) v.push_back(rankOf(station, kv.second, now, prop));
    std::stable_sort(v.begin(), v.end(), [](const AleLqaRank& a, const AleLqaRank& b) { return a.score > b.score; });
    return v;
}

std::vector<AleLqaRank> AleLqa::best(double now, const std::vector<hfp::BandNow>* prop) const {
    std::vector<std::string> names = stations();
    std::vector<AleLqaRank> out;
    for (const auto& n : names) { auto r = rank(n, now, prop); if (!r.empty()) out.push_back(r.front()); }
    std::stable_sort(out.begin(), out.end(), [](const AleLqaRank& a, const AleLqaRank& b) { return a.lastUtc > b.lastUtc; });
    return out;
}

std::vector<std::string> AleLqa::stations() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> v;
    for (const auto& kv : st_) v.push_back(kv.first);
    return v;
}

std::vector<AleLqa::Hour> AleLqa::profile(const std::string& station, double hz, double now) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Hour> out(24);
    auto it = st_.find(station);
    if (it == st_.end()) return out;
    auto c = it->second.ch.find(key(hz));
    if (c == it->second.ch.end()) return out;
    for (int h = 0; h < 24; h++) {
        const Bin b = decayed(c->second.bin[h], now, cfg_.halfLifeDays);
        if (b.w > 0.05) { out[(size_t)h].w = b.w; out[(size_t)h].snrDb = (float)(b.snr / b.w); out[(size_t)h].ber = (float)(b.ber / b.w); }
    }
    return out;
}

uint64_t AleLqa::total() const { std::lock_guard<std::mutex> lk(mu_); return total_; }
void AleLqa::clear() { std::lock_guard<std::mutex> lk(mu_); st_.clear(); total_ = 0; }

// one line per (station, channel, hour): "station hz hour w snr ber stamp lastUtc"; damaged lines are skipped
std::string AleLqa::save() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string o = "alelqa 1\n";
    char b[240];
    for (const auto& s : st_)
        for (const auto& kv : s.second.ch)
            for (int h = 0; h < 24; h++) {
                const Bin& bn = kv.second.bin[h];
                if (bn.w <= 0) continue;
                snprintf(b, sizeof b, "%s %.1f %d %.4f %.4f %.5f %.0f %.0f %.2f %.4f\n", s.first.c_str(), kv.second.hz, h, bn.w, bn.snr, bn.ber, bn.stamp, kv.second.lastUtc, (double)kv.second.lastSnr, (double)kv.second.lastBer);
                o += b;
            }
    return o;
}

void AleLqa::load(const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("alelqa 1", 0) != 0) return;
    while (std::getline(in, line)) {
        char name[64];
        double hz, w, snr, ber, stamp, last, lsnr, lber;
        int h;
        if (sscanf(line.c_str(), "%63s %lf %d %lf %lf %lf %lf %lf %lf %lf", name, &hz, &h, &w, &snr, &ber, &stamp, &last, &lsnr, &lber) != 10) continue;
        if (h < 0 || h > 23 || hz <= 0 || w <= 0 || !std::isfinite(w + snr + ber + stamp + last) || w > 1e6) continue;
        Stn& s = st_[name];
        Chan& c = s.ch[key(hz)];
        c.hz = hz; c.lastUtc = std::max(c.lastUtc, last);
        if (last >= c.lastUtc) { c.lastSnr = (float)lsnr; c.lastBer = (float)lber; }
        c.bin[h].w = w; c.bin[h].snr = snr; c.bin[h].ber = ber; c.bin[h].stamp = stamp;
        s.lastUtc = std::max(s.lastUtc, last);
    }
}

} // namespace dect2
