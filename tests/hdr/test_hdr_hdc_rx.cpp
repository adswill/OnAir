// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// HD Radio sound inside the real receiver (HdrReceiver, FM hybrid test signal), with and without a user-installed HDC library (a fake one,
// tests/hdr/fake_faad_hdc.c; no codec, no sound device). The test signal's audio packets are filler: HD1's are 160 bytes and HD2's 80, which the
// fake turns into R = 1600 and R = 800, so the program that is heard can be told from the sound that comes out.
//  - without the library: the status says so once (the log line the diagnostics' fix-it guide matches), telemetry has hdcFound false, no sound
//  - with it: hdcFound, packets decoded, HD1 sound; the selection moved to HD2 switches the sound; a retune/reset starts over without a crash
#include "dect2/hdr/hdr_gen.h"
#include "dect2/hdr/hdr_hdc.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/engine/mode_synth.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static void setEnv(const char* k, const std::string& v) {
#ifdef _WIN32
    _putenv_s(k, v.c_str());
#else
    setenv(k, v.c_str(), 1);
#endif
}

struct Run {
    HdrReceiver rx;
    std::unique_ptr<ModeSynth> syn;
    std::vector<std::string> log;
    std::mutex mu;
    std::vector<float> lr;
    HdrTelemetry t;
    uint64_t seq = 0;
    double sec = 0;
    static constexpr double rate = 2e6;
    Run() {
        SynthConfig sc;
        sc.mode = 23; sc.modeOpt[0] = 0; sc.snrDb = 25; sc.cfoHz = 300;
        syn = makeHdrSynth(sc, rate);
        rx.configure(rate);
        rx.setSilent(true);
        rx.setLogCallback([this](const std::string& s) { std::lock_guard<std::mutex> lk(mu); log.push_back(s); });
        rx.setAudioTap([this](const float* x, size_t n) { std::lock_guard<std::mutex> lk(mu); lr.insert(lr.end(), x, x + 2 * n); });
    }
    // feeds for `seconds` of signal, or until `until` is true
    template <class F> void go(double seconds, F until) {
        std::vector<cf32> buf(20000);
        const double end = sec + seconds;
        while (sec < end) {
            syn->generate(buf.data(), buf.size());
            rx.feed(buf.data(), buf.size());
            sec += (double)buf.size() / rate;
            if (rx.telemetry(t, seq)) { seq = t.seq; if (until(t)) break; }
        }
    }
    bool said(const char* what) { std::lock_guard<std::mutex> lk(mu); for (const auto& l : log) if (l.find(what) != std::string::npos) return true; return false; }
    // the typical value of channel R over the non-silent part of what was played since `from` frames
    double rTypical(size_t from) {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<float> v;
        for (size_t i = from; i < lr.size() / 2; i++) if (std::fabs(lr[2 * i + 1]) > 0.005f) v.push_back(lr[2 * i + 1]);
        if (v.size() < 2000) return NAN;
        std::nth_element(v.begin(), v.begin() + (long)v.size() / 2, v.end());
        return v[v.size() / 2] * 32768.0;
    }
    size_t frames() { std::lock_guard<std::mutex> lk(mu); return lr.size() / 2; }
};

int main() {
    // ---- without a library
    setEnv("DECT2_FAAD_HDC_PATH", "/nonexistent-folder/libfaad_hdc.so");
    {
        Run r;
        r.go(25, [](const HdrTelemetry& t) { return t.state >= 3 && t.pduCount > 20; });
        r.go(2, [](const HdrTelemetry&) { return false; });
        CHECK(r.t.state >= 3 && r.t.pduCount > 20, "no lock: state %d pdus %llu", r.t.state, (unsigned long long)r.t.pduCount);
        CHECK(!r.t.hdcFound && r.t.hdcProblem.empty() && r.t.hdcDecoded == 0, "telemetry without a library: found %d decoded %llu", (int)r.t.hdcFound, (unsigned long long)r.t.hdcDecoded);
        CHECK(r.said("HDC sound codec not installed"), "the log does not say that the codec is missing");
        CHECK(r.frames() == 0, "sound without a library: %zu frames", r.frames());
        { std::lock_guard<std::mutex> lk(r.mu); int n = 0; for (const auto& l : r.log) if (l.find("HDC sound codec") != std::string::npos) n++; CHECK(n == 1, "said %d times", n); }
        const HdrProgram* p = nullptr;
        for (const auto& q : r.t.programs) if (q.number == 0) p = &q;
        CHECK(p && p->packetsOk > 0, "audio packets still counted");
    }
    // ---- with the (fake) library
    setEnv("DECT2_FAAD_HDC_PATH", FAKE_HDC_GOOD);
    CHECK(dect2::hdr::hdcRescan(), "fake library not accepted");
    {
        Run r;
        r.rx.setProgram(0);
        r.go(25, [&](const HdrTelemetry& t) { return t.state >= 3 && t.hdcDecoded > 40; });
        r.go(4, [](const HdrTelemetry&) { return false; });
        CHECK(r.t.hdcFound && r.t.hdcProblem.empty() && !r.t.hdcPath.empty(), "found %d '%s'", (int)r.t.hdcFound, r.t.hdcProblem.c_str());
        CHECK(r.t.hdcDecoded > 40 && r.t.hdcErrors == 0, "decoded %llu errors %llu lost %llu", (unsigned long long)r.t.hdcDecoded, (unsigned long long)r.t.hdcErrors, (unsigned long long)r.t.hdcLost);
        CHECK(r.t.soundProgram == 0 && r.t.hdcPlaying, "program %d playing %d", r.t.soundProgram, (int)r.t.hdcPlaying);
        CHECK(!r.said("HDC sound codec"), "complains about the codec although it is there");
        const double hd1 = r.rTypical(0);
        CHECK(std::fabs(hd1 - 1600) < 30, "HD1 sound R %.1f (want 1600)", hd1);
        // the selection moves to HD2
        r.rx.setProgram(1);
        const size_t from = r.frames();
        const uint64_t dec0 = r.t.hdcDecoded;
        r.go(8, [&](const HdrTelemetry& t) { return t.hdcDecoded > dec0 + 80; });
        const double hd2 = r.rTypical(from + 20000);
        CHECK(r.t.soundProgram == 1 && std::fabs(hd2 - 800) < 20, "HD2 sound R %.1f (want 800) program %d", hd2, r.t.soundProgram);
        // a retune: the receiver starts over, the sound state with it, nothing breaks
        r.rx.reset();
        r.go(3, [](const HdrTelemetry&) { return false; });
        r.rx.setMuted(true); r.rx.setVolume(0.3f);
        r.go(1, [](const HdrTelemetry&) { return false; });
        printf("hdr_hdc_rx: decoded %llu, HD1 R %.1f, HD2 R %.1f\n", (unsigned long long)r.t.hdcDecoded, hd1, hd2);
    }
    return testSummary("hdr_hdc_rx");
}
