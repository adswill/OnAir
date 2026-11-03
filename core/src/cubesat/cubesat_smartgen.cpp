// Test signals for the Smart CubeSat decoder: see cubesat_smartgen.h.
#include "dect2/cubesat/cubesat_smartgen.h"
#include "dect2/common/mathconst.h"
#include "dect2/cubesat/cubesat_ao40.h"
#include "dect2/cubesat/cubesat_db.h"
#include "dect2/cubesat/cubesat_telem.h"
#include "dect2/packet/packet_ax25.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2 {
namespace cubesat {

namespace {

using dect2::math::kPi;

ax25::Address addr(const std::string& s) {
    ax25::Address a;
    const size_t d = s.find('-');
    a.call = s.substr(0, d);
    a.ssid = d == std::string::npos ? 0 : atoi(s.c_str() + d + 1);
    return a;
}

std::vector<uint8_t> msbBits(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> b;
    for (uint8_t v : bytes) for (int k = 7; k >= 0; k--) b.push_back((uint8_t)((v >> k) & 1));
    return b;
}

// the frequency (or amplitude) pulse of one NRZ symbol: a rectangle of one symbol smoothed by a Gaussian of bandwidth-time bt
double pulse(double t, double T, double bt) {
    const double sigma = std::sqrt(std::log(2.0)) / (2 * kPi * bt) * T;
    const double s = sigma * std::sqrt(2.0);
    return 0.5 * (std::erf((t + T / 2) / s) - std::erf((t - T / 2) / s));
}

// one burst: line levels (0/1) at `baud` -> baseband, FSK (devHz) or BPSK (amplitude), at s.rate, clock error applied
void burstNrz(const SmartSynth& s, const std::vector<uint8_t>& lv, bool bpsk, std::vector<cf32>& out, double& phase, double& t) {
    const double T = (1.0 / s.baud) * (1.0 + s.clockPpm * 1e-6);
    const double dev = s.devHz > 0 ? s.devHz : s.baud / 4.0;
    const double bt = bpsk ? 1.2 : s.gaussBt;
    const long n = (long)std::ceil(((double)lv.size() + 6) * T * s.rate);
    const long reach = 4;
    for (long i = 0; i < n; i++) {
        const double ts = (double)i / s.rate - 3 * T;          // the first three symbol times are lead-in
        const long k0 = (long)std::floor(ts / T + 0.5);
        double v = 0;
        for (long k = k0 - reach; k <= k0 + reach; k++) {
            if (k < 0 || k >= (long)lv.size()) continue;
            v += (lv[(size_t)k] ? 1.0 : -1.0) * pulse(ts - (double)k * T, T, bt);
        }
        const double tt = t + (double)i / s.rate;
        const double fd = s.dopplerHz + s.dopplerRateHzS * tt;
        if (bpsk) {
            phase += 2 * kPi * (s.carrierHz + fd) / s.rate;
            out.push_back(cf32((float)(v * std::cos(phase)), (float)(v * std::sin(phase))));
        } else {
            phase += 2 * kPi * (s.carrierHz + fd + dev * v) / s.rate;
            out.push_back(cf32((float)std::cos(phase), (float)std::sin(phase)));
        }
        if (phase > 2 * kPi) phase -= 2 * kPi; else if (phase < -2 * kPi) phase += 2 * kPi;
    }
    t += (double)n / s.rate;
}

void carrier(const SmartSynth& s, double sec, bool on, std::vector<cf32>& out, double& phase, double& t) {
    const long n = (long)std::lround(sec * s.rate);
    for (long i = 0; i < n; i++) {
        const double tt = t + (double)i / s.rate;
        phase += 2 * kPi * (s.carrierHz + s.dopplerHz + s.dopplerRateHzS * tt) / s.rate;
        if (phase > 2 * kPi) phase -= 2 * kPi;
        out.push_back(on ? cf32((float)std::cos(phase), (float)std::sin(phase)) : cf32(0.f, 0.f));
    }
    t += (double)n / s.rate;
}

void burstAfsk(const SmartSynth& s, const std::vector<uint8_t>& lv, std::vector<cf32>& out, double& phase, double& t) {
    const double T = (1.0 / 1200.0) * (1.0 + s.clockPpm * 1e-6);
    const long n = (long)std::ceil((double)lv.size() * T * s.rate);
    double ap = 0;
    for (long i = 0; i < n; i++) {
        const long k = std::min((long)lv.size() - 1, (long)std::floor((double)i / s.rate / T));
        ap += 2 * kPi * (lv[(size_t)k] ? 1200.0 : 2200.0) / s.rate;
        if (ap > 2 * kPi) ap -= 2 * kPi;
        const double tt = t + (double)i / s.rate;
        phase += 2 * kPi * (s.carrierHz + s.dopplerHz + s.dopplerRateHzS * tt + 2800.0 * std::sin(ap)) / s.rate;
        if (phase > 2 * kPi) phase -= 2 * kPi;
        out.push_back(cf32((float)std::cos(phase), (float)std::sin(phase)));
    }
    t += (double)n / s.rate;
}

void burstCw(const SmartSynth& s, const std::vector<std::pair<bool, double>>& runs, std::vector<cf32>& out, double& phase, double& t) {
    // the key is a raised cosine of 8 ms in and out
    std::vector<float> env;
    for (const auto& r : runs) {
        const long n = (long)std::lround(r.second * s.rate);
        for (long i = 0; i < n; i++) env.push_back(r.first ? 1.f : 0.f);
    }
    const long w = (long)(0.008 * s.rate);
    std::vector<float> sm(env.size(), 0.f);
    for (size_t i = 0; i < env.size(); i++) {
        double a = 0; long c = 0;
        for (long k = -w / 2; k <= w / 2; k += std::max(1L, w / 8)) { const long j = (long)i + k; if (j >= 0 && j < (long)env.size()) { a += env[(size_t)j]; c++; } }
        sm[i] = (float)(a / std::max(1L, c));
    }
    for (size_t i = 0; i < sm.size(); i++) {
        const double tt = t + (double)i / s.rate;
        phase += 2 * kPi * (s.carrierHz + s.dopplerHz + s.dopplerRateHzS * tt) / s.rate;
        if (phase > 2 * kPi) phase -= 2 * kPi;
        out.push_back(sm[i] * cf32((float)std::cos(phase), (float)std::sin(phase)));
    }
    t += (double)sm.size() / s.rate;
}

} // namespace

std::vector<std::pair<bool, double>> morseRuns(const std::string& text, double wpm) {
    static const char* const kM[] = {".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..", "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-", "-.--", "--..",
        "-----", ".----", "..---", "...--", "....-", ".....", "-....", "--...", "---..", "----."};
    const double u = 1.2 / wpm;
    std::vector<std::pair<bool, double>> r;
    auto gap = [&](double d) { if (!r.empty() && !r.back().first) r.back().second += d; else r.emplace_back(false, d); };
    for (char c : text) {
        if (c == ' ') { gap(4 * u); continue; }       // 3 + 4 = 7 units with the char gap
        const char* m = nullptr;
        if (c >= 'A' && c <= 'Z') m = kM[c - 'A']; else if (c >= '0' && c <= '9') m = kM[26 + c - '0'];
        if (!m) continue;
        for (const char* p = m; *p; p++) { r.emplace_back(true, *p == '.' ? u : 3 * u); gap(u); }
        gap(2 * u);
    }
    return r;
}

std::vector<cf32> smartSynth(const SmartSynth& s, std::vector<SynthSent>* sent) {
    std::vector<cf32> out;
    std::mt19937 rng((uint32_t)s.seed);
    double phase = 0, t = 0;
    carrier(s, s.leadSec, false, out, phase, t);
    uint32_t scr = 0;
    for (int fi = 0; fi < s.frames; fi++) {
        SynthSent ss;
        ss.framing = s.framing;
        const std::string txt = s.text.empty() ? (s.callsign + " beacon " + std::to_string(fi) + " temp 21C bat 7.9V") : s.text;
        if (s.mod == ModCw) {
            std::string m = s.text.empty() ? "DE " + s.callsign + " " + std::to_string(100 + fi) : s.text;
            ss.framing = kFramingCw; ss.text = m;
            ss.data.assign(m.begin(), m.end());
            carrier(s, 1.5, false, out, phase, t);
            burstCw(s, morseRuns(m, s.wpm), out, phase, t);
            carrier(s, 3.0, false, out, phase, t);
            if (sent) sent->push_back(ss);
            continue;
        }
        std::vector<uint8_t> air;                                    // the bytes sent (framings with a byte layout)
        std::vector<uint8_t> levels;                                 // the line levels
        bool diffBpsk = false;
        const int baud = s.mod == ModAfsk ? 1200 : s.baud;
        auto ax25bytes = [&](const std::string& info) {
            ax25::Frame f;
            f.from = addr(s.callsign + "-" + std::to_string(1 + fi % 3)); f.to = addr("BEACON");
            f.info = info;
            return ax25::buildFrame(f);
        };
        auto csp = [&](const std::string& p) {
            CspHeader h; h.prio = 2; h.src = 1 + fi; h.dst = 10; h.dport = 31; h.sport = 0;
            return buildCsp(h, std::vector<uint8_t>(p.begin(), p.end()));
        };
        switch (s.framing) {
        case Ax25Afsk1200: case Ax25G3ruh: case Ax100Mode6: {
            std::vector<uint8_t> bytes;
            if (s.framing == Ax100Mode6) { ss.data = csp("mode 6 telemetry " + std::to_string(fi) + " " + s.callsign); bytes = ax100Mode6Encode(ss.data); }
            else { bytes = ax25bytes(">" + txt); ss.data.assign(bytes.begin(), bytes.end() - 2); ss.text = txt; }
            const int pre = s.mod == ModBpsk ? (baud >= 4800 ? 150 : 80) : baud >= 4800 ? 60 : 40;
            std::vector<uint8_t> nrzi = ax25::nrziEncode(ax25::hdlcBits(bytes, pre, 6));
            levels = s.scramble || s.framing == Ax100Mode6 ? ax25::scramble(nrzi, scr) : nrzi;
            break;
        }
        case Ax100Mode5: {
            ss.data = csp("mode 5 beacon " + std::to_string(fi) + " eps 8.1V " + s.callsign);
            air.assign(48, 0x55);
            const std::vector<uint8_t> f = ax100Mode5Encode(ss.data);
            air.insert(air.end(), f.begin(), f.end());
            air.insert(air.end(), 4, 0x55);
            levels = msbBits(air);
            break;
        }
        case CcsdsRs: {
            ss.data.assign(223, 0);
            const std::string p = "CCSDS frame " + std::to_string(fi) + " " + s.callsign;
            std::copy(p.begin(), p.end(), ss.data.begin());
            air.assign(48, 0x55);
            const std::vector<uint8_t> f = ccsdsEncode(ss.data, 255);
            air.insert(air.end(), f.begin(), f.end());
            air.insert(air.end(), 4, 0x55);
            levels = msbBits(air);
            break;
        }
        case kFramingText: {
            ss.text = txt;
            air.assign(16, 0xAA);
            air.insert(air.end(), txt.begin(), txt.end());
            air.insert(air.end(), 4, 0xAA);
            ss.data.assign(txt.begin(), txt.end());
            levels = msbBits(air);
            break;
        }
        case kFramingAo40: {
            // a FUNcube-1 realtime frame header (AO-73, WO3) and made-up data
            ss.data.assign(256, 0);
            for (auto& b : ss.data) b = (uint8_t)rng();
            ss.data[0] = (uint8_t)((2 << 6) | 2);
            const std::vector<float> w = ao40Encode(ss.data);
            // differential BPSK: a symbol of the window that is positive keeps the phase, a negative one flips it
            levels.assign(300, 1);                                   // a short unmodulated lead
            uint8_t cur = 1;
            for (float v : w) { if (v < 0) cur ^= 1; levels.push_back(cur); }
            levels.insert(levels.end(), 40, cur);
            diffBpsk = true;
            break;
        }
        default: break;
        }
        if (s.mod == ModAfsk) burstAfsk(s, levels, out, phase, t);
        else burstNrz(s, levels, s.mod == ModBpsk, out, phase, t);
        (void)diffBpsk;
        carrier(s, s.gapSec, true, out, phase, t);
        if (sent) sent->push_back(ss);
    }
    carrier(s, 0.5, false, out, phase, t);
    if (s.snrDb < 99) {
        const float sigma = (float)std::sqrt(s.rate / 10e3 / std::pow(10.0, s.snrDb / 10.0) / 2.0);
        addNoise(out, sigma, s.seed * 7919 + 13);
    }
    return out;
}

void mixInto(std::vector<cf32>& out, const std::vector<cf32>& x, size_t at, float a) {
    if (out.size() < at + x.size()) out.resize(at + x.size(), cf32(0.f, 0.f));
    for (size_t i = 0; i < x.size(); i++) out[at + i] += a * x[i];
}

void addNoise(std::vector<cf32>& x, float sigma, uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<float> g(0.f, 1.f);
    for (auto& v : x) v += cf32(sigma * g(rng), sigma * g(rng));
}

} // namespace cubesat
} // namespace dect2
