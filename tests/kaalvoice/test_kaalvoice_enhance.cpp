// KAAL voice codec 4, decoder-side speech enhancer (docs/KAAL-ENHANCE.md): when it is on, every stage switch, that it never worsens
// the intelligibility proxies (STOI-like envelope correlation, bark loudness disturbance) at any profile, stability on losses,
// concealment, DTX / SID / lost SIDs and the first frame after silence, the external post-filter slot, CPU time and a determinism
// digest of the enhanced output. All numbers are objective proxies: no listening test.
#include "kv_test_common.h"
#include "../common/golden.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include "../common/cpu_time.h"
#include "../common/test_util.h"

using namespace kvtest;

static constexpr uint64_t kGoldenAll = 0x5c6b5c649e3047a1ull, kGoldenPortable = 0x5c6b5c649e3047a1ull;

namespace {
bool finiteAndBounded(const std::vector<int16_t>& x) {
    for (int16_t v : x)
        if (v == 32767 || v == -32768) return false;   // the soft limiter keeps decoded speech below full scale
    return true;
}
// the largest sample-to-sample step in [a, b)
int maxStep(const std::vector<int16_t>& x, size_t a, size_t b) {
    int m = 0;
    for (size_t i = a + 1; i < b && i < x.size(); i++) m = std::max(m, std::abs((int)x[i] - (int)x[i - 1]));
    return m;
}
double rmsOf(const std::vector<int16_t>& x, size_t a, size_t b) {
    double e = 0;
    for (size_t i = a; i < b && i < x.size(); i++) e += (double)x[i] * x[i];
    return std::sqrt(e / (double)(b > a ? b - a : 1));
}

struct PassThrough : ExternalPostFilter {
    int calls = 0, speech = 0, concealed = 0, comfort = 0, gaps = 0;
    bool lpcOk = true, sizeOk = true;
    float gain = 1.0f;
    void process(const EnhanceSide& s, float* pcm) override {
        calls++;
        if (s.kind == EnhanceSide::Speech) speech++;
        if (s.kind == EnhanceSide::Concealed) concealed++;
        if (s.kind == EnhanceSide::ComfortNoise) comfort++;
        if (s.firstAfterGap) gaps++;
        if (s.kind != EnhanceSide::ComfortNoise)
            for (int k = 0; k < s.subframes; k++) lpcOk &= s.lpc[k][0] == 1.0f;
        sizeOk &= s.frameSamples > 0 && (s.kind == EnhanceSide::ComfortNoise || s.subframes > 0);
        for (int i = 0; i < s.frameSamples; i++) pcm[i] *= gain;
    }
};
}  // namespace

