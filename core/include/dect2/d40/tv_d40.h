// 4.0 television / radio-digital helpers (pure, no GUI). Tests: tests/d40/test_tv_d40.cpp
//  1. guessQam       DVB-C / J.83 / cable: which QAM order is in a cloud of equalised symbols (16, 32, 64, 128, 256): fit every ideal grid (square or cross), best fit above its own quantisation floor wins (a finer grid always fits a little better,
//                    so each fit is judged against d^2/6), and the answer is "none" when no grid beats that floor by 3 dB.
//  2. HdrLevels      HD Radio (FM IBOC): digital sideband level over the analogue host in dBc per sideband (the hybrid carries each about -20 dBc, -10 dBc in the extended mode), the imbalance
//                    between the two sides (adjacent-channel interference or a weak antenna null on one side), and hybrid / all-digital / analogue only.
//  3. merPerCarrier  DRM / OFDM: the MER of every carrier from the equalised cells against their ideal values, worst carriers, and notches (runs of carriers well below the mean = a narrowband
//                    interferer or a fading hole), plus the slope across the band (tilt = an unequalised filter).
//  4. classifyAtv    analogue TV standard from measurements of the video line: line rate -> 625 / 525 lines, colour subcarrier -> PAL / NTSC / PAL-M / PAL-N / NTSC 4.43, SECAM ident, mono.
//  5. videoScore     one 0 - 100 number for an analogue video link (FPV or ATV) from SNR, sync stability, line jitter and the share of lines that locked, with the weakest part named.
//  6. pickFpvChannels free channels for N pilots out of the 5.8 GHz plans: not occupied, not adjacent to an occupied one, and spaced from each other.
#pragma once
#include "dect2/fpv/fpv_chan.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <string>
#include <vector>

