// LTR: word round trip, check bits (table cross-checked from an independent bit-matrix construction), bit errors, and the full chain
// (generated FM signal with voice and noise -> receiver), with impairment sweeps. Generated signal only: no real LTR recording was available.
#include "dect2/ltr/ltr_proto.h"
#include "dect2/ltr/ltr_rx.h"
#include "dect2/ltr/ltr_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>

static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { std::printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

using namespace dect2;

static unsigned rngState = 12345;
static float gauss() { float s = 0; for (int i = 0; i < 6; i++) { rngState = rngState * 1664525u + 1013904223u; s += ((rngState >> 8) & 0xFFFF) / 65536.f - 0.5f; } return s * 2.0f * 0.7071f; }   // about unit variance

static std::vector<ltr::Word> sentWords() {
    std::vector<ltr::Word> w;
    auto add = [&](int area, int gt, int home, int id, int fr, int chk) { ltr::Word x; x.area = area; x.gotoCh = gt; x.home = home; x.id = id; x.freeCh = fr; x.chk = chk; if (chk >= 0) x.chk = ltr::checksum(x); w.push_back(x); };
    add(0, 3, 3, 17, 5, 0x2A);
    add(0, 5, 1, 101, 7, 0x11);
    add(1, 12, 12, 254, 2, 0x5C);
    add(0, 0, 8, 255, 9, 0x03);
    add(0, 7, 2, 33, 20, 0x71);
    add(0, 19, 19, 200, 1, 0x40);
    return w;
}

static std::vector<uint8_t> stream(const std::vector<ltr::Word>& w, int repeats) {
    std::vector<uint8_t> b;
    for (int i = 0; i < 40; i++) b.push_back((uint8_t)(i & 1));   // lead-in so the slicer thresholds settle
    for (int r = 0; r < repeats; r++)
        for (const auto& x : w) {
            const auto e = ltr::encodeWord(x);
            b.insert(b.end(), e.begin(), e.end());
            for (int i = 0; i < 12; i++) b.push_back((uint8_t)((i * 7 + r) & 1));
        }
    return b;
}

static LtrTelemetry run(const std::vector<cf32>& sig, double fs, double off, double noiseSigma, bool noiseOnly = false) {
    LtrReceiver rx;
    rx.configure(fs);
    rx.setSignalOffset(off);
    LtrTelemetry t;
    uint64_t seq = 0;
    std::vector<cf32> blk(4096);
    const size_t total = noiseOnly ? (size_t)(fs * 40) : sig.size() + (size_t)(fs * 0.3);
    for (size_t i = 0; i < total; i += blk.size()) {
        const size_t n = std::min(blk.size(), total - i);
        for (size_t k = 0; k < n; k++) {
            cf32 v = (noiseOnly || i + k >= sig.size()) ? cf32(0, 0) : sig[i + k];
            if (noiseSigma > 0) v += cf32((float)noiseSigma * gauss(), (float)noiseSigma * gauss());
            blk[k] = v;
        }
        rx.feed(blk.data(), n);
        LtrTelemetry x;
        if (rx.telemetry(x, seq)) { seq = x.seq; t = x; }
    }
    return t;
}

static int found(const LtrTelemetry& t, const std::vector<ltr::Word>& sent, int& stray) {
    std::set<std::string> want;
    for (const auto& w : sent) want.insert(ltr::talkgroup(w));
    std::set<std::string> got;
    stray = 0;
    for (const auto& m : t.messages) { got.insert(m.talkgroup); if (!want.count(m.talkgroup)) stray++; }
    int n = 0;
    for (const auto& s : want) n += got.count(s) ? 1 : 0;
    return n;
}

int main() {
    const auto sent = sentWords();
    {   // protocol round trip
        for (const auto& w : sent) {
            ltr::Word r;
            const auto b = ltr::encodeWord(w);
            CHECK(b.size() == 40, "word has %zu bits", b.size());
            CHECK(ltr::parseWord(b.data(), r), "sync not found");
            CHECK(r.area == w.area && r.gotoCh == w.gotoCh && r.home == w.home && r.id == w.id && r.freeCh == w.freeCh && r.chk == w.chk, "fields differ for %s", ltr::talkgroup(w).c_str());
            CHECK(ltr::plausible(r), "plausible failed for %s", ltr::talkgroup(w).c_str());
        }
        CHECK(ltr::kindOf(255) == "idle" && ltr::kindOf(254) == "callsign ID" && ltr::kindOf(17).empty(), "kinds");
        // every single-bit error in the 24 information bits or the check bits is caught; syndromes are distinct and non-zero
        {
            std::set<int> syn;
            for (int i = 0; i < 24; i++) {
                ltr::Word z; z.area = z.gotoCh = z.home = z.id = z.freeCh = 0; z.chk = 0;
                int* f[5] = {&z.area, &z.gotoCh, &z.home, &z.id, &z.freeCh};
                const int w[5] = {1, 5, 5, 8, 5};
                int b = i;
                for (int k = 0; k < 5; k++) { if (b < w[k]) { *f[k] = 1 << (w[k] - 1 - b); break; } b -= w[k]; }
                const int c = ltr::checksum(z);
                CHECK(c != 0 && syn.insert(c).second, "info bit %d syndrome %02X not unique", i, c);
            }
            for (int i = 0; i < 7; i++) CHECK(syn.insert(1 << i).second, "check bit %d collides with an info bit", i);
            for (const auto& w : sent) {
                ltr::Word r = w;
                CHECK(ltr::checkOk(r, false), "good word fails its check");
                for (int bit = 0; bit < 7; bit++) { r.chk = w.chk ^ (1 << bit); CHECK(!ltr::checkOk(r, false), "check bit flip accepted"); }
                r = w; r.id ^= 0x10; CHECK(!ltr::checkOk(r, false), "info bit flip accepted");
                r = w; r.chk ^= 127; CHECK(!ltr::checkOk(r, false) && ltr::checkOk(r, true), "inverted check handling");
            }
        }
        // a bit error in the sync is rejected
        auto b = ltr::encodeWord(sent[0]);
        ltr::Word r;
        b[3] ^= 1;
        CHECK(!ltr::parseWord(b.data(), r), "a word with a bad sync was accepted");
        ltr::Word bad; bad.home = 25;
        CHECK(!ltr::plausible(bad), "home 25 plausible");
    }
    const double fs = 480000, off = -50000;
    const auto bits = stream(sent, 3);
    {   // full chain, clean, with a voice tone on top
        const auto sig = ltr::modulate(bits, fs, off, 600.0, 0.5f, 1000.0, 1500.0);
        const LtrTelemetry t = run(sig, fs, off, 0);
        int stray = 0;
        const int n = found(t, sent, stray);
        CHECK(n == (int)sent.size(), "clean: %d of %zu words found", n, sent.size());
        CHECK(stray == 0, "clean: %d words that were not sent", stray);
        CHECK(t.blocksOk >= sent.size() * 2, "clean: only %llu good words", (unsigned long long)t.blocksOk);
        CHECK(t.locked, "clean: not locked");
        CHECK(t.repeaters.size() >= 8, "clean: %zu repeaters", t.repeaters.size());
        std::printf("clean: found %d of %zu, good %llu bad %llu, repeaters %zu\n", n, sent.size(), (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, t.repeaters.size());
    }
    {   // inverted polarity and a carrier offset
        const auto sig = ltr::modulate(bits, fs, off + 700, 600.0, 0.5f, 0, 0, true);
        const LtrTelemetry t = run(sig, fs, off, 0);
        int stray = 0;
        const int n = found(t, sent, stray);
        CHECK(n == (int)sent.size() && stray == 0, "inverted + offset: found %d, stray %d", n, stray);
    }
    {   // 4.0: a carrier 3.1 kHz off (a 20 ppm radio at 160 MHz) through noise: the 3.9 receiver found nothing; 3.5 kHz filter + carrier follow-up (docs/SENSITIVITY-4.0-A3.md)
        for (double car : {3100.0, -3100.0, 0.0}) {
            const auto sig = ltr::modulate(bits, fs, off + car, 600.0, 0.5f, 1000.0, 1500.0);
            const LtrTelemetry t = run(sig, fs, off, 1.0);
            int stray = 0;
            const int n = found(t, sent, stray);
            CHECK(n >= (int)sent.size() - 1 && stray == 0, "carrier %+.0f Hz, noise 1.0: found %d of %zu, stray %d", car, n, sent.size(), stray);
        }
    }
    {   // noise
        const auto sig = ltr::modulate(bits, fs, off, 600.0, 0.5f, 1000.0, 1500.0);
        const LtrTelemetry t = run(sig, fs, off, 0.15);
        int stray = 0;
        const int n = found(t, sent, stray);
        CHECK(n >= (int)sent.size() - 1 && stray == 0, "noise: found %d of %zu, stray %d", n, sent.size(), stray);
        std::printf("noise: found %d of %zu, stray %d, good %llu bad %llu\n", n, sent.size(), stray, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    {   // sweep: carrier offset, deviation, polarity, sample rate, baud clock error +-0.3 %, amplitude
        int cases = 0, bad = 0;
        for (double fsw : {240000.0, 480000.0, 960000.0})
            for (double o : {-60000.0, 0.0, 45000.0})
                for (double dev : {300.0, 600.0, 1000.0})
                    for (int inv = 0; inv < 2; inv++)
                        for (double ce : {-0.003, 0.0, 0.003}) {
                            const double carrier = o + 900.0;                      // 900 Hz off the tuned offset
                            const auto sig = ltr::modulate(bits, fsw * (1.0 + ce), carrier, dev, 0.4f, 0, 0, inv != 0);
                            const LtrTelemetry t = run(sig, fsw, o, 0.03);
                            int stray = 0;
                            const int n = found(t, sent, stray);
                            cases++;
                            if (n != (int)sent.size() || stray) { bad++; std::printf("sweep miss fs %.0f off %.0f dev %.0f inv %d clk %+.3f: found %d stray %d\n", fsw, o, dev, inv, ce, n, stray); }
                        }
        CHECK(bad == 0, "sweep: %d of %d cases failed", bad, cases);
        std::printf("sweep: %d cases, %d failed\n", cases, bad);
    }
    {   // corrupted words (one information bit flipped, check bits left) are counted bad and never listed; ISW-style inverted check accepted
        std::vector<ltr::Word> w2 = sent;
        std::vector<uint8_t> b;
        for (int i = 0; i < 40; i++) b.push_back((uint8_t)(i & 1));
        for (int r = 0; r < 3; r++)
            for (size_t k = 0; k < w2.size(); k++) {
                ltr::Word x = w2[k];
                if (k % 2) x.id ^= 0x04; // corrupt every second word
                else if (r == 2) x.chk ^= 127; // inbound style inverted check
                const auto e = ltr::encodeWord(x);
                b.insert(b.end(), e.begin(), e.end());
                for (int i = 0; i < 12; i++) b.push_back((uint8_t)((i * 7 + r) & 1));
            }
        const LtrTelemetry t = run(ltr::modulate(b, fs, off, 600.0, 0.5f), fs, off, 0);
        std::set<std::string> got; for (const auto& m : t.messages) got.insert(m.talkgroup);
        for (size_t k = 0; k < w2.size(); k++) {
            const bool shouldList = (k % 2 == 0);
            CHECK(got.count(ltr::talkgroup(w2[k])) == (shouldList ? 1u : 0u), "corrupted-word handling for %s", ltr::talkgroup(w2[k]).c_str());
        }
        CHECK(t.blocksBad >= 3, "corrupted words not counted bad (%llu)", (unsigned long long)t.blocksBad);
    }
    {   // noise only: no word is accepted as good
        const LtrTelemetry t = run({}, fs, off, 0.1, true);
        CHECK(t.blocksOk == 0, "noise only: %llu good words", (unsigned long long)t.blocksOk);
        std::printf("noise only: good %llu bad %llu\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    }
    std::printf("%s: %d failure(s)\n", fails ? "test_ltr_rx FAILED" : "test_ltr_rx OK", fails);
    return fails ? 1 : 0;
}
