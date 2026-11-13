// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB physical layer: header, layout, chirp, loopback of every profile, carrier / timing / phase impairments, chunked input, noise and fuzz robustness.
#include "kavb_test_util.h"
#include <cstdlib>
using namespace dect2::kavb;
using namespace kavbtest;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static std::vector<std::vector<uint8_t>> makePayloads(Rng& r, const Profile& p, int n) {
    std::vector<std::vector<uint8_t>> v;
    for (int i = 0; i < n; i++) v.push_back(randomBytes(r, (size_t)framePayloadBytes(p)));
    return v;
}

// Number of payload frames received intact (matching the transmitted bytes).
static int countGood(const std::vector<RxFrame>& rx, const std::vector<std::vector<uint8_t>>& tx, const Profile& p) {
    int good = 0;
    for (const auto& f : rx) {
        if (!f.allOk || f.hdr.seq < 0 || f.hdr.seq >= (int)tx.size()) continue;
        std::vector<uint8_t> bytes;
        for (const auto& b : f.blocks) bytes.insert(bytes.end(), b.bytes.begin(), b.bytes.end());
        if (bytes == tx[f.hdr.seq]) good++;
    }
    (void)p;
    return good;
}

static void testHeader() {
    FrameHeader h;
    h.profile = 2; h.robust = true; h.fec = 5; h.sweepIdx = 777; h.seq = 54321; h.linkQ = 99;
    auto bits = headerBits(h);
    CHECK(bits.size() == 64, "header bits %zu", bits.size());
    FrameHeader g;
    CHECK(parseHeader(bits.data(), g), "parse");
    CHECK(g.profile == 2 && g.robust && g.fec == 5 && g.sweepIdx == 777 && g.seq == 54321 && g.linkQ == 99, "fields");
    for (int i = 0; i < 64; i++) {
        auto b2 = bits;
        b2[i] ^= 1;
        FrameHeader z;
        CHECK(!parseHeader(b2.data(), z), "bit %d flip accepted", i);
    }
    CHECK(headerSymbols(h).size() == (size_t)kHeaderCodedBits, "header symbols");
}

static void testLayout() {
    for (int id = 0; id < kNumProfiles; id++)
        for (int rb = 0; rb < 2; rb++) {
            const Profile p = profileOf(id, rb);
            const auto& rm = regionMap(p.dataSyms);
            CHECK((int)rm.data.size() == p.dataSyms, "data count");
            CHECK(p.regionSyms == (int)(rm.data.size() + rm.pilot.size()), "region");
            CHECK(p.dataSyms * p.bitsPerSym == kFrameCodedBits, "bits per frame %d", p.dataSyms * p.bitsPerSym);
            CHECK(p.sweepFrames >= 1 && p.sweepFrames <= kMaxSweepIdx, "sweep frames %d", p.sweepFrames);
            auto fs = frameSymbols(p, FrameHeader(), std::vector<uint8_t>(framePayloadBytes(p), 0x5A).data());
            CHECK((int)fs.size() == p.frameSyms, "frame syms");
            double pw = 0;
            for (int k = kPreambleSyms + kHeaderCodedBits; k < p.frameSyms; k++) pw += std::norm(fs[k]);
            pw /= p.regionSyms;
            CHECK(std::abs(pw - 1.0) < 0.05, "data power %.3f", pw);
            const double net = framePayloadBytes(p) * 8.0 / frameSeconds(p);
            printf("KAVB_PROFILE %c%s symRate=%.0f frameSyms=%d frameMs=%.3f sweepFrames=%d payloadBytes/frame=%d net=%.3f Mbit/s\n", p.name, rb ? "(robust)" : "",
                   p.symRate, p.frameSyms, frameSeconds(p) * 1e3, p.sweepFrames, framePayloadBytes(p), net / 1e6);
        }
}

static void testInterleaver() {
    Rng r(3);
    std::vector<uint8_t> in(kFrameCodedBits), tx(kFrameCodedBits);
    for (auto& b : in) b = r.below(2);
    interleaveScramble(in.data(), tx.data());
    std::vector<float> l(kFrameCodedBits), back(kFrameCodedBits);
    for (int i = 0; i < kFrameCodedBits; i++) l[i] = tx[i] ? -1.0f : 1.0f;
    deinterleaveDescrambleLlr(l.data(), back.data());
    int bad = 0;
    for (int i = 0; i < kFrameCodedBits; i++) bad += ((back[i] < 0) != (in[i] != 0));
    CHECK(bad == 0, "interleaver round trip: %d bad", bad);
    std::vector<uint8_t> zeros(kFrameCodedBits, 0), z2(kFrameCodedBits);
    interleaveScramble(zeros.data(), z2.data());
    int ones = 0;
    for (auto b : z2) ones += b;
    CHECK(ones > kFrameCodedBits * 0.45 && ones < kFrameCodedBits * 0.55, "scrambler balance %d", ones);
}

