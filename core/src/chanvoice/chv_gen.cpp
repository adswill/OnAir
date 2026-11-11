#include "dect2/chanvoice/chv_gen.h"
#include "dect2/chanvoice/chv_rx.h"
#include "dect2/dsp/resampler.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/tones/tones_gen.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>

namespace dect2 {
namespace chv {

namespace {
constexpr double kPi = 3.14159265358979323846;

// Hilbert transformer FIR (odd length, Hamming window): the quadrature part of the audio
std::vector<float> hilbert(const std::vector<float>& a) {
    const int N = 127, h = N / 2;
    std::vector<double> k((size_t)N, 0.0);
    for (int i = 0; i < N; i++) { const int m = i - h; if (m % 2 != 0) k[(size_t)i] = 2.0 / (kPi * m) * (0.54 - 0.46 * std::cos(2 * kPi * i / (N - 1))); }
    std::vector<float> q(a.size(), 0.f);
    for (size_t n = 0; n < a.size(); n++) {
        double s = 0;
        for (int i = 0; i < N; i++) { const long j = (long)n + h - i; if (j >= 0 && j < (long)a.size()) s += k[(size_t)i] * a[(size_t)j]; }
        q[n] = (float)s;
    }
    return q;
}
}

std::vector<cf32> makeChanVoiceScene(const GenScene& sc) {
    const size_t N = (size_t)(sc.durSec * sc.rate);
    std::vector<cf32> out(N, cf32{});
    const double ar = 48000.0;
    for (const GenTx& t : sc.tx) {
        const bool wide = t.mod == Demod::Wfm;
        const double rate48 = wide ? 192000.0 : ar;
        const size_t n48 = (size_t)(t.durSec * rate48);
        // audio at 48 kHz (or 192 kHz for the microphone): speech or a tone
        ToneScene ts(48000, t.seed);
        ts.pad(t.durSec + 0.2);
        double voiceStart = 0;
        std::vector<float> mdcAudio;
        if (t.mdc) { mdcAudio = mdcEncode(t.mdcPacket, 48000, 0.2); voiceStart = (double)mdcAudio.size() / 48000.0 + 0.15; }
        if (voiceStart < t.durSec - 0.2) {
            if (t.speech) { ts.noise(voiceStart, t.durSec - voiceStart, 0.01); ts.speech(voiceStart, t.durSec - voiceStart, t.mod == Demod::Am || t.mod == Demod::Usb || t.mod == Demod::Lsb ? 0.4 : 0.35); }
            else for (double hz : t.tones) ts.tone(hz, voiceStart, t.durSec - voiceStart, 0.3 / (double)std::max<size_t>(1, t.tones.size()));
        }
        if (t.ctcssHz > 0) ts.ctcss(t.ctcssHz, 0, t.durSec, 0.08);
        if (t.dcsCode >= 0) ts.dcs(t.dcsCode, t.dcsInverted, 0, t.durSec, 0.1);
        std::vector<float> a(ts.audio().begin(), ts.audio().begin() + (long)std::min(ts.audio().size(), (size_t)(t.durSec * 48000)));
        a.resize((size_t)(t.durSec * 48000), 0.f);
        for (size_t i = 0; i < mdcAudio.size() && i < a.size(); i++) a[i] += mdcAudio[i];
        std::vector<cf32> bb;
        if (wide) {
            // wide FM at 192 kHz: audio upsampled by 4 (linear), peak deviation 40 kHz x devScale
            bb.resize(n48);
            double ph = 0;
            for (size_t i = 0; i < n48; i++) {
                const double p = (double)i / 4.0; const size_t i0 = (size_t)p; const double fr = p - (double)i0;
                const double v = (i0 + 1 < a.size()) ? a[i0] * (1 - fr) + a[i0 + 1] * fr : (i0 < a.size() ? a[i0] : 0.0);
                ph += 2 * kPi * (v * 2.0 * 40000.0 * t.devScale) / rate48;   // 0.5 of full scale is the usual peak: 40 kHz
                bb[i] = std::polar((float)t.amp, (float)ph);
            }
        } else {
            bb.resize(a.size());
            if (t.mod == Demod::Nfm) {
                double ph = 0;
                for (size_t i = 0; i < a.size(); i++) { ph += 2 * kPi * (a[i] * 5000.0 * t.devScale) / ar; bb[i] = std::polar((float)t.amp, (float)ph); }
            } else if (t.mod == Demod::Am) {
                float pk = 1e-6f; for (float v : a) pk = std::max(pk, std::fabs(v));
                for (size_t i = 0; i < a.size(); i++) bb[i] = cf32((float)(t.amp * (1.0 + t.depth * a[i] / pk)), 0.f);
            } else {   // USB / LSB: analytic signal of the audio, carrier reference at 0 Hz
                {   // the transmitter's audio filter (about 300 - 2800 Hz): the speech and its noise floor are 24 kHz wide at 48 kHz, which put
                    // energy up to 24 kHz above the carrier and false calls in the channels beside it
                    const int L = 255, h = L / 2;
                    std::vector<float> taps((size_t)L);
                    for (int i = 0; i < L; i++) {
                        const double m = i - h, w = 0.5 - 0.5 * std::cos(2 * kPi * i / (L - 1));
                        auto lp = [&](double fc) { const double x = 2 * fc / ar; return m == 0 ? x : std::sin(kPi * x * m) / (kPi * m); };
                        taps[(size_t)i] = (float)(w * (lp(2800.0) - lp(300.0)));
                    }
                    std::vector<float> y(a.size(), 0.f);
                    for (size_t i = 0; i < a.size(); i++) {
                        float acc = 0;
                        for (int k = 0; k < L; k++) { const long j = (long)i + k - h; if (j >= 0 && j < (long)a.size()) acc += taps[(size_t)k] * a[(size_t)j]; }
                        y[i] = acc;
                    }
                    a.swap(y);
                }
                float pk = 1e-6f; for (float v : a) pk = std::max(pk, std::fabs(v));
                const std::vector<float> q = hilbert(a);
                for (size_t i = 0; i < a.size(); i++) {
                    const float re = a[i] / pk * (float)t.amp, im = q[i] / pk * (float)t.amp;
                    bb[i] = t.mod == Demod::Usb ? cf32(re, im) : cf32(re, -im);
                }
            }
        }
        // to the span: resample, mix, add at the start time
        RationalResampler rs;
        if (!rs.configure(rate48, sc.rate, nullptr, 0)) continue;
        std::vector<cf32> up;
        up.reserve((size_t)((double)bb.size() * sc.rate / rate48) + 64);
        rs.process(bb.data(), bb.size(), up);
        if (t.mod == Demod::Usb || t.mod == Demod::Lsb) {   // SSB: the resampler's images of the 3 kHz signal at every 48 kHz (about -70 dB) are
            // well above a quiet test signal's floor and the finder took them for carriers; a low-pass at 6 kHz (Blackman, 48 kHz - 3 kHz away) removes them
            const int L = std::max(31, (int)std::ceil(12.0 * sc.rate / 40e3)) | 1, h = L / 2;
            std::vector<float> taps((size_t)L);
            double sum = 0;
            for (int i = 0; i < L; i++) {
                const double m = i - h, x = 2 * 6000.0 / sc.rate;
                const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (L - 1)) + 0.08 * std::cos(4 * kPi * i / (L - 1));
                taps[(size_t)i] = (float)(w * (m == 0 ? x : std::sin(kPi * x * m) / (kPi * m)));
                sum += taps[(size_t)i];
            }
            for (float& v : taps) v = (float)(v / sum);
            std::vector<cf32> y(up.size());
            for (size_t i = 0; i < up.size(); i++) {
                cf32 acc(0, 0);
                for (int k = 0; k < L; k++) { const long j = (long)i + k - h; if (j >= 0 && j < (long)up.size()) acc += taps[(size_t)k] * up[(size_t)j]; }
                y[i] = acc;
            }
            up.swap(y);
        }
        const double f = (t.mhz - sc.centreMhz) * 1e6 + t.cfoHz;
        const size_t s0 = (size_t)(t.startSec * sc.rate);
        const double w = 2 * kPi * f / sc.rate;
        for (size_t i = 0; i < up.size() && s0 + i < N; i++) {
            const double ph = w * (double)(s0 + i);
            out[s0 + i] += up[i] * cf32((float)std::cos(ph), (float)std::sin(ph));
        }
    }
    std::mt19937 rng(77);
    std::normal_distribution<float> nd(0.f, (float)sc.noiseSigma);
    for (auto& s : out) s += cf32(nd(rng), nd(rng));
    return out;
}

GenScene chanVoiceDemoScene(Service sv, int region, double rate, double snrDb) {
    GenScene sc; sc.rate = rate;
    const Span sp = planSpan(sv, region);
    sc.centreMhz = sp.centreMhz;
    sc.durSec = 6.0;
    const double amp = 0.12;
    sc.noiseSigma = amp * std::sqrt(rate / (2 * 10e3)) * std::pow(10.0, -snrDb / 20.0);
    // not below a third of a step of an 8-bit radio (the test source quantises to 8 bits): with no noise to dither it, the quantiser turned a
    // quiet scene's SSB speech into spurs 46 and 92 kHz away that the finder took for calls (a real radio's own noise is about a step)
    sc.noiseSigma = std::max(sc.noiseSigma, 0.3 / 128.0);
    auto tx = [&](double mhz, Demod m, double start, double dur) { GenTx t; t.mhz = mhz; t.mod = m; t.amp = amp; t.startSec = start; t.durSec = dur; t.seed = (unsigned)(mhz * 1000) & 0xFFFF; return t; };
    const auto& ch = channels(sv, region);
    auto chan = [&](const char* label) { const Channel* c = findLabel(sv, region, label); return c ? c->mhz : sc.centreMhz; };
    switch (sv) {
    case Service::Cb: {
        GenTx a = tx(chan("19"), Demod::Am, 0.3, 2.2), b = tx(chan("9"), Demod::Nfm, 1.5, 2.0), c = tx(chan("16"), Demod::Usb, 3.2, 2.4), d = tx(chan("17"), Demod::Lsb, 4.4, 1.4);
        if (region == 2) { a.mod = Demod::Nfm; }
        b.devScale = 0.6; sc.tx = {a, b, c, d}; break; }
    case Service::Frs: {
        GenTx a = tx(chan("3"), Demod::Nfm, 0.3, 2.0), b = tx(chan("17"), Demod::Nfm, 1.8, 2.0), c = tx(chan("20"), Demod::Nfm, 3.6, 1.8);
        a.devScale = b.devScale = c.devScale = 0.5; a.ctcssHz = 88.5; b.dcsCode = 23; sc.tx = {a, b, c}; break; }
    case Service::Pmr: {
        GenTx a = tx(chan("5"), Demod::Nfm, 0.3, 2.0), b = tx(chan("12"), Demod::Nfm, 1.8, 2.0), c = tx(chan("2"), Demod::Nfm, 3.8, 1.8);
        a.devScale = b.devScale = c.devScale = 0.5; a.ctcssHz = 67.0; b.dcsCode = 754; b.dcsInverted = true; sc.tx = {a, b, c}; break; }
    case Service::Ham: {
        const double m1 = ch.size() > 3 ? ch[3].mhz : sc.centreMhz, m2 = ch.size() > 9 ? ch[9].mhz : sc.centreMhz + 0.05, m3 = ch.size() > 6 ? ch[6].mhz : sc.centreMhz - 0.05;
        GenTx a = tx(m1, Demod::Nfm, 0.3, 2.2), b = tx(m2, Demod::Nfm, 1.5, 2.4), c = tx(m3, Demod::Nfm, 3.6, 1.8);
        a.devScale = b.devScale = c.devScale = 0.7; a.ctcssHz = 123.0; b.ctcssHz = 88.5; sc.tx = {a, b, c}; break; }
    case Service::HfVoice: {
        // 9 s: the looped test signal needs a pause longer than the SSB squelch's hang (40 blocks) after the call, or the call never ends
        GenTx a = tx(chan("HFGCS 11175"), Demod::Usb, 0.3, 4.5); a.amp = 0.15; sc.tx = {a}; sc.durSec = 9.0; break; }
    case Service::Mdc: {
        GenTx a = tx(sc.centreMhz - 0.0375, Demod::Nfm, 0.3, 2.2), b = tx(sc.centreMhz + 0.0625, Demod::Nfm, 2.0, 2.4);
        a.devScale = b.devScale = 0.7; a.mdc = true; a.mdcPacket.op = 0x01; a.mdcPacket.arg = 0x80; a.mdcPacket.unitId = 0x1A2B;
        b.mdc = true; b.mdcPacket.op = 0x01; b.mdcPacket.arg = 0x00; b.mdcPacket.unitId = 0xC0DE; sc.tx = {a, b}; break; }
    default: {   // wireless microphones
        sc.durSec = std::min(6.0, 20e6 / rate);   // a wide span is big: the loop is shorter
        const double dl = sc.durSec - 0.3;
        GenTx a = tx(sc.centreMhz - 0.6, Demod::Wfm, 0.1, dl), b = tx(sc.centreMhz + 0.85, Demod::Wfm, 0.1, dl), c = tx(sc.centreMhz + 0.25, Demod::Wfm, 0.1, dl);
        a.amp = b.amp = c.amp = 0.3;
        a.speech = false; a.tones = {440, 880}; b.devScale = 0.7; c.speech = false; c.devScale = 0.5; sc.tx = {a, b, c}; break; }
    }
    return sc;
}

} // namespace chv

namespace {
class LoopSynth : public ModeSynth {
public:
    LoopSynth(std::vector<cf32> scene, double rate) : x_(std::move(scene)), rate_(rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) { out[i] = x_[pos_]; if (++pos_ >= x_.size()) pos_ = 0; }
    }
private:
    std::vector<cf32> x_;
    double rate_;
    size_t pos_ = 0;
};
}

std::unique_ptr<ModeSynth> makeChanVoiceSynth(const SynthConfig& cfg, double sampleRate, chv::Service s) {
    chv::GenScene sc = chv::chanVoiceDemoScene(s, std::max(0, cfg.modeOpt[0]), sampleRate, cfg.snrDb);
    // as a radio receives it: the radio sits the mode's tune offset above the plan's centre, so the channels lie that far below the
    // middle of the capture (the engine moves them back by the offset; HF voice's one channel was 12 kHz off the receiver's slot)
    sc.centreMhz += chanVoiceTuning(s).tuneOffsetHz / 1e6;
    return std::make_unique<LoopSynth>(chv::makeChanVoiceScene(sc), sampleRate);
}

} // namespace dect2
