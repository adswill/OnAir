// Test signals for the second block of 4.0 signal modes (families 886 and up). Generators exist only in the tests; none is reachable from the transmit side.
#pragma once
#include "x40_gen.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace x40gen {

// noise power per sample that gives a carrier of amplitude 1 the stated carrier to noise ratio in a bandwidth bwHz (complex input rate fs)
inline double noiseFor(double cnrDb, double bwHz, double fs) { return std::pow(10.0, -cnrDb / 10) * fs / bwHz; }

// ---- EM4100 -------------------------------------------------------------------------------------------------------------------------------
// 64-bit frame of a 40-bit payload (8-bit version + 32-bit ID): 9 ones, ten rows of 4 bits + even parity, four column parities, stop 0
inline std::vector<int> em4100Bits(uint64_t d40) {
    std::vector<int> b(9, 1); int col[4] = {0, 0, 0, 0};
    for (int r = 0; r < 10; r++) {
        int p = 0;
        for (int c = 0; c < 4; c++) { const int bit = (int)((d40 >> (39 - (4 * r + c))) & 1); b.push_back(bit); p ^= bit; col[c] ^= bit; }
        b.push_back(p);
    }
    for (int c = 0; c < 4; c++) b.push_back(col[c]);
    b.push_back(0);
    return b;
}
struct EmTag { uint64_t id = 0x0123456789ull; double depth = 0.08, rfDiv = 64, carrierHz = 125000, clockPpm = 0, start = 0.05; bool invert = false; };
// Complex baseband at the carrier: amplitude 1 carrier loaded by depth while the Manchester half-bit is high. The edges are smoothed (one pole, 20 kHz).
inline std::vector<cf32> emTag(const EmTag& t, double fs, double seconds) {
    std::vector<cf32> x((size_t)(fs * seconds));
    const auto bits = em4100Bits(t.id);
    const double half = t.rfDiv / t.carrierHz / 2 * (1 + t.clockPpm * 1e-6);
    float y = 0; const float a = (float)(1 - std::exp(-2 * kPi * 20000 / fs));
    for (size_t i = 0; i < x.size(); i++) {
        const double tt = i / fs - t.start;
        int lvl = 0;
        if (tt >= 0) {
            const long hidx = (long)(tt / half); const long bi = (hidx / 2) % 64; const int ph = (int)(hidx % 2);
            lvl = ph == 0 ? bits[bi] : !bits[bi];
            if (t.invert) lvl = !lvl;
        }
        y += a * ((float)lvl - y);
        x[i] = cf32((float)(1 - t.depth * y), 0);
    }
    return x;
}


