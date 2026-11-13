// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// QO-100 narrowband auto decoder and SSB conversation manager on a synthetic passband (qo100_auto.h):
//  1. one 500 kHz passband with FT8 (several stations), FT4, PSK31, CW, SSTV and two SSB conversations (two stations taking turns on one
//     frequency, and a later single station) at different levels: every signal is found and classified, the ticked decoders decode their
//     signals (FT8 / FT4 messages, PSK31 and CW text, an SSTV picture), the conversations are grouped (two, with 2 stations estimated in the
//     first), Listen on a conversation moves the dial to it and plays audio, the recording is written, speech to text finds a callsign
//  2. the CPU guard: a tiny limit drops the channels; the channel limit holds
//  3. unticked decoders do nothing; the callsign finder
#include "dect2/cw/cw_gen.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/psk/psk_gen.h"
#include "dect2/qo100/qo100_auto.h"
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/qo100/qo100_rx.h"
#include "dect2/qo100/qo100_tel.h"
#include "dect2/source/source.h"
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/test_long.h"

namespace {

constexpr double kFs = 2.0e6;
constexpr double kCentreRf = qo100::kNarrowCentreHz;   // 10490.1 MHz: the radio
constexpr double kT0 = 1767225600.0;                   // a UTC second at the start of a slot of every mode
constexpr double kPi = 3.14159265358979323846;

struct Rng {
    uint64_t s = 88172645463325252ull;
    double u() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return (double)(s >> 11) / 9007199254740992.0; }
};

// speech-like voice: harmonics of a pitch, 300 .. 2600 Hz, a syllable envelope with pauses; the analytic signal at 48 kHz, played in turns
struct Voice {
    std::vector<cf32> a;            // 48 kHz analytic signal of the whole run
    double dialOffHz = 0;           // the dial against the radio's centre
    std::complex<double> rot{1, 0}, step{1, 0};
    void build(double secs, double f0, double rms, const std::vector<std::pair<double, double>>& turns, uint64_t seed) {
        const int R = 48000;
        const size_t N = (size_t)(secs * R);
        a.assign(N, cf32(0, 0));
        Rng rng; rng.s ^= seed * 0x9E3779B97F4A7C15ull;
        std::vector<float> env(N, 0.f);
        for (const auto& t : turns) {
            double at = t.first;
            while (at < t.second) {
                const double syl = 0.18 + 0.2 * rng.u(), gap = 0.05 + 0.25 * rng.u();
                for (size_t i = (size_t)(at * R); i < std::min(N, (size_t)((at + syl) * R)); i++) {
                    const double x = ((double)i / R - at) / syl;
                    env[i] = (float)std::sin(kPi * x);
                }
                at += syl + gap;
                if (rng.u() < 0.12) at += 0.6;   // a breath
            }
        }
        std::vector<std::pair<double, double>> parts;   // (frequency, amplitude)
        for (int h = (int)std::ceil(300 / f0); h * f0 <= 2600; h++) {
            const double fr = h * f0, form = 1.0 / (1.0 + std::pow((fr - 700) / 500, 2)) + 0.6 / (1.0 + std::pow((fr - 1800) / 700, 2));
            parts.push_back({fr, form});
        }
        double norm = 0;
        for (auto& p : parts) norm += p.second * p.second;
        const double amp = rms / std::sqrt(norm);
        std::vector<std::complex<double>> ph(parts.size(), std::complex<double>(1, 0)), st(parts.size());
        for (size_t k = 0; k < parts.size(); k++) st[k] = std::polar(1.0, 2 * kPi * parts[k].first / R);
        for (size_t i = 0; i < N; i++) {
            if (env[i] <= 0.f) { for (size_t k = 0; k < parts.size(); k++) ph[k] *= st[k]; continue; }
            std::complex<double> v(0, 0);
            for (size_t k = 0; k < parts.size(); k++) { v += parts[k].second * ph[k]; ph[k] *= st[k]; }
            a[i] = cf32((float)(v.real() * amp * env[i]), (float)(v.imag() * amp * env[i]));
            if ((i & 4095) == 0) for (auto& p : ph) p /= std::abs(p);
        }
        rot = {1, 0};
    }
    void add(cf32* out, size_t n, size_t at) {   // adds samples at..at+n of the run (2 Msps), linear interpolation from 48 kHz
        step = std::polar(1.0, 2 * kPi * dialOffHz / kFs);
        for (size_t i = 0; i < n; i++) {
            const double pos = (double)(at + i) * 48000.0 / kFs;
            const size_t k = (size_t)pos;
            if (k + 1 < a.size()) {
                const float f = (float)(pos - (double)k);
                const cf32 v = a[k] + (a[k + 1] - a[k]) * f;
                out[i] += v * cf32((float)rot.real(), (float)rot.imag());
            }
            rot *= step;
        }
        rot /= std::abs(rot);
    }
    void seek(size_t at) { rot = std::polar(1.0, 2 * kPi * dialOffHz * (double)at / kFs); }
};

