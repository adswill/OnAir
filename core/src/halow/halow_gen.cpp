// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) test signal (see halow_gen.h).
#include "dect2/halow/halow_gen.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/halow/halow_proto.h"
#include "dect2/halow/halow_rx.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

using namespace halow;

namespace {
using dect2::math::kPi;
constexpr int kHalf = 12;   // sinc taps each side

// the PPDU spec of a schedule entry (a bad FCS flips one PSDU bit after the FCS was computed)
PpduSpec specOfFrame(const HalowGenFrame& f) {
    PpduSpec ps;
    ps.w = f.width; ps.pre = f.pre; ps.mcs = f.mcs; ps.nss = f.nss; ps.stbc = f.stbc; ps.ldpc = f.ldpc; ps.sgi = f.sgi; ps.agg = f.agg; ps.ndp = f.ndp;
    ps.beamformed = f.beamformed; ps.beamTilt = f.beamTilt; ps.seed = f.seed; ps.psdu = f.psdu;
    if (f.badFcs && !ps.psdu.empty()) ps.psdu[ps.psdu.size() / 2] ^= 0x10;
    return ps;
}
}

struct HalowGenerator::Impl {
    HalowGenConfig cfg;
    std::deque<cf32> q;
    size_t idx = 0;
    bool done = false;
    double ph = 0;
    std::mt19937 rng;
    genutil::NoiseSource noise;
    float sigma = 0;
    std::unique_ptr<ExactResampler> rs[kNumWidths];   // 8 and 16 MHz PPDUs: the long filter keeps the band edge tones (the 24 tap sinc below droops there)
    explicit Impl(const HalowGenConfig& c) : cfg(c), rng(c.seed), noise(c.seed) {}

    // Windowed sinc interpolation of the PPDU at position u (samples), with the given cutoff for a lower radio rate.
    static cf32 interp(const std::vector<cf32>& x, double u, double cutoff) {
        const int64_t i0 = (int64_t)std::floor(u);
        const double fr = u - (double)i0;
        std::complex<double> s(0, 0);
        for (int k = -kHalf + 1; k <= kHalf; k++) {
            const int64_t j = i0 + k;
            if (j < 0 || j >= (int64_t)x.size()) continue;
            const double d = (double)k - fr;
            const double a = d * cutoff * kPi;
            const double sn = std::fabs(a) < 1e-9 ? 1.0 : std::sin(a) / a;
            const double w = 0.5 + 0.5 * std::cos(kPi * d / kHalf);
            s += std::complex<double>(x[(size_t)j].real(), x[(size_t)j].imag()) * (sn * w * cutoff);
        }
        return cf32((float)s.real(), (float)s.imag());
    }

    void silence(double sec) { const size_t n = (size_t)std::llround(sec * cfg.rate); for (size_t i = 0; i < n; i++) q.push_back(cf32(0.f, 0.f)); }

    // one echo of the PPDU (multipath): x[n] + g e^{j phi} x[n - d], d in samples of the PPDU's grid
    void addEcho(std::vector<cf32>& x, int w) {
        if (cfg.echoGainDb > -90 && cfg.echoDelaySec > 0) {
            const size_t d = (size_t)std::llround(cfg.echoDelaySec * gridRate(w));
            const double ph = 2 * kPi * (double)(rng() & 0xFFFF) / 65536.0;
            const cf32 g = (float)std::pow(10.0, cfg.echoGainDb / 20) * cf32((float)std::cos(ph), (float)std::sin(ph));
            if (d > 0 && d < x.size()) for (size_t i = x.size(); i-- > d;) x[i] += g * x[i - d];
        }
    }

