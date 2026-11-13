// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi beacon test signal (see wifi_gen.h).
#include "dect2/wifi/wifi_gen.h"
#include "dect2/wifi/wifi_proto.h"
#include "dect2/wifi/wifi_ofdm.h"
#include "dect2/wifi/wifi_chan.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include <cmath>
#include <complex>
#include <algorithm>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
} // namespace

struct WifiGenerator::Impl {
    WifiGenConfig cfg;
    genutil::NoiseSource noise;
    float sigma = 0;
    std::vector<cf32> q;   // samples made and not yet handed out, from qPos on (a deque per sample was a good part of the time)
    size_t qPos = 0;
    size_t idx = 0;
    bool done = false;
    double ph = 0;
    uint64_t nGen = 0;           // samples queued so far: the carrier phase of the OFDM frames follows it

    explicit Impl(const WifiGenConfig& c) : cfg(c), noise(c.seed) {}

    void silence(double sec) { const size_t n = (size_t)std::llround(sec * cfg.rate); q.insert(q.end(), n, cf32(0.f, 0.f)); nGen += n; }

    // Catmull-Rom interpolation of the 20 Msps waveform at fractional position t (the radio's rate and sample clock differ from 20 Msps)
    static cf32 cubic(const std::vector<cf32>& w, double t) {
        const long i = (long)std::floor(t);
        const float f = (float)(t - (double)i);
        auto at = [&](long k) { return k < 0 || k >= (long)w.size() ? cf32(0, 0) : w[(size_t)k]; };
        cf32 p0, p1, p2, p3;
        if (i >= 1 && i + 2 < (long)w.size()) { const cf32* q = &w[(size_t)(i - 1)]; p0 = q[0]; p1 = q[1]; p2 = q[2]; p3 = q[3]; }   // inside: no bounds per tap
        else { p0 = at(i - 1); p1 = at(i); p2 = at(i + 1); p3 = at(i + 2); }
        // Horner form on re and im by hand (the same polynomial as before)
        auto cr = [f](float a0, float a1, float a2, float a3) {
            return 0.5f * (2.0f * a1 + f * ((a2 - a0) + f * ((2.0f * a0 - 5.0f * a1 + 4.0f * a2 - a3) + f * (3.0f * a1 - a0 - 3.0f * a2 + a3))));
        };
        return cf32(cr(p0.real(), p1.real(), p2.real(), p3.real()), cr(p0.imag(), p1.imag(), p2.imag(), p3.imag()));
    }

    // one OFDM PPDU at the output rate, mixed to its position (and the duplicates') plus the carrier offset
    void ofdmFrame(const WifiGenFrame& fr, const std::vector<uint8_t>& mp) {
        const std::vector<cf32> w = wifi::ofdm::txPpdu(mp, fr.scrSeed, fr.phy == 4);
        const double step = wifi::ofdm::kRate * (1 + cfg.clockPpm * 1e-6) / cfg.rate;   // input samples per output sample
        const size_t ns = (size_t)std::ceil(((double)w.size() + 2) / step);
        const double amp = cfg.amp * fr.ampScale;
        std::vector<double> pos = {fr.offHz};
        pos.insert(pos.end(), fr.dupHz.begin(), fr.dupHz.end());
        // one oscillator per position, started at the phase of sample nGen and turned by a fixed step (re-anchored every 4096 samples)
        // instead of cos and sin per sample and position; the same phase as before to rounding
        const size_t np = pos.size();
        std::vector<double> wr(np), wi(np), sr(np), si(np), fq(np);
        auto anchor = [&](uint64_t at) {
            for (size_t j = 0; j < np; j++) {
                const double a = 2 * kPi * std::fmod(fq[j] * (double)at / cfg.rate, 1.0);
                wr[j] = std::cos(a); wi[j] = std::sin(a);
            }
        };
        for (size_t j = 0; j < np; j++) {
            fq[j] = pos[j] + cfg.cfoHz + fr.cfoHz;
            const double a = 2 * kPi * fq[j] / cfg.rate;
            sr[j] = std::cos(a); si[j] = std::sin(a);
        }
        q.reserve(q.size() + ns);
        for (size_t i = 0; i < ns; i++) {
            if ((i & 4095) == 0) anchor(nGen);
            const cf32 v = (double)i * step <= (double)w.size() ? cubic(w, (double)i * step) : cf32(0, 0);
            double ar = 0, ai = 0;
            for (size_t j = 0; j < np; j++) {
                ar += wr[j]; ai += wi[j];
                const double nr = wr[j] * sr[j] - wi[j] * si[j];
                wi[j] = wr[j] * si[j] + wi[j] * sr[j];
                wr[j] = nr;
            }
            const double vr = v.real() * amp, vi = v.imag() * amp;
            q.push_back(cf32((float)(ar * vr - ai * vi), (float)(ar * vi + ai * vr)));
            nGen++;
        }
    }

