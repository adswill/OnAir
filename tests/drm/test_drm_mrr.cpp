// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The six test cases of the DRM Minimum Receiver Requirements (DRM MRR v2, 2015): the lowest carrier to noise ratio at which the bit error rate of
// the main service channel stays below 1e-4, on the channel models of Annex B.1:
//   1 A, 64-QAM, rate 0.78 (protection level 3), short interleaver, channel 1 (AWGN)
//   2 A, 64-QAM, rate 0.71 (level 2), channel 2 (Rice with delay)
//   3 B, 64-QAM, rate 0.71, long interleaver, channel 3 (US consortium)
//   4 B, 64-QAM, rate 0.6 (level 1), channel 4 (CCIR poor)
//   5 C, 16-QAM, rate 0.62 (level 1), channel 5
//   6 D, 64-QAM, rate 0.5 (level 0), channel 6
// The bit errors are counted in the audio super frames against the transmitter's own (the same encoder and sound, testkit.h); a super frame that
// does not come out, or cannot be read, counts as half its bits wrong. The table is printed; the checks only catch a receiver that broke (5 dB
// above the values measured when the test was written). Labelled slow (about 6 minutes on one core per case, the cases run in parallel).
#include "../data/drm/testkit.h"
#include "../common/jobs.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
using namespace drmtest;
using testjobs::jprintf;
static std::atomic<int> fails{0};
static testjobs::Jobs jobs;
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

struct Ber { double ber = 1; uint64_t frames = 0, expected = 0; double bits = 0, errs = 0; double lockSecs = -1; DrmTelemetry tel; uint64_t lost = 0; };

static int popc(uint8_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

// one run: BER over the super frames that end after `skip` seconds
static Ber runBer(const Sig& s, double secs, double skip) {
    const double rate = 48000;
    Ber b;
    Gen gen = synthGen(s, rate);
    if (!gen) return b;
    DrmReceiver rx;
    rx.setSilent(true);
    rx.configure(rate);
    rx.setLogCallback([&b](const std::string& l) { if (l.find("signal lost") != std::string::npos) b.lost++; });
    double now = 0;
    std::unique_ptr<DrmAudioSource> refSrc;
    std::vector<std::vector<std::vector<uint8_t>>> refFrames;
    int refPayload = -1, payloadBits = 0;
    long lastRef = -1;
    auto ensure = [&](long j) {
        while ((size_t)j >= refFrames.size() && refFrames.size() < 4000) {
            std::vector<std::vector<uint8_t>> fr; std::vector<uint8_t> crc;
            refSrc->nextSuperFrame(10, refPayload, fr, crc);
            refFrames.push_back(std::move(fr));
        }
        return (size_t)j < refFrames.size();
    };
    auto diffBits = [&](const drm::AacSuperFrame& sf, long j) {
        const auto& r = refFrames[(size_t)j];
        if (sf.frames.size() != r.size()) return -1.0;
        double e = 0;
        for (size_t f = 0; f < r.size(); f++) {
            if (sf.frames[f].size() != r[f].size()) return -1.0;
            for (size_t k = 0; k < r[f].size(); k++) e += popc((uint8_t)(sf.frames[f][k] ^ r[f][k]));
        }
        return e;
    };
    rx.setStreamCallback([&](int, const uint8_t* p, int len, int lenA, const DrmTelemetry&) {
        const int tb = 4;   // the text message unit at the end of the logical frame
        const int payload = len - tb - drm::aacHeaderBytes(10) - 10;
        if (payload > 0 && (!refSrc || payload != refPayload)) {
            DrmTxConfig tc; tc.audioRateHz = 24000; tc.seed = 1;
            refSrc = makeDrmAacSource(tc, 3); refPayload = payload; payloadBits = payload * 8; refFrames.clear(); lastRef = -1;
        }
        if (now < skip || !refSrc) return;
        b.frames++;
        drm::AacSuperFrame sf;
        const bool parsed = len > tb + 40 && drm::aacSuperFrameParse(p, len - tb, lenA, 10, sf) && !sf.frames.empty();
        // the reference: an exact match near the last one, else the next one in sequence
        long found = -1;
        if (parsed) for (long j = lastRef + 1; j < lastRef + 40 && found < 0; j++) if (ensure(j) && diffBits(sf, j) == 0) found = j;
        if (parsed && found < 0 && lastRef < 0) for (long j = 0; j < 100 && found < 0; j++) if (ensure(j) && diffBits(sf, j) == 0) found = j;
        b.bits += payloadBits;
        if (found >= 0) { lastRef = found; return; }
        const long j = lastRef + 1;
        const double e = parsed && lastRef >= 0 && ensure(j) ? diffBits(sf, j) : -1.0;
        b.errs += e >= 0 ? e : 0.5 * payloadBits;
        if (lastRef >= 0) lastRef = j;
    });
    uint64_t seq = 0;
    const size_t total = (size_t)(secs * rate), chunk = 16384;
    std::vector<cf32> buf(chunk);
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(chunk, total - done);
        gen(buf.data(), n);
        done += n;
        now = (double)done / rate;
        rx.feed(buf.data(), n);
        DrmTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; b.tel = t; if (b.lockSecs < 0 && t.dataValid) b.lockSecs = now; }
    }
    // the super frames that never came: half their bits wrong
    b.expected = (uint64_t)((secs - skip) / 0.4);
    if (b.frames < b.expected && payloadBits > 0) { b.errs += 0.5 * payloadBits * (double)(b.expected - b.frames); b.bits += (double)payloadBits * (double)(b.expected - b.frames); }
    b.ber = b.bits > 0 ? b.errs / b.bits : 1;
    return b;
}