    // Queues the next PPDU of the schedule at the radio rate, with its impairments, and the gap after it.
    void refill() {
        if (done || cfg.frames.empty()) { silence(0.001); return; }
        const HalowGenFrame& f = cfg.frames[idx];
        const int w = f.width;
        std::vector<cf32> x = buildPpdu(specOfFrame(f));
        addEcho(x, w);
        const int pad = 64;
        x.insert(x.begin(), (size_t)pad + (rng() & 7), cf32(0, 0));
        x.insert(x.end(), (size_t)pad, cf32(0, 0));
        const double grid = gridRate(w);
        const double off = cfg.dialOffsetHz + cfg.cfoHz + f.cfoHz;
        const double amp = cfg.amp * f.ampScale;
        std::vector<cf32> y;
        if (w >= W8) {
            auto& r = rs[w];
            if (!r) { r = std::make_unique<ExactResampler>(); if (!r->configure(grid * (1 + cfg.clockPpm * 1e-6), cfg.rate)) r.reset(); }
            if (r) { r->reset(); r->process(x.data(), x.size(), y); }
        }
        if (y.empty()) {
            const double step = grid * (1 + cfg.clockPpm * 1e-6) / cfg.rate;
            const double frac = (double)(rng() & 0xFFFF) / 65536.0;
            const size_t ns = (size_t)std::ceil(((double)x.size() - kHalf - 1) / step);
            const double cutoff = std::min(1.0, cfg.rate / grid);
            for (size_t i = 0; i < ns; i++) {
                const double u = ((double)i - frac) * step;
                y.push_back(step == 1.0 && frac == 0.0 ? x[(size_t)u] : interp(x, u, cutoff));
            }
        }
        {   // the frame's offset by a rotating phasor (renormalised every 1024 samples), not a sine and a cosine per sample
            const double w = 2 * kPi * off / cfg.rate;
            cf32 z((float)std::cos(ph), (float)std::sin(ph));
            const cf32 st((float)std::cos(w), (float)std::sin(w));
            size_t k = 0;
            for (const cf32& s : y) {
                z *= st;
                if ((++k & 1023) == 0) z /= std::abs(z);
                q.push_back((float)amp * s * z);
            }
            ph = std::remainder(ph + w * (double)y.size(), 2 * kPi);
        }
        silence(f.gapSec);
        if (++idx >= cfg.frames.size()) { idx = 0; if (!cfg.loop) done = true; }
    }
};

// Noise per real component from the SNR in 1 MHz and the radio rate.
double HalowGenerator::noiseSigma(const HalowGenConfig& c) {
    if (c.snrDb >= 99) return 0;
    const double n1 = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // noise power in 1 MHz
    return std::sqrt(n1 * c.rate / 1e6 / 2);
}

HalowGenerator::HalowGenerator(const HalowGenConfig& c) : p_(std::make_unique<Impl>(c)) {
    p_->sigma = (float)noiseSigma(c);
    p_->silence(c.leadSec);
    for (size_t left = c.skipSamples; left > 0;) {   // start inside a PPDU: the first samples are produced and dropped
        if (p_->q.empty()) p_->refill();
        const size_t k = std::min(left, p_->q.size());
        p_->q.erase(p_->q.begin(), p_->q.begin() + (ptrdiff_t)k);
        left -= k;
    }
}

// duration of a schedule entry's PPDU on the air
double halowFrameSeconds(const HalowGenFrame& f) { return (double)ppduSamples(specOfFrame(f)) / gridRate(f.width); }
HalowGenerator::~HalowGenerator() = default;

// Fills out with n samples of the schedule plus noise.
void HalowGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    for (size_t i = 0; i < n; i++) {
        if (m.q.empty()) m.refill();
        out[i] = m.q.front();
        m.q.pop_front();
    }
    if (m.sigma > 0) m.noise.add(out, n, m.sigma);
}

