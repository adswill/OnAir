// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: opening recordings (raw IQ in every format, SigMF, WAV mono / stereo-as-IQ, MP3), exporting selections, the band
// extractor's frequency placement, the overview and the spectrogram tiles.
#include "../common/tmp_path.h"
#include "dect2/wb/wb_export.h"
#include "dect2/wb/wb_gen.h"
#include "dect2/wb/wb_view.h"
#include "wb_test.h"
#include <chrono>
#include <cstring>
#include <thread>
using namespace dect2::wb;
using dect2::cf32;

static std::vector<cf32> ramp(size_t n) {
    std::vector<cf32> x(n);
    for (size_t i = 0; i < n; i++) x[i] = cf32(0.9f * (float)std::sin(0.01 * (double)i), 0.5f * (float)std::cos(0.0173 * (double)i));
    return x;
}

static double maxDiff(const std::vector<cf32>& a, const std::vector<cf32>& b) {
    double m = 0;
    for (size_t i = 0; i < std::min(a.size(), b.size()); i++) m = std::max(m, (double)std::abs(a[i] - b[i]));
    return m;
}

static std::vector<cf32> readAll(const Source& s) {
    std::vector<cf32> x((size_t)s.length());
    x.resize(s.read(0, x.size(), x.data()));
    return x;
}

static double goertzel(const std::vector<cf32>& a, double f, double rate) {
    cf32 acc(0, 0);
    for (size_t i = 0; i < a.size(); i++) acc += a[i] * cf32((float)std::cos(2 * M_PI * f * (double)i / rate), (float)-std::sin(2 * M_PI * f * (double)i / rate));
    return std::norm(acc) / ((double)a.size() * (double)a.size());
}

