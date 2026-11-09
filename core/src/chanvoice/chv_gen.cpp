#include "dect2/chanvoice/chv_gen.h"
#include "dect2/dsp/resampler.h"
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
            } else {   // USB / LSB: analytic signal of the audio, band-limited by the speech itself; carrier reference at 0 Hz
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

} // namespace chv
} // namespace dect2