static void testChirp() {
    const double P = 100000.0;
    const cf a = chirpUnit(0, P), b = chirpUnit(P, P), m = chirpUnit(P / 2, P);
    CHECK(std::abs(a - b) < 1e-4, "periodic");
    CHECK(std::abs(a - m) < 1e-4, "half period phase 0");
    // instantaneous frequency by phase difference: starts at +0.45, goes through 0 at P/4, -0.45 at P/2, back through 0 at 3P/4
    auto f = [&](double u) { return std::arg(chirpUnit(u + 1, P) * std::conj(chirpUnit(u, P))) / 6.283185307179586; };
    CHECK(std::abs(f(10) - kChirpSpan) < 0.01, "start freq %.3f", f(10));
    CHECK(std::abs(f(P / 4)) < 0.01, "mid freq %.3f", f(P / 4));
    CHECK(std::abs(f(P / 2 - 10) + kChirpSpan) < 0.01, "low freq %.3f", f(P / 2 - 10));
    CHECK(f(3 * P / 4 - 100) < 0 && f(3 * P / 4 + 100) > 0, "V shape");
    // power of the transmitted signal with chirp = 1.1 x without
    Rng r(1);
    const Profile p = profileOf(0, false);
    auto frames = makePayloads(r, p, 2);
    auto on = Transmitter(0, false, true).modulate(frames), off = Transmitter(0, false, false).modulate(frames);
    double pon = 0, poff = 0;
    for (auto v : on) pon += std::norm(v);
    for (auto v : off) poff += std::norm(v);
    const double ratio = pon / poff;
    CHECK(ratio > 1.07 && ratio < 1.13, "chirp power ratio %.3f", ratio);
    printf("KAVB_INFO chirp power ratio %.3f\n", ratio);
}

static void testLoopback() {
    for (int id = 0; id < kNumProfiles; id++)
        for (int rb = 0; rb < 2; rb++) {
            Rng r(100 + id * 2 + rb);
            const Profile p = profileOf(id, rb);
            auto frames = makePayloads(r, p, 4);
            Transmitter tx(id, rb);
            Channel c;
            c.snrDb = 35.0;
            auto iq = applyChannel(tx.modulate(frames), c, r);
            Receiver rx;
            auto out = rx.push(iq.data(), iq.size());
            CHECK((int)out.size() == 4, "profile %c%d: %zu frames", p.name, rb, out.size());
            CHECK(countGood(out, frames, p) == 4, "profile %c%d: good %d", p.name, rb, countGood(out, frames, p));
            for (size_t i = 0; i < out.size(); i++) CHECK(out[i].hdr.seq == (int)i && out[i].hdr.profile == id && out[i].hdr.robust == (rb != 0), "header %zu", i);
        }
}

static void testImpairments() {
    const Profile p = profileOf(0, false);
    int done = 0;
    for (double cfo : {-0.02, -0.01, 0.0, 0.01, 0.02})
        for (double dly : {0.0, 0.37, 0.8}) {
            Rng r(500 + done);
            auto frames = makePayloads(r, p, 3);
            Channel c;
            c.snrDb = 18.0;
            c.cfoCycPerSym = cfo;
            c.delaySamples = dly;
            c.phase = 0.4 + done;
            auto iq = applyChannel(Transmitter(0).modulate(frames), c, r, 80 + done);
            Receiver rx;
            auto out = rx.push(iq.data(), iq.size());
            const int g = countGood(out, frames, p);
            CHECK(g == 3, "cfo %.3f delay %.2f: good %d of 3 (frames %zu)", cfo, dly, g, out.size());
            if (!out.empty()) CHECK(std::abs(out[0].cfoHz - cfo * p.symRate) < 300.0, "cfo estimate %.0f vs %.0f Hz", out[0].cfoHz, cfo * p.symRate);
            done++;
        }
    // 64-QAM at 30 dB with offset
    {
        const Profile d = profileOf(3, false);
        Rng r(900);
        auto frames = makePayloads(r, d, 3);
        Channel c;
        c.snrDb = 30.0;
        c.cfoCycPerSym = 0.02;
        c.delaySamples = 0.5;
        auto iq = applyChannel(Transmitter(3).modulate(frames), c, r);
        Receiver rx;
        auto out = rx.push(iq.data(), iq.size());
        CHECK(countGood(out, frames, d) == 3, "profile D with offset: good %d", countGood(out, frames, d));
    }
}

