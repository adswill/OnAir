// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF propagation dashboard model (hfbeacon_prop.h).
#include "dect2/hfbeacon/hfbeacon_prop.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace dect2::hfp {

namespace {
struct Edge { const char* name; double lo, hi; };
// amateur bands (ITU region 1 edges, a little wider so a dial plus audio offset still lands inside)
const Edge kEdge[kBands] = {
    {"160 m", 1.80e6, 2.00e6}, {"80 m", 3.50e6, 3.80e6}, {"60 m", 5.25e6, 5.45e6}, {"40 m", 7.00e6, 7.20e6},
    {"30 m", 10.10e6, 10.15e6}, {"20 m", 14.00e6, 14.35e6}, {"17 m", 18.068e6, 18.168e6}, {"15 m", 21.00e6, 21.45e6},
    {"12 m", 24.89e6, 24.99e6}, {"10 m", 28.00e6, 29.70e6}, {"6 m", 50.00e6, 54.00e6}};
const char* const kSrcName[kSources] = {"NCDXF/IARU", "single letters", "Long Dashes", "time signal", "WSPR", "FT8", "FT4", "FT2"};
const double kWindow = 1800;   // the "now" window, s
}

const char* bandName(int b) { return b >= 0 && b < kBands ? kEdge[b].name : ""; }
const char* sourceName(int s) { return s >= 0 && s < kSources ? kSrcName[s] : ""; }
int bandOf(double hz) {
    // the broadcast / beacon frequencies close to a band count too: Long Dashes 11999 kHz and 8999 kHz are not amateur and stay out
    for (int i = 0; i < kBands; i++) if (hz >= kEdge[i].lo - 5e3 && hz <= kEdge[i].hi + 5e3) return i;
    return -1;
}

void Model::add(const Obs& o) {
    const int b = bandOf(o.hz);
    if (b < 0 || !(o.utc > 0) || o.source < 0 || o.source >= kSources) return;
    const double start = std::floor(o.utc / kBinSec) * kBinSec;
    const int idx = (int)(((long long)(start / kBinSec)) % kBins);
    Slot& s = slots_[b][idx];
    if (s.b.stamp != start) {
        if (s.b.stamp > start) return;       // older than what the slot holds
        s = Slot();
        s.b.stamp = start;
    }
    s.b.count++;
    if (s.ids.size() < 400) s.ids.insert(o.id);
    s.b.stations = (int)s.ids.size();
    s.b.bestSnr = std::max(s.b.bestSnr, o.snrDb);
    if (o.distKm > s.b.maxKm) s.b.maxKm = o.distKm;
    s.bySource[o.source]++;
    total_++;
}

std::vector<BandNow> Model::now(double nowUtc) const {
    std::vector<BandNow> out(kBands);
    const double cur = std::floor(nowUtc / kBinSec) * kBinSec;
    for (int b = 0; b < kBands; b++) {
        BandNow& r = out[(size_t)b];
        r.band = b;
        std::set<std::string> ids;
        for (int k = 0; k < kBins; k++) {
            const Slot& s = slots_[b][k];
            if (s.b.stamp < 0) continue;
            // a bin counts when it ends after nowUtc - window and started at or before now
            if (s.b.stamp + kBinSec <= nowUtc - kWindow || s.b.stamp > cur) continue;
            r.obs += s.b.count;
            r.bestSnr = std::max(r.bestSnr, s.b.bestSnr);
            r.maxKm = std::max(r.maxKm, s.b.maxKm);
            ids.insert(s.ids.begin(), s.ids.end());
            for (int q = 0; q < kSources; q++) r.bySource[q] += s.bySource[q];
        }
        r.stations = (int)ids.size();
        if (r.stations > 0) {
            const double snrTerm = std::min(1.0, std::max(0.0, (r.bestSnr + 20.0) / 30.0));
            r.score = 0.5 * std::min(1.0, r.stations / 8.0) + 0.3 * snrTerm + 0.2 * std::min(1.0, r.maxKm / 10000.0);
            r.state = (r.stations >= 3 && r.score >= 0.4) ? Open : Marginal;
        }
    }
    return out;
}