    // A DSSS PPDU at 2 Mbit/s (phy 2: long preamble, 3: short): DBPSK up to the header (or the short SFD), DQPSK after it, Barker-11 chips
    void dsss2Frame(const WifiGenFrame& fr, const std::vector<uint8_t>& mp) {
        const wifi::DsssPpdu pp = wifi::dsssPpdu(mp, 2, fr.phy == 3, fr.phy == 3 ? 0x6C : 0x1B);
        std::vector<cf32> sym;
        sym.reserve(pp.bits.size());
        int qt = 0;   // the phase in quarter turns
        static const int kQ[4] = {0, 3, 1, 2};   // index d0 + 2 d1: d0 d1 = 00 -> 0, 10 -> 3 (-pi/2), 01 -> 1 (+pi/2), 11 -> 2 (pi)
        static const cf32 kP[4] = {cf32(1, 0), cf32(0, 1), cf32(-1, 0), cf32(0, -1)};
        for (size_t i = 0; i < pp.bits.size();) {
            if (i < pp.qpskFrom) { if (pp.bits[i]) qt += 2; i++; }
            else { const int d0 = pp.bits[i], d1 = i + 1 < pp.bits.size() ? pp.bits[i + 1] : 0; qt += kQ[d0 | d1 << 1]; i += 2; }
            sym.push_back(kP[qt & 3]);
        }
        const double chipsPerSample = wifi::kChipRate * (1 + cfg.clockPpm * 1e-6) / cfg.rate;
        const size_t nChips = sym.size() * 11;
        const size_t ns = (size_t)std::ceil(nChips / chipsPerSample);
        const double off = cfg.cfoHz + fr.cfoHz + fr.offHz;
        const double amp = cfg.amp * fr.ampScale;
        const double dph = 2 * kPi * off / cfg.rate;
        for (size_t i = 0; i < ns; i++) {
            // the chips averaged over the sample's interval (an ADC behind its filter), not sampled and held: chip edges fall between samples
            const double a = (double)i * chipsPerSample, b = a + chipsPerSample;
            const size_t k = (size_t)a;
            auto chip = [&](size_t c) { return c < nChips ? sym[c / 11] * (float)wifi::kBarker[c % 11] : cf32(0, 0); };
            const double wa = std::min(b, (double)(k + 1)) - a, wb = std::max(0.0, b - (double)(k + 1));
            const cf32 v = (chip(k) * (float)wa + chip(k + 1) * (float)wb) / (float)chipsPerSample;
            ph = std::remainder(ph + dph, 2 * kPi);
            const cf32 lo((float)std::cos(ph), (float)std::sin(ph));
            q.push_back((float)amp * v * lo);
            nGen++;
        }
    }

    void refill() {
        if (qPos > 0) { q.erase(q.begin(), q.begin() + (ptrdiff_t)qPos); qPos = 0; }
        if (done || cfg.frames.empty()) { silence(0.001); return; }
        const WifiGenFrame& fr = cfg.frames[idx];
        std::vector<uint8_t> mp = fr.mpdu;
        if (fr.badFcs && !mp.empty()) mp.back() ^= 0x10;
        if (fr.phy == 2 || fr.phy == 3) {
            dsss2Frame(fr, mp);
            silence(fr.gapSec);
            if (++idx >= cfg.frames.size()) { idx = 0; if (!cfg.loop) done = true; }
            return;
        }
        if (fr.phy == 1 || fr.phy == 4) {
            ofdmFrame(fr, mp);
            silence(fr.gapSec);
            if (++idx >= cfg.frames.size()) { idx = 0; if (!cfg.loop) done = true; }
            return;
        }
        const std::vector<uint8_t> bits = wifi::ppduBits(mp);
        // DBPSK: a one turns the phase by pi
        std::vector<int8_t> sym(bits.size());
        int s = 1;
        for (size_t i = 0; i < bits.size(); i++) { if (bits[i]) s = -s; sym[i] = (int8_t)s; }
        const double chipsPerSample = wifi::kChipRate * (1 + cfg.clockPpm * 1e-6) / cfg.rate;
        const size_t nChips = sym.size() * 11;
        const size_t ns = (size_t)std::ceil(nChips / chipsPerSample);
        const double off = cfg.cfoHz + fr.cfoHz + fr.offHz;
        const double amp = cfg.amp * fr.ampScale;
        const double dph = 2 * kPi * off / cfg.rate;
        for (size_t i = 0; i < ns; i++) {
            // the chips averaged over the sample's interval (an ADC behind its filter), not sampled and held: chip edges fall between samples
            const double a = (double)i * chipsPerSample, b = a + chipsPerSample;
            const size_t k = (size_t)a;
            auto chip = [&](size_t c) { return c < nChips ? (double)sym[c / 11] * wifi::kBarker[c % 11] : 0.0; };
            const double wa = std::min(b, (double)(k + 1)) - a, wb = std::max(0.0, b - (double)(k + 1));
            const double v = (chip(k) * wa + chip(k + 1) * wb) / chipsPerSample;
            ph = std::remainder(ph + dph, 2 * kPi);
            q.push_back(cf32((float)(amp * v * std::cos(ph)), (float)(amp * v * std::sin(ph))));
            nGen++;
        }
        silence(fr.gapSec);
        if (++idx >= cfg.frames.size()) { idx = 0; if (!cfg.loop) done = true; }
    }
};

