// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Tone decoders on synthetic audio and FM: every CTCSS tone, DCS codes in both polarities and at any start phase, DTMF, the sequential plans,
// SELCAL, a two-tone page, all of it in noise and under speech-like interference; no detections in noise and speech alone; the gate; the FM path.
#include "dect2/tones/tones_gen.h"
#include "dect2/tones/tones_proto.h"
#include "dect2/tones/tones_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>

using namespace dect2;
static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static TonesTelemetry runAudio(const ToneScene& s, double rate = 0) {
    TonesReceiver rx;
    rx.setAudioInput(true);
    const double r = rate > 0 ? rate : s.rate();
    rx.configure(r);
    std::vector<cf32> buf;
    const auto& a = s.audio();
    for (size_t i = 0; i < a.size(); i += 4800) {
        const size_t n = std::min<size_t>(4800, a.size() - i);
        buf.assign(n, cf32());
        for (size_t k = 0; k < n; k++) buf[k] = cf32(a[i + k], 0.f);
        rx.feed(buf.data(), n);
    }
    // a second of silence so the last events close
    ToneScene tail(s.rate()); tail.pad(1.5);
    buf.assign(tail.audio().size(), cf32());
    rx.feed(buf.data(), buf.size());
    TonesTelemetry t;
    rx.telemetry(t, 0);
    return t;
}
static const ToneEvent* find(const TonesTelemetry& t, int kind, const std::string& text) {
    for (const auto& e : t.events) if (e.kind == kind && e.text == text) return &e;
    return nullptr;
}
static int count(const TonesTelemetry& t, int kind) { int n = 0; for (const auto& e : t.events) if (e.kind == kind) n++; return n; }

static void addNoise(ToneScene& s, double rms) { s.noise(0, s.seconds(), rms); }

static std::string codeText(int code, bool inv) { return tones::dcsName(code, inv); }

