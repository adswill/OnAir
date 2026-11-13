// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// W1A voiceband modes (families 800 - 802): each decoder against its generator, with carrier offsets, noise, a sample clock error and the other
// signals (each detector must stay quiet on the others). Simulated only: no real recording went through this test. Short by default; the full
// sweep runs with ONAIR_TEST_LONG=1.
#include "dect2/w1a/w1a_dsp.h"
#include "dect2/w1a/w1a_rx.h"
#include "dect2/w1a/w1a_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
using namespace dect2::w1a;
using dect2::cf32;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Run { Status st; std::string text; std::vector<std::string> events; std::vector<float> audio; };
static Run run(int kind, const std::vector<cf32>& x, double forcedFc = 0) {
    auto d = makeDecoder(kind);
    Run r;
    d->setTextSink([&](const std::string& s) { r.text += s; });
    d->setEventSink([&](const std::string& s) { r.events.push_back(s); });
    d->setAudioSink([&](const float* a, size_t n) { r.audio.insert(r.audio.end(), a, a + n); });
    if (forcedFc > 0) d->setForced(forcedFc);
    for (size_t i = 0; i < x.size(); i += 480) d->push(x.data() + i, std::min<size_t>(480, x.size() - i));
    r.st = d->status();
    return r;
}
static std::string params(const Status& s) { std::string t; for (const auto& p : s.params) t += p.key + "=" + p.value + "; "; return t; }
static std::vector<float> genInvertedAudio(const std::vector<float>& s, double fc) { return invertAudio(s, fc); }
static size_t countLines(const std::string& t) { size_t n = 0; for (char c : t) n += c == '\n'; return n; }
static std::vector<cf32> shiftUsb(const std::vector<cf32>& x, double hz) {
    std::vector<cf32> y(x);
    for (size_t i = 0; i < y.size(); i++) { const double p = 2 * 3.14159265358979 * hz * (double)i / kRate; y[i] *= cf32((float)std::cos(p), (float)std::sin(p)); }
    return y;
}
// the correlation of the descrambled audio with the original speech-like audio, searching a small delay
static double audioMatch(const std::vector<float>& ref, const std::vector<float>& got) {
    double best = 0;
    for (int dly = 0; dly < 400; dly += 2) {
        double sab = 0, saa = 0, sbb = 0;
        const size_t from = 24000, to = std::min(ref.size(), got.size() > (size_t)dly ? got.size() - (size_t)dly : 0);
        for (size_t i = from; i < to; i++) { const double a = ref[i], b = got[i + (size_t)dly]; sab += a * b; saa += a * a; sbb += b * b; }
        if (saa > 0 && sbb > 0) best = std::max(best, sab / std::sqrt(saa * sbb));
    }
    return best;
}

// correlation of the long-term spectra (100 Hz bands in dB, 300 - 2900 Hz) of two audio signals
static double specCorr(const std::vector<float>& a, const std::vector<float>& b) {
    auto bands = [](const std::vector<float>& x) {
        std::vector<double> p(26, 0.0);
        for (size_t s0 = 24000; s0 + 1200 <= x.size(); s0 += 1200)
            for (int k = 0; k < 26; k++) {
                const double w = 2 * 3.14159265358979 * (300 + 100.0 * k + 50) / kRate;
                double re = 0, im = 0;
                for (int i = 0; i < 1200; i += 2) { re += x[s0 + (size_t)i] * std::cos(w * i); im += x[s0 + (size_t)i] * std::sin(w * i); }
                p[(size_t)k] += re * re + im * im;
            }
        for (double& v : p) v = 10 * std::log10(v + 1e-9);
        return p;
    };
    const std::vector<double> pa = bands(a), pb = bands(b);
    double ma = 0, mb = 0;
    for (int k = 0; k < 26; k++) { ma += pa[(size_t)k]; mb += pb[(size_t)k]; }
    ma /= 26; mb /= 26;
    double sab = 0, saa = 0, sbb = 0;
    for (int k = 0; k < 26; k++) { sab += (pa[(size_t)k] - ma) * (pb[(size_t)k] - mb); saa += (pa[(size_t)k] - ma) * (pa[(size_t)k] - ma); sbb += (pb[(size_t)k] - mb) * (pb[(size_t)k] - mb); }
    return sab / std::sqrt(saa * sbb + 1e-12);
}

