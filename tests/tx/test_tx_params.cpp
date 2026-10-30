// The advanced transmit parameters (TxRequest::opt): the sideband and the carrier injection of SSB, the AM depth, the speech compressor, the VOX gate, the FM broadcast
// level and the shared output stage (trim, mirrored spectrum). Everything is generated into memory; NOTHING is transmitted.
#include "dect2/tx/tx.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<cf32> make(const char* id, const TxRequest& r, double rate, size_t n) {
    std::string err;
    auto g = makeTxGenerator(*txModeById(id), r, rate, err);
    std::vector<cf32> x;
    if (!g) { printf("no generator for %s: %s\n", id, err.c_str()); return x; }
    x.resize(n);
    g->generate(x.data(), n);
    return x;
}
// power of x at frequency f (a DFT bin), skipping the first `skip` samples (filters settling)
static double powerAt(const std::vector<cf32>& x, double f, double fs, size_t skip) {
    std::complex<double> s = 0;
    for (size_t i = skip; i < x.size(); i++) s += std::complex<double>(x[i]) * std::polar(1.0, -2 * M_PI * f * (double)i / fs);
    return std::norm(s) / std::pow((double)(x.size() - skip), 2);
}
static double rms(const std::vector<cf32>& x, size_t skip) {
    double s = 0;
    for (size_t i = skip; i < x.size(); i++) s += std::norm(x[i]);
    return std::sqrt(s / (double)(x.size() - skip));
}
static double db(double p) { return 10 * std::log10(std::max(p, 1e-30)); }

