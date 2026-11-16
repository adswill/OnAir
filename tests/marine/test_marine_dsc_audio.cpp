// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VHF DSC from a sound card or a WAV file (issue from the Discord): the samples are FM-demodulated audio (tones 1300 Hz = Y, 2100 Hz = B, 1200 baud),
// not a radio's IQ. The test takes the same path as the sound-card source (source_audio.cpp): stereo frames -> audioIfProcess (Real mono) ->
// interpolated up to the mode's minimum rate (audioOutFactor) -> MarineReceiver with setAudioInput(true). Recordings: tests/data/dsc_audio (MIT,
// from github.com/blackwellj/vhf-dsc, see its README.md), at their own rate and resampled to other card rates, at several levels.
#include "dect2/dsp/exact_resampler.h"
#include "dect2/marine/marine_dsc.h"
#include "dect2/marine/marine_rx.h"
#include "dect2/marine/marine_tel.h"
#include "dect2/source/audio_if.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Wav { std::vector<float> x; double rate = 0; };

static bool readWav(const std::string& path, Wav& w) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) return false;
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 44 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4)) return false;
    size_t p = 12; int ch = 0, bits = 0;
    while (p + 8 <= b.size()) {
        const uint32_t n = (uint32_t)(b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | (uint32_t)b[p + 7] << 24);
        if (!std::memcmp(&b[p], "fmt ", 4)) { ch = b[p + 10] | b[p + 11] << 8; w.rate = (double)(b[p + 12] | b[p + 13] << 8 | b[p + 14] << 16 | (uint32_t)b[p + 15] << 24); bits = b[p + 22] | b[p + 23] << 8; }
        else if (!std::memcmp(&b[p], "data", 4)) {
            if (ch != 1 || bits != 16) return false;
            const size_t cnt = std::min<size_t>(n, b.size() - p - 8) / 2;
            for (size_t i = 0; i < cnt; i++) w.x.push_back((float)(int16_t)(b[p + 8 + 2 * i] | b[p + 9 + 2 * i] << 8) / 32768.f);
            return true;
        }
        p += 8 + n + (n & 1);
    }
    return false;
}

static std::vector<float> resampled(const Wav& w, double rate) {
    if (rate == w.rate) return w.x;
    ExactResampler r; r.configure(w.rate, rate, 16);
    std::vector<cf32> in(w.x.size()), out;
    for (size_t i = 0; i < in.size(); i++) in[i] = cf32(w.x[i], 0.f);
    r.process(in.data(), in.size(), out);
    std::vector<float> y(out.size());
    for (size_t i = 0; i < y.size(); i++) y[i] = out[i].real();
    return y;
}

// What the sound-card source does with the card's audio, then the receiver.
static std::vector<DscCall> run(const std::vector<float>& audio, double rate, float level, bool leftOnly) {
    AudioIfConfig cfg;
    cfg.mode = AudioIqMode::MonoReal; cfg.chan = leftOnly ? 0 : 2; cfg.rate = (int)rate; cfg.minOutRate = marineTuning().minSampleRate;
    const int f = audioOutFactor(rate, cfg.minOutRate);
    MarineReceiver rx;
    rx.setAudioInput(true);
    rx.configure(rate * f);
    CHECK(rx.ready(), "ready at %.0f x %d", rate, f);
    rx.setFrequencyHz(156.525e6);
    rx.setService(2);
    AudioIfState st;
    ExactResampler up; if (f > 1) up.configure(rate, rate * f);
    std::vector<float> a(audio.size());
    for (size_t i = 0; i < a.size(); i++) a[i] = audio[i] * level;
    a.resize(a.size() + (size_t)(2.0 * rate), 0.f);   // silence after the call: the last reports
    std::vector<float> stereo(2048 * 2);
    std::vector<cf32> iq(2048), upOut;
    for (size_t at = 0; at < a.size();) {
        const int n = (int)std::min<size_t>(1024, a.size() - at);
        for (int i = 0; i < n; i++) { stereo[2 * (size_t)i] = a[at + (size_t)i]; stereo[2 * (size_t)i + 1] = leftOnly ? 0.f : a[at + (size_t)i]; }
        audioIfProcess(stereo.data(), n, cfg, iq.data(), st);
        if (f > 1) { upOut.clear(); up.process(iq.data(), (size_t)n, upOut); rx.feed(upOut.data(), upOut.size()); }
        else rx.feed(iq.data(), (size_t)n);
        at += (size_t)n;
    }
    MarineTelemetry t;
    const bool got = rx.telemetry(t, 0);
    if (t.dsc.empty()) printf("  (no call: telemetry %d seq %llu state %d dscLocked %d snr %.1f dscCount %llu level peak %.2f)\n", got, (unsigned long long)t.seq, t.state, t.dscLocked, (double)t.snrDb, (unsigned long long)t.dscCount, (double)st.peak);
    return t.dsc;
}

