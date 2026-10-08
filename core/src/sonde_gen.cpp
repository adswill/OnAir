// Radiosonde test signal (see sonde_gen.h).
#include "dect2/sonde_gen.h"
#include "dect2/gen_util.h"
#include "dect2/sonde_bits.h"
#include "dect2/sonde_geo.h"
#include "dect2/sonde_rs41.h"
#include "dect2/sonde_rs92.h"
#include "dect2/sonde_tel.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace dect2 {

namespace {

constexpr double kCentreHz = 403.0e6;
constexpr double kFl = 192000.0;                      // rate of the baseband signal of one sonde
constexpr double kStartUnix = 1780272000.0;           // 2026-06-01 00:00:00 UTC: the time in the frames at the start of the signal

struct Spec {
    int kind;
    double freqHz;
    double lat, lon, alt, vv;      // start, vertical speed m/s
    int variant;                   // DFM-06/09/17
    double devHz;
    const char* serial;
};

const Spec kSpecs[7] = {
    {SondeRs41, 403.000e6, 25.20, 55.36, 12000, 5.0, 0, 2400, "N4750123"},
    {SondeDfm, 402.200e6, 25.30, 55.50, 22000, -15.0, 17, 2400, "17012345"},
    {SondeM10, 404.100e6, 25.10, 55.20, 8000, 5.0, 0, 4800, "310-2-11329"},
    {SondeRs41, 403.450e6, 24.90, 55.10, 5000, 5.0, 0, 2400, "N4750456"},
    {SondeM20, 401.600e6, 25.00, 55.60, 15000, 5.0, 0, 4800, "211-4-01234"},
    {SondeDfm, 405.000e6, 25.40, 55.30, 3000, 4.0, 9, 2400, "19076543"},
    {SondeRs92, 405.300e6, 25.15, 55.45, 6000, 5.0, 0, 2400, "P4953934"},
};

double baudOf(int kind) {
    switch (kind) {
    case SondeRs41: case SondeRs92: return 4800;
    case SondeDfm: return 2500;
    case SondeM10: return 9615;
    default: return 9600;
    }
}
const char* typeOf(int kind) {
    switch (kind) {
    case SondeRs41: return "RS41";
    case SondeDfm: return "DFM";
    case SondeM10: return "M10";
    case SondeRs92: return "RS92";
    default: return "M20";
    }
}

// a plain atmosphere for temperature, humidity and pressure at an altitude
void atmosphere(double altM, double& tC, double& rh, double& pHpa) {
    const double h = altM / 1000.0;
    if (h < 11) tC = 15 - 6.5 * h; else if (h < 20) tC = -56.5; else tC = -56.5 + (h - 20);
    rh = h < 11 ? 80.0 * std::exp(-h / 3.5) : 3.0;
    pHpa = h < 11 ? 1013.25 * std::pow(1 - 0.0065 * altM / 288.15, 5.255) : 226.3 * std::exp(-(altM - 11000) / 6342.0);
}

struct Sonde {
    Spec spec;
    double ampl = 0.1;
    double carrierHz = 0;           // relative to the centre
    // baseband modulator
    std::vector<uint8_t> sym;
    size_t symIdx = 0;
    double symPh = 0;
    bool burst = false;
    double nextBurstT = 0;          // seconds of baseband time
    double bbCount = 0;
    double env = 0;                 // carrier on/off ramp
    double lp1 = 0, lp2 = 0, phi = 0;
    cf32 b0, b1;
    double frac = 0;
    // output oscillator
    double nco = 0;
    double period = 1.0;
    int frameCount = 0;
    Rs41Cal cal;
    double lonDrift = 0;
};

class SondeSynth : public ModeSynth {
public:
    SondeSynth(const SynthConfig& cfg, double rate) : rate_(rate), cfg_(cfg), noise_(cfg.modeOpt[2] ? (uint32_t)cfg.modeOpt[2] : 1u) {
        const int mask = cfg.modeOpt[0] ? cfg.modeOpt[0] : 0x7;
        int want = cfg.modeOpt[1] > 0 ? std::min(cfg.modeOpt[1], 7) : 3;
        east_ = cfg.modeVal[0] != 0 ? cfg.modeVal[0] : 10.0;
        driftHzPerMin_ = cfg.modeVal[1];
        const double off2 = cfg.modeVal[2] != 0 ? cfg.modeVal[2] : 450000.0;
        std::mt19937 rng(cfg.modeOpt[2] ? (uint32_t)cfg.modeOpt[2] : 1u);
        for (int i = 0; i < 7 && (int)sondes_.size() < want; i++) {
            const int bit = kSpecs[i].kind == SondeRs41 ? 1 : kSpecs[i].kind == SondeDfm ? 2 : kSpecs[i].kind == SondeM10 ? 4 : kSpecs[i].kind == SondeM20 ? 8 : 16;
            if (!(mask & bit)) continue;
            Sonde s;
            s.spec = kSpecs[i];
            double f = s.spec.freqHz;
            if (i == 3) f = kSpecs[0].freqHz + off2;
            s.carrierHz = f - kCentreHz;
            if (std::fabs(s.carrierHz) > 0.45 * rate) continue;
            s.period = sondeFramePeriodS(typeOf(s.spec.kind));
            if (s.period <= 0.1) s.period = 1.0;
            s.nextBurstT = 0.05 + 0.37 * (double)sondes_.size();
            s.frameCount = (int)(rng() % 1000) + 100;
            s.phi = 2 * M_PI * (double)(rng() % 1000) / 1000.0;
            s.cal = rs41MakeCal(f);
            sondes_.push_back(s);
        }
        const int ns = (int)sondes_.size();
        // total power about 0.04 (rms 0.2): signals plus noise; noise power is set by the SNR in 10 kHz
        const double snr = std::pow(10.0, -cfg.snrDb / 10.0);
        const double k = rate / 1.0e4;
        const double a2 = 0.04 / std::max(1, ns) / (1.0 + k * snr / std::max(1, ns));
        const double a = std::sqrt(a2);
        for (auto& s : sondes_) s.ampl = a;
        noiseSigma_ = (float)std::sqrt(0.5 * a2 * k * snr);       // per real component; the table has unit variance per component
        step_ = kFl / rate * (1.0 + cfg.sroPpm * 1e-6);
        ncoScale_ = 1.0 + cfg.sroPpm * 1e-6;
        for (auto& s : sondes_) { s.b0 = nextBb(s); s.b1 = nextBb(s); }
    }
    double sampleRate() const override { return rate_; }

    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f);
        for (size_t base = 0; base < n; base += 1024) {
            const size_t m = std::min<size_t>(1024, n - base);
            for (auto& s : sondes_) {
                const double drift = driftHzPerMin_ * (double)count_ / rate_ / 60.0;
                const double f = (s.carrierHz + drift + cfg_.cfoHz) * ncoScale_;
                const double dph = 2 * M_PI * f / rate_;
                double ph = s.nco;
                const cf32 inc((float)std::cos(dph), (float)std::sin(dph));
                cf32 osc((float)std::cos(ph), (float)std::sin(ph));
                for (size_t i = 0; i < m; i++) {
                    s.frac += step_;
                    if (s.frac >= 1.0) { s.frac -= 1.0; s.b0 = s.b1; s.b1 = nextBb(s); }
                    const float fr = (float)s.frac;
                    const cf32 b = s.b0 + fr * (s.b1 - s.b0);
                    out[base + i] += b * osc;
                    osc *= inc;
                }
                const double mag = std::abs(osc);
                ph += dph * (double)m;
                s.nco = std::fmod(ph, 2 * M_PI);
                (void)mag;
            }
            noise_.add(out + base, m, noiseSigma_);
            count_ += m;
        }
        for (size_t i = 0; i < n; i++) {
            float re = std::clamp(out[i].real(), -0.89f, 0.89f), im = std::clamp(out[i].imag(), -0.89f, 0.89f);
            out[i] = cf32(re, im);
        }
    }

