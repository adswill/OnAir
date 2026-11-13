// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Sound card (audio device) input through the whole engine, as the app runs it, with a WAV file standing in for the card (issue #137: weather fax
// from a virtual audio cable neither decoded nor showed on the spectrum). Never opens a real device.
//  - the audio of a synthetic weather fax at several sample rates, channel layouts, levels, DC offsets, tuning errors and clock errors goes in as the
//    card; the spectrum must show it at the right bins (centred, and with the audio-range view), and the fax picture must come out as the test chart;
//  - the mode's tuneOffsetHz (a radio is tuned that far off its dial) must not be applied to a sound card;
//  - the centre finder, the clipping flag and the L = R hint.
#include "dect2/engine/engine.h"
#include "../common/ci_slack.h"
#include "../common/tmp_path.h"
#include "dect2/engine/engine_testkit.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/dsp/resampler.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include <set>
#include "dect2/hfdig/hfdig_rx.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/marine/marine_rx.h"
#include "dect2/marine/marine_fax.h"
#include "dect2/source/audio_if.h"
#include "../data/marine/fax/fax_util.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
using namespace dect2::enginetest;

#include "../common/test_util.h"

static std::string tmp(const std::string& n) { return tmpPath(n); }

// 16-bit stereo PCM WAV
static bool writeWav(const std::string& path, int rate, const std::vector<float>& l, const std::vector<float>& r) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t n = l.size();
    std::vector<int16_t> d(n * 2);
    for (size_t i = 0; i < n; i++) {
        d[2 * i] = (int16_t)std::lround(std::max(-1.f, std::min(1.f, l[i])) * 32767.f);
        d[2 * i + 1] = (int16_t)std::lround(std::max(-1.f, std::min(1.f, r[i])) * 32767.f);
    }
    const uint32_t bytes = (uint32_t)(n * 4), riff = 36 + bytes, fl = 16, sr = (uint32_t)rate, br = sr * 4;
    const uint16_t pcm = 1, ch = 2, al = 4, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&al, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f); fwrite(d.data(), 2, d.size(), f);
    fclose(f);
    return true;
}

enum Layout { Dup, LeftOnly, RightOnly };

struct Case {
    const char* name;
    int rate = 48000;
    Layout layout = Dup;
    double gain = 0.8;        // fax amplitude about 0.5 x gain... as a fraction of full scale
    double dc = 0;
    double mistuneHz = 0;     // the whole signal shifted (a USB tuning error)
    double ppm = 0;           // the card's clock error
    int audioChan = 2;        // 0 left, 1 right, 2 mix
    double ifHz = 0;          // the card's IF centre setting
    bool afView = false;      // the spectrum shows the audio range 0..Nyquist
    int iqMode = 3;           // 3 Real (mono), 1 USB audio
    double snrDb = 1000;
    int lines = 60;
};

struct Result {
    bool started = false;
    double specPeakHz = 0;    // where the strongest spectrum bin is, in Hz from the display centre
    double specBandDb = 0;    // mean level in the fax band less the median
    int lines = 0, state = 0, lpm = 0, ioc = 0;
    double corr = 0, drift = 0;
    bool detected = false; double centre = 0, width = 0;
    FaxImage img;
};

