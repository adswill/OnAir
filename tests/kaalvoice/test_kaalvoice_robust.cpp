// KAAL voice codec 4 on a damaged channel: random and burst frame erasures (also the KAAL v4 PHY pattern: a whole 40 ms payload, i.e.
// two 20 ms frames, at once), recovery after a burst, damaged importance classes, a bit-error sweep, the importance order of the classes,
// and random garbage fed to the decoder.
#include "kv_test_common.h"
#include <cstdio>
#include <cstdlib>
#include "../common/test_util.h"

using namespace kvtest;

static double clipFraction(const std::vector<int16_t>& y) {   // samples in the limiter range (> 28000)
    size_t c = 0;
    for (int16_t v : y) c += v > 28000 || v < -28000;
    return (double)c / (double)y.size();
}

int main() {
    const bool print = getenv("KV_PRINT") != nullptr;
    const std::vector<int16_t> x = eval::synthSpeech(16000, 0, 4.0, 31);
    const double px = peak(x);

    for (int ms : {20, 40}) {
        const Config c = cfg(Profile::K6_0, ms);
        const Run clean = run(c, x);
        const eval::Metrics m0 = score(x, clean, 16000);
        double prevBld = m0.bld;
        // random erasures; for 20 ms frames the losses come in pairs (one 40 ms PHY payload)
        for (double loss : {0.01, 0.03, 0.10, 0.20}) {
            Lcg r(ms * 1000 + (uint32_t)(loss * 1000));
            bool lostPrev = false;
            const Run d = run(c, x, [&](ChannelFrame& f) {
                if (ms == 20 && (f.index & 1)) { f.lost = lostPrev; return; }
                lostPrev = r.uni() < loss;
                f.lost = lostPrev;
            });
            const eval::Metrics m = score(x, d, 16000);
            if (print) printf("  %d ms, %.0f%% payload loss: segSNRlo %.2f (clean %.2f) BLD %.3f (clean %.3f)\n", ms, loss * 100, m.segSnrLow, m0.segSnrLow, m.bld, m0.bld);
            CHECK(peak(d.out) < 1.6 * px && clipFraction(d.out) < 1e-3, "%d ms %.0f%%: output stays bounded (peak %.0f)", ms, loss * 100, peak(d.out));
            CHECK(m.bld >= prevBld - 0.02, "%d ms: damage grows with the loss rate (%.3f after %.3f)", ms, m.bld, prevBld);
            if (loss <= 0.011) CHECK(m.segSnrLow > m0.segSnrLow - 1.5 && m.bld < m0.bld + 0.04, "%d ms: 1%% loss costs little (%.2f / %.3f)", ms, m.segSnrLow, m.bld);
            prevBld = m.bld;
        }
        // a 400 ms burst in the middle: fades out, then recovers
        const long first = 2000 / ms, len = 400 / ms;
        const Run b = run(c, x, [&](ChannelFrame& f) { f.lost = f.index >= first && f.index < first + len; });
        const int n = 16 * ms, D = b.delay;
        auto seg = [&](const std::vector<int16_t>& y, long f0, long f1) {
            double e = 0;
            for (long i = f0 * n; i < f1 * n && i < (long)y.size(); i++) e += (double)y[(size_t)i] * y[(size_t)i];
            return 10 * std::log10(e / ((f1 - f0) * n) + 1.0);
        };
        const double before = seg(clean.out, first - 200 / ms, first), tailOfBurst = seg(b.out, first + len - 100 / ms, first + len);
        CHECK(tailOfBurst < before - 10.0, "%d ms burst: concealment fades out (%.1f dB vs %.1f before)", ms, tailOfBurst, before);
        // 200 ms after the burst the output follows the clean decode again
        double se = 0, ss = 0;
        for (long i = (first + len + 200 / ms) * n; i < (first + len + 600 / ms) * n && i < (long)b.out.size(); i++) {
            const double a = clean.out[(size_t)i], d = b.out[(size_t)i];
            ss += a * a;
            se += (a - d) * (a - d);
        }
        const double rec = 10 * std::log10(ss / (se + 1.0));
        if (print) printf("  %d ms: burst tail %.1f dB (speech %.1f), recovery SNR vs clean decode %.1f dB\n", ms, tailOfBurst, before, rec);
        CHECK(rec > 15.0, "%d ms: recovery after a burst (%.1f dB against the clean decode)", ms, rec);
        (void)D;
    }

    // damaged classes cost less than losing the frame
    {
        const Config c = cfg(Profile::K8_0, 40);
        auto pick = [](long i) { return (i * 2654435761u) % 10 == 3; };   // 10 % of the frames
        const eval::Metrics er = score(x, run(c, x, [&](ChannelFrame& f) { f.lost = pick(f.index); }), 16000);
        const eval::Metrics bc = score(x, run(c, x, [&](ChannelFrame& f) { if (pick(f.index)) f.badClasses = kBadC; }), 16000);
        const eval::Metrics bb = score(x, run(c, x, [&](ChannelFrame& f) { if (pick(f.index)) f.badClasses = kBadB | kBadC; }), 16000);
        if (print) printf("  10%% frames: erased BLD %.3f segSNRlo %.2f | class C bad %.3f %.2f | B+C bad %.3f %.2f\n", er.bld, er.segSnrLow, bc.bld, bc.segSnrLow, bb.bld, bb.segSnrLow);
        CHECK(bc.segSnrLow > er.segSnrLow && bc.bld < er.bld + 0.01, "class C damage beats an erasure");
        CHECK(bb.bld < er.bld + 0.03, "class B+C damage is not worse than an erasure (%.3f vs %.3f)", bb.bld, er.bld);
    }

    // bit errors without any check (worst case: the container passes damaged bits)
    {
        const Config c = cfg(Profile::K6_0, 40);
        const Info inf = info(c);
        double prev = 0;
        for (double ber : {1e-4, 1e-3, 1e-2, 5e-2}) {
            Lcg r((uint32_t)(ber * 1e6));
            const Run d = run(c, x, [&](ChannelFrame& f) {
                for (size_t i = 0; i < f.bytes.size() * 8; i++)
                    if (r.uni() < ber) f.bytes[i >> 3] ^= (uint8_t)(0x80 >> (i & 7));
            });
            const eval::Metrics m = score(x, d, 16000);
            if (print) printf("  BER %.0e: segSNRlo %.2f BLD %.3f\n", ber, m.segSnrLow, m.bld);
            CHECK(clipFraction(d.out) < (ber > 0.02 ? 0.03 : ber > 2e-3 ? 0.01 : 2e-3), "BER %.0e: bounded output (%.4f near full scale)", ber, clipFraction(d.out));
            CHECK(m.bld >= prev - 0.02, "BER %.0e: damage grows", ber);
            prev = m.bld;
        }
        // importance order: one flipped bit per frame hurts more in class A than in class C
        double dmg[3];
        for (int cls = 0; cls < 3; cls++) {
            Lcg r(77 + cls);
            const int lo = cls == 0 ? 0 : cls == 1 ? inf.classBits[0] : inf.classBits[0] + inf.classBits[1];
            const int nb = inf.classBits[cls];
            const Run d = run(c, x, [&](ChannelFrame& f) {
                const int i = lo + (int)(r.next() % (uint32_t)nb);
                f.bytes[(size_t)i >> 3] ^= (uint8_t)(0x80 >> (i & 7));
            });
            dmg[cls] = score(x, d, 16000).bld;
        }
        if (print) printf("  one bit error per frame: class A BLD %.3f, B %.3f, C %.3f\n", dmg[0], dmg[1], dmg[2]);
        CHECK(dmg[0] > dmg[2] && dmg[1] > dmg[2] * 0.9, "class A bits matter more than class C bits (%.3f %.3f %.3f)", dmg[0], dmg[1], dmg[2]);
    }

    // garbage: random lengths, types, class flags and bytes must not break the decoder: once valid frames arrive again it follows the
    // clean decode (a random SID that passes the length check is legitimately loud, so loudness during the garbage is not judged)
    for (int ms : {20, 40}) {
        for (bool ib : {false, true}) {
            Config c = cfg(Profile::K4_8, ms, 16000, true);
            c.inBandType = ib;
            const Run ref = run(c, x);
            Decoder d(c);
            Lcg r(5 + ms + ib);
            const int n = d.info().frameSamples;
            std::vector<int16_t> out((size_t)n);
            for (int k = 0; k < 300; k++) {
                const int len = (r.next() % 4 == 0) ? (int)(r.next() % 64) : (r.next() & 1 ? d.info().payloadBytes : d.info().sidBytes);
                std::vector<uint8_t> b((size_t)len);
                for (auto& v : b) v = (uint8_t)r.next();
                const FrameType t = (FrameType)(r.next() % 3);
                d.decode(t, len ? b.data() : nullptr, b.size(), out.data(), r.next() % 8 == 0 ? (uint32_t)(r.next() % 8) : 0);
            }
            std::vector<int16_t> got, want;
            for (size_t f = 0; f < ref.frames.size(); f++) {
                d.decode(ref.types[f], ref.frames[f].empty() ? nullptr : ref.frames[f].data(), ref.frames[f].size(), out.data());
                if (f * n < 1500 * 16) continue;   // 1.5 s to settle
                got.insert(got.end(), out.begin(), out.end());
                want.insert(want.end(), ref.out.begin() + (long)(f * n), ref.out.begin() + (long)((f + 1) * n));
            }
            // the high band and the comfort noise are noise sources: compare the low band of the speech frames
            const double snr = eval::compare(want, got, 16000, 0).segSnrLow;
            if (print) printf("  after garbage (%d ms, in-band %d): SNR against the clean decode %.1f dB\n", ms, ib, snr);
            CHECK(snr > 15.0, "decoder recovers after garbage (%d ms, in-band %d): %.1f dB", ms, ib, snr);
        }
    }
    return testSummary("kaalvoice_robust");
}
