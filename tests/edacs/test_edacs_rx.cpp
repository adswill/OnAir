// EDACS: BCH check bits against an independent encoder (DSD-FME's, 48 words), frame structure (sync word 0x555557125555), votes, and the full chain (generated FM
// signal with noise -> receiver) with impairment sweeps: offset, deviation, sample rate, baud clock error +-0.1 % (real clocks are within tens of ppm), polarity (detected, not set). Generated signal only.
#include "dect2/edacs/edacs_proto.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

using namespace dect2;

static unsigned rngState = 999;
static float gauss() { float s = 0; for (int i = 0; i < 6; i++) { rngState = rngState * 1664525u + 1013904223u; s += ((rngState >> 8) & 0xFFFF) / 65536.f - 0.5f; } return s * 2.0f * 0.7071f; }

static const uint32_t kOracle[][2] = {
    {0x0000001, 0x539},
    {0x0000002, 0xA72},
    {0x0000004, 0x1DD},
    {0x0000008, 0x3BA},
    {0x0000010, 0x774},
    {0x0000020, 0xEE8},
    {0x0000040, 0x8E9},
    {0x0000080, 0x4EB},
    {0x0000100, 0x9D6},
    {0x0000200, 0x695},
    {0x0000400, 0xD2A},
    {0x0000800, 0xF6D},
    {0x0001000, 0xBE3},
    {0x0002000, 0x2FF},
    {0x0004000, 0x5FE},
    {0x0008000, 0xBFC},
    {0x0010000, 0x2C1},
    {0x0020000, 0x582},
    {0x0040000, 0xB04},
    {0x0080000, 0x331},
    {0x0100000, 0x662},
    {0x0200000, 0xCC4},
    {0x0400000, 0xCB1},
    {0x0800000, 0xC5B},
    {0x1000000, 0xD8F},
    {0x2000000, 0xE27},
    {0x4000000, 0x977},
    {0x8000000, 0x7D7},
    {0x0000000, 0x000},
    {0xF77F2D9, 0xB26},
    {0xD369FFC, 0xDED},
    {0xBC3F5C3, 0x37D},
    {0x2C19018, 0x8DD},
    {0x7180EBD, 0x177},
    {0xB72E4EC, 0x6B0},
    {0x72000A3, 0x353},
    {0x25C2A42, 0x55D},
    {0x948F9EB, 0xA66},
    {0x47626A3, 0xDEE},
    {0x2B9911D, 0x0C9},
    {0xFEE55F8, 0x054},
    {0x1AA0665, 0x2F9},
    {0xCEDC74F, 0x1D4},
    {0x44DF36D, 0x630},
    {0x4382929, 0xCCF},
    {0x7B0A6D8, 0x652},
    {0xB480368, 0x107},
    {0x8EEBC89, 0x481},
    {0x7CE9242, 0x393}};

static uint32_t first[2];
static std::vector<uint8_t> frames(int count, std::set<uint32_t>& words) {
    std::vector<uint8_t> b;
    { unsigned r = 77; for (int i = 0; i < 400; i++) { r = r * 1664525u + 1013904223u; b.push_back((uint8_t)((r >> 20) & 1)); } }   // lead-in: random bits, the slicer thresholds settle: the slicer thresholds settle
    for (int k = 0; k < count; k++) {
        const uint32_t a = (0x1234567u + 0x0F3A5C1u * (uint32_t)k) & 0xFFFFFFF, c = (0xABCDEF1u ^ (0x0377711u * (uint32_t)(k + 3))) & 0xFFFFFFF;
        if (k > 0) { words.insert(a); words.insert(c); } else { first[0] = a; first[1] = c; }   // the first frame may be lost while the thresholds settle
        const auto f = edacs::encodeFrame(a, c);
        b.insert(b.end(), f.begin(), f.end());
    }
    { unsigned r = 91; for (int i = 0; i < 80; i++) { r = r * 1664525u + 1013904223u; b.push_back((uint8_t)((r >> 20) & 1)); } }   // tail: the last frame is voted after its payload and a few bits more
    return b;
}

static EdacsTelemetry run(const std::vector<cf32>& sig, double fs, double off, double sigma, bool inv, bool noiseOnly = false) {
    EdacsReceiver rx;
    rx.configure(fs);
    rx.setSignalOffset(off);
    rx.setInverted(inv);
    EdacsTelemetry t;
    uint64_t seq = 0;
    std::vector<cf32> blk(4096);
    const size_t total = noiseOnly ? (size_t)(fs * 2) : sig.size() + (size_t)(fs * 0.3);   // silence at the end so the last report is published
    for (size_t i = 0; i < total; i += blk.size()) {
        const size_t n = std::min(blk.size(), total - i);
        for (size_t k = 0; k < n; k++) {
            cf32 v = (noiseOnly || i + k >= sig.size()) ? cf32(0, 0) : sig[i + k];
            if (sigma > 0) v += cf32((float)sigma * gauss(), (float)sigma * gauss());
            blk[k] = v;
        }
        rx.feed(blk.data(), n);
        EdacsTelemetry x;
        if (rx.telemetry(x, seq)) { seq = x.seq; t = x; }
    }
    return t;
}

