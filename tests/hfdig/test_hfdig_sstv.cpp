// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HF digital SSTV: test audio -> decoder, every mode, at 20 dB (signal to noise in 3 kHz) with a +-100 Hz tuning error and +-100 ppm of sample
// clock error; the mode picked from the line period when the VIS header is left out; and a Robot 36 picture through the engine.
// The long modes are fed straight from the generator, faster than real time.
#include "dect2/engine/engine.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/engine/modes.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Noise {
    uint64_t s = 12345;
    double u() { s = s * 6364136223846793005ULL + 1442695040888963407ULL; return ((s >> 11) + 0.5) / 9007199254740992.0; }
    double g() { return std::sqrt(-2 * std::log(u())) * std::cos(6.283185307179586 * u()); }
};

// feeds the test audio of one mode to a decoder until a picture is done (or the time is up); returns the telemetry
static HfdigSstvTelemetry run(const SynthConfig& cfg, double snrDb, double maxSec) {
    auto gen = makeSstvTestAudio(cfg);
    auto dec = makeSstvDecoder();
    Noise nz;
    const double sigma = std::sqrt(0.125 / std::pow(10.0, snrDb / 10.0) * 4000.0 / 3000.0);
    std::vector<float> buf(800);
    HfdigSstvTelemetry t;
    for (int blk = 0; blk < (int)(maxSec * 10); blk++) {
        gen->generate(buf.data(), buf.size());
        for (float& v : buf) v += (float)(sigma * nz.g());
        dec->feedAudio(buf.data(), buf.size());
        if (blk % 10 == 0) { dec->telemetry(t); if (t.picturesDone >= 1) break; }
    }
    dec->telemetry(t);
    return t;
}

static double mae(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 255;
    double s = 0;
    for (size_t i = 0; i < a.size(); i++) s += std::abs((int)a[i] - (int)b[i]);
    return s / (double)a.size();
}

