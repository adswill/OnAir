// Test signals of the ham science event monitors (hamsci_evt.h). Simulated: nothing here is a recording of a real event and nothing is transmitted.
// Noise is complex white noise of variance 1 per sample at the radio rate (scaled by EvtGenOpt::noiseDb); levels are given at the output of the monitor's
// integrate-and-dump front end (decimation to about 200 Hz / 24 kHz / 2 kHz), where white noise of variance 1 per input sample has variance 1 / decimation.
#include "dect2/hamsci/hamsci_evt.h"
#include "hamsci_int.h"
#include <algorithm>
#include <cmath>

namespace dect2 {
namespace hamsci {

namespace {
using cf = std::complex<float>;
using detail::kPi;
using detail::Rng;

int decOf(double rate, double target) { return std::max(1, (int)std::floor(rate / target + 0.5)); }
double noiseSigma(const EvtGenOpt& o) { return std::pow(10.0, o.noiseDb / 20.0); }

// Ornstein-Uhlenbeck process with standard deviation sd and correlation time tau, stepped every dt
struct Ou {
    double x = 0, a = 0, b = 0;
    Ou(double sd, double tau, double dt) { a = std::exp(-dt / std::max(tau, 1e-9)); b = sd * std::sqrt(1 - a * a); }
    double step(Rng& r) { x = a * x + b * r.gauss(); return x; }
};
}  // namespace

// ------------------------------------------------------------------------------------------------------------------ Schumann
// Five resonances at the published frequencies (7.83, 14.1, 20.3, 26.3, 32.5 Hz, scaled with fundamentalHz / 7.83), each a stationary narrowband process of
// about 20 % relative width made from 9 sinusoids with Rayleigh amplitudes; the first peaks stand about 10 dB over a white background in the 200 Hz output,
// the higher ones less. 50 Hz mains (amplitude 0.5) is added at the input rate. When o.level > 1 a decaying 12 Hz transient of amplitude 4 * level comes
// every 60 s from 70 s on (a stand-in for ELF transients; not a model of a real Q-burst). o.noiseDb raises the white background (peaks down).
std::vector<cf> evtGenSchumann(double rate, const EvtGenOpt& o, double f1) {
    Rng r(o.seed);
    const size_t n = (size_t)(o.seconds * rate);
    std::vector<cf> out(n);
    const int dec = decOf(rate, 200.0);
    const double fo = rate / dec;
    static const double kMode[5] = {7.83, 14.1, 20.3, 26.3, 32.5};
    static const double kRatio[5] = {10.0, 7.0, 5.0, 4.0, 3.0};
    const int K = 21;
    struct Comp { double f, a, ph; };
    std::vector<Comp> comps;
    const double bgVar = 1.0;                       // per sample at the 200 Hz output (white)
    for (int m = 0; m < 5; m++) {
        const double fm = kMode[m] * f1 / 7.83 + o.offsetHz;
        const double bw = 0.2 * fm;
        // peak PSD ratio R over the flat background: power = R * (bw) * bgVar / (fo / 2)
        const double pw = kRatio[m] * bw * bgVar / (fo / 2.0);
        for (int k = 0; k < K; k++) {
            const double f = fm + bw * ((k + 0.5) / K - 0.5) * 1.5;
            const double w = std::exp(-0.5 * std::pow(((k + 0.5) / K - 0.5) * 3.0, 2.0));   // a Gaussian-ish shape
            comps.push_back({f, std::sqrt(2.0 * pw / K * w * 1.8) * std::sqrt(-std::log(std::max(1e-9, r.uni()))), 2 * kPi * r.uni()});
        }
    }
    const double sg = noiseSigma(o);
    const size_t step = (size_t)dec;
    double lastV = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        if (i % step == 0) {   // one 200 Hz sample, held over the decimation block (the monitor averages it back)
            double v = 0;
            const double to = std::floor(t * fo) / fo;
            for (const auto& c : comps) v += c.a * std::cos(2 * kPi * (c.f + o.driftHz * to / std::max(1.0, o.seconds)) * to + c.ph);
            v += sg * r.gauss();
            if (o.level > 1.0 && t >= 70.0) {
                const double tt = std::fmod(t - 70.0, 60.0);
                v += 6.0 * o.level * std::exp(-0.5 * std::pow((tt - 0.5) / 0.25, 2.0)) * std::sin(2 * kPi * 12.0 * tt);
            }
            lastV = v;
        }
        out[i] = cf((float)(lastV + 0.5 * std::sin(2 * kPi * 50.0 * t)), 0.f);
    }
    return out;
}

// ------------------------------------------------------------------------------------------------------------------ whistler and tweek
// Real audio. White noise (sigma 1 per sample), sferic impulses about every 0.7 s (2 ms decaying noise bursts), whistlers t = t0 + D / sqrt(f) from 9 kHz down
// to 1.5 kHz starting at the times given (t0 = the causative sferic, which is also put in), and tweeks: after the impulse at the time given, the two lowest
// waveguide modes (cut-off cutoffHz and twice that) with the group delay of a waveguide, f(dt) = fc / sqrt(1 - 1 / (1 + dt / tau)^2) with tau = 10 ms.
// o.level is the tone to noise ratio per FFT bin in dB (default 1.0 means 15 dB).
std::vector<cf> evtGenWhistler(double rate, const EvtGenOpt& o, const std::vector<double>& whistlerAtSec, const std::vector<double>& tweekAtSec, double D, double fc) {
    Rng r(o.seed);
    const size_t n = (size_t)(o.seconds * rate);
    std::vector<double> x(n, 0.0);
    const int dec = decOf(rate, 24000.0);
    const double fo = rate / dec;
    const double sg = noiseSigma(o);
    const double snrLin = std::pow(10.0, (o.level == 1.0 ? 15.0 : o.level) / 10.0);
    const double A = std::sqrt(snrLin / 85.0) * sg / std::sqrt((double)dec) * 1.5;   // the tone amplitude at the output of the front end (noise variance sg^2 / dec per sample)
    for (size_t i = 0; i < n; i++) x[i] = sg * r.gauss();
    auto impulse = [&](double t, double amp) {
        const size_t i0 = (size_t)(t * rate), L = (size_t)(0.002 * rate);
        for (size_t k = 0; k < L && i0 + k < n; k++) x[i0 + k] += amp * std::exp(-3.0 * (double)k / (double)L) * r.gauss();
    };
    // random sferics, away from the events by 0.1 s
    for (double t = 0.4; t < o.seconds - 0.5; t += 0.5 + r.uni() * 0.4) {
        bool nearEv = false;
        for (double w : whistlerAtSec) if (t > w - 0.1 && t < w + 2.2) nearEv = true;
        for (double w : tweekAtSec) if (t > w - 0.3 && t < w + 0.3) nearEv = true;
        if (!nearEv) impulse(t, 12.0 * sg * std::sqrt((double)dec) * (0.5 + r.uni()));
    }
    for (double tw : whistlerAtSec) {
        impulse(tw, 16.0 * sg * std::sqrt((double)dec));
        const double t1 = D / std::sqrt(9000.0), t2 = D / std::sqrt(1500.0);
        for (size_t i = (size_t)((tw + t1) * rate); i < n && (double)i / rate < tw + t2; i++) {
            const double tau = (double)i / rate - tw;
            const double env = std::min(1.0, std::min((tau - t1) / 0.03, (t2 - tau) / 0.1));
            x[i] += A * std::max(0.0, env) * std::cos(-2 * kPi * D * D / tau + o.offsetHz);
        }
    }
    for (double tk : tweekAtSec) {
        impulse(tk, 16.0 * sg * std::sqrt((double)dec));
        for (int mode = 1; mode <= 2; mode++) {
            const double fcm = fc * mode;
            double ph = 0;
            const double tau0 = 0.010;
            for (size_t i = (size_t)((tk + 0.001) * rate); i < n && (double)i / rate < tk + 0.12; i++) {
                const double dt = (double)i / rate - tk;
                const double q = 1.0 / (1.0 + dt / tau0);
                const double f = fcm / std::sqrt(1.0 - q * q);
                ph += 2 * kPi * f / rate;
                x[i] += 2.0 * A * (mode == 1 ? 1.0 : 0.6) * std::exp(-dt / 0.05) * std::cos(ph);
            }
        }
    }
    (void)fo;
    std::vector<cf> out(n);
    for (size_t i = 0; i < n; i++) out[i] = cf((float)x[i], 0.f);
    return out;
}

// ------------------------------------------------------------------------------------------------------------------ Moon echo
// Complex noise plus a carrier at 30 Hz + o.offsetHz keyed on at each start for txLenSec (50 dB over the noise in 4 Hz), and its echo: the same keying
// delaySec later, shifted by dopplerHz, with a Doppler spread of spreadHz (Ornstein-Uhlenbeck frequency modulation, correlation time 0.3 s), echoSnrDb over the
// noise in 12 Hz. o.driftHz drifts the carrier linearly over the run (the echo follows it with the delay).
std::vector<cf> evtGenEme(double rate, const EvtGenOpt& o, const std::vector<double>& txStart, double txLen, double delay, double doppler, double spread, double echoSnr) {
    Rng r(o.seed);
    const size_t n = (size_t)(o.seconds * rate);
    std::vector<cf> out(n);
    const int dec = decOf(rate, 2000.0);
    const double sg = noiseSigma(o);
    const double nOut = sg * sg / dec;   // noise variance per output sample
    const double fOut = rate / dec;
    const double Atx = std::sqrt(1e5 * 4.0 / fOut * nOut), Ae = std::sqrt(std::pow(10.0, echoSnr / 10.0) * 12.0 / fOut * nOut);
    Ou ou(spread / 2.5, 0.3, 1.0 / rate);
    double phTx = 0, phE = 0;
    const double f0 = 30.0 + o.offsetHz;
    auto keyed = [&](double t, double shift) { for (double s : txStart) if (t >= s + shift && t < s + shift + txLen) return true; return false; };
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        const double fd = o.driftHz * t / std::max(1.0, o.seconds);
        phTx += 2 * kPi * (f0 + fd) / rate;
        phE += 2 * kPi * (f0 + fd + doppler + ou.step(r)) / rate;
        cf v((float)(sg * r.gauss() / std::sqrt(2.0)), (float)(sg * r.gauss() / std::sqrt(2.0)));
        if (keyed(t, 0.0)) v += cf((float)(Atx * std::cos(phTx)), (float)(Atx * std::sin(phTx)));
        if (keyed(t, delay)) v += cf((float)(Ae * std::cos(phE)), (float)(Ae * std::sin(phE)));
        out[i] = v;
    }
    return out;
}