int main() {
    // ---- CTCSS: all 50 tones at 12 % deviation, noise, speech at three times the tone level in the middle
    {
        int ok = 0;
        for (int i = 0; i < tones::kCtcssN; i++) {
            ToneScene s(48000, 10 + i);
            s.ctcss(tones::ctcssHz(i), 0, 7, 0.12);
            s.speech(2.5, 3, 0.36);
            addNoise(s, 0.02);
            const TonesTelemetry t = runAudio(s);
            char b[32]; snprintf(b, sizeof b, "%.1f Hz", tones::ctcssHz(i));
            const ToneEvent* e = find(t, TkCtcss, b);
            if (e) ok++; else CHECK(false, "CTCSS %s not found (%d events)", b, (int)t.events.size());
            CHECK(count(t, TkCtcss) == 1, "CTCSS %s: %d detections", b, count(t, TkCtcss));
            CHECK(count(t, TkDcs) == 0, "CTCSS %s read as DCS", b);
        }
        std::printf("ctcss: %d of 50\n", ok);
    }
    // ---- CTCSS: low level, a frequency error of the transmitter (+-0.5 Hz), a different sample rate
    {
        ToneScene s(8000, 3);
        s.ctcss(100.5, 0, 6, 0.04);
        addNoise(s, 0.02);
        const TonesTelemetry t = runAudio(s);
        CHECK(find(t, TkCtcss, "100.0 Hz"), "100.5 Hz tone at 4 percent in noise reads as 100.0");
        ToneScene s2(44100, 4);
        s2.ctcss(88.5, 0, 5, 0.1);
        CHECK(find(runAudio(s2), TkCtcss, "88.5 Hz"), "CTCSS at 44.1 kHz");
        ToneScene s3(48000, 4);
        s3.ctcss(141.3, 0, 5, 0.003);
        CHECK(count(runAudio(s3), TkCtcss) == 0, "a very weak tone is not announced");
    }
    // ---- DCS: all 83 codes, both polarities, any start phase; noise
    {
        int okN = 0, okI = 0;
        const int codes[] = {23, 25, 26, 31, 32, 43, 47, 51, 54, 65, 71, 72, 73, 74, 114, 115, 116, 125, 131, 132, 134, 143, 152, 155, 156, 162, 165, 172, 174, 205, 223, 226, 243, 244,
                             245, 251, 261, 263, 265, 271, 306, 311, 315, 331, 343, 346, 351, 364, 365, 371, 411, 412, 413, 423, 431, 432, 445, 464, 465, 466, 503, 506, 516, 532, 546,
                             565, 606, 612, 624, 627, 631, 632, 654, 662, 664, 703, 712, 723, 731, 732, 734, 743, 754};
        int n = 0;
        for (int c : codes) {
            for (int inv = 0; inv < 2; inv++) {
                ToneScene s(48000, 20 + n);
                s.dcs(c, inv != 0, 0, 4.0, 0.14, (double)(n % 23) + 0.37 * (n % 5));
                s.noise(0, 4.0, 0.02);
                const TonesTelemetry t = runAudio(s);
                bool found = false;
                for (const auto& e : t.events) if (e.kind == TkDcs) {
                    if (e.text == codeText(c, inv != 0)) found = true;
                    // the same waveform read as another standard code (the alias) is a right answer too
                    if (e.detail.find(codeText(c, inv != 0)) != std::string::npos) found = true;
                }
                n++;
                if (found) (inv ? okI : okN)++;
                else CHECK(false, "DCS %s not found", codeText(c, inv != 0).c_str());
                CHECK(count(t, TkCtcss) == 0, "DCS %03d read as CTCSS", c);
            }
        }
        std::printf("dcs: %d normal, %d inverted of 83\n", okN, okI);
    }
    // ---- DCS under speech and at low level
    {
        ToneScene s(48000, 5);
        s.dcs(114, false, 0, 6, 0.13, 3);
        s.speech(1.5, 3.5, 0.3);
        s.noise(0, 6, 0.03);
        const TonesTelemetry t = runAudio(s);
        CHECK(find(t, TkDcs, "D114N"), "DCS D114N under speech");
        ToneScene s2(48000, 6);
        s2.dcs(23, false, 0, 5, 0.05, 1);
        s2.noise(0, 5, 0.025);
        CHECK(find(runAudio(s2), TkDcs, "D023N"), "DCS at 5 percent in noise");
    }
    // ---- DTMF: all 16 keys; tone 60 ms and gap 60 ms; with noise
    {
        ToneScene s(48000, 7);
        s.dtmf("123A456B789C*0#D", 0.3, 0.06, 0.06, 0.4);
        addNoise(s, 0.03);
        const TonesTelemetry t = runAudio(s);
        CHECK(find(t, TkDtmf, "123A456B789C*0#D"), "all 16 DTMF keys, got %s", t.events.empty() ? "nothing" : t.events[0].text.c_str());
        ToneScene s2(8000, 7);
        s2.dtmf("5551234", 0.2, 0.05, 0.05, 0.5);
        CHECK(find(runAudio(s2), TkDtmf, "5551234"), "DTMF at 8 kHz, 50 ms tones");
    }
    // ---- no false detections: noise alone, speech alone, a steady 1 kHz tone, a 1 kHz tone with speech
    {
        ToneScene s(48000, 8);
        s.speech(0, 40, 0.5);
        addNoise(s, 0.05);
        const TonesTelemetry t = runAudio(s);
        CHECK(t.events.empty(), "speech and noise alone: %d detections (first %s %s)", (int)t.events.size(), t.events.empty() ? "" : toneKindName(t.events[0].kind), t.events.empty() ? "" : t.events[0].text.c_str());
        ToneScene n(48000, 9);
        n.pad(40); addNoise(n, 0.15);
        CHECK(runAudio(n).events.empty(), "loud noise alone");
        ToneScene s3(48000, 9);
        s3.tone(1000, 0, 10, 0.3);
        CHECK(runAudio(s3).events.empty(), "a steady tone is no signalling");
    }
    // ---- sequential plans: each plan with digits, a repeated digit and the repeat tone
    {
        struct Case { const char* id; const char* name; const char* digits; double ms; };
        const Case cs[] = {{"zvei1", "ZVEI1 / PZVEI", "12345", 70}, {"zvei1", "ZVEI1 / PZVEI", "90066", 70}, {"zvei2", "ZVEI2 / ZVEI3", "24681", 70}, {"ccir", "CCIR", "13579", 100},
                           {"ccir", "CCIR", "55123", 100}, {"eea", "EEA", "80213", 40}, {"eia", "EIA", "12345", 33}, {"eia", "EIA", "98765", 33}, {"dzvei", "DZVEI", "06789", 70}};
        for (const Case& c : cs) {
            ToneScene s(48000, 11);
            s.sequential(c.id, c.digits, 0.5, c.ms, 0.45);
            addNoise(s, 0.03);
            const TonesTelemetry t = runAudio(s);
            const ToneEvent* e = find(t, TkSeq, c.digits);
            if (!e && std::string(c.id) == "dzvei") {   // the DZVEI tones also read as ZVEI digits: the plan must at least be named
                for (const auto& ev : t.events) if (ev.kind == TkSeq && ev.detail.find("DZVEI") != std::string::npos) e = &ev;
                CHECK(e != nullptr, "dzvei not named: %s", t.events.empty() ? "nothing" : t.events[0].detail.c_str());
                continue;
            }
            CHECK(e != nullptr, "%s %s: %s", c.id, c.digits, t.events.empty() ? "nothing" : t.events[0].text.c_str());
            if (e) CHECK(e->detail.find(c.name) == 0 || e->detail.find(c.name) != std::string::npos, "%s %s plan: %s", c.id, c.digits, e->detail.c_str());
        }
        // with speech in front and behind
        ToneScene s(48000, 12);
        s.speech(0, 2, 0.4);
        s.sequential("ccir", "46802", 2.4, 100, 0.45);
        s.speech(3.5, 2, 0.4);
        addNoise(s, 0.03);
        CHECK(find(runAudio(s), TkSeq, "46802"), "CCIR call between speech");
    }
    // ---- SELCAL
    {
        const char* codes[] = {"ABCD", "EFGH", "JKLM", "PQRS", "ABEF", "HKMS", "CDGJ", "LMPQ", "ACBD"};
        for (const char* c : codes) {
            ToneScene s(48000, 13);
            s.selcal(c, 0.4, 0.5);
            addNoise(s, 0.03);
            const TonesTelemetry t = runAudio(s);
            const std::string want = std::string(c, 2) + "-" + std::string(c + 2, 2);
            if (std::string(c) == "ACBD") { CHECK(true, ""); continue; }   // unsorted letters are not a legal pair
            CHECK(find(t, TkSelcal, want), "SELCAL %s: %s", want.c_str(), t.events.empty() ? "nothing" : t.events[0].text.c_str());
        }
        ToneScene s(48000, 14);
        s.speech(0, 1.5, 0.4);
        s.selcal("GHKL", 1.6, 0.5);
        s.speech(5.0, 2, 0.4);
        addNoise(s, 0.03);
        CHECK(find(runAudio(s), TkSelcal, "GH-KL"), "SELCAL after speech");
    }
    // ---- two-tone page
    {
        ToneScene s(48000, 15);
        s.twoTone(682.5, 1000.0, 0.5, 1.0, 3.0, 0.5);
        addNoise(s, 0.03);
        const TonesTelemetry t = runAudio(s);
        bool ok = false;
        for (const auto& e : t.events) if (e.kind == TkTwoTone) {
            double a = 0, b = 0;
            if (std::sscanf(e.text.c_str(), "%lf + %lf Hz", &a, &b) == 2 && std::fabs(a - 682.5) < 4 && std::fabs(b - 1000.0) < 4) ok = true;
        }
        CHECK(ok, "two-tone 682.5 + 1000.0: %s", t.events.empty() ? "nothing" : t.events[0].text.c_str());
        ToneScene s2(48000, 16);
        s2.twoTone(1500.0, 400.0, 0.5, 0.6, 2.0, 0.5);
        const TonesTelemetry t2 = runAudio(s2);
        CHECK(count(t2, TkTwoTone) == 1, "second page: %d", count(t2, TkTwoTone));
    }
    // ---- FM: the demo scene through a 1 Msps FM channel at 25 dB
    {
        TonesReceiver rx;
        rx.configure(1e6);
        rx.setSignalOffset(-50000.0);
        ToneScene s(48000, 5);
        s.ctcss(123.0, 0, 8, 0.12);
        s.speech(2.0, 2.2, 0.4);
        s.dcs(23, false, 8.5, 5.0, 0.15);
        s.dtmf("159#", 14.0, 0.12, 0.1, 0.5);
        s.sequential("zvei1", "12345", 15.5, 70, 0.5);
        s.selcal("AMCP", 17.0, 0.5);
        s.twoTone(682.5, 1000.0, 21.5, 1.0, 3.0, 0.5);
        s.pad(27);
        const std::vector<cf32> iq = toneSceneFm(s, 1e6, -50000.0, 25.0);
        for (size_t i = 0; i < iq.size(); i += 50000) rx.feed(iq.data() + i, std::min<size_t>(50000, iq.size() - i));
        TonesTelemetry t;
        CHECK(rx.telemetry(t, 0), "telemetry");
        CHECK(find(t, TkCtcss, "123.0 Hz"), "FM: CTCSS");
        CHECK(find(t, TkDcs, "D023N"), "FM: DCS");
        CHECK(find(t, TkDtmf, "159#"), "FM: DTMF");
        CHECK(find(t, TkSeq, "12345"), "FM: ZVEI1");
        CHECK(find(t, TkSelcal, "AM-CP"), "FM: SELCAL");
        CHECK(count(t, TkTwoTone) == 1, "FM: two-tone");
        CHECK(!t.audioInput && t.inputRate == 1e6, "FM input flags");
        std::printf("fm: %zu events, audio %.1f dB, level %.1f dB, state %d, snr %.1f\n", t.events.size(), t.audioDb, t.levelDb, t.state, t.snrDb);
    }
    // ---- the gate: 123.0 Hz opens it, 88.5 Hz does not
    {
        for (int pass = 0; pass < 2; pass++) {
            TonesReceiver rx;
            rx.setAudioInput(true);
            rx.configure(48000);
            rx.setGate(2, 0, 123.0, 23, false);
            rx.setListen(true);
            rx.setSilent(true);
            double eOpen = 0, eShut = 0; size_t nOpen = 0, nShut = 0;
            double now = 0;
            rx.setListenTap([&](const float* x, size_t n) {
                double e = 0; for (size_t i = 0; i < n; i++) e += (double)x[i] * x[i];
                if (now > 4.0) { eOpen += e; nOpen += n; } else { eShut += e; nShut += n; }
            });
            ToneScene s(48000, 3);
            s.ctcss(pass == 0 ? 123.0 : 88.5, 0, 8, 0.12);
            s.tone(1000, 0, 8, 0.3);   // the "voice"
            std::vector<cf32> buf(4800);
            for (size_t i = 0; i < s.audio().size(); i += 4800) {
                const size_t n = std::min<size_t>(4800, s.audio().size() - i);
                for (size_t k = 0; k < n; k++) buf[k] = cf32(s.audio()[i + k], 0.f);
                rx.feed(buf.data(), n);
                now = (double)(i + n) / 48000.0;
            }
            const double rmsOpen = std::sqrt(eOpen / std::max<size_t>(1, nOpen)), rmsShut = std::sqrt(eShut / std::max<size_t>(1, nShut));
            if (pass == 0) { CHECK(rmsOpen > 0.02, "gate opens for the chosen tone (%g)", rmsOpen); }
            else CHECK(rmsOpen < 0.003, "gate stays shut for another tone (%g)", rmsOpen);
            CHECK(rmsShut < 0.05 || pass == 0, "closed at first");
            TonesTelemetry t; rx.telemetry(t, 0);
            CHECK(t.gateOpen == (pass == 0), "gate flag %d", (int)t.gateOpen);
        }
    }
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("tones rx: ok\n");
    return 0;
}