// A clean call made here: phase-continuous AFSK, Y = 1300 Hz, B = 2100 Hz, 1200 baud, at `rate`
static std::vector<float> synthCall(double rate, int dots) {
    const std::vector<int> body = dscBuildDistress("232123456", 103, 50.85, -1.3, 12, 34, 0);
    const std::vector<uint8_t> bits = dscFrameBits(body, 127, dots);
    std::vector<float> a((size_t)(0.3 * rate), 0.f);
    double ph = 0, pos = 0;
    const double spb = rate / 1200.0;
    for (size_t k = 0; k < bits.size(); k++) {
        const double f = bits[k] ? 1300.0 : 2100.0;
        const size_t end = (size_t)std::lround((double)(k + 1) * spb);
        for (; pos < (double)end; pos += 1) { ph += 2 * 3.14159265358979 * f / rate; a.push_back(0.5f * (float)std::sin(ph)); }
    }
    return a;
}

int main() {
    for (int dots : {20, 200}) {
        for (double r : {48000.0, 11025.0}) {
            const std::vector<DscCall> calls = run(synthCall(r, dots), r, 1.0f, false);
            int good = 0;
            for (const auto& d : calls) { printf("  synthetic %d dots %.0f Hz: %s ecc %d: %s\n", dots, r, d.fromMmsi.c_str(), d.eccOk, d.text.c_str()); if (d.vhf && d.eccOk && d.fromMmsi == "232123456" && d.distress && d.hasPos) good++; }
            CHECK(good >= 1, "synthetic call, %d dots, %.0f Hz: %d good calls of %zu", dots, r, good, calls.size());
        }
    }
    struct Case { const char* file; const char* self; int format; bool distress; };
    const Case cases[] = {
        {"individual_call.wav", "002275200", 120, false},
        {"individual_call_2.wav", "002275100", 120, false},
        {"distress_relay.wav", "002275300", -1, true},
        {"all_ships_call.wav", "002320014", 116, false},
        {"distress_acknowledgement.wav", "002275200", -1, true},
    };
    const std::string dir = std::string(DECT2_SOURCE_DIR) + "/tests/data/dsc_audio/";
    const double rates[] = {0, 11025, 22050, 44100, 48000};   // 0: the file's own rate (16 kHz)
    const float levels[] = {0.9f, 0.03f};
    for (const Case& c : cases) {
        Wav w;
        CHECK(readWav(dir + c.file, w), "cannot read %s", c.file);
        if (w.x.empty()) continue;
        for (double r0 : rates) {
            const double r = r0 > 0 ? r0 : w.rate;
            const std::vector<float> a = resampled(w, r);
            for (float lv : levels) {
                for (int left = 0; left < 2; left++) {
                    if (left && (r0 != 0 || lv != 0.9f)) continue;   // one run with the signal on the left channel only (a mono feed: Real mono mixes, left picks)
                    const std::vector<DscCall> calls = run(a, r, lv, left != 0);
                    int good = 0;
                    for (const auto& d : calls) {
                        printf("  %s %.0f Hz level %.2f: format %d from %s to %s eos %d ecc %d erasures %d distress %d: %s\n", c.file, r, lv, d.format, d.fromMmsi.c_str(), d.to.c_str(), d.eos, d.eccOk, d.erasures, d.distress, d.text.c_str());
                        if (d.vhf && d.eccOk && d.fromMmsi == c.self && (c.format < 0 || d.format == c.format) && d.distress == c.distress) good++;
                    }
                    CHECK(good >= 1, "%s at %.0f Hz, level %.2f%s: %d matching calls of %zu", c.file, r, lv, left ? ", left channel" : "", good, calls.size());
                }
            }
        }
    }
    // an empty channel decodes nothing
    {
        Wav w;
        CHECK(readWav(dir + "noise.wav", w), "cannot read noise.wav");
        for (double r0 : rates) {
            const double r = r0 > 0 ? r0 : w.rate;
            const std::vector<DscCall> calls = run(resampled(w, r), r, 0.9f, false);
            CHECK(calls.empty(), "noise.wav at %.0f Hz: %zu calls", r, calls.size());
        }
    }
    return testSummary("marine dsc audio");
}
