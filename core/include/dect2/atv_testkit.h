// Analog TV: helpers shared by the tests and the tool: run the generator through the receiver with impairments, measure the pictures and the sound.
// Header only; nothing here is used by the receiver.
#pragma once
#include "atv_card.h"
#include "atv_gen.h"
#include "atv_rx.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace atvkit {

struct Run {
    AtvTelemetry tel;
    std::shared_ptr<const AtvFrame> frame;      // the last picture
    std::vector<std::shared_ptr<const AtvFrame>> frames;   // every picture (kept only when asked for)
    std::vector<float> audio;                   // 48 kHz
    std::vector<std::string> log;
    double signalSecs = 0, cpuSecs = 0;         // seconds of signal fed, seconds of CPU spent in feed()
    uint64_t telCount = 0;
    int lockedAfterMs = -1;                     // first telemetry with state 2
    std::vector<std::pair<double, int>> stateLog;   // (seconds of signal fed, state) at every telemetry report
    std::vector<double> frameTimes;             // seconds of signal fed when each picture came out
};


// ---- impairments for Options::impair (they work on the samples of one chunk; `first` is the number of the first sample of the stream)
using Impair = std::function<void(cf32* x, size_t n, uint64_t first)>;

inline Impair dcOffset(float re, float im) {
    return [=](cf32* x, size_t n, uint64_t) { for (size_t i = 0; i < n; i++) x[i] += cf32(re, im); };
}
// the Q branch has `gain` times the amplitude and leaks the I branch at `degrees`
inline Impair iqImbalance(double gain, double degrees) {
    const float g = (float)gain, s = (float)std::sin(degrees * M_PI / 180), c = (float)std::cos(degrees * M_PI / 180);
    return [=](cf32* x, size_t n, uint64_t) { for (size_t i = 0; i < n; i++) x[i] = cf32(x[i].real(), g * (x[i].imag() * c + x[i].real() * s)); };
}
// the signal is replaced by zeros for `ms` milliseconds from `startSec`
inline Impair dropout(double startSec, double ms, double rate) {
    const uint64_t a = (uint64_t)(startSec * rate), b = a + (uint64_t)(ms * 1e-3 * rate);
    return [=](cf32* x, size_t n, uint64_t first) { for (size_t i = 0; i < n; i++) if (first + i >= a && first + i < b) x[i] = cf32(0, 0); };
}
// a carrier of amplitude `amp` at `hz` from the centre
inline Impair tone(double hz, double amp, double rate) {
    return [=](cf32* x, size_t n, uint64_t first) {
        for (size_t i = 0; i < n; i++) { const double ph = 2 * M_PI * hz * (double)(first + i) / rate; x[i] += (float)amp * cf32((float)std::cos(ph), (float)std::sin(ph)); }
    };
}
// bursts of `len` samples of amplitude `amp` every `periodSamples` samples (ignition noise)
inline Impair impulses(uint64_t periodSamples, int len, float amp) {
    return [=](cf32* x, size_t n, uint64_t first) {
        for (size_t i = 0; i < n; i++) { const uint64_t k = first + i; if (k % periodSamples < (uint64_t)len) x[i] += cf32(amp * (float)((k & 1) ? 1 : -1), 0.5f * amp); }
    };
}
// the level is multiplied by `gain` from fromSec to toSec
inline Impair levelStep(double fromSec, double toSec, float gain, double rate) {
    const uint64_t a = (uint64_t)(fromSec * rate), b = (uint64_t)(toSec * rate);
    return [=](cf32* x, size_t n, uint64_t first) { for (size_t i = 0; i < n; i++) if (first + i >= a && first + i < b) x[i] *= gain; };
}
// from `atSec` on the whole signal is shifted by `hz` (the transmitter or the radio's oscillator jumps)
inline Impair frequencyStep(double atSec, double hz, double rate) {
    const uint64_t a = (uint64_t)(atSec * rate);
    return [=](cf32* x, size_t n, uint64_t first) {
        for (size_t i = 0; i < n; i++) {
            const uint64_t k = first + i;
            if (k < a) continue;
            const double ph = 2 * M_PI * hz * (double)(k - a) / rate;
            x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph));
        }
    };
}
inline Impair chain(std::vector<Impair> list) {
    return [list](cf32* x, size_t n, uint64_t first) { for (const auto& f : list) f(x, n, first); };
}

