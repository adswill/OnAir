// ADS-B test signal generator (see adsb_gen.h).
#include "dect2/adsb_gen.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <set>

namespace dect2 {

using namespace adsb;

// ---------------------------------------------------------------- random numbers

namespace {

struct Rng {
    uint64_t s[2];
    explicit Rng(uint64_t seed) {
        auto sm = [&]() { seed += 0x9E3779B97F4A7C15ull; uint64_t z = seed; z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); };
        s[0] = sm(); s[1] = sm();
    }
    uint64_t next() {   // xorshift128+
        uint64_t x = s[0]; const uint64_t y = s[1];
        s[0] = y; x ^= x << 23;
        s[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
        return s[1] + y;
    }
    double uni() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
    double uni(double a, double b) { return a + (b - a) * uni(); }
    int range(int a, int b) { return a + (int)(next() % (uint64_t)(b - a + 1)); }
    double gauss() { double u = uni(), v = uni(); return std::sqrt(-2.0 * std::log(std::max(u, 1e-300))) * std::cos(2 * M_PI * v); }
};

// Gaussian noise from a table of 2^18 values (Box-Muller at start-up), read at random: fast enough for 20 Msps and with tails to 4.6 sigma
struct NoiseTable {
    std::vector<float> t;
    NoiseTable() {
        Rng r(12345);
        t.resize(1 << 18);
        for (auto& v : t) v = (float)r.gauss();
    }
};
const NoiseTable& noiseTable() { static const NoiseTable n; return n; }

} // namespace

// ---------------------------------------------------------------- mixer

AdsbMixer::AdsbMixer(double rate, double filterMHz, double sroPpm) : rate_(rate), fs_(rate * (1.0 + sroPpm * 1e-6)) {
    if (filterMHz <= 0) filterMHz = std::min(5.0, std::max(1.75, 0.875 * rate / 1e6));
    const double wc = 2.0 * M_PI * 0.5 * filterMHz;          // rad per microsecond
    // Step response of the filter (first-order section, then second-order with damping 1: a third-order Butterworth), integrated with RK4 on a fine grid
    const int n = (int)std::lround(umax_ / du_);
    const double h = du_ / 4;                                 // integration step, microseconds
    std::vector<double> s(2 * (size_t)n + 1, 0.0);            // index i <-> time (i - n) du, taken from the 50 % crossing
    std::vector<double> pos;                                  // step response from time 0 on, every du
    {
        double y1 = 0, y = 0, v = 0;
        auto f = [&](double a1, double a, double b, double& d1, double& da, double& db) {
            d1 = wc * (1.0 - a1);
            da = b;
            db = wc * wc * (a1 - a) - wc * b;
        };
        const int total = n + (int)std::lround(10.0 / du_);   // enough to find the 50 % crossing
        for (int i = 0; i <= total; i++) {
            pos.push_back(y);
            for (int k = 0; k < 4; k++) {
                double k1[3], k2[3], k3[3], k4[3];
                f(y1, y, v, k1[0], k1[1], k1[2]);
                f(y1 + h / 2 * k1[0], y + h / 2 * k1[1], v + h / 2 * k1[2], k2[0], k2[1], k2[2]);
                f(y1 + h / 2 * k2[0], y + h / 2 * k2[1], v + h / 2 * k2[2], k3[0], k3[1], k3[2]);
                f(y1 + h * k3[0], y + h * k3[1], v + h * k3[2], k4[0], k4[1], k4[2]);
                y1 += h / 6 * (k1[0] + 2 * k2[0] + 2 * k3[0] + k4[0]);
                y += h / 6 * (k1[1] + 2 * k2[1] + 2 * k3[1] + k4[1]);
                v += h / 6 * (k1[2] + 2 * k2[2] + 2 * k3[2] + k4[2]);
            }
        }
    }
    size_t cross = 0;
    while (cross + 1 < pos.size() && pos[cross] < 0.5) cross++;   // the filter's delay: the edge is placed where the step reaches half
    const double frac = cross ? (0.5 - pos[cross - 1]) / (pos[cross] - pos[cross - 1]) : 0.0;
    const double cross0 = (double)(cross - 1) + frac;         // in du
    auto stepRaw = [&](double u) {                            // u in du from the half-way point, 0 before the step starts
        const double x = u + cross0;
        if (x <= 0) return 0.0;
        const size_t k = (size_t)x;
        if (k + 1 >= pos.size()) return pos.back();
        const double f = x - (double)k;
        return pos[k] * (1 - f) + pos[k + 1] * f;
    };
    // the transmitter's rise time: Gaussian of 25 ns (10 to 90 % in 64 ns; the standard allows 50 to 100 ns)
    const double sTx = 0.025;
    const int K = (int)std::lround(4 * sTx / du_);
    std::vector<double> ker(2 * (size_t)K + 1);
    double ks = 0;
    for (int k = -K; k <= K; k++) { ker[(size_t)(k + K)] = std::exp(-0.5 * (k * du_ / sTx) * (k * du_ / sTx)); ks += ker[(size_t)(k + K)]; }
    step_.resize(2 * (size_t)n + 1);
    for (int i = -n; i <= n; i++) {
        double acc = 0;
        for (int k = -K; k <= K; k++) acc += ker[(size_t)(k + K)] * stepRaw(i + k);
        step_[(size_t)(i + n)] = acc / ks;
    }
}

void AdsbMixer::add(const AdsbTx& tx) {
    Burst b;
    const int bits = tx.frame.bits;
    // pulses of the preamble and of the data, merged where they touch (a 0 followed by a 1 is one long pulse)
    std::vector<std::pair<double, double>> iv = {{0.0, 0.5}, {1.0, 1.5}, {3.5, 4.0}, {4.5, 5.0}};
    for (int k = 0; k < bits && tx.pulses.empty(); k++) {
        const bool one = (tx.frame.b[k >> 3] >> (7 - (k & 7))) & 1;
        const double a = 8.0 + k + (one ? 0.0 : 0.5);
        if (!iv.empty() && std::fabs(iv.back().second - a) < 1e-9) iv.back().second = a + 0.5;
        else iv.push_back({a, a + 0.5});
    }
    double span = 8.0 + bits;
    if (!tx.pulses.empty()) {
        iv = tx.pulses;
        span = 0;
        for (const auto& p : iv) span = std::max(span, p.second);
    }
    const double t0us = tx.t * 1e6;
    const double sps = fs_ * 1e-6;        // samples per microsecond
    b.first = (int64_t)std::floor((t0us - umax_) * sps);
    const int64_t last = (int64_t)std::ceil((t0us + span + umax_) * sps);
    const size_t n = (size_t)(last - b.first + 1);
    b.env.assign(n, 0.f);
    const int nStep = (int)std::lround(umax_ / du_);
    auto stepAt = [&](double u) {                                      // step response at u microseconds from the edge
        const double x = u / du_ + nStep;
        if (x <= 0) return 0.0;
        if (x >= 2.0 * nStep) return 1.0;
        const int k = (int)x; const double f = x - k;
        return step_[(size_t)k] * (1 - f) + step_[(size_t)k + 1] * f;
    };
    for (const auto& p : iv) {
        const int64_t i0 = std::max<int64_t>(b.first, (int64_t)std::floor((t0us + p.first - umax_) * sps));
        const int64_t i1 = std::min<int64_t>(last, (int64_t)std::ceil((t0us + p.second + umax_) * sps));
        for (int64_t i = i0; i <= i1; i++) {
            const double t = (double)i / sps - t0us;                  // microseconds into the burst
            b.env[(size_t)(i - b.first)] += (float)(stepAt(t - p.first) - stepAt(t - p.second));
        }
    }
    b.amp = tx.amp;
    const double ph0 = tx.phase + 2 * M_PI * tx.cfoHz * ((double)b.first / fs_ - tx.t);
    b.z = cf32((float)std::cos(ph0), (float)std::sin(ph0));
    const double dw = 2 * M_PI * tx.cfoHz / fs_;
    b.w = cf32((float)std::cos(dw), (float)std::sin(dw));
    if (b.first < pos_) {   // the first samples are gone: start where the stream is, with the phase it would have there
        const size_t skip = (size_t)std::min<int64_t>(pos_ - b.first, (int64_t)b.env.size());
        b.at = skip;
        const double ph = ph0 + dw * (double)skip;
        b.z = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    bursts_.push_back(std::move(b));
}

void AdsbMixer::render(cf32* out, size_t n) {
    std::fill(out, out + n, cf32(0.f, 0.f));
    const int64_t end = pos_ + (int64_t)n;
    for (auto& b : bursts_) {
        int64_t i = b.first + (int64_t)b.at;           // the next sample of this burst to render
        if (i >= end) continue;                        // it starts after this block
        const int64_t stop = std::min(end, b.first + (int64_t)b.env.size());
        for (; i < stop; i++) {
            const size_t k = (size_t)(i - b.first);
            out[i - pos_] += b.z * (b.env[k] * b.amp);
            b.z *= b.w;
            b.at = k + 1;
            if ((k & 255) == 255) b.z /= std::abs(b.z);
        }
    }
    pos_ = end;
    bursts_.erase(std::remove_if(bursts_.begin(), bursts_.end(), [&](const Burst& b) { return b.first + (int64_t)b.env.size() <= pos_; }), bursts_.end());
}

// ---------------------------------------------------------------- noise

uint64_t AdsbNoise::next() {
    uint64_t x = rng_[0]; const uint64_t y = rng_[1];
    rng_[0] = y; x ^= x << 23;
    rng_[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
    return rng_[1] + y;
}

AdsbNoise::AdsbNoise(double rate, double filterMHz, double snrDb, double ampRef, uint64_t seed) : rate_(rate), ampRef_(ampRef) {
    uint64_t z = seed * 0x9E3779B97F4A7C15ull + 0x1234567;
    auto sm = [&]() { z += 0x9E3779B97F4A7C15ull; uint64_t v = z; v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ull; v = (v ^ (v >> 27)) * 0x94D049BB133111EBull; return v ^ (v >> 31); };
    rng_[0] = sm(); rng_[1] = sm();
    if (filterMHz <= 0) filterMHz = std::min(5.0, std::max(1.75, 0.875 * rate / 1e6));
    const double fc = 0.5 * filterMHz * 1e6;
    // a low-pass that is not well below the Nyquist frequency is left out: the samples are then as white as the filter would make them
    filtered_ = fc < 0.42 * rate;
    if (filtered_) {
        const double c = 2.0 * rate, wc = c * std::tan(M_PI * fc / rate);       // bilinear transform with the cutoff pre-warped
        const double d1 = c + wc;
        b1_[0] = b1_[1] = wc / d1; a1_ = (wc - c) / d1;
        const double d2 = c * c + wc * c + wc * wc;
        b2_[0] = b2_[2] = wc * wc / d2; b2_[1] = 2 * wc * wc / d2;
        a2_[0] = (-2 * c * c + 2 * wc * wc) / d2; a2_[1] = (c * c - wc * c + wc * wc) / d2;
        // power gain for unit white variance: the sum of the squared impulse response of the cascade
        double s1 = 0, t[2][2] = {}, p = 0;
        double u1[2] = {};
        for (int i = 0; i < 20000; i++) {
            const double in = i == 0 ? 1.0 : 0.0;
            const double y1 = b1_[0] * in + u1[0]; u1[0] = b1_[1] * in - a1_ * y1;
            const double y2 = b2_[0] * y1 + t[0][0]; t[0][0] = b2_[1] * y1 - a2_[0] * y2 + t[0][1]; t[0][1] = b2_[2] * y1 - a2_[1] * y2;
            p += y2 * y2;
        }
        (void)s1;
        gain_ = p;
    }
    setSnr(snrDb);
}

void AdsbNoise::setSnr(double snrDb) {
    // noise density N0 such that N0 * 2 MHz = ampRef^2 / snr; white variance per sample N0 * fs, which the low-pass cuts by `gain_`
    const double n0 = ampRef_ * ampRef_ * std::pow(10.0, -snrDb / 10.0) / 2e6;
    const double white = n0 * rate_;                       // complex variance of the white noise
    sigma_ = (float)std::sqrt(white / 2.0);                // per component
}

void AdsbNoise::add(cf32* x, size_t n) {
    const NoiseTable& tab = noiseTable();
    const float* nz = tab.t.data();
    const uint64_t mask = tab.t.size() - 1;
    for (size_t i = 0; i < n; i++) {
        const uint64_t r = next();
        float ni = sigma_ * nz[r & mask], nq = sigma_ * nz[(r >> 20) & mask];
        if (filtered_) {
            // I and Q through the two sections (state: s1_ for the first-order section, s2_[k] for the second)
            auto run = [&](float in, int k) {
                const double y1 = b1_[0] * in + (k ? s1_[1] : s1_[0]);
                (k ? s1_[1] : s1_[0]) = b1_[1] * in - a1_ * y1;
                const double y2 = b2_[0] * y1 + s2_[k][0];
                s2_[k][0] = b2_[1] * y1 - a2_[0] * y2 + s2_[k][1];
                s2_[k][1] = b2_[2] * y1 - a2_[1] * y2;
                return (float)y2;
            };
            ni = run(ni, 0); nq = run(nq, 1);
        }
        x[i] += cf32(ni, nq);
    }
}

// ---------------------------------------------------------------- airspace

namespace {

enum Kind { kPos, kVel, kIdent, kOpStat, kTarget, kAllCall, kDf4, kDf5, kDf20, kDf21, kDf0, kDf16, kStatus, kKinds };

struct Chan { Kind k; double period; double jitter; double next; };

struct Sim {
    uint32_t icao = 0;
    std::string callsign;
    int cat = 3;                 // CA field of the identification (A3 large)
    int tcCat = 4;
    int squawk = 2000;
    double lat = 0, lon = 0, alt = 30000, gs = 450, track = 0, vrate = 0, targetAlt = 30000;
    bool ground = false, gillham = false, hasTarget = true, emergency = false;
    int tcPos = 11, version = 2, nacp = 10;
    double cfo = 0;
    double fade = 0, fadeTarget = 0;
    bool odd = false;
    int bdsTurn = 0;
    double selAlt = 0, baro = 1013.2, selHdg = 0, rollDeg = 0, rate = 0, mach = 0.78, ias = 280;
    std::vector<Chan> chans;
    double t = 0;                // time the state is valid for
    uint32_t gen = 0;            // changes when the aircraft is replaced, to drop its old events
};

double deg(double r) { return r * 180.0 / M_PI; }
double rad(double d) { return d * M_PI / 180.0; }

void destination(double lat, double lon, double brgDeg, double distNm, double& lat2, double& lon2, double& finalBrg) {
    const double d = distNm / 3440.065, b = rad(brgDeg), p1 = rad(lat), l1 = rad(lon);
    const double p2 = std::asin(std::sin(p1) * std::cos(d) + std::cos(p1) * std::sin(d) * std::cos(b));
    const double l2 = l1 + std::atan2(std::sin(b) * std::sin(d) * std::cos(p1), std::cos(d) - std::sin(p1) * std::sin(p2));
    lat2 = deg(p2);
    lon2 = std::remainder(deg(l2), 360.0);
    finalBrg = std::fmod(bearingDeg(lat2, lon2, lat, lon) + 180.0, 360.0);
}

const char* kAirlines[] = {"UAE", "FDB", "ETD", "QTR", "SVA", "GFA", "OMA", "THY", "DLH", "BAW", "AFR", "KLM", "SIA", "AIC", "PIA", "MEA", "RJA", "ABY", "SWR", "FDX"};

} // namespace

struct AdsbAirspace::Impl {
    AdsbAirspaceConfig cfg;
    Rng rng;
    std::vector<Sim> ac;
    std::set<uint32_t> used;
    double now = 0;
    double mult = 1;
    struct Ev { double t; size_t who; size_t chan; uint32_t gen; bool operator<(const Ev& o) const { return t > o.t; } };
    std::priority_queue<Ev> q;

    explicit Impl(const AdsbAirspaceConfig& c) : cfg(c), rng(c.seed ? c.seed : 1) {
        mult = std::clamp(c.rateMultiplier > 0 ? c.rateMultiplier : 1.0, 0.1, 20.0);
        const int n = std::clamp(c.aircraft, 0, 300);
        for (int i = 0; i < n; i++) ac.push_back(spawn(i == 0 && n >= 4, false));
        for (size_t i = 0; i < ac.size(); i++) {
            if (c.emergencyDemo && i == ac.size() / 2 && !ac[i].ground) makeEmergency(ac[i]);
            for (size_t k = 0; k < ac[i].chans.size(); k++) q.push({ac[i].chans[k].next, i, k, ac[i].gen});
        }
    }

    void makeEmergency(Sim& s) {
        s.emergency = true; s.squawk = 7700;
        s.chans.push_back({kStatus, 1.25, 0.2, now + rng.uni(0, 1.25)});
    }

    uint32_t freshIcao() {
        for (;;) {
            const uint32_t a = (uint32_t)rng.range(0x400000, 0xAFFFFF);
            if (used.insert(a).second) return a;
        }
    }

    std::string freshCallsign() {
        char b[16];
        const char* air = kAirlines[rng.range(0, 19)];
        snprintf(b, sizeof b, "%s%d", air, rng.range(1, 9) * (rng.uni() < 0.5 ? 1 : 100) + rng.range(0, 99));
        if (std::strlen(b) > 8) b[8] = 0;
        return b;
    }

    Sim spawn(bool onGround, bool entering) {
        Sim s;
        s.icao = freshIcao();
        s.callsign = freshCallsign();
        s.squawk = rng.range(0, 7) * 1000 + rng.range(0, 7) * 100 + rng.range(0, 7) * 10 + rng.range(0, 7);
        if (s.squawk == 7700 || s.squawk == 7600 || s.squawk == 7500 || s.squawk == 0) s.squawk = 2201;
        s.cfo = std::clamp(rng.gauss() * 100e3, -400e3, 400e3);
        s.gillham = rng.uni() < 0.12;
        s.hasTarget = rng.uni() < 0.5;
        s.version = rng.uni() < 0.7 ? 2 : (rng.uni() < 0.5 ? 1 : 0);
        s.nacp = rng.range(8, 10);
        s.tcPos = rng.uni() < 0.7 ? 11 : rng.range(9, 14);
        s.cat = rng.uni() < 0.2 ? 5 : (rng.uni() < 0.85 ? 3 : 2);
        s.baro = 1013.2 - rng.range(-6, 8) * 0.8;
        if (onGround) {
            const double d = rng.uni(0.4, 2.5), b = rng.uni(0, 360);
            double fb;
            destination(cfg.refLat, cfg.refLon, b, d, s.lat, s.lon, fb);
            s.ground = true; s.alt = 0; s.gs = rng.uni(8, 22); s.track = rng.uni(0, 360); s.vrate = 0;
            s.squawk = 2000;
        } else {
            // positions uniform over the disc between 12 and 190 NM (entering: near the edge, flying inwards)
            const double d = entering ? rng.uni(185, 205) : 12.0 + 178.0 * std::sqrt(rng.uni());
            const double b = rng.uni(0, 360);
            double fb;
            destination(cfg.refLat, cfg.refLon, b, d, s.lat, s.lon, fb);
            s.track = entering ? std::fmod(bearingDeg(s.lat, s.lon, cfg.refLat, cfg.refLon) + rng.uni(-55, 55) + 360.0, 360.0) : rng.uni(0, 360);
            const double r = rng.uni();
            if (r < 0.6) { s.alt = std::floor(rng.uni(240, 400) / 10.0) * 1000.0; s.targetAlt = s.alt; s.gs = rng.uni(430, 520); s.vrate = 0; }
            else if (r < 0.8) { s.alt = rng.uni(4000, 30000); s.targetAlt = std::floor(rng.uni(300, 390) / 10.0) * 1000.0; s.vrate = rng.uni(1400, 2600); s.gs = rng.uni(300, 450); }
            else { s.alt = rng.uni(12000, 36000); s.targetAlt = rng.uni(2500, 9000); s.vrate = -rng.uni(1200, 2400); s.gs = rng.uni(300, 450); }
            s.alt = std::round(s.alt / 25.0) * 25.0;
        }
        s.selAlt = s.targetAlt; s.selHdg = std::floor(s.track); s.mach = std::clamp(s.gs / 600.0, 0.3, 0.88); s.ias = std::clamp(s.gs * 0.62, 150.0, 320.0);
        auto jit = [&](double p) { return now + rng.uni(0, p); };
        if (s.ground) {
            s.chans = {{kPos, 0.5, 0.2, jit(0.5)}, {kIdent, 5, 0.1, jit(5)}, {kOpStat, 2.5, 0.1, jit(2.5)}, {kAllCall, 2.0, 0.5, jit(2)}};
        } else {
            s.chans = {{kPos, 0.5, 0.2, jit(0.5)}, {kVel, 0.5, 0.2, jit(0.5)}, {kIdent, 5, 0.1, jit(5)}, {kOpStat, 2.5, 0.1, jit(2.5)}, {kAllCall, 2.0, 0.5, jit(2)}};
            if (s.hasTarget) s.chans.push_back({kTarget, 1.25, 0.2, jit(1.25)});
        }
        if (cfg.replies) {
            s.chans.push_back({kDf4, 3.0, 0.5, jit(3)});
            s.chans.push_back({kDf5, 4.0, 0.5, jit(4)});
            if (!s.ground) {
                s.chans.push_back({kDf20, 5.0, 0.5, jit(5)});
                s.chans.push_back({kDf21, 7.0, 0.5, jit(7)});
                s.chans.push_back({kDf0, 8.0, 0.5, jit(8)});
                s.chans.push_back({kDf16, 12.0, 0.5, jit(12)});
            }
        }
        return s;
    }

    double distance(const Sim& s) const { return distanceNm(cfg.refLat, cfg.refLon, s.lat, s.lon); }

    // motion only
    void flyDeterministic(Sim& s, double dt) const {
        if (dt <= 0) return;
        double lat2, lon2, fb;
        destination(s.lat, s.lon, s.track, s.gs * dt / 3600.0, lat2, lon2, fb);
        if (s.ground) {
            // stay within 3 NM of the reference point: turn back when too far
            if (distanceNm(cfg.refLat, cfg.refLon, lat2, lon2) > 3.0) s.track = bearingDeg(s.lat, s.lon, cfg.refLat, cfg.refLon);
            else { s.lat = lat2; s.lon = lon2; }
            return;
        }
        s.lat = lat2; s.lon = lon2; s.track = fb;   // the track turns slowly along a great circle
        if (s.vrate != 0) {
            s.alt += s.vrate * dt / 60.0;
            if ((s.vrate > 0 && s.alt >= s.targetAlt) || (s.vrate < 0 && s.alt <= s.targetAlt)) { s.alt = s.targetAlt; s.vrate = 0; }
        }
    }

    // motion and the random changes (called at the aircraft's own transmissions only)
    void fly(Sim& s, double dt) {
        if (dt <= 0) return;
        if (s.ground) { if (rng.uni() < dt * 0.1) s.track = std::fmod(s.track + rng.uni(-90, 90) + 360.0, 360.0); }
        else {
            s.fade += (s.fadeTarget - s.fade) * std::min(1.0, dt * 0.5);
            if (rng.uni() < dt * 0.5) s.fadeTarget = rng.uni(-2.5, 2.0);
        }
        flyDeterministic(s, dt);
    }

    void respawn(size_t i) {   // an aircraft that has flown out of range is replaced by a new one entering from the edge
        used.erase(ac[i].icao);
        const bool wasEmergency = ac[i].emergency;
        const uint32_t g = ac[i].gen + 1;
        ac[i] = spawn(false, true);
        ac[i].gen = g;
        ac[i].t = now;
        if (wasEmergency && cfg.emergencyDemo) makeEmergency(ac[i]);
        for (size_t k = 0; k < ac[i].chans.size(); k++) { ac[i].chans[k].next = now + rng.uni(0, std::max(0.2, ac[i].chans[k].period)); q.push({ac[i].chans[k].next, i, k, g}); }
    }

    Frame make(Sim& s, Kind k) {
        const int ca = s.ground ? 4 : 5;
        const int altQ = (int)std::round(s.alt / 25.0) * 25;
        const int altG = (int)std::round(s.alt / 100.0) * 100;
        switch (k) {
        case kPos:
            s.odd = !s.odd;
            if (s.ground) return encodeSurfacePosition(s.icao, ca, 7, s.gs, s.track, s.odd, s.lat, s.lon);
            return encodeAirbornePosition(s.icao, ca, s.tcPos, s.gillham ? altG : altQ, s.gillham, s.odd, s.lat, s.lon);
        case kVel:
            if (s.icao % 11 == 0) return encodeAirspeed(s.icao, ca, s.ias, false, s.track, (int)s.vrate);
            return encodeVelocity(s.icao, ca, s.gs, s.track, (int)std::round(s.vrate / 64.0) * 64, true, 25 * (int)((s.icao >> 4) % 9) - 100);
        case kIdent: return encodeIdentification(s.icao, ca, 4, s.cat, s.callsign);
        case kOpStat: return encodeOperationalStatus(s.icao, ca, s.version, s.nacp, 3);
        case kTarget: return encodeTargetState(s.icao, ca, (int)s.selAlt, s.baro, s.selHdg, s.nacp);
        case kAllCall: return encodeAllCall(s.icao, ca, rng.uni() < 0.7 ? 0 : rng.range(1, 15));
        case kStatus: return encodeAircraftStatus(s.icao, ca, 1, 7700);
        case kDf4: return encodeAltitudeReply(s.icao, s.ground ? 1 : 0, s.ground ? 0 : (s.gillham ? altG : altQ), s.gillham);
        case kDf5: return encodeIdentityReply(s.icao, s.ground ? 1 : 0, s.squawk);
        case kDf0: return encodeAirAir(s.icao, false, altQ, false);
        case kDf16: return encodeAirAir(s.icao, false, altQ, true);
        case kDf20: case kDf21: {
            uint8_t mb[7];
            const int which = s.bdsTurn++ & 3;
            if (which == 0) mbCallsign(mb, s.callsign);
            else if (which == 1) mbSelectedVertical(mb, (int)s.selAlt / 16 * 16, (int)s.selAlt / 16 * 16, s.baro);
            else if (which == 2) {
                const double tas = s.gs * 1.04;
                const double roll = std::clamp(s.rollDeg, -25.0, 25.0);
                const double rate = 9.80665 * std::tan(rad(roll)) / (tas * 0.514444) * 180.0 / M_PI;   // a coordinated turn
                mbTrackAndTurn(mb, roll, s.track, s.gs, rate, tas);
            } else mbHeadingAndSpeed(mb, s.track, s.ias, s.mach, (int)s.vrate / 32 * 32, (int)s.vrate / 32 * 32);
            return encodeCommB(s.icao, k == kDf21, 0, k == kDf21 ? s.squawk : altQ, mb);
        }
        default: return encodeAllCall(s.icao, ca, 0);
        }
    }

    void run(double until, std::vector<AdsbTx>& out) {
        while (!q.empty() && q.top().t < until) {
            const Ev e = q.top(); q.pop();
            if (e.who >= ac.size()) continue;
            Sim& s = ac[e.who];
            if (e.chan >= s.chans.size() || e.gen != s.gen) continue;
            fly(s, e.t - s.t); s.t = e.t;
            now = e.t;
            Chan& c = s.chans[e.chan];
            if (distance(s) > 215.0 && !s.ground) { respawn(e.who); continue; }
            AdsbTx tx;
            tx.t = e.t;
            tx.frame = make(s, c.k);
            const double d = std::max(distance(s), 12.0);
            tx.amp = (float)std::min(0.5, cfg.a50 * (50.0 / d) * std::pow(10.0, s.fade / 20.0));
            tx.cfoHz = s.cfo;
            tx.phase = (float)rng.uni(0, 2 * M_PI);
            out.push_back(tx);
            const double period = c.period / mult;
            c.next = e.t + period * (1.0 + c.jitter * (2 * rng.uni() - 1));
            q.push({c.next, e.who, e.chan, s.gen});
        }
        // (the aircraft are not moved to `until` here: their random choices must not depend on how the signal is asked for in blocks)
        now = std::max(now, until);
        std::sort(out.begin(), out.end(), [](const AdsbTx& a, const AdsbTx& b) { return a.t < b.t; });
    }
};

AdsbAirspace::AdsbAirspace(const AdsbAirspaceConfig& c) : p_(std::make_unique<Impl>(c)) {}
AdsbAirspace::~AdsbAirspace() = default;
void AdsbAirspace::run(double untilSec, std::vector<AdsbTx>& out) { p_->run(untilSec, out); }
void AdsbAirspace::setRateMultiplier(double m) { p_->mult = std::clamp(m > 0 ? m : 1.0, 0.1, 20.0); }
std::vector<AdsbAirspace::Info> AdsbAirspace::aircraft() const {
    std::vector<Info> v;
    for (Sim s : p_->ac) {          // a copy, flown on to the time of the last run without any random choices
        const double dt = p_->now - s.t;
        if (dt > 0) p_->flyDeterministic(s, dt);
        v.push_back({s.icao, s.callsign, s.lat, s.lon, s.alt, s.gs, s.track, p_->distance(s), s.ground, s.squawk, s.ias, s.icao % 11 == 0 && !s.ground});
    }
    return v;
}

// ---------------------------------------------------------------- the synthetic source's generator

namespace {

class AdsbSynth : public ModeSynth {
public:
    AdsbSynth(const SynthConfig& c, double rate) : cfg_(c), rate_(rate), air_(airConfig(c)), mix_(rate, c.modeVal[1], c.sroPpm),
                                                   noise_(rate, c.modeVal[1], c.snrDb, 0.1, c.modeOpt[2] ? (uint64_t)c.modeOpt[2] + 777 : 778) {}
    double sampleRate() const override { return rate_; }

    void generate(cf32* out, size_t n) override {
        // transmissions for the next samples (and a little further, a burst is 120 us long)
        const double until = mix_.time() + (double)n / rate_ + 200e-6;
        if (until > horizon_) {
            tx_.clear();
            air_.run(until + 0.05, tx_);
            horizon_ = until + 0.05;
            for (AdsbTx& t : tx_) { t.cfoHz += cfg_.cfoHz; mix_.add(t); }
        }
        mix_.render(out, n);
        noise_.add(out, n);
        // keep the sum of overlapping bursts inside the converter's range
        for (size_t i = 0; i < n; i++) {
            float re = out[i].real(), im = out[i].imag();
            re = std::clamp(re, -0.95f, 0.95f); im = std::clamp(im, -0.95f, 0.95f);
            out[i] = cf32(re, im);
        }
    }

    bool configure(const SynthConfig& c) override {
        // the aircraft, replies, seed, filter and reference cannot change while it plays; the noise, carrier offset and message rate can
        for (int i = 0; i < 4; i++) if (c.modeOpt[i] != cfg_.modeOpt[i]) return false;
        if (c.modeVal[1] != cfg_.modeVal[1] || c.modeVal[2] != cfg_.modeVal[2] || c.modeVal[3] != cfg_.modeVal[3] || c.sroPpm != cfg_.sroPpm) return false;
        if (c.cfoHz != cfg_.cfoHz) return false;
        cfg_ = c;
        noise_.setSnr(c.snrDb);
        air_.setRateMultiplier(c.modeVal[0]);
        return true;
    }

private:
    static AdsbAirspaceConfig airConfig(const SynthConfig& c) {
        AdsbAirspaceConfig a;
        a.aircraft = c.modeOpt[0] > 0 ? c.modeOpt[0] : (c.modeOpt[0] < 0 ? 0 : 12);
        a.replies = c.modeOpt[1] == 0;
        a.seed = c.modeOpt[2] > 0 ? (uint32_t)c.modeOpt[2] : 1;
        a.emergencyDemo = c.modeOpt[3] == 1;
        a.rateMultiplier = c.modeVal[0] > 0 ? c.modeVal[0] : 1.0;
        if (c.modeVal[2] != 0 || c.modeVal[3] != 0) { a.refLat = c.modeVal[2]; a.refLon = c.modeVal[3]; }
        return a;
    }
    SynthConfig cfg_;
    double rate_;
    AdsbAirspace air_;
    AdsbMixer mix_;
    AdsbNoise noise_;
    double horizon_ = 0;
    std::vector<AdsbTx> tx_;
};

} // namespace

std::unique_ptr<ModeSynth> makeAdsbSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 2e6 - 1) return nullptr;
    return std::make_unique<AdsbSynth>(cfg, sampleRate);
}

} // namespace dect2
