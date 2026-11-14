// KAAL voice codec 4: building blocks (pyramid VQ enumeration, LSF conversion, bit layout, packing) and the round trip of every
// profile, frame length and sample rate on synthetic speech, vowels and fricatives, plus the wrappers of the old KAAL codecs.
#include "../../core/src/kaalvoice/kv_core.h"
#include "dect2/kaal/kaal_codec.h"
#include "dect2/kaal/kaal_codec3.h"
#include "kv_test_common.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../common/test_util.h"

using namespace dect2::kaalvoice;
using namespace dect2::kaalvoice::detail;
using namespace kvtest;

static void testPvq() {
    Lcg r(3);
    for (int n : {40, 80}) {
        CHECK(pvqCount(n, 1) == (uint64_t)(2 * n), "V(%d,1)", n);
        CHECK(pvqCount(n, 2) == (uint64_t)(2 * n + 4 * (n * (n - 1) / 2)), "V(%d,2)", n);
        for (int k = 1; k <= 14; k++) {
            if (pvqBits(n, k) > 63) break;
            CHECK(pvqBits(n, k) >= pvqBits(n, k - 1), "bits grow");
            for (int t = 0; t < 200; t++) {
                int y[80] = {}, z[80];
                for (int p = 0; p < k; p++) y[r.next() % n] += (r.next() & 1) ? 1 : -1;
                int s = 0;
                for (int i = 0; i < n; i++) s += std::abs(y[i]);
                if (s != k) continue;   // cancelled pulses: not a vector of this pyramid
                const uint64_t idx = pvqIndex(y, n, k);
                CHECK(idx < pvqCount(n, k), "index range n %d k %d", n, k);
                pvqVector(idx, n, k, z);
                CHECK(!std::memcmp(y, z, sizeof(int) * n), "PVQ round trip n %d k %d", n, k);
            }
        }
    }
    // every index of a small pyramid maps to a distinct vector of the right norm
    std::vector<std::vector<int>> seen;
    for (uint64_t i = 0; i < pvqCount(5, 3); i++) {
        int z[5];
        pvqVector(i, 5, 3, z);
        int s = 0;
        for (int v : z) s += std::abs(v);
        CHECK(s == 3, "norm of vector %llu", (unsigned long long)i);
        CHECK(pvqIndex(z, 5, 3) == i, "index of vector %llu", (unsigned long long)i);
    }
}

static void testLsf() {
    const std::vector<int16_t> x = eval::synthSpeech(8000, 2, 2.0, 5);
    double worst = 0;
    int n = 0;
    for (size_t p = 0; p + 240 <= x.size(); p += 160) {
        float f[240];
        for (int i = 0; i < 240; i++) f[i] = x[p + i] * (float)(0.54 - 0.46 * kvCos(2 * kPi * i / 239));
        double r[kP + 1], lsf[kP];
        float a[kP + 1], b[kP + 1];
        autocorr(f, nullptr, 240, kP, r);
        lagWindow(r, kP);
        if (!levinson(r, kP, a)) continue;
        CHECK(lpcToLsf(a, lsf), "LSF roots of frame %zu", p / 160);
        lsfToLpc(lsf, b);
        for (int i = 1; i <= kP; i++) worst = std::max(worst, (double)std::fabs(a[i] - b[i]));
        n++;
    }
    CHECK(n > 50 && worst < 1e-3, "LPC -> LSF -> LPC: %d frames, worst %.2e", n, worst);
    CHECK(std::fabs(kvSin(1.0) - std::sin(1.0)) < 1e-15 && std::fabs(kvCos(2.0) - std::cos(2.0)) < 1e-15, "sin / cos");
    CHECK(std::fabs(kvLog2(10.0) - std::log2(10.0)) < 1e-14 && std::fabs(kvExp2(-3.3) - std::exp2(-3.3)) < 1e-15, "log2 / exp2");
}