// ------------------------------------------------------------------------------------------------------------------ Jove / CALLISTO bursts
// Complex noise (variance 1 / sample times o.noiseDb) and bursts at the times given, each burstSec long and burstBwHz wide, o.level dB over the background in
// every bin it covers (default 6 dB when level is 1.0), centred on o.offsetHz and drifting by driftHzPerSec (the covered tones are chosen every 10 ms).
// The burst is a comb of tones 5 kHz apart with random phases. o.driftHz adds a slow gain drift of the whole band (dB over the run).
std::vector<cf> evtGenBursts(double rate, const EvtGenOpt& o, const std::vector<double>& burstAt, double burstSec, double driftHzPerSec, double burstBw) {
    Rng r(o.seed);
    const size_t n = (size_t)(o.seconds * rate);
    std::vector<cf> out(n);
    const double sg = noiseSigma(o);
    const double lvl = o.level == 1.0 ? 6.0 : o.level;
    const double add = std::pow(10.0, lvl / 10.0) - 1.0;
    const double spacing = 5000.0;
    const int K = (int)(rate * 0.9 / spacing);
    struct Tone { double f, ph, w; };
    std::vector<Tone> tones;
    for (int k = 0; k < K; k++) tones.push_back({(-0.45 * rate) + spacing * k, 2 * kPi * r.uni(), 0.0});
    const double v = add * burstBw / rate;                // added variance per sample over the covered width
    const int cover = std::max(1, (int)(burstBw / spacing));
    const double a = std::sqrt(v / cover) * sg;
    std::vector<cf> rot(tones.size()), inc(tones.size());
    for (size_t k = 0; k < tones.size(); k++) { rot[k] = cf((float)std::cos(tones[k].ph), (float)std::sin(tones[k].ph)); inc[k] = cf((float)std::cos(2 * kPi * tones[k].f / rate), (float)std::sin(2 * kPi * tones[k].f / rate)); }
    const size_t upd = std::max<size_t>(1, (size_t)(0.01 * rate));
    int active = -1;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        if (i % upd == 0) {
            for (auto& tn : tones) tn.w = 0;
            active = -1;
            for (size_t b = 0; b < burstAt.size(); b++) if (t >= burstAt[b] && t < burstAt[b] + burstSec) active = (int)b;
            if (active >= 0) {
                const double c = o.offsetHz + driftHzPerSec * (t - burstAt[(size_t)active]);
                for (auto& tn : tones) if (std::fabs(tn.f - c) <= burstBw / 2.0) tn.w = a;
            }
        }
        cf v2((float)(sg * r.gauss() / std::sqrt(2.0)), (float)(sg * r.gauss() / std::sqrt(2.0)));
        if (active >= 0) for (size_t k = 0; k < tones.size(); k++) if (tones[k].w > 0) v2 += rot[k] * (float)tones[k].w;
        if ((i & 255) == 0) for (size_t k = 0; k < rot.size(); k++) rot[k] /= std::abs(rot[k]);
        for (size_t k = 0; k < rot.size(); k++) rot[k] *= inc[k];
        if (o.driftHz != 0.0) v2 *= (float)std::pow(10.0, o.driftHz * t / o.seconds / 20.0);
        out[i] = v2;
    }
    return out;
}