struct Options {
    size_t chunk = 65536;
    bool quantise = true;                       // 8 bits like the radio
    bool keepFrames = false;
    Impair impair;                              // changes the samples (before quantisation)
    std::function<void(AtvReceiver&, double secs)> onTime;               // called after each chunk
    double scale = 1.0;                         // signal level before quantisation (HackRF: 0.1 .. 0.5 of full scale)
};

inline Run run(const AtvGenConfig& cfg, double secs, const Options& o = Options()) {
    Run r;
    AtvGenerator g(cfg);
    AtvReceiver rx;
    rx.setSilent(true);
    rx.setAudioTap([&](const float* l, const float*, size_t n) { r.audio.insert(r.audio.end(), l, l + n); });
    rx.setLogCallback([&](const std::string& m) { r.log.push_back(m); });
    rx.configure(cfg.rate);
    std::vector<cf32> buf;
    const size_t total = (size_t)(secs * cfg.rate);
    uint64_t seq = 0, fseq = 0;
    double cpu = 0;
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(o.chunk, total - done);
        buf.resize(n);
        g.generate(buf.data(), n);
        if (o.scale != 1.0) for (auto& s : buf) s *= (float)o.scale;
        if (o.impair) o.impair(buf.data(), n, done);
        if (o.quantise)
            for (auto& s : buf) {
                auto q = [](float v) { return std::round(std::min(127.f, std::max(-128.f, v * 128.f))) / 128.f; };
                s = cf32(q(s.real()), q(s.imag()));
            }
        const std::clock_t c0 = std::clock();
        rx.feed(buf.data(), n);
        cpu += (double)(std::clock() - c0) / CLOCKS_PER_SEC;       // CPU time of the process: does not grow when the machine is busy with other things
        done += n;
        AtvTelemetry t;
        if (rx.telemetry(t, seq)) {
            seq = t.seq; r.tel = t; r.telCount++;
            r.stateLog.push_back({(double)done / cfg.rate, t.state});
            if (t.state == 2 && r.lockedAfterMs < 0) r.lockedAfterMs = (int)(1000.0 * (double)done / cfg.rate);
        }
        while (auto f = rx.frame(fseq)) { r.frame = f; r.frameTimes.push_back((double)done / cfg.rate); if (o.keepFrames) r.frames.push_back(f); }
        if (o.onTime) o.onTime(rx, (double)done / cfg.rate);
    }
    r.signalSecs = secs; r.cpuSecs = cpu;
    return r;
}

// ---- pictures

// the mean colour of a rectangle of the picture, 0..1
inline void meanRgb(const AtvFrame& f, int x0, int y0, int x1, int y1, double rgb[3]) {
    double s[3] = {0, 0, 0};
    int n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            const uint8_t* p = &f.rgba[((size_t)y * f.width + (size_t)x) * 4];
            for (int k = 0; k < 3; k++) s[k] += p[k] / 255.0;
            n++;
        }
    for (int k = 0; k < 3; k++) rgb[k] = n ? s[k] / n : 0;
}

// the colour bars of a card: mean colour of each of the 8 bars (the middle of the bar, away from its edges and from the top and bottom)
inline void barColours(const AtvFrame& f, const AtvCard& card, double out[8][3]) {
    int x0, x1, y0, y1;
    card.barsArea(x0, x1, y0, y1);
    for (int i = 0; i < 8; i++) {
        const int a = x0 + (x1 - x0) * i / 8, b = x0 + (x1 - x0) * (i + 1) / 8;
        meanRgb(f, a + (b - a) / 4, y0 + (y1 - y0) / 5, b - (b - a) / 4, y1 - (y1 - y0) / 5, out[i]);
    }
}