int main() {
    const double fs = 2.5e6;   // QO-100 needs at least 2 Msps
    const size_t n = 500000;
    // SSB: the chosen sideband carries the tone, the other is empty
    TxRequest usb; usb.modeId = "qo100"; usb.kind = 2; usb.toneHz = 1000; usb.audioSrc = 2;
    usb.opt["audio.hpHz"] = 0; usb.opt["audio.lpHz"] = 0;
    auto x = make("qo100", usb, fs, n);
    CHECK(!x.empty(), "qo100 generator");
    if (!x.empty()) {
        const double up = db(powerAt(x, 1000, fs, 60000)), lo = db(powerAt(x, -1000, fs, 60000));
        CHECK(up - lo > 25, "USB: +1 kHz %.1f dB, -1 kHz %.1f dB", up, lo);
        CHECK(db(powerAt(x, 0, fs, 60000)) < up - 25, "suppressed carrier");
    }
    TxRequest lsb = usb; lsb.kind = 3;
    x = make("qo100", lsb, fs, n);
    if (!x.empty()) {
        const double up = db(powerAt(x, 1000, fs, 60000)), lo = db(powerAt(x, -1000, fs, 60000));
        CHECK(lo - up > 25, "LSB: +1 kHz %.1f dB, -1 kHz %.1f dB", up, lo);
    }
    TxRequest car = usb; car.opt["ssb.carrierPct"] = 100;
    x = make("qo100", car, fs, n);
    if (!x.empty()) CHECK(db(powerAt(x, 0, fs, 60000)) > db(powerAt(x, 1000, fs, 60000)) - 12, "carrier injection puts a carrier at the dial frequency");
    TxRequest two = usb; two.opt["audio.tone2Hz"] = 1900;
    x = make("qo100", two, fs, n);
    if (!x.empty()) CHECK(db(powerAt(x, 1900, fs, 60000)) > db(powerAt(x, 1000, fs, 60000)) - 3, "two-tone test has the second tone");
    // the low-pass: a 3 kHz tone through a 1 kHz low-pass drops a lot
    TxRequest tone3 = usb; tone3.toneHz = 3000;
    const double plain = x.empty() ? 0 : db(powerAt(make("qo100", tone3, fs, n), 3000, fs, 60000));
    tone3.opt["audio.lpHz"] = 1000;
    const double filt = db(powerAt(make("qo100", tone3, fs, n), 3000, fs, 60000));
    CHECK(plain - filt > 25, "low-pass: %.1f dB without, %.1f dB with", plain, filt);
    // the compressor turns a loud tone down
    TxRequest loud = usb; loud.opt["audio.toneLevelPct"] = 100;
    const double r0 = rms(make("qo100", loud, fs, n), 60000);
    loud.opt["audio.compOn"] = 1; loud.opt["audio.compThreshDb"] = -30; loud.opt["audio.compRatio"] = 20;
    const double r1 = rms(make("qo100", loud, fs, n), 60000);
    CHECK(r1 < r0 * 0.8, "compressor: rms %.4f without, %.4f with", r0, r1);
    // VOX: silence closes the gate
    TxRequest vox = usb; vox.audioSrc = 3; vox.opt["vox.on"] = 1; vox.opt["vox.hangMs"] = 50;
    x = make("qo100", vox, fs, n);
    if (!x.empty()) CHECK(rms(x, n / 2) < 0.01, "VOX closed on silence: rms %.4f", rms(x, n / 2));
    // AM: the envelope swings with the depth (airband)
    TxRequest am; am.modeId = "airband"; am.toneHz = 1000; am.audioSrc = 2; am.opt["audio.toneLevelPct"] = 100; am.opt["audio.lpHz"] = 0;
    auto env = [&](double depth) {
        am.opt["am.depthPct"] = depth;
        const auto y = make("airband", am, fs, n);
        double lo = 1e9, hi = 0;
        for (size_t i = 60000; i < y.size(); i++) { const double m = std::abs(y[i]); lo = std::min(lo, m); hi = std::max(hi, m); }
        return hi / std::max(lo, 1e-9);
    };
    const double flat = env(0), half = env(50);
    CHECK(flat < 1.05, "depth 0 is a plain carrier: ratio %.3f", flat);
    CHECK(half > 2.5 && half < 3.5, "depth 50 %% at full tone: peak / trough %.2f (expect 3)", half);
    // FM broadcast: the output level
    TxRequest fm; fm.modeId = "fm"; fm.audioSrc = 2;
    fm.opt["bfm.level"] = 0.2;
    const double f0 = rms(make("fm", fm, 1.0e6, 60000), 0);
    fm.opt["bfm.level"] = 0.4;
    const double f1 = rms(make("fm", fm, 1.0e6, 60000), 0);
    CHECK(std::fabs(f1 / f0 - 2.0) < 0.05, "FM level scales the output: %.4f -> %.4f", f0, f1);
    // the shared output stage: trim and mirrored spectrum
    TxRequest o; o.opt["out.trimDb"] = -6.0206; o.opt["out.swapIq"] = 1;
    std::vector<cf32> v(4, cf32(0.5f, 0.1f));
    txOutputStage(o, v.data(), v.size(), 1u << 30, fs);
    CHECK(std::fabs(v[0].real() - 0.05f) < 1e-3 && std::fabs(v[0].imag() - 0.25f) < 1e-3, "trim and swap: %.4f %.4f", v[0].real(), v[0].imag());
    TxRequest ramp; ramp.opt["out.rampMs"] = 1;
    std::vector<cf32> w(6000, cf32(1, 0));
    txOutputStage(ramp, w.data(), w.size(), 0, fs);   // 1 ms at 2.5 MHz = 2500 samples
    CHECK(w[0].real() == 0 && w[1250].real() > 0.45f && w[1250].real() < 0.55f && w[5000].real() == 1.f, "fade-in ramp");
    // #374: a mode the radio is tuned off the dial for (the General receiver is 250 kHz above) puts its analog voice at the dial frequency, not on the radio's centre
    for (const char* id : {"general", "airband"}) {
        const double off = txModeTuneOffsetHz(*txModeById(id));
        CHECK(off != 0, "%s has a tuning offset", id);
        if (off == 0) continue;
        for (int kind : {0, 1, 2, 3}) {
            TxRequest v; v.modeId = id; v.analogVoice = true; v.kind = kind; v.toneHz = 1000; v.audioSrc = 2; v.deviationHz = 1000;
            v.opt["audio.hpHz"] = 0; v.opt["audio.lpHz"] = 0;
            auto y = make(id, v, fs, n);
            CHECK(!y.empty(), "%s voice generator", id);
            if (y.empty()) continue;
            // the tone / carrier is within 3 kHz of the dial frequency (-off in the samples), not of 0 Hz
            double at = 0, centre = 0;
            for (double d = -3000; d <= 3000; d += 250) { at = std::max(at, powerAt(y, -off + d, fs, 60000)); centre = std::max(centre, powerAt(y, d, fs, 60000)); }
            CHECK(at > centre * 100, "%s kind %d: power at the dial %.1f dB, at the radio's centre %.1f dB", id, kind, db(at), db(centre));
        }
    }
    // #373: the CTCSS tone comes from the pull-down of standard tones (fm.ctcssIdx), at 600 Hz peak deviation with the default 5 kHz
    {
        const int idx = 14;   // 103.5 Hz
        const double want = txCtcssToneHz(idx);
        CHECK(std::fabs(want - 103.5) < 0.01 && txCtcssToneHz(0) == 0 && txCtcssToneHz(51) == 0, "tone table: %.2f", want);
        TxRequest c; c.modeId = "general"; c.analogVoice = true; c.audioSrc = 3; c.opt["fm.ctcssIdx"] = idx;
        auto y = make("general", c, fs, n);
        CHECK(!y.empty(), "ctcss generator");
        if (!y.empty()) {
            std::vector<cf32> f(y.size() - 1);   // instantaneous frequency, as a real number in the real part
            for (size_t i = 1; i < y.size(); i++) f[i - 1] = cf32((float)(std::arg(std::conj(y[i - 1]) * y[i]) * fs / (2 * M_PI)), 0.f);
            double mean = 0;
            for (size_t i = 60000; i < f.size(); i++) mean += f[i].real();
            mean /= (double)(f.size() - 60000);
            for (auto& v : f) v = cf32(v.real() - (float)mean, 0.f);   // the dial offset is a constant
            const double at = std::sqrt(powerAt(f, want, fs, 60000)) * 2, off = std::sqrt(powerAt(f, 203.5, fs, 60000)) * 2;
            CHECK(std::fabs(at - 600) < 80, "CTCSS peak deviation %.0f Hz (want about 600)", at);
            CHECK(off < 60, "no energy at another tone: %.0f Hz", off);
        }
    }
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