// ------------------------------------------------------------------------------------------------------------------ aurora / Es / TEP beacons
// Complex noise plus a beacon carrier at o.offsetHz that is present during the openings given (openSec long each). The carrier is o.level dB over the
// noise in one 2 Hz bin (default 20 dB when level is 1.0); spreadHz is the Doppler spread (Ornstein-Uhlenbeck frequency modulation, correlation time 0.05 s,
// 0 = clean carrier) and flutterDb the standard deviation of the slow amplitude fading (correlation time 3 s). o.driftHz drifts the frequency over the run.
std::vector<cf> evtGenBeacon(double rate, const EvtGenOpt& o, const std::vector<double>& openAt, double openSec, double spread, double flutterDb) {
    Rng r(o.seed);
    const size_t n = (size_t)(o.seconds * rate);
    std::vector<cf> out(n);
    const int dec = decOf(rate, 2000.0);
    const double sg = noiseSigma(o);
    const double nOut = sg * sg / dec;
    const double lvl = o.level == 1.0 ? 20.0 : o.level;
    const double Ac = std::sqrt(std::pow(10.0, lvl / 10.0) * nOut / (0.667 * 1024.0));
    Ou fm(spread / 2.5, 0.05, 1.0 / rate), am(flutterDb, 3.0, 1.0 / rate);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / rate;
        const double fdev = spread > 0 ? fm.step(r) : 0.0;
        ph += 2 * kPi * (o.offsetHz + o.driftHz * t / std::max(1.0, o.seconds) + fdev) / rate;
        cf v((float)(sg * r.gauss() / std::sqrt(2.0)), (float)(sg * r.gauss() / std::sqrt(2.0)));
        bool open = false;
        for (double s : openAt) if (t >= s && t < s + openSec) open = true;
        const double g = flutterDb > 0 ? am.step(r) : 0.0;
        if (open) v += cf((float)(Ac * std::pow(10.0, g / 20.0) * std::cos(ph)), (float)(Ac * std::pow(10.0, g / 20.0) * std::sin(ph)));
        out[i] = v;
    }
    return out;
}

}  // namespace hamsci
}  // namespace dect2