static void testLayout() {
    for (int p = 0; p < (int)Profile::Count; p++)
        for (int ms : {20, 40})
            for (int fs : {8000, 16000})
                for (bool ib : {false, true}) {
                    Config c = cfg((Profile)p, ms, fs);
                    c.inBandType = ib;
                    std::string why;
                    const bool ok = valid(c, &why);
                    if ((Profile)p == Profile::K3_2 && ms == 20) { CHECK(!ok, "3.2 kbit/s needs 40 ms frames"); continue; }
                    CHECK(ok, "profile %s %d ms %d Hz: %s", profileName((Profile)p), ms, fs, why.c_str());
                    const Info i = info(c);
                    CHECK(i.payloadBits == profileBitsPer20ms((Profile)p) * ms / 20, "payload bits %d", i.payloadBits);
                    CHECK(i.classBits[0] + i.classBits[1] + i.classBits[2] + i.padBits == i.payloadBits, "classes add up (%s %d ms)",
                          profileName((Profile)p), ms);
                    CHECK(i.padBits <= 6, "padding %d bits", i.padBits);
                    CHECK(i.classBits[0] > 0 && i.classBits[2] > i.classBits[0] / 2, "class sizes %d %d %d", i.classBits[0], i.classBits[1], i.classBits[2]);
                    CHECK(i.payloadBytes == (i.payloadBits + 7) / 8 && i.frameSamples == fs / 1000 * ms, "sizes");
                    if (!ib) CHECK(i.sidBytes == 5 && i.sidBytes != i.payloadBytes, "SID size");
                    FrameType t;
                    CHECK(typeOfLength(c, (size_t)i.payloadBytes, &t) && t == FrameType::Speech, "type of payload length");
                    CHECK(typeOfLength(c, 0, &t) && t == FrameType::NoData, "type of empty frame");
                    CHECK(!typeOfLength(c, (size_t)i.payloadBytes + 1, &t), "a wrong length is no frame");
                    // packing round trip with random field values
                    Prof P;
                    makeProf(c, P, nullptr);
                    Lcg r((uint32_t)(p * 100 + ms + fs));
                    for (int t2 = 0; t2 < 20; t2++) {
                        FrameQ q, q2;
                        size_t k = 0;
                        walkFrame(P, q, [&](FK, int, uint64_t& v) {
                            const int b = P.fields[k++].bits;
                            v = (((uint64_t)r.next() << 24) ^ r.next()) & (b >= 64 ? ~0ull : ((1ull << b) - 1));
                        });
                        std::vector<uint8_t> buf((size_t)(P.totalBits + 7) / 8 + 1, 0);
                        packFrame(P, q, buf.data());
                        bool fb[kMaxSub], tb;
                        unpackFrame(P, buf.data(), 0, q2, fb, &tb);
                        std::vector<uint64_t> a, b;
                        walkFrame(P, q, [&](FK, int, uint64_t& v) { a.push_back(v); });
                        walkFrame(P, q2, [&](FK, int, uint64_t& v) { b.push_back(v); });
                        CHECK(a == b, "pack / unpack round trip (%s %d ms)", profileName((Profile)p), ms);
                    }
                }
    Config c = cfg(Profile::K6_0, 40);
    c.payloadBits = 200;   // custom slot size
    CHECK(valid(c) && info(c).payloadBits == 200, "custom payload size");
    c.payloadBits = 100;
    CHECK(!valid(c), "too small a payload is refused");
}

static void testRoundTrip(bool print) {
    struct Case { Profile p; int ms, fs; double minSeg; };
    // minimum low-band segmental SNR (dB) on synthetic speech; measured values are about 1.5 dB higher (see docs/KAAL-CODEC.md)
    const Case cs[] = {
        {Profile::K3_2, 40, 16000, 4.5}, {Profile::K4_8, 40, 16000, 6.5}, {Profile::K6_0, 40, 16000, 8.0}, {Profile::K8_0, 40, 16000, 9.0},
        {Profile::K4_8, 20, 16000, 6.0}, {Profile::K6_0, 20, 16000, 7.5}, {Profile::K8_0, 20, 16000, 8.5}, {Profile::HQ12, 20, 16000, 11.0},
        {Profile::HQ16, 20, 16000, 12.5}, {Profile::K6_0, 40, 8000, 8.0}, {Profile::HQ16, 40, 8000, 12.5},
    };
    const std::vector<int16_t> x16 = eval::synthSpeech(16000, 1, 2.5, 77), x8 = eval::downsample2(x16);
    for (const Case& k : cs) {
        const Config c = cfg(k.p, k.ms, k.fs);
        const std::vector<int16_t>& x = k.fs == 8000 ? x8 : x16;
        const Run r = run(c, x);
        bool sizes = true;
        for (size_t i = 0; i < r.frames.size(); i++) sizes &= r.types[i] == FrameType::Speech && (int)r.frames[i].size() == info(c).payloadBytes;
        CHECK(sizes, "%s: every frame is a speech frame of payloadBytes", profileName(k.p));
        const eval::Metrics m = score(x, r, k.fs);
        if (print) printf("  %-4s %d ms %5d Hz: segSNRlo %.2f SNRlo %.2f LSD %.2f BLD %.3f\n", profileName(k.p), k.ms, k.fs, m.segSnrLow, m.snrLow, m.lsd, m.bld);
        CHECK(m.segSnrLow >= k.minSeg, "%s %d ms %d Hz: segSNRlo %.2f < %.1f", profileName(k.p), k.ms, k.fs, m.segSnrLow, k.minSeg);
        CHECK(peak(r.out) < 1.6 * peak(x), "%s: output peak %.0f vs input %.0f", profileName(k.p), peak(r.out), peak(x));
    }
    // the declared delay is the real one
    {
        const Run r = run(cfg(Profile::K6_0, 20), x16);
        const eval::Metrics m = eval::compare(x16, r.out, 16000, -1, 400);
        CHECK(std::abs(m.delay - r.delay) <= 2, "delay: measured %d, declared %d", m.delay, r.delay);
    }
    // sustained vowel and fricative
    {
        const std::vector<int16_t> a = eval::synthSound(16000, 'a', 130, 1.0), s = eval::synthSound(16000, 's', 0, 1.0);
        const eval::Metrics ma = score(a, run(cfg(Profile::K6_0, 40), a), 16000);
        const Run rs = run(cfg(Profile::K6_0, 40), s);
        const eval::Metrics msx = score(s, rs, 16000);
        // a noise band coded as an envelope cannot match the waveform (two independent noise spectra differ by ~7.9 dB in LSD), so the
        // fricative is scored by its loudness disturbance and its energy
        double ei = 0, eo = 0;
        for (size_t i = 1600; i + 1600 < s.size(); i++) { ei += (double)s[i] * s[i]; eo += (double)rs.out[i + rs.delay] * rs.out[i + rs.delay]; }
        const double edb = 10 * std::log10(eo / ei);
        if (print) printf("  vowel a: SNRlo %.2f   fricative s: BLD %.3f, energy %+.2f dB\n", ma.snrLow, msx.bld, edb);
        CHECK(ma.snrLow > 11.0, "vowel: SNRlo %.2f", ma.snrLow);
        CHECK(msx.bld < 0.12 && std::fabs(edb) < 1.5, "fricative: BLD %.3f, energy %+.2f dB", msx.bld, edb);
    }
}