// Where the white bar of the card begins in the picture, in pixels: the first column that is brighter than half, along a row in the middle of
// the bars, and the first row, along a column in the middle of the white bar. Compared with barsArea() they show a shift of the picture.
inline void whiteBarEdges(const AtvFrame& f, const AtvCard& card, int& x, int& y) {
    int x0, x1, y0, y1;
    card.barsArea(x0, x1, y0, y1);
    auto lum = [&](int xx, int yy) { const uint8_t* q = &f.rgba[((size_t)yy * f.width + (size_t)xx) * 4]; return (0.299 * q[0] + 0.587 * q[1] + 0.114 * q[2]) / 255.0; };
    x = -1; y = -1;
    const int ym = (y0 + y1) / 2, xm = x0 + (x1 - x0) / 16;
    for (int xx = std::max(0, x0 - 20); xx < xm; xx++) if (lum(xx, ym) > 0.6) { x = xx; break; }
    for (int yy = std::max(0, y0 - 12); yy < ym; yy++) if (lum(xm, yy) > 0.6) { y = yy; break; }
}

// Mean absolute difference of the luminance (0..255 scale) between a picture and the reference card over the part of the picture that does
// not move, optionally allowing the picture to be shifted by up to `maxShift` pixels each way. Returns the best difference; dx and dy get the shift.
inline double lumaDifference(const AtvFrame& f, const AtvCard& card, int maxShift, int* dxOut = nullptr, int* dyOut = nullptr) {
    std::vector<uint8_t> ref;
    card.referenceFrame(0, 0, ref);
    const int W = f.width, H = f.height;
    auto Y = [&](const std::vector<uint8_t>& p, int x, int y) { const uint8_t* q = &p[((size_t)y * W + (size_t)x) * 4]; return 0.299 * q[0] + 0.587 * q[1] + 0.114 * q[2]; };
    double best = 1e9;
    int bx = 0, by = 0;
    for (int dy = -maxShift; dy <= maxShift; dy++)
        for (int dx = -maxShift; dx <= maxShift; dx++) {
            double s = 0;
            int n = 0;
            for (int y = (int)(0.07 * H); y < (int)(0.30 * H); y += 3)           // the bars
                for (int x = (int)(0.08 * W); x < (int)(0.92 * W); x += 2) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || xx >= W || yy < 0 || yy >= H) continue;
                    const uint8_t* q = &f.rgba[((size_t)yy * W + (size_t)xx) * 4];
                    s += std::fabs(0.299 * q[0] + 0.587 * q[1] + 0.114 * q[2] - Y(ref, x, y)); n++;
                }
            for (int y = (int)(0.33 * H); y < (int)(0.47 * H); y += 3)           // the ramp and the staircase
                for (int x = (int)(0.07 * W); x < (int)(0.49 * W); x += 2) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || xx >= W || yy < 0 || yy >= H) continue;
                    const uint8_t* q = &f.rgba[((size_t)yy * W + (size_t)xx) * 4];
                    s += std::fabs(0.299 * q[0] + 0.587 * q[1] + 0.114 * q[2] - Y(ref, x, y)); n++;
                }
            const double e = s / std::max(1, n);
            if (e < best) { best = e; bx = dx; by = dy; }
        }
    if (dxOut) *dxOut = bx;
    if (dyOut) *dyOut = by;
    return best;
}

// ---- sound

// amplitude of a tone at `hz` in x[from, to) (48 kHz), by correlation
inline double toneAmp(const std::vector<float>& x, double hz, size_t from, size_t to) {
    if (to > x.size()) to = x.size();
    if (to <= from) return 0;
    double re = 0, im = 0;
    for (size_t i = from; i < to; i++) { const double ph = 2 * M_PI * hz * (double)i / 48000.0; re += x[i] * std::cos(ph); im += x[i] * std::sin(ph); }
    return 2 * std::sqrt(re * re + im * im) / (double)(to - from);
}

// the strongest tone between lo and hi Hz in a stretch of sound, found on a grid of 2 Hz steps
inline double peakTone(const std::vector<float>& x, size_t from, size_t to, double lo = 200, double hi = 4000) {
    double best = 0, bf = 0;
    for (double f = lo; f <= hi; f += 5) { const double a = toneAmp(x, f, from, to); if (a > best) { best = a; bf = f; } }
    for (double f = bf - 5; f <= bf + 5; f += 0.5) { const double a = toneAmp(x, f, from, to); if (a > best) { best = a; bf = f; } }
    return bf;
}

} // namespace atvkit
} // namespace dect2