// ---- UHF RFID (EPC Gen2) reader commands --------------------------------------------------------------------------------------------------
using Bits = std::vector<int>;
inline void putBits(Bits& b, uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((v >> i) & 1); }
inline void addCrc5(Bits& b) { unsigned c = 0x09; for (int x : b) { const bool f = ((c >> 4) & 1) ^ x; c = (c << 1) & 0x1F; if (f) c ^= 0x09; } putBits(b, c, 5); }
inline void addCrc16(Bits& b) { unsigned c = 0xFFFF; for (int x : b) { const bool f = ((c >> 15) & 1) ^ x; c = (c << 1) & 0xFFFF; if (f) c ^= 0x1021; } putBits(b, ~c & 0xFFFF, 16); }
inline Bits uhfQuery(int q, int session, int sel = 0, int m = 0, int trext = 0, int target = 0, int dr = 0) {
    Bits b; putBits(b, 8, 4); putBits(b, dr, 1); putBits(b, m, 2); putBits(b, trext, 1); putBits(b, sel, 2); putBits(b, session, 2); putBits(b, target, 1); putBits(b, q, 4); addCrc5(b); return b;
}
inline Bits uhfQRep(int session) { Bits b; putBits(b, 0, 2); putBits(b, session, 2); return b; }
inline Bits uhfQAdj(int session, int updn) { Bits b; putBits(b, 9, 4); putBits(b, session, 2); putBits(b, updn, 3); return b; }
inline Bits uhfAck(int rn) { Bits b; putBits(b, 1, 2); putBits(b, rn, 16); return b; }
inline Bits uhfNak() { Bits b; putBits(b, 0xC0, 8); return b; }
inline Bits uhfReqRn(int rn) { Bits b; putBits(b, 0xC1, 8); putBits(b, rn, 16); addCrc16(b); return b; }
inline Bits uhfRead(int bank, int ptr, int count, int rn) { Bits b; putBits(b, 0xC2, 8); putBits(b, bank, 2); putBits(b, ptr & 0x7F, 8); putBits(b, count, 8); putBits(b, rn, 16); addCrc16(b); return b; }
inline Bits uhfSelect(int target, int action, int bank, int ptr, const std::string& mask) {
    Bits b; putBits(b, 10, 4); putBits(b, target, 3); putBits(b, action, 3); putBits(b, bank, 2); putBits(b, ptr & 0x7F, 8); putBits(b, (int)mask.size(), 8);
    for (char c : mask) b.push_back(c == '1'); b.push_back(0); addCrc16(b); return b;
}
struct Uhf { double tari = 12.5e-6, one = 1.75, pw = 0.45, depth = 0.9, rtcalTari = 0, trcalRt = 2.0, tariPpm = 0; };
// A reader sending the commands in order, with carrier-only gaps in between. withTrcal: the frame starts with the TRcal preamble (Query). Amplitude of the unmodulated carrier is 1.
inline std::vector<cf32> uhfReader(const Uhf& u, const std::vector<std::pair<Bits, bool>>& cmds, double fs, double gapSec = 0.002, double leadSec = 0.002) {
    std::vector<std::pair<double, float>> seg;      // (duration, level)
    const double k = 1 + u.tariPpm * 1e-6, T = u.tari * k, PW = u.pw * T, lowLevel = (float)(1 - u.depth);
    seg.push_back({leadSec, 1});
    auto sym = [&](double len) { seg.push_back({len - PW, 1}); seg.push_back({PW, (float)lowLevel}); };
    for (auto& c : cmds) {
        seg.push_back({12.5e-6 * k, (float)lowLevel});      // delimiter
        sym(T);                                              // data-0
        const double rtcal = T + u.one * T; sym(rtcal);
        if (c.second) sym(rtcal * u.trcalRt);
        for (int bit : c.first) sym(bit ? u.one * T : T);
        seg.push_back({gapSec, 1});
    }
    seg.push_back({0.5, 1});   // the receiver publishes its list every 0.25 s: leave time for the last one
    double total = 0; for (auto& s : seg) total += s.first;
    std::vector<cf32> x((size_t)(total * fs) + 1);
    const float a = (float)(1 - std::exp(-2 * kPi * 500000 / fs)); float y = 1;
    size_t i = 0; double acc = 0;
    for (auto& s : seg) {
        acc += s.first; const size_t end = std::min(x.size(), (size_t)(acc * fs));
        for (; i < end; i++) { y += a * (s.second - y); x[i] = cf32(y, 0); }
    }
    for (; i < x.size(); i++) x[i] = cf32(y, 0);
    return x;
}


// ---- Qi (receiver to transmitter) ---------------------------------------------------------------------------------------------------------
// Bytes -> bits of a Qi packet: preamble ones, then per byte start 0, 8 data bits LSB first, odd parity, stop 1; the checksum is the XOR of header and message.
inline Bits qiPacket(const std::vector<int>& hdrAndMsg, int preamble = 12) {
    Bits b(preamble, 1); int x = 0;
    auto byte = [&](int v) { b.push_back(0); int par = 0; for (int i = 0; i < 8; i++) { const int bit = (v >> i) & 1; b.push_back(bit); par ^= bit; } b.push_back(par ^ 1); b.push_back(1); };
    for (int v : hdrAndMsg) { byte(v); x ^= v; }
    byte(x); return b;
}
struct Qi { double depth = 0.05, clockPpm = 0, wanderHz = 0, wanderDepth = 0, gapSec = 0.03; };
// The packets back to back in a power signal of amplitude 1; differential bi-phase at 2 kbit/s, smoothed edges (one pole, 12 kHz).
inline std::vector<cf32> qiSignal(const Qi& q, const std::vector<Bits>& pk, double fs, double leadSec = 0.05, double tailSec = 0.6) {
    const double bit = 500e-6 * (1 + q.clockPpm * 1e-6);
    std::vector<int> half;                          // levels of half bits
    int lvl = 0;
    for (auto& p : pk) {
        for (int b : p) { lvl ^= 1; half.push_back(lvl); if (b) lvl ^= 1; half.push_back(lvl); }
        for (int i = 0; i < (int)(q.gapSec / bit * 2); i++) half.push_back(lvl);
    }
    const double total = leadSec + half.size() * bit / 2 + tailSec;
    std::vector<cf32> x((size_t)(total * fs));
    float y = 0; const float a = (float)(1 - std::exp(-2 * kPi * 12000 / fs));
    for (size_t i = 0; i < x.size(); i++) {
        const double t = i / fs - leadSec; int l = 0;
        if (t >= 0) { const size_t h = (size_t)(t / (bit / 2)); l = h < half.size() ? half[h] : 0; }
        y += a * ((float)l - y);
        const double w = q.wanderDepth * std::sin(2 * kPi * q.wanderHz * i / fs);
        x[i] = cf32((float)((1 - q.depth * y) * (1 + w)), 0);
    }
    return x;
}