static void testChunks() {
    Rng r(7);
    const Profile p = profileOf(1, false);
    auto frames = makePayloads(r, p, 3);
    Channel c;
    c.snrDb = 20.0;
    c.cfoCycPerSym = 0.005;
    auto iq = applyChannel(Transmitter(1).modulate(frames), c, r, 500);
    Receiver rx;
    std::vector<RxFrame> out;
    size_t pos = 0;
    while (pos < iq.size()) {
        const size_t n = std::min(iq.size() - pos, (size_t)(1 + r.below(5000)));
        auto f = rx.push(iq.data() + pos, n);
        out.insert(out.end(), f.begin(), f.end());
        pos += n;
    }
    CHECK(countGood(out, frames, p) == 3, "chunked: good %d of 3 (frames %zu)", countGood(out, frames, p), out.size());
}

static void testRobustness() {
    Rng r(11);
    // pure noise: no frame, no crash
    std::vector<cf> noise(300000);
    for (auto& v : noise) v = cf((float)r.gauss(), (float)r.gauss());
    Receiver rx;
    auto out = rx.push(noise.data(), noise.size());
    CHECK(out.empty(), "frames out of noise: %zu", out.size());
    // a good burst with random corruption of blocks of samples
    const Profile p = profileOf(0, false);
    auto frames = makePayloads(r, p, 3);
    for (int t = 0; t < 20; t++) {
        auto iq = Transmitter(0).modulate(frames);
        for (int k = 0; k < 1 + (int)r.below(6); k++) {
            const size_t a = r.below(iq.size());
            const size_t n = std::min(iq.size() - a, (size_t)(1 + r.below(3000)));
            for (size_t i = 0; i < n; i++) iq[a + i] = cf((float)(5 * r.gauss()), (float)(5 * r.gauss()));
        }
        Receiver rxx;
        auto o = rxx.push(iq.data(), iq.size());
        for (const auto& f : o) CHECK(f.blocks.size() == 4, "blocks");
        // every frame reported ok must match what was sent
        CHECK(countGood(o, frames, p) <= 3, "good frames");
        for (const auto& f : o)
            if (f.allOk && f.hdr.seq >= 0 && f.hdr.seq < 3) {
                std::vector<uint8_t> bytes;
                for (const auto& b : f.blocks) bytes.insert(bytes.end(), b.bytes.begin(), b.bytes.end());
                CHECK(bytes == frames[f.hdr.seq], "corrupted frame accepted as ok (trial %d)", t);
            }
    }
    // a receiver set to another symbol rate ignores the frame
    auto iq = Transmitter(0).modulate(frames);
    Receiver::Options o;
    o.expectedSymRate = 1400000.0;
    Receiver rxo(o);
    CHECK(rxo.push(iq.data(), iq.size()).empty(), "symbol-rate mismatch not ignored");
}

static void testMultipath() {
    // one echo 0.35 times the main path, one symbol late, at 22 dB: the one-tap equaliser alone loses most frames, the decision-directed FIR pass recovers them
    const Profile p = profileOf(0, false);
    int good[2] = {0, 0}, passes = 0;
    const int n = 15;
    for (int eq = 0; eq < 2; eq++) {
        Rng r(31);
        for (int i = 0; i < n; i++) {
            auto frames = makePayloads(r, p, 1);
            Channel c;
            c.snrDb = 22.0;
            c.echoAmp = 0.35;
            c.echoSyms = 1.0;
            c.cfoCycPerSym = 0.004;
            c.delaySamples = 0.3;
            auto iq = applyChannel(Transmitter(0).modulate(frames), c, r, 300);
            Receiver::Options o;
            o.equalizer = eq != 0;
            Receiver rx(o);
            auto out = rx.push(iq.data(), iq.size());
            for (const auto& f : out) passes += eq ? f.eqPasses : 0;
            good[eq] += countGood(out, frames, p) == 1 ? 1 : 0;
        }
    }
    printf("KAVB_INFO multipath (echo 0.35 @ 1 symbol, 22 dB, profile A): one-tap only %d/%d frames, with the FIR pass %d/%d (%d passes)\n", good[0], n, good[1], n, passes);
    CHECK(good[1] >= n - 1, "equaliser recovered only %d of %d", good[1], n);
    CHECK(good[0] < good[1], "the equaliser pass made no difference (%d vs %d)", good[0], good[1]);
}

int main() {
    testHeader();
    testLayout();
    testInterleaver();
    testChirp();
    testLoopback();
    testImpairments();
    testChunks();
    testMultipath();
    testRobustness();
    if (g_fail) { printf("test_kavb_phy: %d FAILED\n", g_fail); return 1; }
    printf("test_kavb_phy: ok\n");
    return 0;
}