struct Scene {
    std::vector<std::function<void(cf32*, size_t, size_t)>> src;   // out, n, first sample index
    size_t pos = 0;
    void gen(cf32* out, size_t n) {
        std::fill(out, out + n, cf32(0, 0));
        for (auto& s : src) s(out, n, pos);
        pos += n;
    }
};

// dial offsets of the presets (downlink) against the radio's centre
double offOf(double rfHz) { return rfHz - kCentreRf; }

void addMode(Scene& sc, std::shared_ptr<ModeSynth> syn) {
    sc.src.push_back([syn](cf32* out, size_t n, size_t) {
        static thread_local std::vector<cf32> t;
        t.resize(n);
        syn->generate(t.data(), n);
        for (size_t i = 0; i < n; i++) out[i] += t[i];
    });
}

Scene makeScene(double secs, bool full) {
    Scene sc;
    // FT8 (the test stations of the FT8 generator) at the FT8 preset 10489.540
    {
        SynthConfig c; c.modeOpt[0] = 3; c.snrDb = 22; c.cfoHz = offOf(10489.540e6) + 20000;
        addMode(sc, makeHfdigSynthFromAudio(makeFtxTestAudio(c), c, kFs));
    }
    if (!full) return sc;
    {   // FT4 at 10489.556
        SynthConfig c; c.modeOpt[0] = 4; c.snrDb = 22; c.cfoHz = offOf(10489.556e6) + 20000;
        addMode(sc, makeHfdigSynthFromAudio(makeFtxTestAudio(c), c, kFs));
    }
    {   // SSTV Robot 36 at 10489.590
        SynthConfig c; c.snrDb = 30; c.cfoHz = offOf(10489.590e6) + 20000;
        addMode(sc, makeHfdigSynthFromAudio(makeSstvTx(0, {}), c, kFs));
    }
    {   // PSK31 at 10489.562 + 1 kHz
        PskGenConfig pc; pc.rate = kFs; pc.dialOffsetHz = offOf(10489.562e6); pc.noiseSigma = 0.01;
        PskGenSignal s; s.offsetHz = 1000; s.snrDb = 48; s.text = "CQ CQ DE LY2ABC LY2ABC PSE K\n"; s.pauseSec = 0.5;
        pc.signals.push_back(s);
        auto g = std::make_shared<PskGenerator>(pc);
        sc.src.push_back([g](cf32* out, size_t n, size_t) {
            static thread_local std::vector<cf32> t; t.resize(n); g->generate(t.data(), n);
            for (size_t i = 0; i < n; i++) out[i] += t[i];
        });
    }
    {   // CW at 10489.520 + 800 Hz
        CwGenConfig cc; cc.rate = kFs; cc.dialOffsetHz = offOf(10489.520e6); cc.noiseSigma = 0.01;
        CwGenSignal s; s.offsetHz = 800; s.wpm = 18; s.snrDb = 45; s.text = "CQ DE LY2ABC K"; s.pauseSec = 1.5;
        cc.signals.push_back(s);
        auto g = std::make_shared<CwGenerator>(cc);
        sc.src.push_back([g](cf32* out, size_t n, size_t) {
            static thread_local std::vector<cf32> t; t.resize(n); g->generate(t.data(), n);
            for (size_t i = 0; i < n; i++) out[i] += t[i];
        });
    }
    // two SSB conversations: stations A (pitch 110 Hz, dial 10489.7000) and B (pitch 170 Hz, 10489.7006, 5 dB weaker) in turns; later C on 10489.800
    auto A = std::make_shared<Voice>(), B = std::make_shared<Voice>(), C = std::make_shared<Voice>();
    A->dialOffHz = offOf(10489.7000e6); B->dialOffHz = offOf(10489.7006e6); C->dialOffHz = offOf(10489.8000e6);
    A->build(secs, 110, 0.09, {{3, 9}, {17, 23}, {31, 37}, {45, 51}, {59, 65}}, 1);
    B->build(secs, 170, 0.05, {{10, 16}, {24, 30}, {38, 44}, {52, 58}}, 2);
    C->build(secs, 140, 0.07, {{25, 33}, {36, 44}, {47, 53}}, 3);
    for (auto& v : {A, B, C}) sc.src.push_back([v](cf32* out, size_t n, size_t at) { if (at == 0) v->seek(0); v->add(out, n, at); });
    return sc;
}