int main() {
    const bool print = true;
    // ---- when the enhancer runs
    {
        for (Profile p : {Profile::K3_2, Profile::K4_8, Profile::K6_0, Profile::K8_0, Profile::HQ12, Profile::HQ16}) {
            Config c = cfg(p, 40);
            Decoder d(c);
            const bool want = profileBitsPer20ms(p) < 140;
            CHECK(d.enhancing() == want, "%s: auto enhancer %d", profileName(p), d.enhancing());
            c.enhance = 1;
            CHECK(Decoder(c).enhancing(), "%s: forced on", profileName(p));
            c.enhance = 0;
            CHECK(!Decoder(c).enhancing(), "%s: forced off", profileName(p));
        }
        Config c = cfg(Profile::K6_0, 40);
        const int d0 = Decoder(c).info().delaySamples;
        c.enhanceStages |= kEnhNoiseReduce;
        CHECK(Decoder(c).info().delaySamples == d0 + 80 && Encoder(c).info().delaySamples == d0 + 80, "noise reduction adds one 5 ms hop of delay");
        c.enhanceStages = kEnhDefault;
        Encoder e(c);
        std::vector<int16_t> x = eval::synthSpeech(16000, 0, 0.5, 2);
        std::vector<uint8_t> b0, b1;
        Config c0 = c;
        c0.enhance = 0;
        Encoder e0(c0);
        bool same = true;
        std::vector<int16_t> f(640);
        for (size_t p = 0; p + 640 <= x.size(); p += 640) {
            std::copy(x.begin() + (long)p, x.begin() + (long)p + 640, f.begin());
            e.encode(f.data(), b1);
            e0.encode(f.data(), b0);
            same &= b0 == b1;
        }
        CHECK(same, "the enhancer setting does not change the bit stream");
    }
    // ---- objective proxies: on vs off for every profile, never worse on the intelligibility proxies
    {
        std::vector<std::vector<int16_t>> sigs;
        for (int v = 0; v < 4; v++) sigs.push_back(eval::synthSpeech(16000, v, 2.0, 41 + v));
        sigs.push_back(eval::synthSpeech(16000, 0, 2.0, 51, -15.0));
        sigs.push_back(eval::synthSound(16000, 'a', 120, 1.0, 2));
        sigs.push_back(eval::synthSound(16000, 's', 0, 1.0, 3));
        struct P { Profile p; int ms; };
        const P ps[] = {{Profile::K3_2, 40}, {Profile::K4_8, 40}, {Profile::K6_0, 40}, {Profile::K4_8, 20}, {Profile::K6_0, 20},
                        {Profile::K8_0, 40}, {Profile::HQ16, 20}};
        for (const P& p : ps) {
            double s0 = 0, s1 = 0, b0 = 0, b1 = 0, h0 = 0, h1 = 0, l0 = 0, l1 = 0;
            for (const auto& x : sigs) {
                Config c = cfg(p.p, p.ms);
                c.enhance = 0;
                const eval::Metrics m0 = score(x, run(c, x), 16000);
                c.enhance = 1;
                const eval::Metrics m1 = score(x, run(c, x), 16000);
                s0 += m0.stoi; s1 += m1.stoi; b0 += m0.bld; b1 += m1.bld; h0 += m0.lsdHigh; h1 += m1.lsdHigh; l0 += m0.levelErr; l1 += m1.levelErr;
            }
            const double k = (double)sigs.size();
            if (print)
                printf("  %-4s %d ms: STOI~ %.3f -> %.3f  BLD %.3f -> %.3f  LSDhi %.2f -> %.2f  |level| %.2f -> %.2f dB\n", profileName(p.p), p.ms,
                       s0 / k, s1 / k, b0 / k, b1 / k, h0 / k, h1 / k, l0 / k, l1 / k);
            CHECK(s1 >= s0 - 0.002 * k, "%s %d ms: the enhancer does not lower the STOI-like score (%.3f -> %.3f)", profileName(p.p), p.ms, s0 / k, s1 / k);
            CHECK(b1 <= b0 + 0.002 * k, "%s %d ms: the enhancer does not raise the bark loudness disturbance (%.3f -> %.3f)", profileName(p.p), p.ms, b0 / k,
                  b1 / k);
        }
    }
    // ---- every stage alone and every stage removed: runs, stays bounded, and changes the output (it is really switched)
    {
        const std::vector<int16_t> x = eval::synthSpeech(16000, 1, 1.5, 7);
        Config c = cfg(Profile::K4_8, 40, 16000, true);
        c.enhance = 0;
        const Run off = run(c, x);
        c.enhance = 1;
        const Run all = run(c, x);
        const uint32_t bits[] = {kEnhPitch, kEnhFormant, kEnhBwe, kEnhHbSmooth, kEnhLoudness, kEnhComfort, kEnhDeclick, kEnhNoiseReduce};
        // losses, and an undetected damaged frame right after some of them (decodes to a burst: the de-clicker's case)
        auto lossy = [](ChannelFrame& f) {
            f.lost = f.index % 9 == 4;
            if (f.index % 18 == 5 && f.type == FrameType::Speech)
                for (size_t i = 0; i < f.bytes.size(); i++) f.bytes[i] = (uint8_t)(f.bytes[i] ^ (0x5a + 37 * i));
        };
        for (uint32_t b : bits) {
            c.enhanceStages = kEnhDefault & ~b;
            const Run minus = run(c, x, lossy);
            c.enhanceStages = b;
            const Run alone = run(c, x, lossy);
            CHECK(finiteAndBounded(minus.out) && finiteAndBounded(alone.out), "stage %x: bounded output", b);
            c.enhanceStages = kEnhDefault | b;
            const Run with = run(c, x, lossy);
            c.enhanceStages = kEnhDefault & ~b;
            CHECK(with.out != minus.out, "stage %x is switched by its bit", b);
        }
        CHECK(off.out != all.out, "the enhancer changes the output");
        c.enhanceStages = kEnhDefault;
    }
    // ---- stability: losses, bursts, damaged classes, DTX with lost SIDs, garbage, first frame after silence
    {
        for (Profile p : {Profile::K3_2, Profile::K4_8, Profile::K6_0}) {
            for (int ms : {20, 40}) {
                if (p == Profile::K3_2 && ms == 20) continue;
                const std::vector<int16_t> x = eval::synthSpeech(16000, 0, 3.0, 61, -30.0);
                Config c = cfg(p, ms, 16000, true);
                c.enhance = 1;
                c.enhanceStages = kEnhDefault | kEnhNoiseReduce;
                Lcg r(5);
                auto ch = [&](ChannelFrame& f) {
                    const double u = r.uni();
                    if (u < 0.1) f.lost = true;                       // 10 % losses, SIDs included
                    else if (u < 0.15) f.badClasses = kBadC;
                    else if (u < 0.17) f.badClasses = kBadB | kBadC;
                    else if (u < 0.19) for (auto& byte : f.bytes) byte = (uint8_t)r.next();   // garbage
                    if (f.index >= 30 && f.index < 30 + 400 / ms) f.lost = true;            // a 400 ms burst
                };
                const Run a = run(c, x, ch);
                CHECK(finiteAndBounded(a.out), "%s %d ms: bounded under losses, garbage and DTX", profileName(p), ms);
                // after the burst the speech comes back at a sane level
                const size_t n = (size_t)ms * 16, after = (size_t)(30 + 400 / ms + 3) * n;
                const double lv = rmsOf(a.out, after, after + 8 * n), ref = rmsOf(x, after - (size_t)a.delay, after - (size_t)a.delay + 8 * n);
                CHECK(lv < 4.0 * ref + 300.0, "%s %d ms: no burst after a long loss (%.0f vs %.0f)", profileName(p), ms, lv, ref);
            }
        }
        // the first frame after silence: silence (DTX, comfort noise) then speech, no click at the onset
        std::vector<int16_t> x(16000, 0);
        Lcg r(9);
        for (int16_t& v : x) v = (int16_t)((r.uni() - 0.5) * 60.0);
        const std::vector<int16_t> sp = eval::synthSpeech(16000, 1, 1.5, 71);
        x.insert(x.end(), sp.begin(), sp.end());
        for (Profile p : {Profile::K3_2, Profile::K6_0}) {
            Config c = cfg(p, 40, 16000, true);
            c.enhance = 0;
            const Run a = run(c, x);
            c.enhance = 1;
            const Run b = run(c, x);
            const size_t on = 16000 + (size_t)b.delay;
            const int s0 = maxStep(a.out, on - 640, on + 320), s1 = maxStep(b.out, on - 640, on + 320);
            const double n0 = rmsOf(a.out, 4000, 15000), n1 = rmsOf(b.out, 4000, 15000);
            if (print) printf("  %s first frame after silence: largest step %d (off %d), comfort noise %.1f (off %.1f)\n", profileName(p), s1, s0, n1, n0);
            CHECK(finiteAndBounded(b.out), "%s: bounded around the onset", profileName(p));
            CHECK(s1 <= s0 * 1.5 + 200, "%s: no click at the first speech frame after silence (%d vs %d)", profileName(p), s1, s0);
            CHECK(n1 < n0 * 1.6 + 5 && n1 > n0 / 1.6 - 5, "%s: comfort noise level kept (%.1f vs %.1f)", profileName(p), n1, n0);
        }
    }
    // ---- de-clicking: the first good frame after a loss starts without a step or a burst
    {
        const std::vector<int16_t> x = eval::synthSpeech(16000, 0, 3.0, 81);
        for (int fs : {16000, 8000}) {
            const std::vector<int16_t> xx = fs == 8000 ? eval::downsample2(x) : x;
            Config c = cfg(Profile::K4_8, 40, fs);
            auto ch = [](ChannelFrame& f) {
                f.lost = f.index % 7 == 3;
                if (f.index % 14 == 4)   // an undetected damaged frame after the loss
                    for (size_t i = 0; i < f.bytes.size(); i++) f.bytes[i] = (uint8_t)(f.bytes[i] ^ (0xa5 + 29 * i));
            };
            c.enhanceStages = kEnhDefault & ~kEnhDeclick;
            const Run a = run(c, xx, ch);
            c.enhanceStages = kEnhDefault;
            const Run b = run(c, xx, ch);
            const size_t n = (size_t)fs / 25, q = (size_t)fs / 100;
            long sa = 0, sb = 0;
            double pa = 0, pb = 0;
            for (size_t f = 4; (f + 1) * n < b.out.size(); f += 7) {   // frame f is the first good one after the loss
                sa += maxStep(a.out, f * n - 2, f * n + q / 4);
                sb += maxStep(b.out, f * n - 2, f * n + q / 4);
                pa += rmsOf(a.out, f * n, f * n + q);
                pb += rmsOf(b.out, f * n, f * n + q);
            }
            if (print) printf("  de-click %d Hz: boundary steps %ld -> %ld, first-10 ms level %.0f -> %.0f\n", fs, sa, sb, pa, pb);
            CHECK(sb <= sa, "%d Hz: de-clicking does not make the boundaries after a loss rougher (%ld vs %ld)", fs, sb, sa);
            CHECK(pb <= pa, "%d Hz: de-clicking does not raise the level after a loss (%.0f vs %.0f)", fs, pb, pa);
        }
    }
    // ---- the external post-filter slot (a future neural post-filter)
    {
        const std::vector<int16_t> x = eval::synthSpeech(16000, 2, 1.5, 91);
        Config c = cfg(Profile::K6_0, 40, 16000, true);
        auto ch = [](ChannelFrame& f) { f.lost = f.index == 10; };
        Encoder e(c);
        Decoder d0(c), d1(c), d2(c);
        auto pass = std::make_shared<PassThrough>();
        auto half = std::make_shared<PassThrough>();
        half->gain = 0.5f;
        d1.setPostFilter(pass);
        d2.setPostFilter(half);
        const int n = e.info().frameSamples;
        std::vector<int16_t> f((size_t)n), o0((size_t)n), o1((size_t)n), o2((size_t)n);
        std::vector<uint8_t> b;
        bool same = true, halved = true;
        long idx = 0;
        for (size_t p = 0; p + (size_t)n <= x.size() + 4000; p += (size_t)n, idx++) {
            for (int i = 0; i < n; i++) f[(size_t)i] = p + i < x.size() ? x[p + i] : 0;
            const FrameType t = e.encode(f.data(), b);
            ChannelFrame cf{idx, t, b, false, 0};
            ch(cf);
            for (Decoder* d : {&d0, &d1, &d2}) {
                int16_t* o = d == &d0 ? o0.data() : d == &d1 ? o1.data() : o2.data();
                if (cf.lost) d->erasure(o);
                else d->decode(t, b.empty() ? nullptr : b.data(), b.size(), o);
            }
            same &= o0 == o1;
            for (int i = 0; i < n; i++) halved &= std::abs(o2[(size_t)i] - o0[(size_t)i] / 2.0) <= 1.0 || std::abs(o0[(size_t)i]) > 28000;
        }
        if (print) printf("  external post-filter: %d calls (%d speech, %d concealed, %d comfort noise, %d after a gap)\n", pass->calls, pass->speech,
                          pass->concealed, pass->comfort, pass->gaps);
        CHECK(same, "a pass-through external post-filter leaves the output unchanged");
        CHECK(halved, "the external post-filter's output is what the decoder returns");
        CHECK(pass->calls == idx && pass->speech > 0 && pass->concealed > 0 && pass->comfort > 0 && pass->gaps > 0, "side information: frame kinds");
        CHECK(pass->lpcOk && pass->sizeOk, "side information: LPC and sizes");
        CHECK(d1.lastSide().enhanced, "the side information says the enhancer ran");
    }
    // ---- CPU: decoder with the enhancer, CPU time per second of speech (one stream)
    {
        const std::vector<int16_t> x = eval::synthSpeech(16000, 0, 4.0, 101);
        for (Profile p : {Profile::K3_2, Profile::K6_0}) {
            Config c = cfg(p, 40);
            Encoder e(c);
            std::vector<std::vector<uint8_t>> fr;
            std::vector<FrameType> ty;
            std::vector<int16_t> f(640), o(640);
            for (size_t q = 0; q + 640 <= x.size(); q += 640) {
                std::copy(x.begin() + (long)q, x.begin() + (long)q + 640, f.begin());
                std::vector<uint8_t> bb;
                ty.push_back(e.encode(f.data(), bb));
                fr.push_back(bb);
            }
            double t[2];
            for (int on = 0; on < 2; on++) {
                c.enhance = on;
                Decoder d(c);
                const double t0 = processCpuSecs();
                for (int rep = 0; rep < 3; rep++)
                    for (size_t i = 0; i < fr.size(); i++) d.decode(ty[i], fr[i].data(), fr[i].size(), o.data());
                t[on] = (processCpuSecs() - t0) / (3.0 * fr.size() * 0.04);
            }
            if (print) printf("  %s decoder CPU per second of speech: %.4f (enhancer off %.4f)\n", profileName(p), t[1], t[0]);
            CHECK(t[1] < 0.15, "%s: decoder with the enhancer below 0.15 of a core (%.4f)", profileName(p), t[1]);
        }
    }
    // ---- determinism digest of the enhanced output
    {
        golden::Dual d;
        struct Case { Profile p; int ms, fs; bool dtx; uint32_t stages; int enh; };
        const Case cs[] = {
            {Profile::K3_2, 40, 16000, false, kEnhDefault, -1}, {Profile::K4_8, 20, 16000, false, kEnhDefault, -1},
            {Profile::K6_0, 40, 16000, true, kEnhDefault | kEnhNoiseReduce, -1}, {Profile::K6_0, 40, 8000, true, kEnhDefault, -1},
            {Profile::HQ16, 20, 16000, false, kEnhDefault, 1},
        };
        for (const Case& k : cs) {
            Config c = cfg(k.p, k.ms, k.fs, k.dtx);
            c.enhance = k.enh;
            c.enhanceStages = k.stages;
            const std::vector<int16_t> x = eval::synthSpeech(k.fs, 3, 1.5, 5, k.dtx ? -30.0 : -200.0);
            auto channel = [](ChannelFrame& f) { f.lost = f.index % 11 == 5; if (f.index % 17 == 8) f.badClasses = kBadC; };
            const Run r = run(c, x, channel);
            d.add(r.out);
            golden::Digest one;
            one.add(r.out);
            printf("  %s %d ms %d Hz dtx %d stages %02x: %016llx\n", profileName(k.p), k.ms, k.fs, k.dtx, k.stages, (unsigned long long)one.value());
        }
        fails += golden::checkDual("kaalvoice_enhance", d, kGoldenAll, kGoldenPortable, true);
    }
    return testSummary("kaalvoice_enhance");
}