int main() {
    for (int m = 0; m < sstvModeCount(); m++) {
        const SstvModeInfo& mi = sstvModeInfo(m);
        SynthConfig cfg;
        cfg.modeOpt[1] = m;
        cfg.modeVal[0] = (m & 1) ? -100 : 100;    // a dial error of +-100 Hz and a sound card clock +-100 ppm off, alternating
        cfg.sroPpm = (m & 1) ? -100 : 100;
        const double sec = 0.4 + 0.91 + mi.lineMs * (mi.width > 320 && mi.height > 300 ? mi.height / 2 : mi.height) / 1000.0 + 3;
        const HfdigSstvTelemetry t = run(cfg, 20, sec + 2);
        std::vector<uint8_t> ref;
        sstvTestPicture(mi.width, mi.height, ref);
        const bool ok = t.picturesDone >= 1 && t.image && t.image->complete;
        const double e = ok ? mae(t.image->rgb, ref) : 255;
        printf("%-15s VIS %3d (sent %3d) lines %3d/%3d slant %+6.0f ppm offset %+5.1f Hz  mean error %.2f\n", mi.name, t.visCode, mi.vis, t.lines, mi.height,
               t.slantPpm, t.offsetHz, e);
        CHECK(ok, "%s: no complete picture", mi.name);
        CHECK(t.visCode == mi.vis, "%s: VIS %d", mi.name, t.visCode);
        CHECK(t.mode == mi.name, "%s: mode '%s'", mi.name, t.mode.c_str());
        CHECK(e < 12, "%s: mean error %.2f", mi.name, e);
        CHECK(!t.history.empty() && t.history[0] == t.image, "%s: history", mi.name);
    }
    {   // no VIS header: the mode comes from the line period
        const int idx[2] = {3, 2};
        for (int k = 0; k < 2; k++) {
            const SstvModeInfo& mi = sstvModeInfo(idx[k]);
            SynthConfig cfg;
            cfg.modeOpt[1] = idx[k]; cfg.modeOpt[2] = 1; cfg.modeVal[0] = 30; cfg.sroPpm = 50;
            const HfdigSstvTelemetry t = run(cfg, 20, 0.4 + mi.lineMs * 256 / 1000.0 + 3);
            printf("%s without VIS: picked '%s', lines %d\n", mi.name, t.mode.c_str(), t.lines);
            CHECK(t.mode == mi.name && t.visCode == -1 && t.lines > 200, "no-VIS %s: '%s' vis %d lines %d", mi.name, t.mode.c_str(), t.visCode, t.lines);
        }
    }
    {   // through the engine: Robot 36 (index 0), the synthetic source at 6 times real time
        Engine e;
        e.hfdig().setSilent(true);
        const ModeTuning mt = hfdigTuning();
        DeviceInfo dev;
        TuneSettings t;
        t.centerHz = mt.defMhz * 1e6;
        t.bandwidthMhz = mt.bandwidthMhz;
        t.sampleRate = mt.sampleRate;
        t.synth.mode = 27;
        t.synth.modeOpt[0] = 1;
        t.synth.modeOpt[1] = 0;
        t.synth.snrDb = 25;
        t.synth.pace = 6;
        FileOptions fo;
        e.setStandard(27);
        CHECK(e.start(dev, t, fo), "engine start");
        RxTelemetry rx;
        uint64_t seq = 0;
        bool done = false;
        const auto t0 = std::chrono::steady_clock::now();
        while (!done && std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 40) {
            if (e.latestRx(rx, seq)) { seq = rx.seq; done = rx.hfdig.sstv.picturesDone >= 1; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        e.stop();
        const HfdigSstvTelemetry& s = rx.hfdig.sstv;
        std::vector<uint8_t> ref;
        sstvTestPicture(320, 240, ref);
        const double err = s.image ? mae(s.image->rgb, ref) : 255;
        printf("engine: %.1f s, '%s' VIS %d lines %d, mean error %.2f\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), s.mode.c_str(), s.visCode, s.lines, err);
        CHECK(done && s.mode == "Robot 36" && s.lines == 240, "engine: done %d mode '%s' lines %d", (int)done, s.mode.c_str(), s.lines);
        CHECK(err < 20, "engine: mean error %.2f", err);
    }
    {   // a 300 ms receive dropout (only noise heard) in the middle of a Robot 36 picture: the picture still comes out whole, and the lines before and after the gap match
        const SstvModeInfo& mi = sstvModeInfo(0);
        const int w = mi.width, h = mi.height;
        const double leadMs = 400 + 910;                                  // leader and VIS header
        const double gapStartMs = leadMs + 80 * mi.lineMs + 40, gapMs = 300;
        const size_t total = (size_t)((leadMs + h * mi.lineMs + 1500) * 8);
        std::vector<float> audio(total);
        makeSstvTx(0, {})->generate(audio.data(), audio.size());
        for (size_t i = (size_t)(gapStartMs * 8); i < (size_t)((gapStartMs + gapMs) * 8) && i < total; i++) audio[i] = 0.f;
        Noise nz;
        const double sigma = std::sqrt(0.125 / 100.0 * 4000.0 / 3000.0);   // 20 dB as in run()
        auto dec = makeSstvDecoder();
        for (size_t i = 0; i < total; i += 800) {
            const size_t m = total - i < 800 ? total - i : 800;
            for (size_t k = 0; k < m; k++) audio[i + k] += (float)(sigma * nz.g());
            dec->feedAudio(audio.data() + i, m);
        }
        HfdigSstvTelemetry t;
        dec->telemetry(t);
        std::vector<uint8_t> ref;
        sstvTestPicture(w, h, ref);
        const bool whole = t.image && t.image->complete && (int)t.image->lines == h && t.image->height == h;
        CHECK(whole, "dropout: picture %s, lines %d/%d", t.image ? (t.image->complete ? "complete" : "incomplete") : "missing", t.lines, h);
        if (whole) {
            const size_t rowBytes = (size_t)w * 3;
            auto rowsMae = [&](int y0, int y1) {
                double s = 0;
                for (int y = y0; y < y1; y++) for (size_t x = 0; x < rowBytes; x++) s += std::abs((int)t.image->rgb[(size_t)y * rowBytes + x] - (int)ref[(size_t)y * rowBytes + x]);
                return s / ((double)(y1 - y0) * rowBytes);
            };
            const double before = rowsMae(0, 78), after = rowsMae(85, h);
            printf("dropout %s: lines before the gap %.2f, after %.2f\n", mi.name, before, after);
            CHECK(before < 12, "dropout: rows before the gap, mean error %.2f", before);
            CHECK(after < 12, "dropout: rows after the gap, mean error %.2f", after);
        }
    }
    {   // a 3 s gap (20 lines, more than the 12 missed lines that used to end a picture): one whole picture, the rows after the gap right
        const SstvModeInfo& mi = sstvModeInfo(0);
        const int w = mi.width, h = mi.height;
        const double leadMs = 400 + 910;
        const double gapStartMs = leadMs + 80 * mi.lineMs + 40, gapMs = 3000;
        const size_t total = (size_t)((leadMs + h * mi.lineMs + 1500) * 8);
        std::vector<float> audio(total);
        makeSstvTx(0, {})->generate(audio.data(), audio.size());
        for (size_t i = (size_t)(gapStartMs * 8); i < (size_t)((gapStartMs + gapMs) * 8) && i < total; i++) audio[i] = 0.f;
        Noise nz;
        const double sigma = std::sqrt(0.125 / 100.0 * 4000.0 / 3000.0);
        auto dec = makeSstvDecoder();
        for (size_t i = 0; i < total; i += 800) {
            const size_t m = total - i < 800 ? total - i : 800;
            for (size_t k = 0; k < m; k++) audio[i + k] += (float)(sigma * nz.g());
            dec->feedAudio(audio.data() + i, m);
        }
        HfdigSstvTelemetry t;
        dec->telemetry(t);
        std::vector<uint8_t> ref;
        sstvTestPicture(w, h, ref);
        const bool whole = t.image && t.image->complete && (int)t.image->lines == h && t.picturesDone == 1;
        CHECK(whole, "long gap: picture %s, lines %d/%d, pictures %d", t.image ? (t.image->complete ? "complete" : "incomplete") : "missing", t.lines, h, (int)t.picturesDone);
        if (whole) {
            const size_t rowBytes = (size_t)w * 3;
            double s = 0;
            const int y0 = 80 + (int)(gapMs / mi.lineMs) + 6;
            for (int y = y0; y < h; y++) for (size_t x = 0; x < rowBytes; x++) s += std::abs((int)t.image->rgb[(size_t)y * rowBytes + x] - (int)ref[(size_t)y * rowBytes + x]);
            const double after = s / ((double)(h - y0) * rowBytes);
            CHECK(after < 12, "long gap: rows after the gap, mean error %.2f", after);
        }
    }
    {   // a second VIS header part-way through a picture: the first is saved as it stood, the second decodes whole
        const SstvModeInfo& mi = sstvModeInfo(0);
        const int w = mi.width, h = mi.height;
        const double leadMs = 400 + 910;
        const size_t cut = (size_t)((leadMs + 100 * mi.lineMs) * 8);
        const size_t one = (size_t)((leadMs + h * mi.lineMs + 1500) * 8);
        std::vector<float> full(one);
        makeSstvTx(0, {})->generate(full.data(), full.size());
        std::vector<float> audio(full.begin(), full.begin() + (long)cut);
        audio.insert(audio.end(), full.begin(), full.end());
        Noise nz;
        const double sigma = std::sqrt(0.125 / 100.0 * 4000.0 / 3000.0);
        auto dec = makeSstvDecoder();
        const size_t total = audio.size();
        for (size_t i = 0; i < total; i += 800) {
            const size_t m = total - i < 800 ? total - i : 800;
            for (size_t k = 0; k < m; k++) audio[i + k] += (float)(sigma * nz.g());
            dec->feedAudio(audio.data() + i, m);
        }
        HfdigSstvTelemetry t;
        dec->telemetry(t);
        std::vector<uint8_t> ref;
        sstvTestPicture(w, h, ref);
        const bool two = t.picturesDone == 2 && t.history.size() >= 2 && t.image && t.image->complete && (int)t.image->lines == h;
        CHECK(two, "new VIS: pictures %d, history %d, last %s", (int)t.picturesDone, (int)t.history.size(), t.image ? (t.image->complete ? "complete" : "incomplete") : "missing");
        if (two) {
            const auto& first = t.history[1];
            CHECK(!first->complete && first->lines >= 90 && first->lines < h, "new VIS: first picture complete %d lines %d", (int)first->complete, (int)first->lines);
            const size_t rowBytes = (size_t)w * 3;
            double s = 0;
            for (int y = 0; y < h; y++) for (size_t x = 0; x < rowBytes; x++) s += std::abs((int)t.image->rgb[(size_t)y * rowBytes + x] - (int)ref[(size_t)y * rowBytes + x]);
            const double e = s / ((double)h * rowBytes);
            CHECK(e < 12, "new VIS: second picture mean error %.2f", e);
        }
    }
    printf("%s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