struct Result {
    Qo100Telemetry last;
    std::vector<Qo100AutoTelemetry> snaps;
    double secs = 0, cpuSec = 0;
    size_t tapN = 0; double tapPow = 0;
};

Result runScene(Scene& sc, double secs, const std::function<void(Qo100Receiver&)>& setup, const std::function<void(Qo100Receiver&, double)>& onSec = nullptr) {
    Result r;
    Qo100Receiver rx;
    rx.configure(kFs);
    rx.setLo(9750e6);
    rx.setCenterHz(qo100::ifOf(kCentreRf, 9750e6));
    rx.setSilent(true);
    rx.setAutoStartTime(kT0);
    rx.setListenTap([&](const float* x, size_t n) { r.tapN += n; for (size_t i = 0; i < n; i++) r.tapPow += (double)x[i] * x[i]; });
    setup(rx);
    std::vector<cf32> buf(65536);
    uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const size_t total = (size_t)(secs * kFs);
    int sec = 0;
    for (size_t at = 0; at < total; at += buf.size()) {
        const size_t n = std::min(buf.size(), total - at);
        sc.gen(buf.data(), n);
        rx.feed(buf.data(), n);
        Qo100Telemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; r.last = std::move(t); }
        if ((double)(at + n) / kFs >= sec + 1) { sec++; if (onSec) onSec(rx, sec); }
    }
    for (int i = 0; i < 20; i++) { rx.feed(buf.data(), 0); }
    Qo100Telemetry t;
    if (rx.telemetry(t, 0)) r.last = std::move(t);
    r.secs = secs;
    r.cpuSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

const Qo100Track* trackNear(const Qo100AutoTelemetry& a, double rf, double tol, int cls = -1) {
    const Qo100Track* best = nullptr;
    for (const auto& t : a.tracks) {
        if (cls >= 0 && t.cls != cls) continue;
        if (rf >= t.loHz - tol && rf <= t.hiHz + tol) { if (!best || t.snrDb > best->snrDb) best = &t; }
    }
    return best;
}

bool hasText(const Qo100AutoTelemetry& a, int kind, const char* needle) {
    for (const auto& t : a.texts) if (t.kind == kind && t.text.find(needle) != std::string::npos) return true;
    return false;
}
int decodesOf(const Qo100AutoTelemetry& a, int kind, const char* needle = nullptr) {
    int n = 0;
    for (const auto& d : a.decodes) if (d.kind == kind && (!needle || d.text.find(needle) != std::string::npos)) n++;
    return n;
}