static void line(const char* name, double snr, const Ber& b) {
    const DrmTelemetry& t = b.tel;
    jprintf("  %-32s %5.1f dB  BER %.2e  frames %llu/%llu  lock %.1f s | mode %c state %d FAC %llu/%llu SDC %llu/%llu SNR %.1f Doppler %.1f Hz delay %.2f ms lost %llu\n",
            name, snr, b.ber, (unsigned long long)b.frames, (unsigned long long)b.expected, b.lockSecs, t.modeName, t.state, (unsigned long long)t.facOk,
            (unsigned long long)t.facBad, (unsigned long long)t.sdcOk, (unsigned long long)t.sdcBad, t.snrDb, t.dopplerHz, t.delaySpreadMs, (unsigned long long)b.lost);
}

struct Case { const char* name; int mode, qam16, prot, shortIl, channel; double hi; double maxOk; };

int main() {
    // prot: SynthConfig modeOpt[3] (1 .. 4 = protection level 0 .. 3); maxOk: the check (5 dB above what was measured when the test was written)
    const Case cases[6] = {
        {"1: A 64-QAM R0.78 short, ch 1", 1, 0, 4, 1, 1, 30, 24},   // measured 2026-10-23: 19 dB
        {"2: A 64-QAM R0.71, ch 2", 1, 0, 3, 0, 2, 34, 25},         // 20 dB
        {"3: B 64-QAM R0.71 long, ch 3", 2, 0, 3, 0, 3, 36, 31},    // 26 dB
        {"4: B 64-QAM R0.6, ch 4", 2, 0, 2, 0, 4, 38, 27},          // 22 dB
        // known limit: not reached at 36 / 40 dB (mode C does not lock on channel 5, mode D keeps a BER near 1e-1 on channel 6): no check yet
        {"5: C 16-QAM R0.62, ch 5", 3, 1, 2, 0, 5, 36, 0},
        {"6: D 64-QAM R0.5, ch 6", 4, 0, 1, 0, 6, 40, 0},
    };
    double result[6];
    const char* only = getenv("ONAIR_MRR_CASE");   // "5": that case alone
    for (int i = 0; i < 6; i++) result[i] = 0;
    for (int i = 0; i < 6; i++) if (!only || atoi(only) == i + 1) jobs.add([i, &cases, &result] {
        const Case& c = cases[i];
        Sig s; s.mode = c.mode; s.qam16 = c.qam16; s.prot = c.prot; s.shortIl = c.shortIl; s.channel = c.channel;
        const double secs = 40, skip = 12;
        // 2 dB steps down from hi until the BER goes above 1e-4, then the dB between
        double ok = 1e9;
        double snr = c.hi;
        for (; snr > 0; snr -= 2) {
            s.snrDb = snr;
            const Ber b = runBer(s, secs, skip);
            line(c.name, snr, b);
            if (b.ber > 1e-4) break;
            ok = snr;
        }
        if (ok < 1e8 && snr > 0) {
            s.snrDb = ok - 1;
            const Ber b = runBer(s, secs, skip);
            line(c.name, s.snrDb, b);
            if (b.ber <= 1e-4) ok = ok - 1;
        }
        result[i] = ok;
        if (c.maxOk > 0) CHECK(ok <= c.maxOk, "%s: BER 1e-4 needs %.0f dB (expected %.0f or less)", c.name, ok, c.maxOk);
    });
    jobs.run();
    jprintf("\nDRM MRR test cases: lowest carrier to noise for a BER below 1e-4\n");
    for (int i = 0; i < 6; i++) if (!only || atoi(only) == i + 1) jprintf("  %-32s %s%.0f dB\n", cases[i].name, result[i] > 1e8 ? "not reached, > " : "", result[i] > 1e8 ? cases[i].hi : result[i]);
    if (fails) { jprintf("%d check(s) failed\n", fails.load()); return 1; }
    jprintf("OK\n");
    return 0;
}