static Result run(const Case& c, double secs) {
    Result res;
    const SubMode* sm = subModeById("wefax");
    if (!sm) { CHECK(false, "no wefax entry"); return res; }
    faxt::Opts o;
    o.rate = c.rate;
    o.lines = c.lines;
    o.phasingS = 10;
    o.seed = 5;
    o.gain = c.gain;
    o.dc = 0;
    o.mistuneHz = c.mistuneHz;
    o.snrDb = c.snrDb;
    std::vector<float> a = faxt::makeAudio(o);
    for (float& v : a) v += (float)c.dc;
    // the clock error: the file says a slightly different rate, the card is asked for the nominal one and resamples
    const int fileRate = (int)std::lround(c.rate * (1.0 + c.ppm * 1e-6));
    std::vector<float> z(a.size(), 0.f);
    const std::string wav = tmp(std::string("audio_engine_") + c.name + ".wav");
    if (!writeWav(wav, fileRate, c.layout == RightOnly ? z : a, c.layout == LeftOnly ? z : a)) { CHECK(false, "%s: cannot write the WAV", c.name); return res; }

    Engine e;
    DeviceInfo dev;
    dev.kind = DeviceInfo::Audio;
    dev.serial = "file:" + wav;
    dev.name = "Sound card: test file";
    TuneSettings t;
    const ModeTuning* mt = modeTuning(sm->stdMode);
    t.centerHz = 7.88e6;
    t.bandwidthMhz = mt->bandwidthMhz;
    t.sampleRate = mt->sampleRate;
    t.basebandFilterHz = mt->basebandHz;
    t.radio["audio_iq"] = std::to_string(c.iqMode);
    t.radio["audio_chan"] = std::to_string(c.audioChan);
    t.radio["audio_if_hz"] = std::to_string(c.ifHz);
    t.radio["audio_rate"] = std::to_string(c.rate);
    t.radio["audio_pace"] = ciSlack() > 1 ? "8" : "25";   // x real time: a shared CI runner needs more time for the decoder
    e.setStandard(sm->stdMode);
    applySubMode(e, sm);
    e.marine().setFrequencyHz(0);
    AudioIfConfig cfg = audioIfConfigOf(t);
    e.setSpectrumShiftHz(c.afView ? -audioAfViewOffsetHz(cfg) : 0.0);
    FileOptions fo;
    if (!e.start(dev, t, fo)) { CHECK(false, "%s: engine start", c.name); return res; }
    res.started = true;
    const double sr = e.sampleRate();   // the card's rate interpolated up to a whole multiple that the fax receiver takes (250 kHz and up)
    CHECK(sr >= 250000 && std::fabs(std::fmod(sr, c.rate)) < 1e-6, "%s: the receivers run at %g for a card at %d", c.name, sr, c.rate);

    // the spectrum: average the frames over a stretch of the transmission (the phasing line and the picture)
    const double s0 = signalSecs(e);
    std::vector<double> avg;
    int frames = 0;
    SpectrumFrame sf;
    uint64_t sseq = 0;
    while (signalSecs(e) - s0 < secs) {
        if (e.latestSpectrum(sf, sseq) && signalSecs(e) - s0 > 20) {
            sseq = sf.seq;
            if (avg.size() != sf.dbfs.size()) avg.assign(sf.dbfs.size(), 0.0);
            for (size_t i = 0; i < avg.size(); i++) avg[i] += sf.dbfs[i];
            frames++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (frames > 0) {
        const int n = (int)avg.size();
        for (double& v : avg) v /= frames;
        const double binHz = sr / n;
        int pk = 0;
        for (int i = 0; i < n; i++) if (avg[(size_t)i] > avg[(size_t)pk]) pk = i;
        res.specPeakHz = (pk - n / 2) * binHz;
        std::vector<double> sorted = avg;
        std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
        const double med = sorted[(size_t)n / 2];
        // the fax band 1500..2300 Hz (plus the mistuning), where the display puts it
        const double view = c.afView ? audioAfViewOffsetHz(cfg) : 0.0;
        double sum = 0; int cnt = 0;
        for (int i = 0; i < n; i++) {
            const double f = (i - n / 2) * binHz + view - c.ifHz + 0;   // the actual frequency from the IF centre
            if (f >= 1550 + c.mistuneHz && f <= 2250 + c.mistuneHz) { sum += avg[(size_t)i]; cnt++; }
        }
        res.specBandDb = cnt ? sum / cnt - med : 0;
        res.specPeakHz += view;   // as the decoders see it
        {   // the IF centre finder on this spectrum: the fax occupies about 1500..2300 Hz, centre 1900
            double cc = 0, ww = 0;
            res.detected = audioDetectCentre(avg.size() ? std::vector<float>(avg.begin(), avg.end()).data() : nullptr, n, sr, view, cfg, cc, ww);
            res.centre = cc; res.width = ww;
        }
    }
    RxTelemetry rx;
    uint64_t seq = 0;
    waitFor(5.0, [&] { return e.latestRx(rx, seq); });
    e.latestRx(rx, 0);
    uint64_t is = 0;
    e.marine().latestImage(res.img, is);
    res.lines = rx.marine.fax.lines;
    res.state = rx.marine.fax.state;
    res.lpm = rx.marine.fax.lpm;
    res.ioc = rx.marine.fax.ioc;
    const int w = faxImageWidth(576);
    const FaxImage ref = faxTestChart(w, c.lines, 5);
    FaxImage cut = ref;
    cut.height = std::max(0, std::min(c.lines, res.img.height - 2));
    cut.pix.resize((size_t)cut.height * w);
    const faxt::Quality q = faxt::compare(res.img, cut, 1);
    res.corr = q.meanCorr;
    res.drift = q.drift;
    e.stop();
    remove(wav.c_str());
    return res;
}

static void check(const Case& c) {
    const Result r = run(c, 45 + 60.0 / 2 * 0);   // ~ the start tone, the phasing, the picture
    printf("%-22s rate %6d: spectrum peak %7.0f Hz, fax band %5.1f dB above the median, %d lines, lpm %d ioc %d, correlation %.3f drift %.2f\n", c.name, c.rate,
           r.specPeakHz, r.specBandDb, r.lines, r.lpm, r.ioc, r.corr, r.drift);
    if (!r.started) return;
    const double lo = 1450 + c.mistuneHz, hi = 2350 + c.mistuneHz;
    CHECK(r.detected && r.centre > lo + 50 && r.centre < hi, "%s: the centre finder says %.0f Hz (%.0f wide), not inside the fax band", c.name, r.centre, r.width);
    CHECK(r.specBandDb > 15, "%s: the fax is not on the spectrum (%.1f dB over the median)", c.name, r.specBandDb);
    CHECK(r.specPeakHz > lo && r.specPeakHz < hi, "%s: the strongest bin is at %.0f Hz, not in the fax band %.0f..%.0f", c.name, r.specPeakHz, lo, hi);
    CHECK(r.state >= 2 && r.lpm == 120 && r.ioc == 576, "%s: state %d lpm %d ioc %d", c.name, r.state, r.lpm, r.ioc);
    CHECK(r.lines >= c.lines - 12, "%s: only %d of %d lines", c.name, r.lines, c.lines);
    CHECK(r.corr > 0.85 && std::fabs(r.drift) < 2.5, "%s: the picture does not match the chart (correlation %.3f, drift %.2f)", c.name, r.corr, r.drift);
}

static void testTools() {
    // the centre finder on a spectrum with a 1500..2300 Hz block, as the display shows it
    AudioIfConfig cfg; cfg.mode = AudioIqMode::MonoReal; cfg.rate = 48000; cfg.ifHz = 0;
    const int n = 4096;
    const double view = audioAfViewOffsetHz(cfg);
    CHECK(std::fabs(view - 12000) < 1e-6, "audio-range view centre %g", view);
    std::vector<float> b((size_t)n, -90.f);
    for (int i = 0; i < n; i++) {
        const double f = (i - n / 2) * (48000.0 / n) + view;
        if (f >= 1500 && f <= 2300) b[(size_t)i] = -30.f;
        b[(size_t)i] += (float)((i * 7919) % 13) * 0.2f;
    }
    double c = 0, w = 0;
    CHECK(audioDetectCentre(b.data(), n, 48000, view, cfg, c, w), "centre finder found nothing");
    CHECK(std::fabs(c - 1900) < 30 && std::fabs(w - 800) < 60, "centre %.0f width %.0f (want 1900, 800)", c, w);
    {   // two tones (RTTY 2125 / 2295 Hz): the centre is halfway
        std::vector<float> fsk((size_t)n, -90.f);
        for (int i = 0; i < n; i++) {
            const double f = (i - n / 2) * (48000.0 / n) + view;
            if (std::fabs(f - 2125) < 25 || std::fabs(f - 2295) < 25) fsk[(size_t)i] = -35.f;
            fsk[(size_t)i] += (float)((i * 7919) % 13) * 0.2f;
        }
        CHECK(audioDetectCentre(fsk.data(), n, 48000, view, cfg, c, w) && std::fabs(c - 2210) < 30 && std::fabs(w - 220) < 60, "two-tone centre %.0f width %.0f (want 2210, 220)", c, w);
    }
    std::vector<float> flat((size_t)n, -90.f);
    CHECK(!audioDetectCentre(flat.data(), n, 48000, view, cfg, c, w), "a flat spectrum has no centre");
    // LSB / invert mirrors the answer
    cfg.invert = true;
    const double v2 = audioAfViewOffsetHz(cfg);
    CHECK(std::fabs(v2 + 12000) < 1e-6, "inverted audio-range view centre %g", v2);

    // clipping and the L = R hint through the processing stage
    AudioIfConfig k; k.mode = AudioIqMode::MonoReal; k.rate = 48000;
    AudioIfState st;
    std::vector<float> in(2 * 4800);
    std::vector<cf32> out(4800);
    for (int i = 0; i < 4800; i++) in[2 * i] = in[2 * i + 1] = 0.6f * (float)std::sin(0.3 * i);
    audioIfProcess(in.data(), 4800, k, out.data(), st);
    CHECK(!audioSourceClipping() && audioSourceLayoutHint() == 1, "clean identical channels: clip %d hint %d", audioSourceClipping(), audioSourceLayoutHint());
    k.gainDb = 12;
    audioIfProcess(in.data(), 4800, k, out.data(), st);
    CHECK(audioSourceClipping(), "0.6 with +12 dB must show as clipping");
    AudioIfState st2;
    for (int i = 0; i < 4800; i++) { in[2 * i] = 0.3f * (float)std::cos(0.3 * i); in[2 * i + 1] = 0.3f * (float)std::sin(0.3 * i); }
    k.gainDb = 0;
    audioIfProcess(in.data(), 4800, k, out.data(), st2);
    CHECK(!audioSourceClipping() && audioSourceLayoutHint() == 2, "I/Q pair: clip %d hint %d", audioSourceClipping(), audioSourceLayoutHint());
}


// ---- other modes from a sound card -------------------------------------------------------------------------------------------------------------------
// A narrowband mode's test signal (complex baseband from the synthetic source) turned into what a receiver's audio output carries: the channel is
// brought to `f0` Hz of real audio (upper sideband), low-passed and brought to the card's rate. The IF centre setting f0 takes it back to 0 Hz.
static std::vector<float> audioFromSynth(const TuneSettings& tin, double toffHz, double f0, double secs, int outRate, double* genRate = nullptr) {
    DeviceInfo dev;
    dev.kind = DeviceInfo::Synthetic;
    TuneSettings t = tin;
    t.sampleRate = 250000;
    t.synth.pace = 400;
    auto src = makeSource(dev);
    IqRing ring(1u << 22);
    std::string err;
    std::vector<float> out;
    if (!src || !src->start(t, ring, err)) { printf("synthetic source: %s\n", err.c_str()); return out; }
    const double fs = src->sampleRate();
    if (genRate) *genRate = fs;
    RationalResampler rs;
    if (!rs.configure(fs, outRate)) { src->stop(); return out; }
    const size_t want = (size_t)(secs * fs);
    size_t got = 0;
    double ph = 0;
    const double w = 2 * 3.14159265358979 * (toffHz + f0) / fs;   // the channel sits at -toffHz in the samples
    std::vector<cf32> x(16384), y;
    std::vector<cf32> tail;
    const auto t0 = std::chrono::steady_clock::now();
    while (got < want && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(60)) {
        const size_t n = ring.read(x.data(), x.size());
        if (!n) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
        for (size_t i = 0; i < n; i++) {
            x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph));
            ph += w; if (ph > 3.14159265358979) ph -= 2 * 3.14159265358979;
        }
        y.clear();
        rs.process(x.data(), n, y);
        for (const cf32& v : y) out.push_back(v.real());
        got += n;
    }
    src->stop();
    return out;
}

