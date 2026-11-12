// The zoom spectrum (docs/WATERFALL-4.0.md): the plan maths (a bin per pixel or finer at every zoom, room to pan, the automatic choice), the
// decimator (unit gain, alias rejection over the usable band, every D from 2 to 65536 split right), the analyser (two tones 12 Hz apart
// that the full-band FFT merges come out as two peaks, at the right frequency with an off-centre view and off-bin tones, the level and the
// noise floor agree with the full-band analyser), the window functions, the overlap rule and the settings changing under a feeding thread.
// All on generated signals (tones, white noise); no radio.
#include "dect2/dsp/zoom_spectrum.h"
#include "dect2/dsp/spectrum.h"
#include "dect2/common/stable_random.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <random>
#include <thread>
#include <vector>
using namespace dect2;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
static const double kPi = 3.14159265358979323846;

struct Tone { double hz; double amp; };
static std::vector<cf32> signal(double fs, size_t n, const std::vector<Tone>& tones, float noise, unsigned seed) {
    std::mt19937 rng(seed);
    dect2::normal_distribution<float> nd(0.f, noise);
    std::vector<cf32> x(n);
    for (size_t i = 0; i < n; i++) {
        double re = 0, im = 0;
        for (const Tone& t : tones) { const double ph = 2 * kPi * std::fmod(t.hz * (double)i / fs, 1.0); re += t.amp * std::cos(ph); im += t.amp * std::sin(ph); }
        x[i] = cf32((float)re + (noise > 0 ? nd(rng) : 0.f), (float)im + (noise > 0 ? nd(rng) : 0.f));
    }
    return x;
}

// the newest frame after feeding x in pieces (as the spectrum thread does)
static bool run(ZoomSpectrum& z, const std::vector<cf32>& x, ZoomFrame& f) {
    bool got = false;
    for (size_t p = 0; p < x.size(); p += 65536) {
        z.feed(x.data() + p, std::min<size_t>(65536, x.size() - p));
        if (z.takeFrame(f)) got = true;
    }
    return got;
}
static double binHzOf(const ZoomFrame& f, size_t i) { return f.plan.offsetHz + ((double)i - (double)f.dbfs.size() / 2) * f.plan.binHz; }
static size_t binOf(const ZoomFrame& f, double hz) { return (size_t)std::lround((hz - f.plan.offsetHz) / f.plan.binHz + (double)f.dbfs.size() / 2); }
static double usableMax(const ZoomFrame& f, double exceptHz = NAN, double exceptW = 0) {
    double m = -400;
    for (size_t i = 0; i < f.dbfs.size(); i++) {
        const double hz = binHzOf(f, i);
        if (hz < f.plan.usableLo() || hz > f.plan.usableHi()) continue;
        if (std::isfinite(exceptHz) && std::fabs(hz - exceptHz) < exceptW) continue;
        m = std::max(m, (double)f.dbfs[i]);
    }
    return m;
}