static void testLegacy() {
    const std::vector<int16_t> x = eval::synthSpeech(16000, 0, 1.0, 9);
    for (int kb : {12, 16, 24, 32}) {
        Config c;
        c.payloadBits = kb * 20;
        std::unique_ptr<FrameCodec> fc = makeFrameCodec(CodecId::LegacyV3, c);
        CHECK(fc && fc->info().payloadBits == kb * 20, "legacy v3 %d kbit/s", kb);
        if (!fc) continue;
        const dect2::KaalCodec3 ref(kb);
        const std::vector<uint8_t> rb = ref.encode(x);   // frames: ceil(n / 320) + 1
        dect2::KaalCodec3::StreamDecoder rd(kb);
        bool same = true;
        double maxd = 0;
        for (size_t f = 0; f < x.size() / 320; f++) {
            std::vector<uint8_t> out;
            fc->encode(&x[f * 320], out);
            for (int i = 0; i < kb * 20; i++) same &= ((out[(size_t)i >> 3] >> (7 - (i & 7))) & 1) == rb[f * kb * 20 + i];
            int16_t a[320], b[320];
            fc->decode(FrameType::Speech, out.data(), out.size(), a);
            rd.push(&rb[f * kb * 20], (size_t)kb * 20, b);
            for (int i = 0; i < 320; i++) maxd = std::max(maxd, (double)std::abs(a[i] - b[i]));
        }
        CHECK(same, "legacy v3 %d kbit/s: the wrapper sends the bits of KaalCodec3::encode", kb);
        CHECK(maxd == 0, "legacy v3 %d kbit/s: the wrapper decodes like the stream decoder (max diff %.0f)", kb, maxd);
    }
    Config c;
    std::unique_ptr<FrameCodec> v1 = makeFrameCodec(CodecId::LegacyV1, c);
    CHECK(v1 && v1->info().payloadBits == 320, "legacy v1");
    if (v1) {
        const dect2::KaalCodec ref;
        const std::vector<uint8_t> rb = ref.encode(x);
        bool same = true;
        for (size_t f = 0; f < x.size() / 320; f++) {
            std::vector<uint8_t> out;
            v1->encode(&x[f * 320], out);
            for (int i = 0; i < 320; i++) same &= ((out[(size_t)i >> 3] >> (7 - (i & 7))) & 1) == rb[f * 320 + i];
        }
        CHECK(same, "legacy v1: the wrapper sends the bits of KaalCodec::encode");
    }
    Config bad = c;
    bad.frameMs = 40;
    CHECK(!makeFrameCodec(CodecId::LegacyV3, bad), "legacy codecs are 20 ms only");
    CHECK(makeFrameCodec(CodecId::KV4, cfg(Profile::K4_8, 40)) != nullptr, "KV4 through the factory");
}

int main() {
    const bool print = getenv("KV_PRINT") != nullptr;
    testPvq();
    testLsf();
    testLayout();
    testRoundTrip(print);
    testLegacy();
    return testSummary("kaalvoice_roundtrip");
}