static std::string cardWav(const char* name, const std::vector<float>& a, int rate) {
    const std::string wav = tmp(std::string("audio_engine_") + name + ".wav");
    std::vector<float> z(a.size(), 0.f);
    writeWav(wav, rate, a, a);
    return wav;
}

// the engine on a sound card playing that WAV, in the mode's own entry; `until` is polled with the telemetry until true or the signal time is out
template <class Setup, class Until>
static bool viaCard(const char* name, const std::string& wav, int rate, int stdMode, double centreHz, double ifHz, double secs, Setup setup, Until until) {
    Engine e;
    DeviceInfo dev;
    dev.kind = DeviceInfo::Audio;
    dev.serial = "file:" + wav;
    dev.name = "Sound card: test file";
    TuneSettings t;
    const ModeTuning* mt = modeTuning(stdMode);
    t.centerHz = centreHz;
    t.bandwidthMhz = mt ? mt->bandwidthMhz : 0.01;
    t.sampleRate = mt ? mt->sampleRate : 250000;
    t.radio["audio_iq"] = "3";
    t.radio["audio_chan"] = "2";
    t.radio["audio_if_hz"] = std::to_string(ifHz);
    t.radio["audio_rate"] = std::to_string(rate);
    t.radio["audio_pace"] = ciSlack() > 1 ? "8" : "25";   // x real time: a shared CI runner needs more time for the decoder
    e.setStandard(stdMode);
    setup(e);
    FileOptions fo;
    if (!e.start(dev, t, fo)) { CHECK(false, "%s: engine start", name); return false; }
    RxTelemetry rx;
    uint64_t seq = 0;
    bool ok = false;
    const double s0 = signalSecs(e);
    while (signalSecs(e) - s0 < secs && !ok) {
        if (e.latestRx(rx, seq)) { seq = rx.seq; ok = until(e, rx); }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!ok) { waitFor(3.0, [&] { return e.latestRx(rx, seq); }); e.latestRx(rx, 0); ok = until(e, rx); }
    e.stop();
    remove(wav.c_str());
    return ok;
}

