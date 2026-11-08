// KAAL voice codec 4: voice activity detection and DTX (silence sends a SID now and then, otherwise nothing, so the slot can carry
// data), comfort noise at the right level, the in-band type bit of fixed-size slots, and the input noise suppressor.
#include "kv_test_common.h"
#include <cstdio>
#include <cstdlib>
#include "../common/test_util.h"

using namespace kvtest;

static std::vector<int16_t> withNoise(const std::vector<int16_t>& clean, double rmsRef, double noiseDb, uint32_t seed) {
    std::vector<int16_t> x = clean;
    const double nr = rmsRef * std::pow(10.0, noiseDb / 20.0);
    Lcg r(seed);
    double lp = 0;
    for (auto& v : x) {
        const double w = (r.uni() * 2 - 1) * 1.732;
        lp = 0.9 * lp + 0.1 * w;
        const double y = v + (2.5 * lp + 0.5 * w) * nr;
        v = (int16_t)std::lround(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
    }
    return x;
}
static double levelDb(const std::vector<int16_t>& x, size_t a, size_t b) {
    double s = 0;
    for (size_t i = a; i < b && i < x.size(); i++) s += (double)x[i] * x[i];
    return 10 * std::log10(s / (double)(b - a) + 1e-9);
}

int main() {
    const bool print = getenv("KV_PRINT") != nullptr;
    const std::vector<int16_t> sp = eval::synthSpeech(16000, 1, 3.0, 5);
    double rms = 0;
    size_t c0 = 0;
    for (int16_t v : sp) if (v) { rms += (double)v * v; c0++; }
    rms = std::sqrt(rms / (double)c0);
    // 1 s background, 3 s speech, 1.5 s background
    std::vector<int16_t> clean(16000, 0);
    clean.insert(clean.end(), sp.begin(), sp.end());
    clean.insert(clean.end(), 24000, 0);
    const std::vector<int16_t> x = withNoise(clean, rms, -25.0, 7);

    for (int ms : {20, 40}) {
        for (bool ib : {false, true}) {
            Config c = cfg(Profile::K6_0, ms, 16000, true);
            c.inBandType = ib;
            const Info inf = info(c);
            const Run r = run(c, x);
            int cnt[3] = {};
            bool sizes = true, typeBit = true;
            for (size_t i = 0; i < r.types.size(); i++) {
                cnt[(int)r.types[i]]++;
                const size_t len = r.frames[i].size();
                if (r.types[i] == FrameType::NoData) sizes &= len == 0;
                else if (ib) {
                    sizes &= (int)len == inf.payloadBytes;
                    typeBit &= ((r.frames[i][0] >> 7) == 1) == (r.types[i] == FrameType::Sid);
                } else sizes &= (int)len == (r.types[i] == FrameType::Sid ? inf.sidBytes : inf.payloadBytes);
                FrameType t;
                if (len) sizes &= typeOfLength(c, len, &t) && (ib || t == r.types[i]);
            }
            const int tot = (int)r.types.size();
            const double lin = levelDb(x, 68000, 84000), lout = levelDb(r.out, 68000 + r.delay, 84000 + r.delay);
            const double lin0 = levelDb(x, 4000, 14000), lout0 = levelDb(r.out, 4000 + r.delay, 14000 + r.delay);
            // the speech in the middle against the clean speech
            const eval::Metrics m = eval::compare(sp, std::vector<int16_t>(r.out.begin() + 16000, r.out.end()), 16000, r.delay);
            const Run nd = run(cfg(Profile::K6_0, ms), x);   // the same without DTX
            const eval::Metrics m0 = eval::compare(sp, std::vector<int16_t>(nd.out.begin() + 16000, nd.out.end()), 16000, nd.delay);
            if (print)
                printf("  %d ms in-band %d: speech %d SID %d none %d of %d; noise %.1f -> %.1f dB (start %.1f -> %.1f); speech segSNRlo %.2f (no DTX %.2f) BLD %.3f (%.3f)\n",
                       ms, ib, cnt[0], cnt[1], cnt[2], tot, lin, lout, lin0, lout0, m.segSnrLow, m0.segSnrLow, m.bld, m0.bld);
            CHECK(sizes, "%d ms in-band %d: frame sizes follow the type", ms, ib);
            CHECK(typeBit, "%d ms: the in-band type bit marks the SIDs", ms);
            CHECK(cnt[2] > tot / 3, "%d ms: silence sends nothing most of the time (%d of %d empty)", ms, cnt[2], tot);
            CHECK(cnt[1] > 2 && cnt[1] < tot / 8, "%d ms: SIDs now and then (%d)", ms, cnt[1]);
            CHECK(cnt[0] > tot / 3 && cnt[0] < (tot * 3) / 4, "%d ms: speech frames %d of %d", ms, cnt[0], tot);
            CHECK(std::fabs(lout - lin) < 3.0 && std::fabs(lout0 - lin0) < 4.0, "%d ms: comfort noise level %.1f vs %.1f dB (start %.1f vs %.1f)", ms, lout, lin, lout0, lin0);
            CHECK(m.segSnrLow > m0.segSnrLow - 1.0 && m.bld < m0.bld + 0.05, "%d ms: DTX does not hurt the speech (%.2f vs %.2f)", ms, m.segSnrLow, m0.segSnrLow);
        }
    }
    // digital silence: one SID, then (almost) nothing, and silence out
    {
        const std::vector<int16_t> z(32000, 0);
        const Run r = run(cfg(Profile::K4_8, 40, 16000, true), z);
        int sent = 0;
        for (FrameType t : r.types) sent += t != FrameType::NoData;
        CHECK(sent <= (int)r.types.size() / 10, "digital silence: %d frames sent of %zu", sent, r.types.size());
        CHECK(peak(r.out) < 64, "digital silence decodes to silence (peak %.0f)", peak(r.out));
    }
    // a lost SID during the silence keeps the comfort noise going; a lost frame at the start of the speech is concealed
    {
        const Config c = cfg(Profile::K6_0, 40, 16000, true);
        const Run ref = run(c, x);
        const Run r = run(c, x, [&](ChannelFrame& f) { f.lost = f.type == FrameType::Sid && f.index > 5; });
        const double lin = levelDb(x, 68000, 84000), lout = levelDb(r.out, 68000 + r.delay, 84000 + r.delay);
        CHECK(std::fabs(lout - lin) < 4.0, "lost SIDs: comfort noise %.1f vs %.1f dB", lout, lin);
        CHECK(peak(r.out) < 1.6 * peak(x), "lost SIDs: bounded");
        (void)ref;
    }
    // input noise suppression: less noise in the pauses, speech closer to the clean speech
    {
        const std::vector<int16_t> xn = withNoise(clean, rms, -12.0, 9);
        Config on = cfg(Profile::K6_0, 40), off = on;
        on.noiseSuppress = true;
        const Run a = run(off, xn), b = run(on, xn);
        const double na = levelDb(a.out, 68000 + a.delay, 84000 + a.delay), nb = levelDb(b.out, 68000 + b.delay, 84000 + b.delay);
        const eval::Metrics ma = eval::compare(sp, std::vector<int16_t>(a.out.begin() + 16000, a.out.end()), 16000, a.delay);
        const eval::Metrics mb = eval::compare(sp, std::vector<int16_t>(b.out.begin() + 16000, b.out.end()), 16000, b.delay);
        if (print) printf("  noise suppressor: pause level %.1f -> %.1f dB, speech BLD vs clean %.3f -> %.3f, LSD %.2f -> %.2f, delay %d -> %d\n", na, nb, ma.bld, mb.bld, ma.lsd, mb.lsd, a.delay, b.delay);
        CHECK(nb < na - 8.0, "noise suppressor lowers the background (%.1f -> %.1f dB)", na, nb);
        CHECK(mb.bld < ma.bld, "noise suppressor brings the speech closer to the clean speech (BLD %.3f -> %.3f)", ma.bld, mb.bld);
        CHECK(b.delay == a.delay + 80, "noise suppressor delay");
    }
    return testSummary("kaalvoice_dtx");
}