static int matched(const EdacsTelemetry& t, const std::set<uint32_t>& words, int& stray) {
    std::set<uint32_t> got;
    stray = 0;
    for (const auto& f : t.frames) { got.insert(f.msgA); got.insert(f.msgB); if (!words.count(f.msgA) && f.msgA != first[0]) stray++; if (f.msgB && !words.count(f.msgB) && f.msgB != first[1]) stray++; }
    int n = 0;
    for (uint32_t w : words) n += got.count(w) ? 1 : 0;
    return n;
}

int main() {
    {   // the check bits equal the independent encoder's, a flipped bit anywhere fails the check
        for (const auto& o : kOracle) {
            CHECK(edacs::bch(o[0]) == o[1], "bch(%07X) = %03X, oracle %03X", o[0], edacs::bch(o[0]), o[1]);
            const uint64_t w = ((uint64_t)o[0] << 12) | o[1];
            CHECK(edacs::bchOk(w), "good word fails");
            for (int b = 0; b < 40; b++) CHECK(!edacs::bchOk(w ^ (1ULL << b)), "single-bit error at %d not detected", b);
        }
    }
    {   // frame structure and vote
        const auto f = edacs::encodeFrame(0xABCDEF1, 0x1234567, 0x111, 0x222);
        { uint64_t sync = 0; for (int i = 0; i < 48; i++) sync = (sync << 1) | f[i]; CHECK(sync == 0x555557125555ULL, "sync %llx", (unsigned long long)sync); }
        CHECK((int)f.size() == edacs::kFrameBits, "frame has %zu bits", f.size());
        int d = 0;
        uint64_t v = edacs::vote(f.data() + 48, d);
        CHECK(d == 0 && (uint32_t)(v >> 12) == 0xABCDEF1u && (uint32_t)(v & 0xFFF) == 0x111, "vote %llx disagreements %d", (unsigned long long)v, d);
        v = edacs::vote(f.data() + 168, d);
        CHECK(d == 0 && (uint32_t)(v >> 12) == 0x1234567u, "vote B %llx", (unsigned long long)v);
        // bit errors: one error in a single copy is outvoted, the same bit wrong in two copies is not
        auto g = f;
        g[48 + 5] ^= 1; g[48 + 40 + 9] ^= 1; g[48 + 80 + 20] ^= 1;
        v = edacs::vote(g.data() + 48, d);
        CHECK(d == 3 && (uint32_t)(v >> 12) == 0xABCDEF1u, "single-copy errors not outvoted (d=%d)", d);
        g[48 + 40 + 5] ^= 1;
        v = edacs::vote(g.data() + 48, d);
        CHECK((uint32_t)(v >> 12) != 0xABCDEF1u, "two bad copies of one bit were outvoted");
    }
    const double fs = 480000, off = -100000;
    std::set<uint32_t> words;
    const auto bits = frames(10, words);
    {   // clean
        const auto sig = edacs::modulate(bits, fs, off, 1400.0, 0.5f);
        const EdacsTelemetry t = run(sig, fs, off, 0, false);
        int stray = 0;
        const int n = matched(t, words, stray);
        if (n != (int)words.size()) for (const auto& f : t.frames) std::printf("  got %07X %07X err %d/%d t=%.4f syncs=%llu\n", f.msgA, f.msgB, f.errA, f.errB, f.timeSec, (unsigned long long)t.syncs);
        for (uint32_t w : words) std::printf("  want %07X\n", w);
        CHECK(n == (int)words.size() && stray == 0, "clean: matched %d of %zu words, stray %d (good %llu bad %llu)", n, words.size(), stray, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        CHECK(t.blocksOk >= 9, "clean: %llu good frames", (unsigned long long)t.blocksOk);
        CHECK(t.locked, "clean: not locked");
        std::printf("clean: matched %d of %zu, good %llu bad %llu\n", n, words.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    {   // inverted polarity is found by itself
        const auto sig = edacs::modulate(bits, fs, off, 1400.0, 0.5f, true);
        int stray = 0;
        const EdacsTelemetry a = run(sig, fs, off, 0, false);
        const int n = matched(a, words, stray);
        CHECK(n == (int)words.size() && stray == 0 && a.inverted, "inverted: matched %d, stray %d, flag %d", n, stray, (int)a.inverted);
    }
    {   // sweep: sample rate, offset, deviation, polarity, baud clock error, carrier amplitude
        int cases = 0, bad = 0;
        for (double fsw : {307200.0, 480000.0, 960000.0})
            for (double o : {-90000.0, 0.0, 70000.0})
                for (double dev : {1000.0, 1400.0, 2400.0})
                    for (int inv = 0; inv < 2; inv++)
                        for (double ce : {-0.001, 0.0, 0.001}) {
                            const auto sig = edacs::modulate(bits, fsw * (1.0 + ce), o + 1500.0, dev, 0.4f, inv != 0);   // 1.5 kHz off the tuned offset
                            const EdacsTelemetry t = run(sig, fsw, o, 0.02, false);
                            int stray = 0;
                            const int n = matched(t, words, stray);
                            cases++;
                            if (n < (int)words.size() - 2 || stray) { bad++; std::printf("sweep miss fs %.0f off %.0f dev %.0f inv %d clk %+.3f: matched %d of %zu stray %d\n", fsw, o, dev, inv, ce, n, words.size(), stray); }
                        }
        CHECK(bad == 0, "sweep: %d of %d cases failed", bad, cases);
        std::printf("sweep: %d cases, %d failed\n", cases, bad);
    }
    {   // a long run (80 frames, 2.4 s) with a 0.05 % clock difference: the frame start is followed, no frame is lost
        std::set<uint32_t> w80;
        const auto b80 = frames(80, w80);
        const auto sig = edacs::modulate(b80, fs * 1.0005, off, 1400.0, 0.5f);
        const EdacsTelemetry t = run(sig, fs, off, 0.02, false);
        int stray = 0;
        const int n = matched(t, w80, stray);
        CHECK(n >= (int)w80.size() - 2 && stray == 0 && t.blocksOk >= 78, "long run: matched %d of %zu, good %llu bad %llu", n, w80.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    {   // frames with bad check bits are never reported (the first message is the command), frames with a bad second message still are
        std::vector<uint8_t> b2;
        { unsigned r = 77; for (int i = 0; i < 400; i++) { r = r * 1664525u + 1013904223u; b2.push_back((uint8_t)((r >> 20) & 1)); } }
        std::set<uint32_t> goodWords;
        for (int k = 0; k < 12; k++) {
            const uint32_t a = (0x2345678u + 0x0F3A5C1u * (uint32_t)k) & 0xFFFFFFF, c = (0x9BCDEF1u ^ (0x0377711u * (uint32_t)(k + 3))) & 0xFFFFFFF;
            const bool badA = k % 3 == 1;
            const auto f = edacs::encodeFrame(a, c, badA ? (int)((edacs::bch(a) ^ 0x10) & 0xFFF) : -1);
            if (!badA && k > 0) goodWords.insert(a);
            b2.insert(b2.end(), f.begin(), f.end());
        }
        { unsigned r = 91; for (int i = 0; i < 80; i++) { r = r * 1664525u + 1013904223u; b2.push_back((uint8_t)((r >> 20) & 1)); } }
        const EdacsTelemetry t = run(edacs::modulate(b2, fs, off, 1400.0, 0.5f), fs, off, 0, false);
        std::set<uint32_t> got; for (const auto& f : t.frames) got.insert(f.msgA);
        int hit = 0; for (uint32_t w : goodWords) hit += got.count(w) ? 1 : 0;
        CHECK(hit == (int)goodWords.size(), "good frames reported %d of %zu", hit, goodWords.size());
        for (int k = 1; k < 12; k += 3) CHECK(!got.count((0x2345678u + 0x0F3A5C1u * (uint32_t)k) & 0xFFFFFFF), "frame %d with bad check bits was reported", k);
        CHECK(t.blocksBad >= 3, "bad frames counted %llu", (unsigned long long)t.blocksBad);
    }
    {   // noise
        const auto sig = edacs::modulate(bits, fs, off, 1400.0, 0.5f);
        const EdacsTelemetry t = run(sig, fs, off, 0.1, false);
        int stray = 0;
        const int n = matched(t, words, stray);
        CHECK(n >= (int)words.size() - 4 && stray == 0, "noise: matched %d of %zu, stray %d", n, words.size(), stray);
        std::printf("noise: matched %d of %zu, stray %d, good %llu bad %llu\n", n, words.size(), stray, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    {   // noise only
        const EdacsTelemetry t = run({}, fs, off, 0.1, false, true);
        CHECK(t.blocksOk == 0, "noise only: %llu good frames", (unsigned long long)t.blocksOk);
    }
    std::printf("%s: %d failure(s)\n", fails ? "test_edacs_rx FAILED" : "test_edacs_rx OK", fails);
    return fails ? 1 : 0;
}