// The networks of the test schedule (see halow_gen.h); the 8 and 16 MHz ones only when the radio rate can hold them.
std::vector<HalowGenFrame> halowTestSchedule(double radioRate) {
    std::vector<HalowGenFrame> v;
    auto add = [&](std::vector<uint8_t> psdu, int width, int mcs, double cfo, double gap = 0.003, uint8_t seed = 0x5D, int nss = 1) {
        HalowGenFrame f;
        f.psdu = std::move(psdu); f.width = width; f.mcs = mcs; f.cfoHz = cfo; f.gapSec = gap; f.seed = seed; f.nss = nss;
        v.push_back(std::move(f));
    };
    BssSpec home;   // EU, 1 MHz, WPA3
    { const uint8_t b[6] = {0x24, 0x0A, 0xC4, 0x12, 0x34, 0x56}; std::copy(b, b + 6, home.bssid); }
    home.ssid = "HaLow-Home"; home.security = Wpa3Sae; home.bwInd = 0; home.opClass = 6; home.primary = 3; home.ccf = 3; home.opWidthMhz = 1; home.intervalTu = 100;
    BssSpec farm;   // EU, 2 MHz, WPA2
    { const uint8_t b[6] = {0x00, 0x15, 0x6D, 0xAA, 0xBB, 0x01}; std::copy(b, b + 6, farm.bssid); }
    farm.ssid = "Farm-Sensors"; farm.security = Wpa2Psk; farm.bwInd = 1; farm.opClass = 7; farm.primary = 6; farm.ccf = 6; farm.opWidthMhz = 2; farm.intervalTu = 200;
    BssSpec cam;    // US, 4 MHz, open
    { const uint8_t b[6] = {0x4C, 0x5E, 0x0C, 0x00, 0x11, 0x22}; std::copy(b, b + 6, cam.bssid); }
    cam.ssid = "OpenCam"; cam.security = Open; cam.bwInd = 2; cam.opClass = 2; cam.primary = 4; cam.ccf = 4; cam.opWidthMhz = 4; cam.intervalTu = 100;
    BssSpec meter;  // a network that only sends the compressed SSID in its beacons
    { const uint8_t b[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55}; std::copy(b, b + 6, meter.bssid); }
    meter.ssid = "Meter-Net"; meter.fullSsid = false; meter.security = Wpa3Sae; meter.bwInd = 0; meter.opClass = 6; meter.primary = 7; meter.ccf = 7; meter.opWidthMhz = 1;
    const uint8_t sta[6] = {0xB8, 0x27, 0xEB, 0x01, 0x02, 0x03};
    home.timestamp = 1000000; add(makeS1gBeacon(home), W1, 0, 6e3);
    farm.timestamp = 2000000; add(makeS1gBeacon(farm), W2, 0, -9e3);
    cam.timestamp = 3000000; add(makeS1gBeacon(cam), W4, 0, 14e3);
    meter.timestamp = 4000000; add(makeS1gBeacon(meter), W1, 10, -12e3, 0.003, 0x2B);   // 1 MHz MCS 10 (repetition), the way long range beacons go out
    add(makeProbeResponse(meter, sta), W2, 0, 4e3, 0.003, 0x11);                        // answers a probe with the full SSID
    add(makeDataFrame(home.bssid, sta, 60), W2, 0, -3e3);                                // data: counted, not a network
    { std::vector<uint8_t> u(90, 0x5A); add(u, W2, 3, 8e3, 0.003, 0x5D, 2); }            // a PPDU with two spatial streams: the SIG field only
    if (radioRate >= recommendedRateHz(W8) - 1) {   // US 8 MHz network: an S1G_LONG-preamble beacon at MCS 2
        BssSpec lab;
        { const uint8_t b[6] = {0x8C, 0x1F, 0x64, 0x00, 0x08, 0x08}; std::copy(b, b + 6, lab.bssid); }
        lab.ssid = "Lab-8MHz"; lab.security = Wpa3Sae; lab.bwInd = 3; lab.opClass = 4; lab.primary = 8; lab.ccf = 8; lab.opWidthMhz = 8; lab.timestamp = 5000000;
        add(makeS1gBeacon(lab), W8, 2, -7e3);
    }
    if (radioRate >= recommendedRateHz(W16) - 1) {   // US 16 MHz network
        BssSpec wide;
        { const uint8_t b[6] = {0x8C, 0x1F, 0x64, 0x00, 0x16, 0x16}; std::copy(b, b + 6, wide.bssid); }
        wide.ssid = "Wide-16MHz"; wide.security = Open; wide.bwInd = 4; wide.opClass = 5; wide.primary = 16; wide.ccf = 16; wide.opWidthMhz = 16; wide.timestamp = 6000000;
        add(makeS1gBeacon(wide), W16, 1, 9e3);
    }
    return v;
}

namespace {
class HalowSynth : public ModeSynth {
public:
    HalowSynth(const SynthConfig& s, double rate) : rate_(rate) {
        HalowGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -halowTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.frames = halowTestSchedule(rate);
        gen_ = std::make_unique<HalowGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<HalowGenerator> gen_;
};
} // namespace

// The engine's test signal source for HaLow at the given radio rate.
std::unique_ptr<ModeSynth> makeHalowSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<HalowSynth>(cfg, sampleRate);
}

} // namespace dect2