// ---- FM broadcast (multiplex) ---------------------------------------------------------------------------------------------------------------
struct Fm {
    double audioDev = 30000, audio2Dev = 12000, pilot = 7500, lmr = 15000, rds = 2000, sca67 = 0, sca92 = 0, offsetHz = 0, driftHzPerSec = 0, scale = 1.0;
    double pilotFrom = 0, pilotTo = 1e9, rdsFrom = 0, rdsTo = 1e9, scaFrom = 0;   // seconds the component is present
    double peakHz = 0;                                                            // out: the largest |deviation| of the multiplex
};
inline std::vector<cf32> fmStation(Fm& f, double fs, double seconds, unsigned seed = 1) {
    Rng r(seed); std::vector<cf32> x((size_t)(fs * seconds));
    double ph = 0, rdsSign = 1, nextFlip = 0; f.peakHz = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const double t = i / fs;
        double m = f.audioDev * std::sin(2 * kPi * 1000 * t) + f.audio2Dev * std::sin(2 * kPi * 3300 * t + 1);
        const bool pil = t >= f.pilotFrom && t < f.pilotTo;
        if (pil) {
            m += f.pilot * std::sin(2 * kPi * 19000 * t);
            m += f.lmr * std::sin(2 * kPi * 2000 * t) * std::sin(2 * kPi * 38000 * t);
        }
        if (t >= f.rdsFrom && t < f.rdsTo) { if (t >= nextFlip) { rdsSign = (r.g() > 0 ? 1 : -1); nextFlip = t + 1.0 / 1187.5; } m += f.rds * rdsSign * std::sin(2 * kPi * 57000 * t); }
        if (f.sca67 > 0 && t >= f.scaFrom) m += f.sca67 * std::sin(2 * kPi * 67000 * t + 0.4 * std::sin(2 * kPi * 600 * t));
        if (f.sca92 > 0 && t >= f.scaFrom) m += f.sca92 * std::sin(2 * kPi * 92000 * t + 0.4 * std::sin(2 * kPi * 450 * t));
        m *= f.scale;
        f.peakHz = std::max(f.peakHz, std::fabs(m));
        ph += 2 * kPi * (m + f.offsetHz + f.driftHzPerSec * t) / fs;
        x[i] = cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    return x;
}


// ---- AM broadcast ---------------------------------------------------------------------------------------------------------------------------
struct Am {
    double d1 = 0.5, d2 = 0.2, asym = 0, hum = 0, humHz = 100, offsetHz = 0, driftHzPerSec = 0, cutFrom = 1e9, cutTo = 1e9, audioHz1 = 1000, audioHz2 = 3300;
    bool clip = true;
};
inline std::vector<cf32> amStation(const Am& a, double fs, double seconds) {
    std::vector<cf32> x((size_t)(fs * seconds)); double ph = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const double t = i / fs;
        double m = a.d1 * std::sin(2 * kPi * a.audioHz1 * t) + a.d2 * std::sin(2 * kPi * a.audioHz2 * t + 1);
        m += a.asym * (m > 0 ? m : 0);                    // positive peaks larger than the negative ones
        m += a.hum * std::sin(2 * kPi * a.humHz * t);
        double env = 1 + m; if (a.clip && env < 0) env = 0;
        if (t >= a.cutFrom && t < a.cutTo) env = 0;
        ph += 2 * kPi * (a.offsetHz + a.driftHzPerSec * t) / fs;
        x[i] = (float)env * cf32((float)std::cos(ph), (float)std::sin(ph));
    }
    return x;
}