void testCalls() {
    auto c = qo100CallsFromText("CQ CQ de LY2ABC and dl1xyz, also 9A2AB /P KO24 73");
    bool ly = false, dl = false, nine = false;
    for (auto& s : c) { ly |= s == "LY2ABC"; dl |= s == "DL1XYZ"; nine |= s == "9A2AB"; }
    CHECK(ly && dl && nine, "plain callsigns found (%zu)", c.size());
    CHECK(!qo100CallsFromText("hello world kilo oscar").size(), "no false callsigns in plain words");
    auto s = qo100CallsFromText("this is lima yankee two alpha bravo charlie calling", true);
    CHECK(s.size() == 1 && s[0] == "LY2ABC", "spoken phonetic callsign (%s)", s.empty() ? "none" : s[0].c_str());
    auto s2 = qo100CallsFromText("delta lima one x-ray yankee zulu over", true);
    CHECK(s2.size() == 1 && s2[0] == "DL1XYZ", "spoken callsign with x-ray (%s)", s2.empty() ? "none" : s2[0].c_str());
}

} // namespace

int main(int argc, char** argv) {
    // the default run: 50 s of the full passband (the --quick length) and 20 s for each guard; ONAIR_TEST_LONG=1 105 s and 40 s; --quick skips the guards
    const bool argQuick = argc > 1 && std::string(argv[1]) == "--quick";
    const bool quick = argQuick || !testLong();
    testCalls();

    const double secs = quick ? 50 : 105;
    // ---- 1. the full passband
    {
        Scene sc = makeScene(secs, true);
        // Platform temp dir: "/tmp" does not exist on Windows, and "mkdir -p" is not available there either.
        const std::string dir = (std::filesystem::temp_directory_path() / "qo100-auto-rec").string();
        { std::error_code ec; std::filesystem::create_directories(dir, ec); }
        int listenSeen = 0;
        int convForListen = 0, convRec = 0;
        std::vector<Qo100AutoTelemetry> snaps;
        double dialAtListen = 0;
        auto r = runScene(sc, secs, [&](Qo100Receiver& rx) {
            rx.setAutoDecoders(kQoDecSsb | kQoDecFt8 | kQoDecFt4 | kQoDecSstv | kQoDecCw | kQoDecPsk | kQoDecJs8);
            rx.setAutoMaxSlots(10);
            rx.setAutoCpuLimit(8);
            rx.setRecordDir(dir);
            rx.setSquelch(true);
            rx.setSpeechToText([](const float*, size_t, double) { return std::string("this is lima yankee two alpha bravo charlie calling"); });
        }, [&](Qo100Receiver& rx, double s) {
            { Qo100Telemetry t; if (rx.telemetry(t, 0)) snaps.push_back(t.autoDec); }
            if (s == 20) {
                Qo100Telemetry t;
                if (rx.telemetry(t, 0)) for (const auto& c : t.autoDec.convs) if (c.loHz > 10489.69e6 && c.loHz < 10489.71e6) { convRec = c.id; rx.setRecordConversation(c.id, true); }
            }
            if (s == 35) {   // Listen to the second conversation (the one on 10489.800) once it exists
                Qo100Telemetry t;
                // (telemetry is read by the loop; ask the receiver for the newest here)
                if (rx.telemetry(t, 0)) for (const auto& c : t.autoDec.convs) if (c.loHz > 10489.78e6 && c.active) { convForListen = c.id; }
                if (convForListen) { rx.setListenConversation(convForListen); rx.setListen(true); }
            }
        });
        const Qo100AutoTelemetry& a = r.last.autoDec;
        printf("1. %zu tracks, %zu slots, %zu decodes, %zu texts, %zu images, %zu convs, cpu %.2f, ran %.0f s for %.0f s of signal\n", a.tracks.size(), a.slots.size(),
               a.decodes.size(), a.texts.size(), a.images.size(), a.convs.size(), a.cpuLoad, r.cpuSec, r.secs);
        for (const auto& t : a.tracks) printf("   track %d %.4f MHz w %.0f lev %.1f snr %.1f %s%s dec %d\n", t.id, t.rfHz / 1e6, t.widthHz, t.levelDb, t.snrDb, qo100ClassName(t.cls), t.active ? " on" : "", t.decMask);
        for (const auto& c : a.convs) printf("   conv %d dial %.4f lo %.4f lev %.1f stations %d talk %.1f s drift %.0f %s %s calls %zu rec %d\n", c.id, c.dialHz / 1e6, c.loHz / 1e6, c.levelDb, c.stations, c.talkSec, c.driftHzPerMin, c.active ? "active" : "ended", c.listening ? "listening" : "", c.calls.size(), c.recording);
        for (const auto& s : a.slots) printf("   slot %d %s dial %.4f tracks %d load %.3f results %llu\n", s.id, qo100DecoderName(s.kind), s.dialHz / 1e6, s.tracks, s.load, (unsigned long long)s.results);

        if (getenv("QO_DEBUG")) for (const auto& g : r.last.signals) printf("   signal %.4f MHz w %.0f snr %.1f cls %s\n", g.rfHz / 1e6, g.widthHz, g.snrDb, qo100ClassName(g.cls));
        auto everClass = [&](double rf, double tol, int cls) {
            for (const auto& sn : snaps) if (trackNear(sn, rf, tol, cls)) return true;
            return false;
        };
        auto classesAt = [&](double rf, double tol) {
            std::string out;
            for (const auto& sn : snaps) if (const Qo100Track* t = trackNear(sn, rf, tol)) { const std::string n = qo100ClassName(t->cls); if (out.find(n) == std::string::npos) out += n + " "; }
            return out;
        };
        CHECK(everClass(10489.5415e6, 600, kQoSlot15), "FT8 signals are a 15 s slot class (%s)", classesAt(10489.5415e6, 600).c_str());
        CHECK(everClass(10489.5575e6, 600, kQoFt4), "FT4 signals are the FT4 class (%s)", classesAt(10489.5575e6, 600).c_str());
        CHECK(everClass(10489.5925e6, 600, kQoSstv), "SSTV is steady and about 1.3 kHz wide (%s)", classesAt(10489.5925e6, 600).c_str());
        CHECK(everClass(10489.5208e6, 300, kQoCw), "CW is keyed (%s)", classesAt(10489.5208e6, 300).c_str());
        CHECK(everClass(10489.563e6, 300, kQoPsk) || everClass(10489.563e6, 300, kQoCarrier), "PSK31 is narrow and continuous (%s)", classesAt(10489.563e6, 300).c_str());
        CHECK(everClass(10489.701e6, 1500, kQoSsb) && everClass(10489.801e6, 1500, kQoSsb), "SSB signals on both frequencies");
        int ssbMax = 0;
        for (const auto& sn : snaps) { int k = 0; for (const auto& t : sn.tracks) k += t.cls == kQoSsb && t.active; ssbMax = std::max(ssbMax, k); }
        CHECK(ssbMax >= 2, "two SSB signals at once (%d)", ssbMax);

        CHECK(decodesOf(a, kQoDecFt8) >= 4, "FT8 decodes (%d)", decodesOf(a, kQoDecFt8));
        CHECK(decodesOf(a, kQoDecFt8, "K1ABC") >= 2, "FT8 test messages (%d)", decodesOf(a, kQoDecFt8, "K1ABC"));
        CHECK(decodesOf(a, kQoDecFt4) >= 2, "FT4 decodes (%d)", decodesOf(a, kQoDecFt4));
        for (const auto& d : a.decodes) if (d.kind == kQoDecFt8) { CHECK(d.rfHz > 10489.540e6 && d.rfHz < 10489.546e6, "FT8 decode frequency %.4f MHz inside the segment", d.rfHz / 1e6); break; }
        CHECK(hasText(a, kQoDecPsk, "LY2ABC"), "PSK31 text");
        CHECK(hasText(a, kQoDecCw, "LY2"), "CW text");
        CHECK(!a.images.empty(), "an SSTV picture (%zu)", a.images.size());
        if (!a.images.empty() && !quick) CHECK(a.images[0]->width == 320 && a.images[0]->lines > 100, "SSTV picture %dx%d, %d lines", a.images[0]->width, a.images[0]->height, a.images[0]->lines);

        // conversations
        CHECK(a.convs.size() == 2, "two conversations (%zu)", a.convs.size());
        const Qo100Conversation* c1 = nullptr; const Qo100Conversation* c2 = nullptr;
        for (const auto& c : a.convs) { if (c.loHz < 10489.75e6) c1 = &c; else c2 = &c; }
        CHECK(c1 && c1->id == convRec, "the recorded conversation is the first one (%d, %d)", c1 ? c1->id : -1, convRec);
        CHECK(c1 && c2, "one conversation on each frequency");
        if (c1) {
            CHECK(c1->stations >= 2 && c1->stations <= 3, "stations in conversation 1: %d", c1->stations);
            CHECK(c1->talkSec > 25, "talk time %.1f s", c1->talkSec);
            CHECK(std::fabs(c1->dialHz - 10489.7000e6) < 400, "its dial is %.5f MHz", c1->dialHz / 1e6);
            CHECK(c1->recording && !c1->recordPath.empty(), "conversation 1 is recorded");
            CHECK(!c1->calls.empty() && c1->calls[0] == "LY2ABC", "callsign from speech to text (%zu)", c1->calls.size());
        }
        if (c2) {
            CHECK(c2->stations == 1, "stations in conversation 2: %d", c2->stations);
            CHECK(c1 && c1->startSec < c2->startSec, "conversation 1 started first");
            CHECK(c1 && c1->levelDb > c2->levelDb - 15, "levels are measured");
        }
        CHECK(convForListen != 0 && r.last.autoDec.listenConv == convForListen, "Listen follows conversation %d (%d)", convForListen, r.last.autoDec.listenConv);
        CHECK(r.tapN > 8000 * 5, "audio played while listening to a conversation (%zu samples)", r.tapN);
        CHECK(std::fabs(r.last.dialHz - 10489.8000e6) < 500, "the dial moved to the conversation (%.5f MHz)", r.last.dialHz / 1e6);
        (void)listenSeen; (void)dialAtListen;
        CHECK(a.cpuLoad >= 0, "cpu meter present (%.2f)", a.cpuLoad);
        CHECK(a.slots.size() >= 3, "decoder channels running (%zu)", a.slots.size());
        FILE* f = c1 ? fopen(c1->recordPath.c_str(), "rb") : nullptr;
        CHECK(f != nullptr, "recording file written");
        if (f) { fseek(f, 0, SEEK_END); const long sz = ftell(f); fclose(f); CHECK(sz > 8000 * 2 * 10, "recording is %ld bytes", sz); }
    }
    if (argQuick) return testSummary("qo100_auto");
    const double gSecs = testLongPick(20.0, 40.0);

    // ---- 2. the guards
    {
        Scene sc = makeScene(gSecs, true);
        auto r = runScene(sc, gSecs, [&](Qo100Receiver& rx) { rx.setAutoDecoders(kQoDecFt8 | kQoDecFt4 | kQoDecCw | kQoDecPsk); rx.setAutoMaxSlots(10); rx.setAutoCpuLimit(0.0005f); });
        const auto& a = r.last.autoDec;
        printf("2. cpu guard: %d dropped, %zu slots left\n", a.dropped, a.slots.size());
        CHECK(a.dropped >= 1, "the CPU limit drops channels (%d)", a.dropped);
        CHECK(a.slots.size() <= 1, "no channel survives a tiny limit (%zu)", a.slots.size());
    }
    {
        Scene sc = makeScene(gSecs, true);
        auto r = runScene(sc, gSecs, [&](Qo100Receiver& rx) { rx.setAutoDecoders(kQoDecFt8 | kQoDecFt4 | kQoDecCw | kQoDecPsk); rx.setAutoMaxSlots(1); rx.setAutoCpuLimit(8); });
        const auto& a = r.last.autoDec;
        printf("   slot limit: %zu slots\n", a.slots.size());
        CHECK(a.slots.size() <= 1, "the channel limit holds (%zu)", a.slots.size());
    }
    {
        Scene sc = makeScene(gSecs, true);
        auto r = runScene(sc, gSecs, [&](Qo100Receiver& rx) { rx.setAutoDecoders(0); });
        const auto& a = r.last.autoDec;
        CHECK(a.slots.empty() && a.decodes.empty() && a.texts.empty(), "nothing ticked, nothing decoded");
        CHECK(a.tracks.size() >= 4, "the signals are still listed (%zu)", a.tracks.size());
    }
    return testSummary("qo100_auto");
}