int main() {
    const bool lng = getenv("ONAIR_TEST_LONG") != nullptr;

    // ---- HF packet 300 Bd
    {
        GenOpts o; o.frames = 4;
        const std::vector<cf32> clean = genPacket(o);
        Run r = run(Packet, clean);
        CHECK(r.st.good == 4 && countLines(r.text) == 4, "packet clean: %llu good, %llu bad, text: %s", (unsigned long long)r.st.good, (unsigned long long)r.st.bad, r.text.c_str());
        CHECK(r.text.find("N0CALL") != std::string::npos && r.text.find("CQ") != std::string::npos && r.text.find("HF packet test frame 4 of 4") != std::string::npos, "packet text: %s", r.text.c_str());
        // a long burst, with the tones off by up to 40 Hz either way (SSB tuning), noise and a sample clock error
        o.frames = 8;
        const std::vector<cf32> x8 = genPacket(o);
        const double offs[] = {-40, 0, 35};
        for (double off : offs) {
            Run q = run(Packet, impair(shiftUsb(x8, off), 0, 25, 0, 3));
            CHECK(q.st.good >= 7, "packet tone offset %.0f Hz: %llu of 8 frames", off, (unsigned long long)q.st.good);
        }
        Run p = run(Packet, impair(x8, 0, 25, 300, 5));
        CHECK(p.st.good >= 7, "packet +300 ppm: %llu of 8", (unsigned long long)p.st.good);
        const double snrs[] = {18, 12, 8};
        for (double snr : snrs) {
            Run q = run(Packet, impair(x8, 0, snr, 0, 9));
            printf("packet SNR %.0f dB: %llu of 8 frames\n", snr, (unsigned long long)q.st.good);
            if (snr >= 18) CHECK(q.st.good >= 7, "packet SNR %.0f: %llu of 8", snr, (unsigned long long)q.st.good);
        }
        // noise alone, an ELT sweep and speech must not give a frame
        GenOpts e; e.seconds = 6;
        Run n1 = run(Packet, impair(genElt(e), 0, 20, 0, 4));
        GenOpts v; v.seconds = 6; v.invertHz = 0;
        Run n2 = run(Packet, impair(genInvert(v), 0, 20, 0, 4));
        CHECK(n1.st.good == 0 && n2.st.good == 0, "packet false frames: %llu, %llu", (unsigned long long)n1.st.good, (unsigned long long)n2.st.good);
    }

    // ---- voice inversion
    {
        GenOpts o; o.seconds = lng ? 20 : 10;
        const std::vector<float> speech = genSpeechLike(o);
        const double fcs[] = {2800, 3000, 3300, 3500};
        for (double fc : fcs) {
            o.invertHz = fc;
            Run r = run(Invert, impair(genInvert(o), 120, 28, 0, 11));
            const bool ok = r.st.state == 2 && std::fabs(r.st.cfoHz - fc) <= 200;
            CHECK(ok, "inversion at %.0f Hz: state %d, found %.0f (%s) %s", fc, r.st.state, r.st.cfoHz, r.st.mode.c_str(), params(r.st).c_str());
            if (fc == 3000 || fc == 3300) {
                const double m = specCorr(speech, r.audio), raw = specCorr(speech, genInvertedAudio(speech, fc));
                printf("inversion %.0f Hz: found %.0f Hz, spectrum match of the descrambled audio %.2f (the inverted audio: %.2f)\n", fc, r.st.cfoHz, m, raw);
                CHECK(m > 0.7 && m > raw + 0.25, "descrambled audio spectrum match %.2f at %.0f Hz (inverted %.2f)", m, fc, raw);
            }
        }
        // not inverted speech: judged clear, no descrambling
        o.invertHz = 0;
        Run c = run(Invert, impair(genInvert(o), 0, 28, 0, 12));
        CHECK(c.st.state == 1 && c.st.cfoHz == 0 && c.st.mode.find("not inverted") != std::string::npos, "clear speech: state %d, %s, %s", c.st.state, c.st.mode.c_str(), params(c.st).c_str());
        // noise sweep and a clock error
        o.invertHz = 3000;
        const double snrs[] = {28, 22, 16};
        for (double snr : snrs) {
            Run r = run(Invert, impair(genInvert(o), -200, snr, 150, 13));
            printf("inversion SNR %.0f dB: state %d, carrier %.0f Hz\n", snr, r.st.state, r.st.cfoHz);
            if (snr >= 22) CHECK(r.st.state == 2 && std::fabs(r.st.cfoHz - 3000) <= 200, "inversion SNR %.0f: state %d, %.0f Hz", snr, r.st.state, r.st.cfoHz);
        }
        // a forced carrier descrambles without detection
        Run f = run(Invert, impair(genInvert(o), 0, 40, 0, 14), 3000);
        CHECK(specCorr(speech, f.audio) > 0.9 && audioMatch(speech, f.audio) > 0.5, "forced carrier: spectrum %.2f, waveform match %.2f", specCorr(speech, f.audio), audioMatch(speech, f.audio));
        // other signals: nothing found
        // (an AM signal such as the ELT's gives the FM discriminator noise bursts that follow its tone; that is not tested: use the AM mode there)
        Run n1 = run(Invert, impair(std::vector<cf32>(96000, cf32(0.5f, 0)), 0, 22, 0, 5));   // an unmodulated carrier in noise
        GenOpts p; p.frames = 8;
        Run n2 = run(Invert, impair(genPacket(p), 0, 25, 0, 5));
        CHECK(n1.st.state != 2 && n2.st.state != 2, "inversion false detection on a bare carrier (%d) or packet (%d)", n1.st.state, n2.st.state);
    }

    // ---- ELT
    {
        GenOpts o; o.seconds = 6;
        Run r = run(Elt, impair(genElt(o), 0, 30, 0, 21));
        CHECK(r.st.state == 2 && std::fabs(r.st.cfoHz - 3.0) < 0.3, "ELT clean: state %d, rate %.2f (%s)", r.st.state, r.st.cfoHz, r.st.mode.c_str());
        const double rates[] = {2.0, 2.5, 3.5, 4.0};
        for (double sr : rates) {
            o.sweepHz = sr;
            Run q = run(Elt, impair(genElt(o), 800, 25, 100, 22));
            CHECK(q.st.state == 2 && std::fabs(q.st.cfoHz - sr) < 0.35, "ELT %.1f sweeps/s: state %d, rate %.2f", sr, q.st.state, q.st.cfoHz);
        }
        const double duty[] = {35, 50, 66, 100};
        for (double dp : duty) {
            o.sweepHz = 3; o.dutyPct = dp;
            Run q = run(Elt, impair(genElt(o), -1500, 25, 0, 23));
            CHECK(q.st.state == 2, "ELT duty %.0f %%: state %d (%s)", dp, q.st.state, q.st.mode.c_str());
        }
        o.dutyPct = 60;
        const double snrs[] = {20, 10, 5};
        for (double snr : snrs) {
            Run q = run(Elt, impair(genElt(o), 0, snr, 0, 24));
            printf("ELT SNR %.0f dB: state %d, rate %.2f\n", snr, q.st.state, q.st.cfoHz);
            if (snr >= 10) CHECK(q.st.state == 2, "ELT SNR %.0f: state %d", snr, q.st.state);
        }
        // not an ELT: a tone sweeping UPWARDS, a steady tone, speech, noise, a packet burst
        GenOpts up = o; up.highHz = 300; up.lowHz = 1600;   // sweeps upwards
        Run u = run(Elt, impair(genElt(up), 0, 25, 0, 25));
        GenOpts st = o; st.highHz = 1000; st.lowHz = 1000;
        Run t = run(Elt, impair(genElt(st), 0, 25, 0, 26));
        GenOpts sp; sp.seconds = 8; sp.invertHz = 0;
        Run s = run(Elt, impair(genInvert(sp), 0, 25, 0, 27));
        GenOpts pk; pk.frames = 8;
        Run k = run(Elt, impair(genPacket(pk), 0, 25, 0, 28));
        CHECK(u.st.state != 2 && t.st.state != 2 && s.st.state != 2 && k.st.state != 2, "ELT false detections: up %d, steady %d, speech %d, packet %d", u.st.state, t.st.state, s.st.state, k.st.state);
        std::vector<cf32> noise(72000);
        Run nz = run(Elt, impair(std::vector<cf32>(noise.size(), cf32(0.5f, 0)), 0, 20, 0, 29));
        CHECK(nz.st.state != 2, "ELT false detection on a plain carrier with noise");
    }

    // ---- the receiver (engine contract) and the numbers
    {
        for (int kind = 0; kind < dect2::kW1aKinds; kind++) {
            const dect2::ModeTuning tn = dect2::w1aTuning(kind);
            CHECK(tn.stdMode == dect2::kW1aFamily0 + 2 + kind, "tuning number of kind %d: %d", kind, tn.stdMode);
        }
        dect2::SynthConfig sc; sc.cfoHz = 0; sc.snrDb = 30;
        const double rate = 250000;
        auto syn = dect2::makeW1aSynth(2, sc, rate);
        dect2::W1aReceiver rx(2);
        rx.configure(rate);
        rx.setSignalOffset(-dect2::w1aTuning(2).tuneOffsetHz);
        CHECK(rx.ready(), "receiver not ready at 250 kHz");
        std::vector<cf32> buf(25000);
        for (int i = 0; i < 80; i++) { syn->generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }   // 8 s
        dect2::W1aTelemetry t;
        CHECK(rx.telemetry(t, 0), "no telemetry");
        CHECK(t.state == 2 && t.kind == 2 && std::fabs(t.cfoHz - 3.0) < 0.4, "receiver ELT: state %d, rate %.2f, %s", t.state, t.cfoHz, t.mode.c_str());
        auto syn1 = dect2::makeW1aSynth(0, sc, rate);
        dect2::W1aReceiver rp(0);
        rp.configure(rate);
        rp.setSignalOffset(-dect2::w1aTuning(0).tuneOffsetHz);
        for (int i = 0; i < 80; i++) { syn1->generate(buf.data(), buf.size()); rp.feed(buf.data(), buf.size()); }
        dect2::W1aTelemetry tp;
        CHECK(rp.telemetry(tp, 0) && tp.blocksOk >= 3, "receiver packet: %llu frames, %s", (unsigned long long)tp.blocksOk, tp.mode.c_str());
        auto syn2 = dect2::makeW1aSynth(1, sc, rate);
        dect2::W1aReceiver rv(1);
        rv.configure(rate);
        rv.setSignalOffset(-dect2::w1aTuning(1).tuneOffsetHz);
        for (int i = 0; i < 120; i++) { syn2->generate(buf.data(), buf.size()); rv.feed(buf.data(), buf.size()); }
        dect2::W1aTelemetry tv;
        CHECK(rv.telemetry(tv, 0) && tv.state == 2, "receiver inversion: state %d, %s", tv.state, tv.mode.c_str());
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("w1a: all checks passed\n");
    return 0;
}