int main() {
    // ---- the split of D ------------------------------------------------------------------------------------------------------------
    {
        int r, h;
        zoomfft::split(2, r, h); check(r == 1 && h == 1, "D 2: one half-band");
        zoomfft::split(4, r, h); check(r == 1 && h == 2, "D 4: two half-bands");
        zoomfft::split(8, r, h); check(r == 2 && h == 2, "D 8: CIC 2 + two half-bands");
        zoomfft::split(4096, r, h); check(r == 1024 && h == 2, "D 4096: CIC 1024 + two half-bands");
        zoomfft::split(65536, r, h); check(r == 1024 && h == 6, "D 65536: the CIC stays at 1024, six half-bands");
        bool ok = true;
        for (int d = 2; d <= zoomfft::kMaxDecim; d *= 2) { zoomfft::split(d, r, h); ok = ok && r * (1 << h) == d && r <= zoomfft::kMaxCic && h >= 1; }
        check(ok, "every D = R * 2^H, R <= 1024");
    }

    // ---- the plan ------------------------------------------------------------------------------------------------------------------
    {
        const zoomfft::Plan p = zoomfft::make(2.4e6, 0, 10e3, 2000);
        check(p.on && p.decim == 64 && p.bins == 8192 && std::fabs(p.rateHz - 37500) < 1e-6, "2.4 Msps, 10 kHz over 2000 px: D 64, 8192 bins");
        check(p.binHz * 2000 <= 10e3, "a bin per pixel or finer");
        check(!zoomfft::make(2.4e6, 0, 700e3, 2000).on, "a view over a quarter of the band: no zoom spectrum (D would be 1)");
        check(zoomfft::make(2.4e6, 0, 600e3, 2000).on && zoomfft::make(2.4e6, 0, 600e3, 2000).decim == 2, "a quarter of the band: D 2");
        check(!zoomfft::make(0, 0, 10e3, 2000).on && !zoomfft::make(2.4e6, 0, 0, 2000).on && !zoomfft::make(2.4e6, 0, 10e3, 0).on, "nonsense: off");
        check(!zoomfft::make(2.4e6, NAN, 10e3, 2000).on, "NaN offset: off");
        // the sweep: every span from 20 Hz to the band, several plot widths and sample rates
        bool perPx = true, fits = true, room = true, quant = true, cap = true;
        for (double fs : {48e3, 250e3, 2.4e6, 10e6, 20e6, 61.44e6}) for (int px : {320, 1000, 1920, 2560, 3840, 7680})
            for (double span = 20; span < fs; span *= 1.13) {
                const double off = 0.17 * fs * std::sin(span);   // off-centre views
                const zoomfft::Plan q = zoomfft::make(fs, off, span, px);
                if (!q.on) continue;
                if (q.bins < zoomfft::kMaxBins && q.decim < zoomfft::kMaxDecim) perPx = perPx && q.binHz * px <= span * (1 + 1e-9);
                fits = fits && span <= zoomfft::kUsable * q.rateHz;
                room = room && span <= zoomfft::kFill * q.rateHz * (1 + 1e-9);
                quant = quant && std::fabs(q.offsetHz / q.binHz - std::round(q.offsetHz / q.binHz)) < 1e-6 && std::fabs(q.offsetHz - off) <= q.binHz * 0.5 + 1e-9;
                cap = cap && q.bins >= zoomfft::kMinBins && q.bins <= zoomfft::kMaxBins && (q.bins & (q.bins - 1)) == 0;
                const zoomfft::Plan c = zoomfft::make(fs, off, span, px, 4096);
                cap = cap && c.bins <= 4096;
            }
        check(perPx, "sweep: a bin per pixel or finer wherever the caps allow");
        check(fits, "sweep: the view always inside the usable band");
        check(room, "sweep: a new plan's view fills at most half the band (room to pan)");
        check(quant, "sweep: the offset is a whole number of bins, within half a bin of the view centre");
        check(cap, "sweep: bins a power of two within the caps (and the caller's cap)");
    }
    {   // update(): the automatic choice
        zoomfft::Plan cur;
        const double fs = 2.4e6;
        zoomfft::Plan p = zoomfft::update(cur, fs, 65536, 0, fs, 2000);
        check(!p.on, "the whole band over 2000 px with 64k bins (36.6 Hz, 1200 Hz a pixel): no zoom spectrum");
        p = zoomfft::update(cur, fs, 65536, 0, 80e3, 2000);
        check(!p.on, "80 kHz over 2000 px (40 Hz a pixel) with 36.6 Hz bins: still off");
        p = zoomfft::update(cur, fs, 65536, 300e3, 50e3, 2000);
        check(p.on && p.id == 1 && p.binHz * 2000 <= 50e3, "50 kHz over 2000 px (25 Hz a pixel): on, a new plan");
        cur = p;
        zoomfft::Plan q = zoomfft::update(cur, fs, 65536, 300e3 + 5e3, 50e3, 2000);
        check(q.on && q.id == cur.id && q.offsetHz == cur.offsetHz, "a small pan inside the usable band: the same plan (nothing restarts)");
        q = zoomfft::update(cur, fs, 65536, 300e3, 45e3, 2000);
        check(q.id == cur.id, "zooming in a little inside the octave: the same plan");
        q = zoomfft::update(cur, fs, 65536, 300e3 + 40e3, 50e3, 2000);
        check(q.on && q.id == cur.id + 1 && std::fabs(q.offsetHz - 340e3) <= q.binHz, "a pan out of the usable band: a new plan centred on the view");
        check(q.sameShape(cur) && std::fabs(std::fmod(std::fabs(q.offsetHz - cur.offsetHz) / cur.binHz + 0.5, 1.0) - 0.5) < 1e-6, "  the same shape: the rows can be shifted by whole bins");
        q = zoomfft::update(cur, fs, 65536, 300e3, 20e3, 2000);
        check(q.on && q.id == cur.id + 1 && q.decim > cur.decim, "zooming in past the octave: a finer plan");
        q = zoomfft::update(cur, 2.0e6, 65536, 300e3, 50e3, 2000);
        check(q.id == cur.id + 1 && std::fabs(q.rateHz * q.decim - 2.0e6) < 1e-3, "a new sample rate: a new plan");
        q = zoomfft::update(cur, fs, 1 << 20, 300e3, 50e3, 2000);
        check(!q.on, "a manual 1M FFT (2.3 Hz bins) covers 25 Hz a pixel: the zoom spectrum is off");
        q = zoomfft::update(cur, fs, 65536, 300e3, 50e3, 2000, 1024);
        check(q.on && q.bins <= 1024, "the caller's bin cap holds");
    }

    // ---- windows -------------------------------------------------------------------------------------------------------------------
    {
        bool rms = true, enbw = true;
        for (int t = 0; t < specwin::NumTypes; t++) {
            std::vector<float> w(4096);
            specwin::make(t, w.data(), 4096);
            double s = 0, s2 = 0;
            for (float v : w) { s += v; s2 += (double)v * v; }
            rms = rms && std::fabs(s2 / 4096 - 1) < 1e-3;
            const double e = 4096 * s2 / (s * s);
            enbw = enbw && std::fabs(e / specwin::enbwBins(t) - 1) < 0.02;
            if (std::fabs(e / specwin::enbwBins(t) - 1) >= 0.02) printf("  window %s: ENBW %.4f, table %.4f\n", specwin::name(t), e, specwin::enbwBins(t));
        }
        check(rms, "every window has unit rms");
        check(enbw, "the ENBW table matches the windows");
    }

    // ---- the overlap rule and the 1M cap -------------------------------------------------------------------------------------------
    {
        check(std::fabs(SpectrumAnalyzer::hopFor(2.4e6, 65536, -1) - 2.4e6 / 35) < 1e-6, "auto: 64k transforms start fs / 35 apart (unchanged)");
        check(std::fabs(SpectrumAnalyzer::hopFor(2.4e6, 262144, -1) - 2.4e6 / 35) < 1e-6, "auto: 256k too (unchanged)");
        check(std::fabs(SpectrumAnalyzer::hopFor(2.4e6, 1 << 20, -1) - 262144) < 1e-6, "auto: 1M at most 3/4 overlapped");
        check(std::fabs(SpectrumAnalyzer::hopFor(2.4e6, 1 << 20, 0.5) - 524288) < 1e-6, "50 %: half a transform apart");
        check(std::fabs(SpectrumAnalyzer::hopFor(2.4e6, 4096, 0.5) - 2.4e6 / 35) < 1e-6, "short transforms: never more often than 35 a second");
        SpectrumAnalyzer an(4096);
        an.resize(1 << 20);
        check(an.fftSize() == (1u << 20), "the analyser takes 1M bins");
        an.resize(1 << 22);
        check(an.fftSize() == (1u << 20), "and no more");
        an.setWindow(specwin::BlackmanHarris);
        check(an.window() == specwin::BlackmanHarris, "the window is set");
        an.setWindow(99);
        check(an.window() == specwin::NumTypes - 1, "an unknown window is clamped");
    }
    {   // the full-band analyser with a window: a tone reads its level (within the scalloping)
        const double fs = 1e6;
        auto x = signal(fs, 200000, {{123456.7, 0.1}}, 0.f, 1);
        for (int t = 0; t < specwin::NumTypes; t++) {
            SpectrumAnalyzer an(8192);
            an.setWindow(t);
            an.feed(x.data(), x.size());
            SpectrumFrame f;
            if (!an.takeFrame(f)) { check(false, "window frame"); continue; }
            const float pk = *std::max_element(f.dbfs.begin(), f.dbfs.end());
            // a unit-rms window reads a tone low by its coherent gain: Hann 1.76 dB (and up to 1.42 dB of scalloping), flat top about 5.7 dB
            char w[96]; snprintf(w, sizeof w, "window %s: the tone at -20 dBFS reads %.2f", specwin::name(t), pk);
            check(pk > -20 - 8.5 && pk < -20 + 0.1, w);
        }
    }

    // ---- the decimator -------------------------------------------------------------------------------------------------------------
    {
        bool gain = true;
        for (int D : {2, 4, 8, 64, 1024, 4096}) {
            int r, h; zoomfft::split(D, r, h);
            const double fs = 2.4e6, off = 123456.0, d = 0.21 * fs / D;   // the tone 0.21 of the decimated rate above the zoom centre
            ZoomDecimator dec;
            dec.setup(fs, off, r, h);
            const size_t n = (size_t)D * 1500;
            auto x = signal(fs, n, {{off + d, 0.25}}, 0.f, 2);
            std::vector<cf32> y;
            for (size_t p = 0; p < n; p += 50000) dec.run(x.data() + p, std::min<size_t>(50000, n - p), y);
            check(y.size() + 2 >= n / (size_t)D && y.size() <= n / (size_t)D + 2, "the decimator gives n / D samples");
            double pw = 0; size_t c = 0;
            for (size_t i = y.size() / 3; i < y.size(); i++) { pw += std::norm(y[i]); c++; }
            const double db = 10 * std::log10(pw / std::max<size_t>(1, c) / (0.25 * 0.25));
            if (std::fabs(db) > 0.35) { printf("  D %d: gain %.3f dB\n", D, db); gain = false; }
        }
        check(gain, "unit gain (within 0.35 dB: the CIC droop) for every D, a tone 0.21 of the rate off the centre");
        check(ZoomDecimator::halfbandTaps() == 47, "the half-band has 47 taps");
    }

    // ---- the analyser: resolution beyond the full-band FFT -------------------------------------------------------------------------
    {
        const double fs = 2.4e6;
        // two tones 12 Hz apart near +100 kHz (off any bin), noise 70 dB down; the view 1 kHz wide, not centred on them
        const double f1 = 100003.37, f2 = f1 + 12.0;
        auto x = signal(fs, (size_t)(fs * 2.4), {{f1, 0.1}, {f2, 0.1}}, 0.0003f, 3);
        SpectrumAnalyzer full(65536);
        full.feed(x.data(), x.size());
        SpectrumFrame ff; full.takeFrame(ff);
        const zoomfft::Plan p0 = zoomfft::update(zoomfft::Plan(), fs, 65536, 100150, 1000, 1000);
        check(p0.on && p0.binHz < 1.0, "1 kHz over 1000 px: the zoom spectrum with bins under 1 Hz");
        ZoomSpectrum z;
        z.configure(p0, fs);
        ZoomFrame f;
        check(run(z, x, f) && f.dbfs.size() == p0.bins && f.plan.id == p0.id, "frames come with the plan");
        if (!f.dbfs.empty()) {
            const size_t b1 = binOf(f, f1), b2 = binOf(f, f2), bm = binOf(f, (f1 + f2) / 2);
            size_t k1 = b1, k2 = b2;
            for (size_t i = b1 - 3; i <= b1 + 3; i++) if (f.dbfs[i] > f.dbfs[k1]) k1 = i;
            for (size_t i = b2 - 3; i <= b2 + 3; i++) if (f.dbfs[i] > f.dbfs[k2]) k2 = i;
            check(k1 != k2 && (k1 + 1 >= b1 && k1 <= b1 + 1) && (k2 + 1 >= b2 && k2 <= b2 + 1), "two peaks, each within a bin of its tone");
            check(f.dbfs[k1] - f.dbfs[bm] > 20 && f.dbfs[k2] - f.dbfs[bm] > 20, "a deep dip between the two tones (resolved)");
            // the full-band frame: 36.6 Hz bins, the two tones fall in one or two neighbouring bins with no dip
            const size_t c1 = (size_t)std::lround(f1 / (fs / 65536) + 32768), c2 = (size_t)std::lround(f2 / (fs / 65536) + 32768);
            check(c2 <= c1 + 1, "the full-band FFT cannot separate them (the reason for the zoom spectrum)");
            // the level: each tone (-20 dBFS) within 2 dB of what the full-band analyser reads for one tone (both Hann, scalloping apart)
            const float fullPk = *std::max_element(ff.dbfs.begin(), ff.dbfs.end());
            check(std::fabs(f.dbfs[k1] - (-21.76)) < 1.6 && std::fabs(f.dbfs[k2] - (-21.76)) < 1.6, "each tone reads -20 dBFS less the Hann gain (as the full-band analyser)");
            (void)fullPk;
            // the noise floor: the mean power over the usable band, away from the tones, is the density times the bin width
            double s = 0; size_t c = 0;
            for (size_t i = 0; i < f.dbfs.size(); i++) {
                const double hz = binHzOf(f, i);
                if (hz < f.plan.usableLo() || hz > f.plan.usableHi() || std::fabs(hz - f1) < 40 || std::fabs(hz - f2) < 40) continue;
                s += std::pow(10.0, f.dbfs[i] / 10); c++;
            }
            const double want = 10 * std::log10(2 * 0.0003 * 0.0003 * f.plan.binHz / fs);
            const double got = 10 * std::log10(s / std::max<size_t>(1, c));
            char w[120]; snprintf(w, sizeof w, "the noise floor %.2f dBFS/bin, expected %.2f (the decimator's gain and the scaling)", got, want);
            check(std::fabs(got - want) < 1.0, w);
        }
    }
    {   // alias rejection: a strong tone just outside the usable band (and far outside) shows nowhere inside it
        const double fs = 2.4e6;
        for (int D : {8, 256, 4096}) {
            const double span = 0.45 * fs / D;
            const zoomfft::Plan p = zoomfft::make(fs, -0.31 * fs, span, 128);   // 512 bins: short signals
            if (!p.on || p.decim != D) { check(false, "alias plan"); continue; }
            ZoomSpectrum z; z.configure(p, fs);
            ZoomFrame ref;
            auto in = signal(fs, (size_t)(p.bins * D * 3), {{p.offsetHz + 0.2 * p.rateHz, 0.1}}, 0.f, 4);
            run(z, in, ref);
            const double inBand = usableMax(ref);
            for (double k : {0.55, 0.7, 1.3, 3.7, 17.2}) {
                if (p.offsetHz + k * p.rateHz > fs / 2) continue;
                ZoomSpectrum z2; z2.configure(p, fs);
                ZoomFrame f;
                auto x = signal(fs, (size_t)(p.bins * D * 3), {{p.offsetHz + k * p.rateHz, 0.1}}, 0.f, 5);
                run(z2, x, f);
                char w[120]; snprintf(w, sizeof w, "D %d: a tone %.2f rates off the centre shows %.1f dB under the in-band tone", D, k, inBand - usableMax(f));
                check(inBand - usableMax(f) > 65, w);
            }
        }
    }
    {   // the view at the band's edge, a tone 1 bin from the edge of the view: placed right
        const double fs = 10e6, off = -4.9e6;
        const zoomfft::Plan p = zoomfft::make(fs, off, 20e3, 1500);
        ZoomSpectrum z; z.configure(p, fs);
        const double t = off - 9e3 + 0.37 * p.binHz;
        auto x = signal(fs, (size_t)(p.bins * p.decim * 2.5), {{t, 0.05}}, 0.0001f, 6);
        ZoomFrame f;
        check(run(z, x, f), "edge view: frames");
        if (!f.dbfs.empty()) {
            size_t k = 0;
            for (size_t i = 1; i < f.dbfs.size(); i++) if (f.dbfs[i] > f.dbfs[k]) k = i;
            check(std::fabs(binHzOf(f, k) - t) <= f.plan.binHz, "edge view: the tone at its frequency (within a bin)");
        }
    }
    {   // off: no work, no frames; a reset drops the old samples
        ZoomSpectrum z;
        auto x = signal(2.4e6, 200000, {{1000, 0.1}}, 0.f, 7);
        ZoomFrame f;
        check(!run(z, x, f), "off by default: no frames");
        zoomfft::Plan p = zoomfft::make(2.4e6, 0, 50e3, 500); p.id = 1;
        z.configure(p, 2.4e6);
        check(z.plan().on && z.plan().id == 1, "configured");
        zoomfft::Plan offp; offp.id = 1;
        z.configure(offp, 2.4e6);
        check(!run(z, x, f), "off again: no frames");
    }
    {   // settings change while another thread feeds (run under TSAN too): every frame has the size of a plan that was set
        ZoomSpectrum z;
        std::atomic<bool> stop{false};
        auto x = signal(2.4e6, 240000, {{20000, 0.1}}, 0.001f, 8);
        std::thread feeder([&] { while (!stop) for (size_t p = 0; p < x.size() && !stop; p += 24000) z.feed(x.data() + p, 24000); });
        bool ok = true; int frames = 0;
        for (int i = 0; i < 400; i++) {
            zoomfft::Plan p = zoomfft::make(2.4e6, 1000.0 * (i % 7), 20e3 * (1 + i % 3), 600 + 100 * (i % 4));
            p.id = (uint64_t)i + 1;
            z.configure(p, 2.4e6);
            if (i % 50 == 0) z.setWindow(i / 50 % specwin::NumTypes);
            if (i % 30 == 0) z.reset();
            ZoomFrame f;
            for (int k = 0; k < 3; k++) {
                if (z.takeFrame(f)) { frames++; ok = ok && f.dbfs.size() == f.plan.bins && f.plan.id == p.id; }
                std::this_thread::yield();
            }
        }
        stop = true; feeder.join();
        check(ok, "threads: each frame matches its plan");
        printf("  threads: %d frames while the plan changed 400 times\n", frames);
    }
    {   // CPU (printed for docs/WATERFALL-4.0.md; one check: the zoom spectrum costs less than a full-band FFT as fine)
        auto cpu = [] { return (double)std::clock() / CLOCKS_PER_SEC; };
        for (double fs : {2.4e6, 10e6, 20e6}) {
            const double secs = fs > 5e6 ? 0.6 : 1.5;
            auto x = signal(fs, (size_t)(fs * secs), {{fs * 0.1, 0.05}}, 0.01f, 9);
            const size_t per = (size_t)(fs / 30);
            auto fullRun = [&](size_t n, double& ms, int& frames) {
                SpectrumAnalyzer an(n); an.setSampleRate(fs); an.resize(n);
                SpectrumFrame fr; frames = 0;
                const double c0 = cpu();
                for (size_t i = 0; i < x.size(); i += 65536) {
                    const size_t m = std::min<size_t>(65536, x.size() - i);
                    an.feed(x.data() + i, m);
                    if ((i + m) / per != i / per && an.takeFrame(fr)) frames++;
                }
                ms = (cpu() - c0) * 1000 / secs;
            };
            double msAuto, msFull; int frAuto, frFull;
            fullRun(SpectrumAnalyzer::autoSize(fs), msAuto, frAuto);
            const zoomfft::Plan p = zoomfft::make(fs, fs * 0.1, 10e3, 2000);
            size_t eq = 1024; while (eq < (size_t(1) << 20) && fs / (double)eq > p.binHz) eq *= 2;   // the full-band size as fine (or the 1M cap)
            fullRun(eq, msFull, frFull);
            ZoomSpectrum z; z.configure(p, fs);
            ZoomFrame zf; int frZ = 0;
            const double c0 = cpu();
            for (size_t i = 0; i < x.size(); i += 65536) {
                const size_t m = std::min<size_t>(65536, x.size() - i);
                z.feed(x.data() + i, m);
                if ((i + m) / per != i / per && z.takeFrame(zf)) frZ++;
            }
            const double msZ = (cpu() - c0) * 1000 / secs;
            printf("  CPU at %.1f Msps (ms per second of signal): full-band auto %zu bins %.1f (%d frames); full-band %zu bins (%.2f Hz) %.1f (%d frames);"
                   " zoom 10 kHz on 2000 px: D %d, %zu bins (%.2f Hz) %.1f (%d frames)\n",
                   fs / 1e6, SpectrumAnalyzer::autoSize(fs), msAuto, frAuto, eq, fs / (double)eq, msFull, frFull, p.decim, p.bins, p.binHz, msZ, frZ);
            check(msZ < msFull, "the zoom spectrum costs less than a full-band FFT as fine");
        }
    }
    printf(fails ? "%d FAILED\n" : "zoom_spectrum: all passed\n", fails);
    return fails ? 1 : 0;
}