// ---- power-line / impulse noise -------------------------------------------------------------------------------------------------------------
struct Pl { double rateHz = 100, jitterUs = 60, widthUs = 70, peakDb = 20, ampSpreadDb = 3, dropProb = 0.1, randomRate = 0, randomDb = 12, driftHzPerSec = 0, noiseSigma = 1.0; unsigned seed = 1; };
// noise: complex Gaussian with sigma per quadrature noiseSigma; the envelope mean is 1.2533 sigma. Impulses are Gaussian bursts of random phase, peakDb over that mean.
inline std::vector<cf32> plNoise(const Pl& p, double fs, double seconds) {
    Rng r(p.seed); std::vector<cf32> x((size_t)(fs * seconds));
    for (auto& v : x) v = cf32((float)(p.noiseSigma * r.g()), (float)(p.noiseSigma * r.g()));
    std::mt19937 rr(p.seed + 99); std::uniform_real_distribution<double> U(0, 1);
    auto burst = [&](double t0, double dB) {
        const double A = 1.2533 * p.noiseSigma * std::pow(10.0, dB / 20), tau = p.widthUs * 1e-6 / 2.355, ph = U(rr) * 2 * kPi;
        const long a = (long)((t0 - 5 * tau) * fs), b = (long)((t0 + 5 * tau) * fs);
        for (long i = std::max(0L, a); i < b && i < (long)x.size(); i++) { const double d = i / fs - t0; x[i] += (float)(A * std::exp(-d * d / (2 * tau * tau))) * cf32((float)std::cos(ph), (float)std::sin(ph)); }
    };
    double t = 0.1, f = p.rateHz;
    while (t < seconds - 0.01) {
        if (p.rateHz > 0 && U(rr) >= p.dropProb) burst(t + p.jitterUs * 1e-6 * r.g(), p.peakDb + p.ampSpreadDb * r.g() * 0.5);
        f = p.rateHz + p.driftHzPerSec * t; t += 1.0 / f;
    }
    if (p.randomRate > 0) { double tr = 0.1; std::exponential_distribution<double> E(p.randomRate); while (tr < seconds) { tr += E(rr); burst(tr, p.randomDb + 4 * r.g()); } }
    return x;
}


// ---- radar pulses (rendered block by block: a long stream at 2 Msps would not fit in memory) -------------------------------------------------
struct RadarPulse { double t, w, amp, f; };
struct RadarPlan {
    std::vector<double> priPattern{1e-3};      // the spacings, repeated (one value: constant)
    double jitter = 0, pw = 1e-6, offsetHz = 0, durationSec = 8.0, scanSec = 0, lobeSec = 0.012, sideDb = -40, noiseSigma = 1.0, snrDb = 25;   // amplitude: snrDb over the noise sigma
    unsigned seed = 5;
};
inline std::vector<RadarPulse> radarPulses(const RadarPlan& p) {
    Rng r(p.seed); std::vector<RadarPulse> v; double t = 0.05; size_t k = 0;
    const double A = p.noiseSigma * std::pow(10.0, p.snrDb / 20);
    while (t < p.durationSec - 0.01) {
        double amp = A;
        if (p.scanSec > 0) { double ph = std::fmod(t, p.scanSec) - p.scanSec / 2; amp = A * std::max(std::pow(10.0, p.sideDb / 20), std::exp(-0.5 * ph * ph / (p.lobeSec * p.lobeSec))); }
        v.push_back({t, p.pw, amp, p.offsetHz});
        t += p.priPattern[k++ % p.priPattern.size()] * (1 + p.jitter * (r.g() * 0.5));
    }
    return v;
}
// feeds the receiver block by block; returns its telemetry
inline dect2::X40Telemetry runRadar(const RadarPlan& p, double fs, unsigned seed = 1) {
    const auto pl = radarPulses(p);
    dect2::X40Receiver rx; rx.setVariant(dect2::x40::Radar); rx.configure(fs);
    Rng rn(seed); std::vector<cf32> blk(8192);
    const size_t N = (size_t)(fs * p.durationSec); size_t pi = 0;
    for (size_t i0 = 0; i0 < N; i0 += blk.size()) {
        const size_t n = std::min(blk.size(), N - i0);
        for (size_t i = 0; i < n; i++) blk[i] = cf32((float)(p.noiseSigma * rn.g()), (float)(p.noiseSigma * rn.g()));
        const double ta = i0 / fs, tb = (i0 + n) / fs;
        while (pi < pl.size() && pl[pi].t + pl[pi].w + 1e-6 < ta) pi++;
        for (size_t q = pi; q < pl.size() && pl[q].t < tb; q++) {
            const auto& u = pl[q];
            const size_t a = (size_t)std::max(0.0, (u.t - ta) * fs), b = (size_t)std::min((double)n, (u.t + u.w - ta) * fs + 1);
            for (size_t i = a; i < b; i++) {
                const double tt = (i0 + i) / fs - u.t; const double edge = std::min(1.0, std::min(tt, u.w - tt) * fs / 1.5 + 0.0);   // a rise over about a sample and a half
                const double g = std::max(0.0, std::min(1.0, 0.5 + edge));
                const double ph = 2 * kPi * u.f * tt;
                blk[i] += (float)(u.amp * g) * cf32((float)std::cos(ph), (float)std::sin(ph));
            }
        }
        rx.feed(blk.data(), n);
    }
    dect2::X40Telemetry t; rx.telemetry(t, 0); return t;
}

}