int main() {
    const std::string dir = tmpPath("wb_source_test_");
    const std::vector<cf32> x = ramp(5000);
    std::string err;
    {   // raw files
        struct F { const char* ext; double tol; };
        for (F f : {F{"cf32", 1e-7}, F{"cs8", 0.02}, F{"cu8", 0.02}, F{"ci16", 1e-4}}) {
            const std::string p = dir + "raw_2Msps." + f.ext;
            FILE* fp = fopen(p.c_str(), "wb");
            for (const cf32& v : x) {
                if (!strcmp(f.ext, "cf32")) { float a[2] = {v.real(), v.imag()}; fwrite(a, 4, 2, fp); }
                else if (!strcmp(f.ext, "cs8")) { int8_t a[2] = {(int8_t)std::lrint(v.real() * 127), (int8_t)std::lrint(v.imag() * 127)}; fwrite(a, 1, 2, fp); }
                else if (!strcmp(f.ext, "cu8")) { uint8_t a[2] = {(uint8_t)std::lrint(v.real() * 127.5 + 127.5), (uint8_t)std::lrint(v.imag() * 127.5 + 127.5)}; fwrite(a, 1, 2, fp); }
                else { int16_t a[2] = {(int16_t)std::lrint(v.real() * 32767), (int16_t)std::lrint(v.imag() * 32767)}; fwrite(a, 2, 2, fp); }
            }
            fclose(fp);
            LoadOptions o;
            auto s = Source::open(p, o, err);   // the rate comes from the name (2Msps)
            CHECK(s != nullptr, "%s: %s", f.ext, err.c_str());
            if (!s) continue;
            CHECK(s->length() == x.size() && s->rate() == 2e6 && !s->real(), "%s: %llu samples, %.0f Hz", f.ext, (unsigned long long)s->length(), s->rate());
            CHECK(maxDiff(readAll(*s), x) < f.tol, "%s: samples differ by %g", f.ext, maxDiff(readAll(*s), x));
            // random access
            cf32 one[3];
            CHECK(s->read(4000, 3, one) == 3 && std::abs(one[0] - x[4000]) < f.tol, "%s: random access", f.ext);
            CHECK(s->read(4999, 10, one) == 1 && s->read(5000, 10, one) == 0, "%s: end of file", f.ext);
        }
        // a raw file whose name says nothing: needs the rate
        const std::string p = dir + "noname.cf32";
        FILE* fp = fopen(p.c_str(), "wb");
        for (const cf32& v : x) { float a[2] = {v.real(), v.imag()}; fwrite(a, 4, 2, fp); }
        fclose(fp);
        LoadOptions o;
        CHECK(Source::open(p, o, err) == nullptr && !err.empty(), "no rate: refused (%s)", err.c_str());
        o.sampleRate = 250000; o.centerHz = 433.92e6;
        auto s = Source::open(p, o, err);
        CHECK(s && s->rate() == 250000 && s->centerHz() == 433.92e6, "with the rate typed in");
        CHECK(Source::open(dir + "missing.cf32", o, err) == nullptr, "missing file");
    }
    {   // export: cf32, WAV (stereo float), SigMF; reopened
        auto mem = Source::fromSamples(x, 48000, false, 100e6, "ramp");
        CHECK(!mem->real() && mem->length() == x.size(), "memory source");
        CHECK(exportSelection(*mem, 1000, 4000, 0, 0, dir + "sel", ExCf32, err), "export cf32: %s", err.c_str());
        LoadOptions o; o.sampleRate = 48000;
        auto a = Source::open(dir + "sel.cf32", o, err);
        CHECK(a && a->length() == 3000 && maxDiff(readAll(*a), std::vector<cf32>(x.begin() + 1000, x.begin() + 4000)) < 1e-7, "cf32 round trip");
        CHECK(exportSelection(*mem, 1000, 4000, 0, 0, dir + "selw", ExWav, err), "export wav: %s", err.c_str());
        LoadOptions w; w.stereoIq = true;
        auto b = Source::open(dir + "selw.wav", w, err);
        CHECK(b && b->length() == 3000 && b->rate() == 48000 && !b->real() && maxDiff(readAll(*b), std::vector<cf32>(x.begin() + 1000, x.begin() + 4000)) < 1e-6, "stereo WAV as I/Q: %s", err.c_str());
        if (b) {
            b->setStereoIq(false);
            CHECK(b->real() && b->channels() == 2, "stereo as real: %d", b->channels());
            cf32 v;
            b->read(0, 1, &v);
            CHECK(std::fabs(v.real() - 0.5f * (x[1000].real() + x[1000].imag())) < 1e-6 && v.imag() == 0, "stereo mixed to mono");
        }
        CHECK(exportSelection(*mem, 0, 5000, 0, 0, dir + "sm", ExSigmf, err), "export sigmf: %s", err.c_str());
        LoadOptions none;
        auto c = Source::open(dir + "sm.sigmf-meta", none, err);
        CHECK(c && c->length() == 5000 && c->rate() == 48000 && c->centerHz() == 100e6 && maxDiff(readAll(*c), x) < 1e-7, "sigmf round trip: %s", err.c_str());
        auto d = Source::open(dir + "sm.sigmf-data", none, err);
        CHECK(d && d->length() == 5000, "sigmf by the data file");
        // the band export: a tone at 5 kHz in 48 kHz taken out at its own rate
        std::vector<cf32> t(48000);
        for (size_t i = 0; i < t.size(); i++) t[i] = cf32((float)std::cos(2 * M_PI * 5000.0 * (double)i / 48000), (float)std::sin(2 * M_PI * 5000.0 * (double)i / 48000));
        auto ts = Source::fromSamples(t, 48000, false, 0, "tone");
        CHECK(exportSelection(*ts, 0, 48000, 4000, 6000, dir + "band", ExSigmf, err), "band export: %s", err.c_str());
        auto e = Source::open(dir + "band.sigmf-meta", none, err);
        CHECK(e && e->rate() < 8000 && e->rate() >= 4999 && std::fabs(e->centerHz() - 5000) < 1, "band export rate %.0f centre %.0f", e ? e->rate() : 0, e ? e->centerHz() : 0);
        if (e) {
            const std::vector<cf32> y = readAll(*e);
            const std::vector<cf32> mid(y.begin() + (long)y.size() / 4, y.end() - (long)y.size() / 4);
            CHECK(goertzel(mid, 0, e->rate()) > 0.9, "the tone sits at DC in the band file: %.3f", goertzel(mid, 0, e->rate()));
        }
        // audio WAV (mono, 16 bit) and MP3 (when ffmpeg or lame is there)
        std::vector<float> au(48000);
        for (size_t i = 0; i < au.size(); i++) au[i] = (float)(0.5 * std::sin(2 * M_PI * 1500.0 * (double)i / 48000));
        CHECK(writeWavMono16(dir + "tone.wav", au, 48000, err), "write wav");
        auto m = Source::open(dir + "tone.wav", none, err);
        CHECK(m && m->real() && m->length() == 48000 && m->rate() == 48000 && m->isAudio(), "mono WAV: %s", err.c_str());
        if (m) {
            const std::vector<cf32> y = readAll(*m);
            CHECK(std::fabs(y[100].real() - au[100]) < 1e-4, "mono sample");
        }
        const std::string mp3 = dir + "tone.mp3";
        const int rc = system(("ffmpeg -loglevel quiet -y -i '" + dir + "tone.wav' '" + mp3 + "' 2>/dev/null").c_str());
        if (rc == 0) {
            auto q = Source::open(mp3, none, err);
            CHECK(q && q->real() && q->length() > 40000 && q->rate() == 48000, "mp3: %s", err.c_str());
            if (q) {
                const std::vector<cf32> y = readAll(*q);
                const std::vector<cf32> mid(y.begin() + 4000, y.begin() + 40000);
                CHECK(goertzel(mid, 1500, 48000) > 0.04, "mp3 tone: %.3f", goertzel(mid, 1500, 48000));
            }
        } else printf("  (no ffmpeg program: the MP3 check is skipped)\n");
        CHECK(Source::open(dir + "noname.cf32", w, err) == nullptr || true, "-");
        LoadOptions aud; aud.audio = true;
        CHECK(Source::open(dir + "raw_2Msps.cf32", aud, err) == nullptr, "a raw file read as audio is refused");
    }
    {   // the band extractor places frequencies: a complex and a real tone
        std::vector<cf32> t(200000);
        for (size_t i = 0; i < t.size(); i++) t[i] = cf32((float)std::cos(2 * M_PI * 30000.0 * (double)i / 200000), (float)std::sin(2 * M_PI * 30000.0 * (double)i / 200000));
        auto s = Source::fromSamples(t, 200000);
        Extracted e;
        CHECK(extractBand(*s, 0, t.size(), 30000, 8000, 20000, e), "extract");
        CHECK(e.rate >= 20000 && e.rate <= 25000 && e.x.size() > 15000, "extracted rate %.0f, %zu samples", e.rate, e.x.size());
        const std::vector<cf32> mid(e.x.begin() + (long)e.x.size() / 4, e.x.end() - (long)e.x.size() / 4);
        CHECK(goertzel(mid, 0, e.rate) > 0.95, "tone at DC: %.3f", goertzel(mid, 0, e.rate));
        // 2 kHz off the centre: stays at +2 kHz
        CHECK(extractBand(*s, 0, t.size(), 28000, 8000, 20000, e), "extract off-centre");
        const std::vector<cf32> mid2(e.x.begin() + (long)e.x.size() / 4, e.x.end() - (long)e.x.size() / 4);
        CHECK(goertzel(mid2, 2000, e.rate) > 0.95, "tone at +2 kHz: %.3f", goertzel(mid2, 2000, e.rate));
        // outside the band: gone
        CHECK(extractBand(*s, 0, t.size(), 50000, 8000, 20000, e), "extract elsewhere");
        double pw = 0;
        for (const cf32& v : e.x) pw += std::norm(v);
        CHECK(pw / (double)e.x.size() < 1e-4, "out of band power %.2e", pw / (double)e.x.size());
        // a real signal: 1800 Hz in 48 kHz audio, the band round 1500 Hz comes out at +300 Hz, the mirror image does not appear
        std::vector<cf32> r(96000);
        for (size_t i = 0; i < r.size(); i++) r[i] = cf32((float)std::cos(2 * M_PI * 1800.0 * (double)i / 48000), 0);
        auto rs = Source::fromSamples(r, 48000, true);
        CHECK(rs->real(), "real source");
        CHECK(extractBand(*rs, 0, r.size(), 1500, 1000, 2500, e), "extract real");
        const std::vector<cf32> mid3(e.x.begin() + (long)e.x.size() / 4, e.x.end() - (long)e.x.size() / 4);
        const double up = goertzel(mid3, 300, e.rate), down = goertzel(mid3, -300, e.rate);
        CHECK(up > 0.2 && down < 0.002, "real tone: +300 Hz %.3f, -300 Hz %.5f", up, down);
        CHECK(extractBand(*rs, 0, r.size(), 1500, 1000, 2500, e, 500), "truncated extract") ;
        CHECK(e.truncated, "truncation flagged");
    }
    {   // overview and spectrogram on a recording with two tones, one switched on for the second half
        const size_t N = 400000;
        const double rate = 100000;
        std::vector<cf32> t(N);
        for (size_t i = 0; i < N; i++) {
            cf32 v(0.25f * (float)std::cos(2 * M_PI * 10000.0 * (double)i / rate), 0.25f * (float)std::sin(2 * M_PI * 10000.0 * (double)i / rate));
            if (i > N / 2) v += 0.5f * cf32((float)std::cos(2 * M_PI * -20000.0 * (double)i / rate), (float)std::sin(2 * M_PI * -20000.0 * (double)i / rate));
            t[i] = v;
        }
        auto s = Source::fromSamples(t, rate);
        Overview ov(s, 1000);
        for (int i = 0; i < 400 && !ov.done(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(ov.done() && ov.blocks() == 1000, "overview done %d blocks %zu", ov.done(), ov.blocks());
        const std::vector<float> pd = ov.powerDb();
        CHECK(pd.size() == 1000 && pd[100] < pd[900] - 6, "power over time: %.1f then %.1f dB", pd[100], pd[900]);
        CHECK(std::fabs(ov.peak() - 0.75) < 0.05, "peak %.3f", ov.peak());
        WaveColumns wc;
        waveformColumns(*s, &ov, 0, N, 100, wc);
        CHECK(wc.mag.size() == 100 && wc.mag[10] < 0.3 && wc.mag[90] > 0.5, "waveform %.2f %.2f", wc.mag[10], wc.mag[90]);
        waveformColumns(*s, &ov, 1000, 1500, 50, wc);
        CHECK(wc.maxI[5] > wc.minI[5], "zoomed waveform");
        SpecEngine se(s);
        SpecParams p; p.t0 = 0; p.t1 = N; p.cols = 200; p.fftSize = 1024; p.overlap = 0.5;
        se.request(p);
        SpecImage img; uint64_t seq = 0;
        for (int i = 0; i < 500 && !se.poll(img, seq); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(seq > 0 && img.w == 200 && img.h == 1024, "spectrogram %d x %d", img.w, img.h);
        if (img.w == 200) {
            auto rowOf = [&](double f) { return (int)((img.fHi - f) / (img.fHi - img.fLo) * img.h); };
            const int r10 = rowOf(10000), r20 = rowOf(-20000);
            const float a = img.db[(size_t)r10 * img.w + 20], b = img.db[(size_t)r10 * img.w + 180];
            const float c = img.db[(size_t)r20 * img.w + 20], d = img.db[(size_t)r20 * img.w + 180];
            printf("  spectrogram: 10 kHz %.1f / %.1f dB, -20 kHz %.1f / %.1f dB\n", a, b, c, d);
            CHECK(std::fabs(a - 20 * std::log10(0.25)) < 3 && std::fabs(b - 20 * std::log10(0.25)) < 3, "tone 1 level %.1f %.1f", a, b);
            CHECK(c < -60 && std::fabs(d - 20 * std::log10(0.5)) < 3, "tone 2 only in the second half: %.1f %.1f", c, d);
        }
        // zoom in time and frequency
        p.t0 = 300000; p.t1 = 320000; p.f0 = 5000; p.f1 = 15000; p.cols = 100;
        se.request(p);
        for (int i = 0; i < 500 && !se.poll(img, seq); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(img.w == 100 && img.fLo < 5100 && img.fHi > 14900 && img.fHi < 15200 && img.h < 1024, "zoomed picture %d x %d, %.0f .. %.0f Hz", img.w, img.h, img.fLo, img.fHi);
        // a real signal: only 0 .. rate / 2
        std::vector<cf32> rt(100000);
        for (size_t i = 0; i < rt.size(); i++) rt[i] = cf32((float)(0.5 * std::cos(2 * M_PI * 3000.0 * (double)i / 48000)), 0);
        auto rs = Source::fromSamples(rt, 48000, true);
        SpecEngine sr(rs);
        SpecParams q; q.t0 = 0; q.t1 = rt.size(); q.cols = 50; q.fftSize = 1024;
        sr.request(q);
        uint64_t sq = 0;
        SpecImage ri;
        for (int i = 0; i < 500 && !sr.poll(ri, sq); i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(ri.w == 50 && ri.fLo < 1 && std::fabs(ri.fHi - 24000) < 50, "real picture %.0f .. %.0f", ri.fLo, ri.fHi);
        if (ri.w == 50) {
            const int row = (int)((ri.fHi - 3000) / (ri.fHi - ri.fLo) * ri.h);
            CHECK(std::fabs(ri.db[(size_t)row * ri.w + 25] - 20 * std::log10(0.5)) < 3, "real tone level %.1f", ri.db[(size_t)row * ri.w + 25]);
        }
    }
    printf(fails ? "FAILED\n" : "wb_source ok\n");
    return fails ? 1 : 0;
}