static std::string& lastPsk() { static std::string s; return s; }

static void testModes() {
    for (int rate : {48000, 22050}) {
        char nm[64];
        {   // NAVTEX (SITOR-B FSK): the FSK pair sits at 1000 Hz of audio; the IF centre says so (and the receiver also finds an FSK pair wherever it is)
            for (double ifHz : {1000.0}) {
                TuneSettings t; t.synth.mode = 17; t.synth.snrDb = 25; t.synth.modeOpt[0] = 1; t.synth.modeOpt[2] = 4; t.synth.modeVal[1] = 2;
                const std::vector<float> a = audioFromSynth(t, marineTuning().tuneOffsetHz, 1000, 22, rate);
                snprintf(nm, sizeof nm, "navtex%d_%d", rate, (int)ifHz);
                const std::string wav = cardWav(nm, a, rate);
                const bool ok = viaCard(nm, wav, rate, 17, 518e3, ifHz, 22, [](Engine& e) { e.marine().setFrequencyHz(518e3); },
                    [](Engine&, const RxTelemetry& rx) {
                        const std::string want = "010000 UTC JAN 26\nTEST MESSAGE FROM STATION A. 518 KHZ.";
                        for (const auto& n : rx.marine.navtex) if (n.complete && n.text == want) return true;
                        return false; });
                printf("NAVTEX from a card at %d Hz, IF centre %g: %s\n", rate, ifHz, ok ? "the test message decoded" : "NOT decoded");
                CHECK(ok, "NAVTEX from a sound card at %d Hz (IF centre %g) did not decode", rate, ifHz);
            }
        }
        {   // RTTY, from the audio the hfdig test generator makes (8 kHz), brought to the card's rate
            SynthConfig sc;
            auto gen = makeRttyTestAudio(sc);
            std::vector<float> a8((size_t)(8000 * 40));
            gen->generate(a8.data(), a8.size());
            for (float& v : a8) v *= 0.8f;
            std::vector<cf32> in(a8.size()), up;
            for (size_t i = 0; i < a8.size(); i++) in[i] = cf32(a8[i], 0);
            RationalResampler rs; rs.configure(8000, rate);
            rs.process(in.data(), in.size(), up);
            std::vector<float> a(up.size());
            for (size_t i = 0; i < a.size(); i++) a[i] = up[i].real();
            snprintf(nm, sizeof nm, "rtty%d", rate);
            const bool ok = viaCard(nm, cardWav(nm, a, rate), rate, 27, 14.080e6, 0, 40, [](Engine& e) { e.hfdig().setSilent(true); },
                [](Engine&, const RxTelemetry& rx) { return rx.hfdig.rtty.text.find("CQ CQ DE ONAIR TEST 0123456789 1/2 -.:? THE QUICK BROWN FOX\n") != std::string::npos; });
            printf("RTTY from a card at %d Hz: %s\n", rate, ok ? "text arrived" : "NO TEXT");
            CHECK(ok, "RTTY from a sound card at %d Hz did not decode", rate);
        }
        if (rate == 48000) {   // FT8: slots found from the signal, since a file standing in for a card is not on the clock
            SynthConfig sc;
            const SubMode* sm = subModeById("ft8");
            applySubModeSynth(sc, *sm);
            sc.snrDb = 20;
            auto gen = makeHfdigTestAudio(sc.modeOpt[0], sc);
            std::vector<float> a8((size_t)(8000 * 75));
            if (gen) gen->generate(a8.data(), a8.size());
            std::vector<cf32> in(a8.size()), up;
            for (size_t i = 0; i < a8.size(); i++) in[i] = cf32(a8[i] * 0.8f, 0);
            RationalResampler rs; rs.configure(8000, rate);
            rs.process(in.data(), in.size(), up);
            std::vector<float> a(up.size());
            for (size_t i = 0; i < a.size(); i++) a[i] = up[i].real();
            std::set<std::string> want, got;
            for (const auto& st : ftxTestStations(0)) want.insert(st.msg);
            const bool ok = viaCard("ft8", cardWav("ft8", a, rate), rate, 27, 14.074e6, 0, 75, [sm](Engine& e) { applySubMode(e, sm); e.hfdig().setSilent(true); },
                [&](Engine&, const RxTelemetry& rx) {
                    for (const auto& d : rx.hfdig.ftx.decodes) if (d.mode == 0 && want.count(d.msg)) got.insert(d.msg);
                    return got.size() >= 3; });
            printf("FT8 from a card: %zu of %zu test stations%s\n", got.size(), want.size(), ok ? "" : " (too few)");
            CHECK(ok, "FT8 from a sound card: %zu stations", got.size());
        }
        if (rate == 48000) {   // CW and PSK: the synthetic bands (several signals), as audio
            {
                TuneSettings t; t.synth.mode = 36; t.synth.snrDb = 22;
                const std::vector<float> a = audioFromSynth(t, cwTuning().tuneOffsetHz, 12000, 16, rate);
                const bool ok = viaCard("cw", cardWav("cw", a, rate), rate, 36, cwTuning().defMhz * 1e6, 12000, 16, [](Engine& e) { e.cw().setSilent(true); },
                    [](Engine&, const RxTelemetry& rx) {
                        int found = 0;
                        for (const auto& s : rx.cw.signals) if (s.text.find("LY2ABC") != std::string::npos || s.text.find("DL1XYZ") != std::string::npos || s.text.find("UR 599") != std::string::npos || s.text.find("OK1XX") != std::string::npos) found++;
                        return found >= 1; });
                printf("CW from a card: %s\n", ok ? "callsigns decoded" : "NOT decoded");
                CHECK(ok, "CW from a sound card did not decode");
            }
            {
                TuneSettings t; t.synth.mode = 38; t.synth.snrDb = 14;
                const std::vector<float> a = audioFromSynth(t, pskTuning().tuneOffsetHz, 12000, 18, rate);
                const bool ok = viaCard("psk", cardWav("psk", a, rate), rate, 38, pskTuning().defMhz * 1e6, 12000, 18, [](Engine& e) { e.psk().setSilent(true); },
                    [](Engine&, const RxTelemetry& rx) {
                        int found = 0;
                        lastPsk() = pskSummary(rx.psk); for (const auto& s : rx.psk.signals) lastPsk() += "\n   " + std::to_string((int)s.offsetHz) + " Hz \"" + s.text + "\"";
                        for (const auto& s : rx.psk.signals) if (s.text.find("LY2ABC") != std::string::npos || s.text.find("G4ABC") != std::string::npos || s.text.find("OK1XX") != std::string::npos || s.text.find("EA3XYZ") != std::string::npos) found++;
                        return found >= 1; });
                printf("PSK from a card: %s\n", ok ? "callsigns decoded" : "NOT decoded");
                if (!ok) printf("  %s\n", lastPsk().c_str());
                CHECK(ok, "PSK from a sound card did not decode");
            }
        }
    }
}

int main() {
    testTools();
    testModes();
    Case a{"mono48"};                                   check(a);
    Case b{"left44100"}; b.rate = 44100; b.layout = LeftOnly; b.audioChan = 0; check(b);
    Case c{"right96k_dc"}; c.rate = 96000; c.layout = RightOnly; c.audioChan = 1; c.dc = 0.08; check(c);
    Case d{"mix12k_quiet"}; d.rate = 12000; d.layout = LeftOnly; d.gain = 0.05; check(d);
    Case e{"afview48"}; e.afView = true; check(e);
    Case f{"mistuned"}; f.mistuneHz = 40; check(f);
    Case g{"clock300ppm"}; g.ppm = 300; g.rate = 44100; check(g);
    Case h{"noisy"}; h.snrDb = 18; check(h);
    Case i{"usbmode"}; i.iqMode = 1; i.audioChan = 0; check(i);
    return testSummary("audio engine");
}