private:
    double rate_;
    SynthConfig cfg_;
    genutil::NoiseSource noise_;
    std::vector<Sonde> sondes_;
    double east_ = 10, driftHzPerMin_ = 0, step_ = 0, ncoScale_ = 1;
    float noiseSigma_ = 0;
    uint64_t count_ = 0;

    SondeTruth truthAt(const Sonde& s, double t, int frame) const {
        SondeTruth tr;
        tr.serial = s.spec.serial;
        tr.frame = frame;
        tr.altM = std::max(100.0, s.spec.alt + s.spec.vv * t);
        const double hSpeed = std::fabs(east_);
        tr.lat = s.spec.lat;
        tr.lon = s.spec.lon + east_ * t / (111320.0 * std::cos(s.spec.lat * M_PI / 180.0));
        tr.vSpeed = s.spec.vv;
        tr.hSpeed = hSpeed;
        tr.headingDeg = east_ >= 0 ? 90.0 : 270.0;
        tr.sats = 9;
        tr.unixTime = kStartUnix + t;
        double p;
        atmosphere(tr.altM, tr.tempC, tr.humidity, p);
        tr.pressureHpa = p;
        tr.batteryV = 2.9;
        return tr;
    }

    // next sample of one sonde's baseband signal (at kFl)
    cf32 nextBb(Sonde& s) {
        const double t = s.bbCount / kFl;
        s.bbCount += 1;
        if (!s.burst && t >= s.nextBurstT) {
            const SondeTruth tr = truthAt(s, t, s.frameCount);
            std::vector<uint8_t> sy;
            if (s.spec.kind == SondeRs41) sy = rs41Symbols(rs41Frame(tr, s.cal, s.frameCount % kRs41CalFrames));
            else if (s.spec.kind == SondeDfm) sy = dfmSymbols(tr, s.spec.variant);
            else if (s.spec.kind == SondeM10) sy = m10Symbols(tr);
            else if (s.spec.kind == SondeRs92) { Rs92Truth rt; rt.serial = tr.serial; rt.frame = tr.frame; rt.unixTime = tr.unixTime; rt.sats = tr.sats; rt.freqHz = s.spec.freqHz; sy = rs92Symbols(rs92Frame(rt, s.frameCount % kRs92CalFrames)); }
            else sy = m20Symbols(tr);
            s.frameCount++;
            s.nextBurstT += s.period;
            if (!sy.empty()) { s.sym = std::move(sy); s.symIdx = 0; s.symPh = 0; s.burst = true; }
        }
        double target = 0, level = 0;
        if (s.burst) {
            target = 1;
            level = s.sym[s.symIdx] ? 1.0 : -1.0;
            s.symPh += baudOf(s.spec.kind) / kFl;
            if (s.symPh >= 1.0) {
                s.symPh -= 1.0;
                if (++s.symIdx >= s.sym.size()) { s.burst = false; s.sym.clear(); }
            }
        }
        // shaping of the frequency: two one-pole filters at 0.75 of the baud rate
        const double a = 1.0 - std::exp(-2 * M_PI * 0.75 * baudOf(s.spec.kind) / kFl);
        s.lp1 += a * (level - s.lp1);
        s.lp2 += a * (s.lp1 - s.lp2);
        s.phi += 2 * M_PI * s.spec.devHz * s.lp2 / kFl;
        if (s.phi > 1e6) s.phi = std::fmod(s.phi, 2 * M_PI);
        // carrier on and off with a ramp of about 0.5 ms
        s.env += (target - s.env) * (1.0 - std::exp(-1.0 / (0.0005 * kFl)));
        const float amp = (float)(s.ampl * s.env);
        return cf32(amp * (float)std::cos(s.phi), amp * (float)std::sin(s.phi));
    }
};

} // namespace

std::unique_ptr<ModeSynth> makeSondeSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < 1e6) return nullptr;
    return std::make_unique<SondeSynth>(cfg, sampleRate);
}

} // namespace dect2