double WifiGenerator::noiseSigma(const WifiGenConfig& c) {
    if (c.snrDb >= 99) return 0;
    const double n20 = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // noise power in 20 MHz
    return std::sqrt(n20 * c.rate / 20e6 / 2);
}

WifiGenerator::WifiGenerator(const WifiGenConfig& c) : p_(std::make_unique<Impl>(c)) {
    p_->sigma = (float)noiseSigma(c);
    p_->silence(c.leadSec);
}
WifiGenerator::~WifiGenerator() = default;

void WifiGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    size_t i = 0;
    while (i < n) {
        if (m.qPos >= m.q.size()) { m.refill(); continue; }
        const size_t k = std::min(n - i, m.q.size() - m.qPos);
        std::copy(m.q.begin() + (ptrdiff_t)m.qPos, m.q.begin() + (ptrdiff_t)(m.qPos + k), out + i);
        m.qPos += k;
        i += k;
    }
    if (m.sigma > 0) m.noise.add(out, n, m.sigma);
}

std::vector<WifiGenFrame> wifiTestSchedule() {
    using namespace wifi;
    std::vector<WifiGenFrame> v;
    auto add = [&](std::vector<uint8_t> f, double gap, double cfo = 0, double amp = 1) {
        WifiGenFrame g;
        g.mpdu = std::move(f); g.gapSec = gap; g.cfoHz = cfo; g.ampScale = amp;
        v.push_back(std::move(g));
    };
    ApSpec home;
    const uint8_t hb[6] = {0x00, 0x04, 0x0E, 0x12, 0x34, 0x56};   // AVM
    std::copy(hb, hb + 6, home.bssid);
    home.ssid = "OnAir-Home"; home.channel = 6; home.security = Wpa2; home.tsf = 123456789; home.seq = 100;
    ApSpec cafe;
    const uint8_t cb[6] = {0xFC, 0xEC, 0xDA, 0xAB, 0xCD, 0xEF};   // Ubiquiti
    std::copy(cb, cb + 6, cafe.bssid);
    cafe.ssid = "OnAir-Cafe"; cafe.channel = 6; cafe.security = Wpa2Wpa3; cafe.tsf = 987654321; cafe.seq = 2000;
    ApSpec hidden;
    const uint8_t xb[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};   // locally administered
    std::copy(xb, xb + 6, hidden.bssid);
    hidden.ssid = "secret"; hidden.hidden = true; hidden.channel = 6; hidden.security = Open; hidden.gRates = false; hidden.interval = 200;
    const uint8_t sta[6] = {0x3A, 0x01, 0x02, 0x03, 0x04, 0x05};
    add(makeBeacon(home), 0.002, 0, 1.0);
    add(makeBeacon(cafe), 0.002, 12e3, 0.6);
    add(makeDataFrame(home.bssid, sta, 120), 0.001);
    add(makeBeacon(hidden), 0.002, -8e3, 0.8);
    add(makeBeacon(home, true, sta), 0.003);
    return v;
}

namespace {
class WifiSynth : public ModeSynth {
public:
    WifiSynth(const SynthConfig& s, double rate) : rate_(rate) {
        WifiGenConfig c;
        c.rate = rate;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        const int band = s.modeOpt[0] == 1 ? 1 : s.modeOpt[0] == 2 ? 2 : 0;   // 2.4 / 5 / 6 GHz
        int ch = s.modeOpt[1];
        if (band && !wifi::validChannel(band, ch)) ch = band == 1 ? 36 : 5;
        c.frames = band ? wifiTestSchedule56(band, ch) : wifiTestSchedule();
        gen_ = std::make_unique<WifiGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<WifiGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeWifiSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<WifiSynth>(cfg, sampleRate);
}

} // namespace dect2
