// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Satellite AIS: geometry, collision-tolerant burst decoding, Doppler tracking and the pass statistics (ais_sat.h).
#include "dect2/ais/ais_sat.h"
#include "dect2/ais/ais_gen.h"
#include "dect2/ais/ais_proto.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <set>

namespace dect2 {

using dect2::math::kPi;

namespace {
constexpr double kRe = 6371.0, kMu = 398600.4418, kC = 299792.458;
}

// ------------------------------------------------------------------------------------------------ geometry

double aisMaxDopplerHz(double freqHz, double altKm) {
    const double r = kRe + altKm;
    const double v = std::sqrt(kMu / r);
    return freqHz * v * (kRe / r) / kC;    // the velocity's part along the line to the horizon (in the orbit plane)
}

double aisSlantRangeKm(double altKm, double elevDeg) {
    const double r = kRe + altKm, e = elevDeg * kPi / 180;
    const double c = kRe * std::cos(e);
    return std::sqrt(r * r - c * c) - kRe * std::sin(e);
}

double aisSwathKm(double altKm, double minElevDeg) {
    const double r = kRe + altKm, e = minElevDeg * kPi / 180;
    const double lam = std::acos(kRe * std::cos(e) / r) - e;   // earth central angle from the sub-satellite point to the edge
    return 2 * kRe * std::max(0.0, lam);
}

// ------------------------------------------------------------------------------------------------ burst model and cancellation

std::vector<cf32> aisBurstTemplate(const ais::Bits& payload, double start, double cfoHz, size_t n) {
    AisBurstSpec s;
    s.payload = payload; s.channel = 'A'; s.startSec = start / kAisWorkRate; s.level = 1.0 / 0.3; s.cfoHz = cfoHz;
    AisRenderConfig c;
    c.rate = kAisWorkRate; c.durationSec = ((double)n + 0.5) / kAisWorkRate; c.snrDb = -200; c.tuneOffsetHz = -25000;   // channel A at 0 Hz, no noise
    std::vector<cf32> y = renderAisBursts({s}, c);
    y.resize(n, cf32(0, 0));
    return y;
}

static void filterCausal(std::vector<cf32>& y, const std::vector<float>* taps) {
    if (!taps || taps->empty()) return;
    const std::vector<float>& h = *taps;
    std::vector<cf32> o(y.size());
    for (size_t i = 0; i < y.size(); i++) {
        float re = 0, im = 0;
        const size_t m = std::min(h.size(), i + 1);
        for (size_t j = 0; j < m; j++) { re += h[j] * y[i - j].real(); im += h[j] * y[i - j].imag(); }
        o[i] = cf32(re, im);
    }
    y.swap(o);
}

bool aisCancelBurst(std::vector<cf32>& x, const ais::Bits& payload, double cfoHz, size_t from, size_t to, const std::vector<float>* taps,
                    double* power, double* startOut, double* cfoOut) {
    const size_t n = x.size();
    const size_t lineBits = ais::burstLineBits(payload).size();
    const size_t len = (lineBits + 2) * kAisSps + (taps ? taps->size() : 0);
    if (n < 200 || lineBits < 40) return false;
    to = std::min(to, n > len / 2 ? n - len / 2 : 0);
    if (to <= from) return false;
    // coarse timing: the template's correlation in blocks of 64 samples, added without their phases, so a frequency error of a few hundred Hz
    // (the burst's measured frequency depends on its data) does not hide the peak; steps of two samples, then every sample around the best
    std::vector<cf32> t0 = aisBurstTemplate(payload, 0, cfoHz, len);
    filterCausal(t0, taps);
    auto corrAt = [&](const std::vector<cf32>& t, long k, size_t m) {
        std::complex<double> acc(0, 0);
        for (size_t i = 0; i < m; i++) {
            const long j = k + (long)i;
            if (j < 0 || j >= (long)n) continue;
            acc += std::complex<double>(x[(size_t)j]) * std::conj(std::complex<double>(t[i]));
        }
        return acc;
    };
    auto corrNc = [&](long k) {
        double sum = 0;
        for (size_t b0 = 0; b0 < len; b0 += 64) {
            std::complex<float> acc(0, 0);
            const size_t b1 = std::min(len, b0 + 64);
            for (size_t i = b0; i < b1; i++) {
                const long j = k + (long)i;
                if (j < 0 || j >= (long)n) continue;
                acc += x[(size_t)j] * std::conj(t0[i]);
            }
            sum += std::abs(acc);
        }
        return sum;
    };
    long best = -1; double bestV = 0;
    for (size_t k = from; k < to; k += 2) {
        const double v = corrNc((long)k);
        if (v > bestV) { bestV = v; best = (long)k; }
    }
    if (best < 0) return false;
    for (long k = std::max<long>((long)from, best - 1); k <= std::min<long>((long)to, best + 1); k++) {
        const double v = corrNc(k);
        if (v > bestV) { bestV = v; best = k; }
    }
    double start = (double)best, f = cfoHz;
    auto render = [&](double st, double fr) { std::vector<cf32> t = aisBurstTemplate(payload, st, fr, n); filterCausal(t, taps); return t; };
    // frequency: the phase of the correlation over 16 parts of the burst, a straight line through them (16, so a first error of up to some
    // 300 Hz still turns less than half a cycle from one part to the next)
    auto refineFreq = [&](std::vector<cf32>& tpl) {
        const size_t s0 = (size_t)std::max(0.0, start), s1 = std::min(n, s0 + (lineBits + 1) * kAisSps);
        constexpr int K = 16;
        for (int it = 0; it < 4 && s1 > s0 + 20 * K; it++) {
            double ph[K], tm[K], wt[K];
            const size_t q = (s1 - s0) / K;
            for (int j = 0; j < K; j++) {
                std::complex<double> acc(0, 0);
                for (size_t i = s0 + j * q; i < s0 + (j + 1) * q; i++) acc += std::complex<double>(x[i]) * std::conj(std::complex<double>(tpl[i]));
                ph[j] = std::arg(acc); tm[j] = (double)(s0 + j * q + q / 2); wt[j] = std::abs(acc);
            }
            for (int j = 1; j < K; j++) { while (ph[j] - ph[j - 1] > kPi) ph[j] -= 2 * kPi; while (ph[j] - ph[j - 1] < -kPi) ph[j] += 2 * kPi; }
            double sw = 0, mt = 0, mp = 0;
            for (int j = 0; j < K; j++) { sw += wt[j]; mt += wt[j] * tm[j]; mp += wt[j] * ph[j]; }
            if (sw <= 0) break;
            mt /= sw; mp /= sw;
            double num = 0, den = 0;
            for (int j = 0; j < K; j++) { num += wt[j] * (tm[j] - mt) * (ph[j] - mp); den += wt[j] * (tm[j] - mt) * (tm[j] - mt); }
            const double df = den > 0 ? num / den * kAisWorkRate / (2 * kPi) : 0;
            f += df;
            tpl = render(start, f);
            if (std::fabs(df) < 0.05) break;
        }
    };
    std::vector<cf32> tpl = render(start, f);
    refineFreq(tpl);
    {   // the whole sample again with the frequency known (the coarse search was biased by the frequency error)
        const std::vector<cf32> t1 = render(0, f);
        long b2 = best; double bv = -1;
        for (long k = std::max<long>(0, best - 4); k <= best + 4; k++) { const double v = std::abs(corrAt(t1, k, len)); if (v > bv) { bv = v; b2 = k; } }
        best = b2;
    }
    // fractional timing on the whole burst: a grid of quarter samples, then a parabola through the best three
    double v[5];
    const double d0[5] = {-0.5, -0.25, 0.0, 0.25, 0.5};
    int bi = 2;
    for (int i = 0; i < 5; i++) { v[i] = std::abs(corrAt(render((double)best + d0[i], f), 0, n)); if (v[i] > v[bi]) bi = i; }
    double off = d0[bi];
    if (bi > 0 && bi < 4) {
        const double den = v[bi - 1] - 2 * v[bi] + v[bi + 1];
        if (den < 0) off += 0.25 * 0.5 * (v[bi - 1] - v[bi + 1]) / den;
    }
    start = (double)best + off;
    tpl = render(start, f);
    refineFreq(tpl);
    // amplitude and phase, then subtract
    std::complex<double> xt(0, 0); double tt = 0;
    for (size_t i = 0; i < n; i++) { xt += std::complex<double>(x[i]) * std::conj(std::complex<double>(tpl[i])); tt += std::norm(tpl[i]); }
    if (tt <= 0) return false;
    const std::complex<double> g = xt / tt;
    size_t sup = 0;
    for (size_t i = 0; i < n; i++) {
        if (std::norm(tpl[i]) > 1e-6) sup++;
        x[i] -= cf32((float)(g * std::complex<double>(tpl[i])).real(), (float)(g * std::complex<double>(tpl[i])).imag());
    }
    if (power) *power = std::norm(g) * tt / (double)std::max<size_t>(1, sup);
    if (startOut) *startOut = start;
    if (cfoOut) *cfoOut = f;
    return true;
}

// ------------------------------------------------------------------------------------------------ collision-tolerant decoding

static std::vector<std::pair<size_t, size_t>> powerWindows(const cf32* x, size_t n, size_t core0, size_t core1) {
    std::vector<std::pair<size_t, size_t>> w;
    if (core1 > n) core1 = n;
    if (core1 < core0 + 600) return w;
    const size_t box = 48;
    std::vector<double> cum(n + 1, 0.0);
    for (size_t i = 0; i < n; i++) cum[i + 1] = cum[i] + std::norm(x[i]);
    auto pw = [&](size_t a) { const size_t b = std::min(n, a + box); return (cum[b] - cum[a]) / (double)std::max<size_t>(1, b - a) + 1e-12; };
    std::vector<size_t> edges = {core0};
    for (size_t i = core0; i + 2 * box < core1; i += box / 2) {
        const double r = pw(i + box) / pw(i);
        if (r > 2.5 || r < 0.4) {
            const size_t e = i + box;
            if (e > edges.back() + 200) edges.push_back(e);
        }
    }
    if (core1 > edges.back() + 200) edges.push_back(core1); else edges.back() = core1;
    if (edges.size() < 3) return w;
    for (size_t i = 0; i + 1 < edges.size(); i++)
        for (size_t j = i + 1; j < edges.size(); j++) {
            if (i == 0 && j + 1 == edges.size()) continue;      // the whole burst: tried already
            if (edges[j] - edges[i] >= 600) w.push_back({edges[i], edges[j]});
        }
    std::sort(w.begin(), w.end(), [](const auto& a, const auto& b) { return a.second - a.first > b.second - b.first; });
    if (w.size() > 6) w.resize(6);
    return w;
}

AisCollisionResult aisDecodeCollided(const cf32* x, size_t n, size_t core0, size_t core1, const AisPhyConfig& cfg,
                                     const std::vector<double>& hints, const AisCollisionConfig& cc) {
    AisCollisionResult out;
    std::vector<cf32> w(x, x + n);
    double e0 = 0;
    for (size_t i = std::min(core0, n); i < std::min(core1, n); i++) e0 += std::norm(x[i]);
    for (int layer = 0; layer <= cc.maxLayers; layer++) {
        std::vector<size_t> fresh;
        auto take = [&](const AisBurstResult& r, int label) {
            for (const auto& f : r.frames) {
                bool dup = false;
                for (const auto& g : out.frames) if (g.payload == f) { dup = true; break; }
                if (dup) continue;
                AisFound a; a.payload = f; a.cfoHz = r.cfoHz; a.layer = label;
                out.frames.push_back(a);
                fresh.push_back(out.frames.size() - 1);
            }
        };
        const AisBurstResult r = aisDecodeBurst(w.data(), n, core0, core1, cfg);
        out.attempts++;
        if (layer == 0) { out.bad = r.bad; out.cfoHz = r.cfoHz; }
        take(r, layer == 0 ? 0 : layer + 1);
        if (fresh.empty() && cc.splitWindows) {
            for (const auto& win : powerWindows(w.data(), n, core0, core1)) {
                const AisBurstResult rw = aisDecodeBurst(w.data(), n, win.first, win.second, cfg);
                out.attempts++;
                take(rw, layer == 0 ? 1 : layer + 1);
                if (!fresh.empty()) break;
            }
        }
        if (fresh.empty()) {
            std::vector<double> cand;
            for (double h : hints) cand.push_back(h);
            if (layer == 0 || hints.empty())
                for (double d = cc.gridHz; d <= cc.searchHz + 1; d += cc.gridHz) { cand.push_back(r.cfoHz + d); cand.push_back(r.cfoHz - d); }
            std::vector<double> tried = {r.cfoHz};
            int used = 0;
            for (double h : cand) {
                if (used >= cc.maxCandidates) break;
                bool near = false;
                for (double t : tried) if (std::fabs(t - h) < cc.minSepHz) { near = true; break; }
                if (near) continue;
                tried.push_back(h);
                used++;
                const AisBurstResult rc = aisDecodeBurstAt(w.data(), n, core0, core1, h, cfg);
                out.attempts++;
                take(rc, layer == 0 ? 1 : layer + 1);
                if (!fresh.empty()) break;
            }
        }
        if (fresh.empty() || layer == cc.maxLayers) break;
        for (size_t idx : fresh) {
            double pw = 0, f = 0;
            if (aisCancelBurst(w, out.frames[idx].payload, out.frames[idx].cfoHz, 0, n, cc.chanTaps, &pw, nullptr, &f)) {
                out.frames[idx].power = pw;
                out.frames[idx].cfoHz = f;
            }
        }
        double e1 = 0;
        for (size_t i = std::min(core0, n); i < std::min(core1, n); i++) e1 += std::norm(w[i]);
        if (e0 <= 0 || e1 < 0.03 * e0) break;    // nothing left worth decoding
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ Doppler tracking

void AisDopplerTracker::add(double t, double hz) {
    pts_.push_back({t, hz});
    while (!pts_.empty() && (pts_.front().first < t - 120 || pts_.size() > 200)) pts_.erase(pts_.begin());
    fit();
}

void AisDopplerTracker::fit() {
    if (pts_.empty()) return;
    const double tl = pts_.back().first;
    double st = 0, sf = 0; int n = 0;
    for (const auto& p : pts_) if (p.first >= tl - 60) { st += p.first; sf += p.second; n++; }
    if (!n) return;
    t0_ = st / n; icpt_ = sf / n;
    double num = 0, den = 0;
    for (const auto& p : pts_) if (p.first >= tl - 60) { num += (p.first - t0_) * (p.second - icpt_); den += (p.first - t0_) * (p.first - t0_); }
    rate_ = n >= 3 && den > 1.0 ? num / den : 0.0;
    if (n < 3) {   // the median while there are few
        std::vector<double> v;
        for (const auto& p : pts_) v.push_back(p.second);
        std::nth_element(v.begin(), v.begin() + (long)(v.size() / 2), v.end());
        icpt_ = v[v.size() / 2];
    }
}

double AisDopplerTracker::estimate(double t) const { return pts_.empty() ? 0.0 : icpt_ + rate_ * (t - t0_); }

std::vector<double> AisDopplerTracker::recent(double t, double maxAge) const {
    std::vector<double> v;
    for (auto it = pts_.rbegin(); it != pts_.rend(); ++it) if (it->first >= t - maxAge) v.push_back(it->second);
    return v;
}

// ------------------------------------------------------------------------------------------------ statistics

struct AisSatStats::Impl {
    Sgp4 sgp;
    bool ok = false;
    std::string name;
    std::set<uint64_t> lrSeen;    // (pass << 32) | mmsi: the vessel sent message 27 in that pass
};

AisSatStats::AisSatStats() : sat_(std::make_unique<Impl>()) {}
AisSatStats::~AisSatStats() = default;

void AisSatStats::setSatellite(const Tle& t) { sat_->ok = sat_->sgp.init(t); sat_->name = t.name; }
void AisSatStats::clearSatellite() { sat_->ok = false; sat_->name.clear(); }
bool AisSatStats::hasSatellite() const { return sat_->ok; }

void AisSatStats::clear() {
    passes_.clear(); vessels_.clear(); rx_.clear(); mix_ = {}; total_ = 0; lastN_ = 0; anyN_ = false;
    sat_->lrSeen.clear();
}

double AisSatStats::elevationOf(double utc, double lat, double lon) const {
    if (!sat_->ok) return -90;
    Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = 0;
    Look l;
    return lookAt(sat_->sgp, o, utc, l) ? l.elDeg : -90;
}

int AisSatStats::openPass(double utc, const std::string& name, bool predicted, double los) {
    AisSatPass p;
    p.index = (int)passes_.size();
    p.satName = name.empty() ? sat_->name : name;
    p.startUtc = utc; p.endUtc = predicted ? los : utc;
    p.predicted = predicted;
    passes_.push_back(std::move(p));
    return (int)passes_.size() - 1;
}

void AisSatStats::beginPass(const std::string& name, double aos, double los) {
    for (const auto& p : passes_) if (p.predicted && std::fabs(p.startUtc - aos) < 30) return;   // already open
    openPass(aos, name, true, los);
}

int AisSatStats::passFor(double utc) {
    for (int i = (int)passes_.size() - 1; i >= 0; i--) {
        const AisSatPass& p = passes_[(size_t)i];
        if (p.predicted && utc >= p.startUtc - 60 && utc <= p.endUtc + 60) return i;
    }
    if (!passes_.empty()) {
        AisSatPass& p = passes_.back();
        if (!p.predicted && utc >= p.startUtc - 1 && utc - p.endUtc <= gapSec_) { p.endUtc = std::max(p.endUtc, utc); return (int)passes_.size() - 1; }
    }
    return openPass(utc, "", false, utc);
}

void AisSatStats::add(const AisRxRecord& r) {
    total_++;
    const int ti = r.type >= 1 && r.type <= 27 ? r.type : 0;
    mix_[(size_t)ti]++;
    AisSatVessel& v = vessels_[r.mmsi];
    if (!v.msgs) { v.mmsi = r.mmsi; v.firstUtc = r.utc; }
    v.msgs++;
    v.lastUtc = std::max(v.lastUtc, r.utc);
    v.lastSnrDb = r.snrDb;
    if (r.type == 27) v.longRangeMsgs++;
    if ((r.type >= 1 && r.type <= 3) || r.type == 18 || r.type == 19) v.commonMsgs++;
    if (r.hasPos) { v.hasPos = true; v.lat = r.lat; v.lon = r.lon; }
    const int pi = passFor(r.utc);
    AisSatPass& p = passes_[(size_t)pi];
    p.detections++;
    p.typeMix[(size_t)ti]++;
    p.snrSum += r.snrDb;
    auto it = std::lower_bound(p.vessels.begin(), p.vessels.end(), r.mmsi);
    if (it == p.vessels.end() || *it != r.mmsi) p.vessels.insert(it, r.mmsi);
    if (r.type == 27) {
        p.longRange++;
        if (sat_->lrSeen.insert(((uint64_t)pi << 32) | r.mmsi).second) p.newTypeVessels++;
    }
    if (v.lastPass != pi) { v.passes++; v.lastPass = pi; }
    Rx x; x.r = r; x.pass = pi;
    if (sat_->ok && v.hasPos) {
        x.elevDeg = (float)elevationOf(r.utc, v.lat, v.lon);
        p.maxElevDeg = std::max(p.maxElevDeg, x.elevDeg);
    }
    if (rx_.size() < 200000) rx_.push_back(x);
}

void AisSatStats::addAll(const std::vector<AisRxRecord>& recent) {
    for (const AisRxRecord& r : recent) {
        if (anyN_ && r.n <= lastN_) continue;
        add(r);
        lastN_ = r.n; anyN_ = true;
    }
}

uint32_t AisSatStats::newTypeCount() const {
    uint32_t n = 0;
    for (const auto& kv : vessels_) n += kv.second.longRangeMsgs > 0;
    return n;
}
double AisSatStats::newTypeRatio() const { return vessels_.empty() ? 0.0 : (double)newTypeCount() / (double)vessels_.size(); }
double AisSatStats::newTypeStillCommonShare() const {
    uint32_t lr = 0, both = 0;
    for (const auto& kv : vessels_) if (kv.second.longRangeMsgs) { lr++; both += kv.second.commonMsgs > 0; }
    return lr ? (double)both / lr : 0.0;
}

std::vector<AisSnrBin> AisSatStats::snrVsElevation(double binDeg) const {
    std::vector<AisSnrBin> b;
    if (binDeg <= 0) binDeg = 10;
    const int nb = (int)std::ceil(90.0 / binDeg);
    b.resize((size_t)nb);
    std::vector<double> sum((size_t)nb, 0.0);
    for (int i = 0; i < nb; i++) { b[(size_t)i].elevLo = i * binDeg; b[(size_t)i].elevHi = std::min(90.0, (i + 1) * binDeg); }
    for (const Rx& x : rx_) {
        if (x.elevDeg < 0) continue;
        const int i = std::min(nb - 1, (int)(x.elevDeg / binDeg));
        b[(size_t)i].n++; sum[(size_t)i] += x.r.snrDb;
    }
    for (int i = 0; i < nb; i++) if (b[(size_t)i].n) b[(size_t)i].snrMeanDb = sum[(size_t)i] / b[(size_t)i].n;
    return b;
}

void AisSatStats::finishFootprint(AisSatPass& p) const {
    p.expected = p.expectedHeard = 0;
    if (!sat_->ok) return;
    const double t0 = p.startUtc, t1 = std::max(p.endUtc, p.startUtc + 1);
    const double step = std::max(15.0, (t1 - t0) / 40);
    for (const auto& kv : vessels_) {
        const AisSatVessel& v = kv.second;
        if (!v.hasPos || v.firstUtc > t1 + 86400 || v.lastUtc < t0 - 86400) continue;   // ships known around the pass
        bool in = false;
        for (double t = t0; t <= t1 && !in; t += step) in = elevationOf(t, v.lat, v.lon) >= minEl_;
        if (!in) continue;
        p.expected++;
        if (std::binary_search(p.vessels.begin(), p.vessels.end(), v.mmsi)) p.expectedHeard++;
    }
}

static void ymOf(double utc, int& y, int& m) {
    const time_t t = (time_t)utc;
    struct tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    y = g.tm_year + 1900; m = g.tm_mon + 1;
}

std::vector<AisMonthStat> AisSatStats::months() const {
    std::map<int, std::vector<const AisSatPass*>> by;
    for (const auto& p : passes_) { int y, m; ymOf(p.startUtc, y, m); by[y * 100 + m].push_back(&p); }
    std::vector<AisMonthStat> out;
    for (const auto& kv : by) {
        AisMonthStat s; s.year = kv.first / 100; s.month = kv.first % 100;
        std::set<uint32_t> all, lr;
        uint64_t heardSum = 0, exp = 0, expHeard = 0;
        for (const AisSatPass* p : kv.second) {
            s.passes++;
            for (uint32_t m : p->vessels) {
                all.insert(m);
                if (sat_->lrSeen.count(((uint64_t)p->index << 32) | m)) lr.insert(m);
            }
            heardSum += p->vessels.size();
            AisSatPass c = *p;
            finishFootprint(c);
            exp += c.expected; expHeard += c.expectedHeard;
        }
        s.vessels = (uint32_t)all.size(); s.newType = (uint32_t)lr.size();
        if (exp > 0) { s.detectionProb = (double)expHeard / (double)exp; s.fromFootprint = true; }
        else if (s.passes && !all.empty()) s.detectionProb = (double)heardSum / ((double)s.passes * (double)all.size());
        out.push_back(s);
    }
    return out;
}

// ---- CSV
static std::string q(const std::string& s) {
    if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
    std::string o = "\"";
    for (char c : s) { if (c == '"') o += '"'; o += c; }
    return o + "\"";
}
static std::string isoUtc(double utc) {
    if (utc <= 0) return "";
    const time_t t = (time_t)std::floor(utc);
    struct tm g{};
#ifdef _WIN32
    gmtime_s(&g, &t);
#else
    gmtime_r(&t, &g);
#endif
    char b[40];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%06.3fZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec + (utc - std::floor(utc)));
    return b;
}
static std::string f(double v, int d) { char b[40]; snprintf(b, sizeof b, "%.*f", d, v); return b; }

std::string AisSatStats::receptionsCsv() const {
    std::string o = "utc,unix_time,signal_time_s,channel,frequency_hz,type,mmsi,lat,lon,sog_kn,cog_deg,nav_status,position_latency,snr_db,freq_offset_hz,layer,elevation_deg,pass\r\n";
    for (const Rx& x : rx_) {
        const AisRxRecord& r = x.r;
        const int ci = aisChannelIndex(r.channel);
        o += isoUtc(r.utc) + "," + f(r.utc, 3) + "," + f(r.timeSec, 4) + "," + std::string(1, r.channel) + "," + f(aisChannelHz(ci), 0) + "," +
             std::to_string(r.type) + "," + std::to_string(r.mmsi) + "," + (r.hasPos ? f(r.lat, 5) : "") + "," + (r.hasPos ? f(r.lon, 5) : "") + "," +
             (r.sog >= 0 ? f(r.sog, 1) : "") + "," + (r.cog >= 0 ? f(r.cog, 1) : "") + "," + (r.navStatus >= 0 ? std::to_string(r.navStatus) : "") + "," +
             (r.posLatency >= 0 ? std::to_string(r.posLatency) : "") + "," + f(r.snrDb, 1) + "," + f(r.cfoHz, 0) + "," + std::to_string(r.layer) + "," +
             (x.elevDeg > -90 ? f(x.elevDeg, 1) : "") + "," + std::to_string(x.pass) + "\r\n";
    }
    return o;
}

std::string AisSatStats::passesCsv() const {
    std::string o = "pass,satellite,start_utc,end_utc,predicted,detections,message27,vessels,new_type_vessels,expected_in_footprint,expected_heard,max_elevation_deg,mean_snr_db\r\n";
    for (const AisSatPass& p0 : passes_) {
        AisSatPass p = p0;
        finishFootprint(p);
        o += std::to_string(p.index) + "," + q(p.satName) + "," + isoUtc(p.startUtc) + "," + isoUtc(p.endUtc) + "," + (p.predicted ? "1" : "0") + "," +
             std::to_string(p.detections) + "," + std::to_string(p.longRange) + "," + std::to_string(p.vessels.size()) + "," + std::to_string(p.newTypeVessels) + "," +
             (sat_->ok ? std::to_string(p.expected) : "") + "," + (sat_->ok ? std::to_string(p.expectedHeard) : "") + "," +
             (p.maxElevDeg > -90 ? f(p.maxElevDeg, 1) : "") + "," + (p.detections ? f(p.snrSum / p.detections, 1) : "") + "\r\n";
    }
    return o;
}

std::string AisSatStats::vesselsCsv() const {
    std::string o = "mmsi,first_utc,last_utc,messages,message27,common_reports,passes,new_type,lat,lon,last_snr_db\r\n";
    for (const auto& kv : vessels_) {
        const AisSatVessel& v = kv.second;
        o += std::to_string(v.mmsi) + "," + isoUtc(v.firstUtc) + "," + isoUtc(v.lastUtc) + "," + std::to_string(v.msgs) + "," + std::to_string(v.longRangeMsgs) + "," +
             std::to_string(v.commonMsgs) + "," + std::to_string(v.passes) + "," + (v.longRangeMsgs ? "1" : "0") + "," + (v.hasPos ? f(v.lat, 5) : "") + "," +
             (v.hasPos ? f(v.lon, 5) : "") + "," + f(v.lastSnrDb, 1) + "\r\n";
    }
    return o;
}

std::string AisSatStats::monthsCsv() const {
    std::string o = "year,month,passes,vessels,new_type_vessels,new_type_ratio,detection_probability,method\r\n";
    for (const AisMonthStat& m : months())
        o += std::to_string(m.year) + "," + std::to_string(m.month) + "," + std::to_string(m.passes) + "," + std::to_string(m.vessels) + "," + std::to_string(m.newType) + "," +
             f(m.vessels ? (double)m.newType / m.vessels : 0.0, 4) + "," + f(m.detectionProb, 4) + "," + (m.fromFootprint ? "footprint" : "passes heard") + "\r\n";
    return o;
}

} // namespace dect2