namespace dect2::d40 {

// ---------- 1. QAM order
inline std::vector<std::complex<double>> qamGrid(int order) {
    std::vector<std::complex<double>> g;
    int side = 0;
    switch (order) { case 16: side = 4; break; case 32: side = 6; break; case 64: side = 8; break; case 128: side = 12; break; case 256: side = 16; break; default: return g; }
    const int cut = order == 32 ? 1 : (order == 128 ? 2 : 0);   // cross: drop cut x cut corners
    for (int i = 0; i < side; i++) for (int q = 0; q < side; q++) {
        const bool cornerI = i < cut || i >= side - cut, cornerQ = q < cut || q >= side - cut;
        if (cornerI && cornerQ) continue;
        g.push_back({2.0 * i - (side - 1), 2.0 * q - (side - 1)});
    }
    double p = 0; for (auto& c : g) p += std::norm(c); p /= (double)g.size();
    for (auto& c : g) c /= std::sqrt(p);
    return g;
}
struct QamGuess { int order = 0; double merDb = 0; std::vector<std::pair<int, double>> all; };
inline QamGuess guessQam(const std::vector<std::complex<double>>& pts) {
    QamGuess r;
    if (pts.size() < 400) return r;
    double p = 0; for (auto& c : pts) p += std::norm(c); p /= (double)pts.size();
    if (!(p > 0)) return r;
    const double sc = 1.0 / std::sqrt(p);
    for (int order : {16, 32, 64, 128, 256}) {
        const auto g = qamGrid(order);
        double e = 0;
        for (const auto& c0 : pts) {
            const auto c = c0 * sc; double best = 1e9;
            for (const auto& q : g) best = std::min(best, std::norm(c - q));
            e += best;
        }
        e /= (double)pts.size();
        const double mer = 10 * std::log10(1.0 / std::max(e, 1e-12));
        r.all.push_back({order, mer});
    }
    // a finer grid always fits a cloud better (its own quantisation error), so each fit is judged against what a random cloud would score on that grid: d^2/6 for lattice spacing d
    double bestEx = -1e9;
    for (auto& a : r.all) {
        const auto g = qamGrid(a.first);
        double dmin = 1e9; for (size_t i = 1; i < g.size(); i++) { const double d = std::abs(g[i] - g[0]); if (d > 1e-9) dmin = std::min(dmin, d); }
        const double floorDb = 10 * std::log10(6.0 / (dmin * dmin));
        const double ex = a.second - floorDb;
        if (ex > bestEx) { bestEx = ex; r.order = a.first; r.merDb = a.second; }
    }
    if (bestEx < 3.0) { r.order = 0; r.merDb = 0; }
    return r;
}

// ---------- 2. HD Radio
struct HdrLevels { bool ok = false; double lowerDbc = -99, upperDbc = -99, imbalanceDb = 0; std::string kind; std::string note; };
// db centred on the station, binHz <= 2 kHz, span at least +-220 kHz (the noise is taken from beyond +-200 kHz when present, else the 15th percentile)
inline HdrLevels hdrLevels(const std::vector<float>& db, double binHz) {
    HdrLevels r;
    const int n = (int)db.size(), c = n / 2;
    if (!(binHz > 0) || (c + 200e3 / binHz >= n)) return r;
    auto band = [&](double f0, double f1) { double s = 0; int m = 0; for (int i = (int)std::lround(f0 / binHz); i <= (int)std::lround(f1 / binHz); i++) { s += std::pow(10.0, db[(size_t)(c + i)] / 10.0); m++; } return std::make_pair(s, m); };
    std::vector<double> all; for (float v : db) all.push_back(v);
    std::sort(all.begin(), all.end());
    const double noiseDb = all[all.size() * 15 / 100];
    const double noiseLin = std::pow(10.0, noiseDb / 10.0);
    auto pw = [&](double f0, double f1) { auto b = band(f0, f1); return std::max(1e-12, b.first - noiseLin * b.second); };
    const double host = pw(-75e3, 75e3);
    const double up = pw(129e3, 198e3), lo = pw(-198e3, -129e3);
    // per sideband power is compared with the host power directly (a fixed -20 dBc total is the hybrid spec; the sideband's own width is what the standard refers to)
    r.upperDbc = 10 * std::log10(up / host); r.lowerDbc = 10 * std::log10(lo / host);
    r.imbalanceDb = r.upperDbc - r.lowerDbc;
    const double hostFloor = pw(-75e3, 75e3) / (150e3 / binHz);
    const double sbFloor = std::max(up, lo) / (69e3 / binHz);
    const bool sbOn = std::min(up, lo) / (69e3 / binHz) > noiseLin * 3;   // both sidebands over 4.8 dB above the noise
    const bool hostOn = hostFloor > noiseLin * 3;
    (void)sbFloor;
    r.ok = true;
    if (sbOn && hostOn) r.kind = "hybrid";
    else if (sbOn && !hostOn) r.kind = "all-digital";
    else if (hostOn) r.kind = "analogue only";
    else r.kind = "no signal";
    if (r.kind == "hybrid" && std::fabs(r.imbalanceDb) > 6) r.note = std::string(r.imbalanceDb > 0 ? "lower" : "upper") + " sideband " + std::to_string((int)std::lround(std::fabs(r.imbalanceDb))) + " dB weaker: first-adjacent interference on that side or an antenna null";
    return r;
}

// ---------- 3. MER per carrier
struct CarrierMer { std::vector<double> merDb; double meanDb = 0, worstDb = 0; int worstCarrier = -1; double tiltDbPerCarrier = 0; std::vector<std::pair<int, int>> notches; };
// rx, ref: [symbol * nCarriers + carrier]
inline CarrierMer merPerCarrier(const std::vector<std::complex<float>>& rx, const std::vector<std::complex<float>>& ref, int nCarriers, double notchDb = 6, int notchMin = 3) {
    CarrierMer r;
    if (nCarriers < 8 || rx.size() != ref.size() || rx.size() < (size_t)nCarriers * 4) return r;
    const int nSym = (int)(rx.size() / (size_t)nCarriers);
    r.merDb.assign((size_t)nCarriers, 0);
    for (int k = 0; k < nCarriers; k++) {
        double e = 0, s = 0;
        for (int t = 0; t < nSym; t++) { const auto a = rx[(size_t)t * nCarriers + k], b = ref[(size_t)t * nCarriers + k]; e += std::norm(a - b); s += std::norm(b); }
        r.merDb[(size_t)k] = 10 * std::log10(std::max(1e-12, s) / std::max(1e-12, e));
    }
    double sum = 0; for (double v : r.merDb) sum += v;
    r.meanDb = sum / nCarriers;
    r.worstDb = 1e9; for (int k = 0; k < nCarriers; k++) if (r.merDb[(size_t)k] < r.worstDb) { r.worstDb = r.merDb[(size_t)k]; r.worstCarrier = k; }
    // slope: least squares
    double sx = 0, sxx = 0, sxy = 0;
    for (int k = 0; k < nCarriers; k++) { sx += k; sxx += (double)k * k; sxy += k * r.merDb[(size_t)k]; }
    const double den = nCarriers * sxx - sx * sx;
    r.tiltDbPerCarrier = (nCarriers * sxy - sx * sum) / den;
    int run = 0;
    for (int k = 0; k <= nCarriers; k++) {
        const bool low = k < nCarriers && r.merDb[(size_t)k] < r.meanDb - notchDb;
        if (low) run++;
        else { if (run >= notchMin) r.notches.push_back({k - run, k - 1}); run = 0; }
    }
    return r;
}

// ---------- 4. analogue TV standard
struct AtvGuess { bool ok = false; int lines = 0; std::string colour; std::string name; };
inline AtvGuess classifyAtv(double lineRateHz, double burstHz, bool secamIdent, bool palSwitch) {
    AtvGuess g;
    auto near = [](double v, double t, double tol) { return std::fabs(v - t) <= t * tol; };
    if (near(lineRateHz, 15625.0, 0.004)) g.lines = 625;
    else if (near(lineRateHz, 15734.264, 0.004)) g.lines = 525;
    else return g;
    g.ok = true;
    if (secamIdent) g.colour = g.lines == 625 ? "SECAM" : "SECAM (525)";
    else if (burstHz <= 0) g.colour = "mono";
    else if (near(burstHz, 4433618.75, 0.003)) g.colour = (g.lines == 625 && palSwitch) ? "PAL" : (g.lines == 625 ? "NTSC 4.43" : "PAL 525 (4.43)");
    else if (near(burstHz, 3575611.49, 0.0003)) g.colour = "PAL-M";        // these three differ by 700 - 1100 ppm: a crystal is 50 ppm
    else if (near(burstHz, 3582056.25, 0.0003)) g.colour = g.lines == 625 ? "PAL-N" : "NTSC-N";
    else if (near(burstHz, 3579545.0, 0.0003)) g.colour = g.lines == 525 ? "NTSC" : "NTSC 3.58 on 625";
    else { g.ok = false; return g; }
    g.name = g.colour + " " + std::to_string(g.lines) + " lines";
    return g;
}

// ---------- 5. video quality score
struct VideoScore { int score = 0; std::string label, weakest; };
inline VideoScore videoScore(double videoSnrDb, double syncStabilityPct, double lineJitterNs, double lockedShare) {
    auto clamp01 = [](double v) { return std::max(0.0, std::min(1.0, v)); };
    const double sSnr = clamp01((videoSnrDb - 10.0) / 30.0);          // 10 dB unwatchable, 40 dB studio
    const double sSync = clamp01(1.0 - syncStabilityPct / 20.0);      // sync tip spread as a percentage of its amplitude
    const double sJit = clamp01(1.0 - lineJitterNs / 400.0);
    const double sLock = clamp01(lockedShare);
    VideoScore v;
    v.score = (int)std::lround(100.0 * (0.55 * sSnr + 0.2 * sSync + 0.1 * sJit + 0.15 * sLock));
    v.label = v.score >= 85 ? "excellent" : v.score >= 65 ? "good" : v.score >= 40 ? "watchable" : v.score >= 15 ? "poor" : "unusable";
    const double part[4] = {sSnr, sSync, sJit, sLock}; const char* nm[4] = {"SNR", "sync stability", "line jitter", "lock"};
    int w = 0; for (int i = 1; i < 4; i++) if (part[i] < part[w]) w = i;
    v.weakest = nm[w];
    return v;
}

// ---------- 6. free FPV channels
struct FpvPick { std::string name; double mhz = 0; double levelDb = 0; };
// levelDb(mhz) readings: pairs of frequency (MHz) and level (dB) for every channel of interest, in any order
inline std::vector<FpvPick> pickFpvChannels(const std::vector<std::pair<double, double>>& levels, int pilots, double occupiedOverFloorDb = 10, double guardMhz = 20, double minSpacingMhz = 37, int bandsMask = 0x3F /* A B E F R L */) {
    std::vector<FpvPick> out;
    if (levels.empty() || pilots < 1) return out;
    std::vector<double> lv; for (auto& p : levels) lv.push_back(p.second);
    std::sort(lv.begin(), lv.end());
    const double floorDb = lv[lv.size() / 5];
    auto levelAt = [&](double mhz) { double best = -200, bd = 1e9; for (auto& p : levels) { const double d = std::fabs(p.first - mhz); if (d < bd) { bd = d; best = p.second; } } return bd <= 6 ? best : -200.0; };
    std::vector<std::pair<double, double>> occ;   // occupied frequencies
    for (auto& p : levels) if (p.second >= floorDb + occupiedOverFloorDb) occ.push_back(p);
    struct Cand { FpvPick p; double score; };
    std::vector<Cand> cands;
    for (const auto& ch : fpvAllChannels()) {
        if (!(bandsMask & (1 << ch.band)) || kFpvBands[ch.band].letter == 'G') continue;
        const double mhz = ch.freqHz / 1e6;
        if (mhz < 5600) { if (!(bandsMask & (1 << ch.band))) continue; }
        double lvl = levelAt(mhz); if (lvl < -199) continue;   // never measured: unknown, not offered
        bool bad = false; double nearest = 1e9;
        for (auto& o : occ) { const double d = std::fabs(o.first - mhz); nearest = std::min(nearest, d); if (d < guardMhz) bad = true; }
        if (bad) continue;
        cands.push_back({{ch.name, mhz, lvl}, std::min(nearest, 200.0) - 2.0 * (lvl - floorDb)});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
    for (const auto& c : cands) {
        bool ok = true;
        for (const auto& o : out) if (std::fabs(o.mhz - c.p.mhz) < minSpacingMhz) { ok = false; break; }
        if (ok) out.push_back(c.p);
        if ((int)out.size() >= pilots) break;
    }
    return out;
}

}  // namespace dect2::d40