std::vector<Bin> Model::history(int band, double nowUtc) const {
    std::vector<Bin> out((size_t)kBins);
    if (band < 0 || band >= kBands) return out;
    const double cur = std::floor(nowUtc / kBinSec) * kBinSec;
    for (int i = 0; i < kBins; i++) {
        const double st = cur - (kBins - 1 - i) * kBinSec;
        const int idx = (int)(((long long)(st / kBinSec)) % kBins);
        const Slot& s = slots_[band][idx];
        out[(size_t)i].stamp = st;
        if (s.b.stamp == st) out[(size_t)i] = s.b;
    }
    return out;
}

int Model::best(double nowUtc, std::string* why) const {
    const auto v = now(nowUtc);
    int bi = -1;
    for (const auto& r : v)
        if (r.state >= Marginal && (bi < 0 || r.score > v[(size_t)bi].score)) bi = r.band;
    if (why) {
        why->clear();
        if (bi >= 0) {
            const BandNow& r = v[(size_t)bi];
            char b[160];
            if (r.maxKm > 0) snprintf(b, sizeof b, "%d stations in 30 min, best %+.0f dB, farthest %.0f km", r.stations, (double)r.bestSnr, r.maxKm);
            else snprintf(b, sizeof b, "%d stations in 30 min, best %+.0f dB", r.stations, (double)r.bestSnr);
            *why = b;
        }
    }
    return bi;
}

void Model::clear() { for (auto& row : slots_) for (auto& s : row) s = Slot(); total_ = 0; }

std::string Model::save() const {
    std::string o = "hfprop 1\n";
    char b[160];
    for (int bd = 0; bd < kBands; bd++)
        for (int k = 0; k < kBins; k++) {
            const Slot& s = slots_[bd][k];
            if (s.b.stamp < 0 || s.b.count == 0) continue;
            snprintf(b, sizeof b, "%d %.0f %d %d %.1f %.0f", bd, s.b.stamp, s.b.count, s.b.stations, (double)s.b.bestSnr, s.b.maxKm);
            o += b;
            for (int q = 0; q < kSources; q++) { snprintf(b, sizeof b, " %d", s.bySource[q]); o += b; }
            o += "\n";
        }
    return o;
}

void Model::load(const std::string& text, double nowUtc) {
    clear();
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != "hfprop 1") return;
    while (std::getline(in, line)) {
        int bd, cnt, st, src[kSources];
        double stamp, snr, km;
        if (sscanf(line.c_str(), "%d %lf %d %d %lf %lf %d %d %d %d %d %d %d %d", &bd, &stamp, &cnt, &st, &snr, &km,
                   &src[0], &src[1], &src[2], &src[3], &src[4], &src[5], &src[6], &src[7]) != 6 + kSources) continue;
        if (bd < 0 || bd >= kBands || cnt <= 0 || st < 0 || !(stamp > 0)) continue;
        if (std::fmod(stamp, kBinSec) != 0 || stamp > nowUtc || stamp <= nowUtc - kBins * kBinSec) continue;
        const int idx = (int)(((long long)(stamp / kBinSec)) % kBins);
        Slot& s = slots_[bd][idx];
        s = Slot();
        s.b.stamp = stamp; s.b.count = cnt; s.b.stations = st; s.b.bestSnr = (float)snr; s.b.maxKm = km;
        for (int q = 0; q < kSources; q++) s.bySource[q] = std::max(0, src[q]);
        // the station names are not kept: stand-ins keep the counts when bins are merged with new observations of the same bin
        for (int i = 0; i < std::min(st, 400); i++) s.ids.insert("#" + std::to_string(i));
        total_ += (uint64_t)cnt;
    }
}

} // namespace dect2::hfp
