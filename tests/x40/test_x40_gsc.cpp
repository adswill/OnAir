#include "x40_gen_gsc.h"
#include "dect2/common/bits.h"
#include <cstdio>
#include <cstring>
using namespace x40gen; using namespace dect2; using namespace dect2::gsc;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::string bitsOf(uint32_t v, int n) { std::string s; for (int i = n - 1; i >= 0; i--) s += (v >> i & 1) ? '1' : '0'; return s; }
static bool has(const X40Telemetry& t, const char* kind, const std::string& id, const std::string& what) {
    for (auto& r : t.rows) if (r.kind == kind && (id.empty() || r.id == id) && r.text.find(what) != std::string::npos) return true;
    return false;
}
static int pageRows(const X40Telemetry& t) { return countKind(t, "Tone page") + countKind(t, "Numeric page") + countKind(t, "Alpha page") + countKind(t, "Voice page"); }
static void dump(const X40Telemetry& t) { for (auto& f : t.facts) printf("    %s: %s\n", f.first.c_str(), f.second.c_str()); for (auto& r : t.rows) printf("    [%s] %s %s\n", r.kind.c_str(), r.id.c_str(), r.text.c_str()); }

static void protoChecks() {
    using namespace dect2::gsc;
    const char* par[10] = {"10000000011", "00001111110", "11000001001", "01111111000", "00001111010", "00000111001", "00011110000", "01000001111", "01111111001", "11100010001"};
    for (int i = 0; i < 10; i++) CHECK(bitsOf(golayParity((uint32_t)kPreambleWords[i]), 11) == par[i], "preamble %d parity %s", i, bitsOf(golayParity((uint32_t)kPreambleWords[i]), 11).c_str());
    CHECK(bitsOf(golayParity(kStartCode), 11) == "01000000011", "start code parity");
    const Words w = wordsOfCode(954853);
    CHECK(w.preamble == 3 && w.w1Index == 4 && w.w2 == 1753 && w.ok, "954853: preamble %d word 1 %d word 2 %d", w.preamble, w.w1Index, w.w2);
    CHECK(bitsOf(golayParity(1753), 11) == "00010101101", "word 2 1753 parity");
    int bad = 0;
    for (int c = 0; c < 100000; c += 7) {
        const Words q = wordsOfCode(c); int g = 0, a = 0;
        if (!codeOfWords(q.w1Index, q.w2, g, a) || g * 1000 + a != c % 100000) bad++;
    }
    CHECK(bad == 0, "%d pager codes do not map back", bad);
    CHECK(!wordsOfCode(10363).ok && !wordsOfCode(51292).ok && wordsOfCode(10362).ok, "illegal codes");
    for (int i = 0; i < 128; i++) { const uint16_t cw = bchWord(i); CHECK(bchInfo(cw) == i, "BCH info %d", i); }
    int dmin = 99, d23 = 99;
    for (int i = 1; i < 128; i++) dmin = std::min(dmin, dect2::bits::popcount((uint32_t)bchWord(i)));
    for (int i = 1; i < 4096; i++) d23 = std::min(d23, dect2::bits::popcount(golayWord((uint32_t)i)));
    CHECK(dmin == 5, "BCH (15,7) minimum distance %d", dmin);
    CHECK(d23 == 7, "Golay (23,12) minimum distance %d", d23);
    std::vector<int> sym = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 12, 13}, back; bool more = false; int info[8];
    packBlock(sym, 4, true, info);
    CHECK(unpackBlock(info, 4, back, more) && back == sym && more, "numeric block round trip");
    std::string s = "HELLO 42";
    for (char c : s) { const int v = alphaValue(c); CHECK(alphaChar(v) == c, "alpha '%c' -> %d -> '%c'", c, v, alphaChar(v)); }
    CHECK(alphaValue('A') == 0x21 && alphaValue(' ') == 0 && alphaValue('0') == 0x10, "alpha symbol values");
}

int main() {
    protoChecks();
    const std::vector<GscPage> mixPages = {
        {954853, 1, GscType::Tone, ""},
        {10362, 2, GscType::Numeric, "555 1234-99"},
        {123456, 3, GscType::Alpha, "CALL THE OFFICE AT 9"},
        {789012, 4, GscType::Voice, "", 2.0},
    };
    struct Case { const char* what; double fs, rxOff; GscPath p; };
    auto P = [](double off, double drift, double ppm, double ppmPerSec, double dev, bool inv) { GscPath p; p.offsetHz = off; p.driftHzPerSec = drift; p.ppm = ppm; p.ppmPerSec = ppmPerSec; p.devHz = dev; p.invert = inv; return p; };
    const Case cases[] = {
        {"24 ksps, clean", 24000, 0, P(0, 0, 0, 0, 4500, false)},
        {"offset +2.5 kHz", 24000, 0, P(2500, 0, 0, 0, 4500, false)},
        {"offset -2.5 kHz, drift +150 Hz/s", 24000, 0, P(-2500, 150, 0, 0, 4500, false)},
        {"clock +800 ppm", 24000, 0, P(300, 0, 800, 0, 4500, false)},
        {"clock -800 ppm drifting +120 ppm/s", 24000, 0, P(-300, 0, -800, 120, 4500, false)},
        {"deviation 3 kHz", 48000, 0, P(700, 0, 200, 0, 3000, false)},
        {"spectrum inverted", 24000, 0, P(400, 0, 0, 0, 4500, true)},
        {"250 ksps, off-centre -40 kHz", 250000, -40000, P(-40000 + 900, 0, -300, 0, 4500, false)},
    };
    for (const Case& c : cases) {
        auto x = gscSignal(gscSingle(mixPages), c.fs, c.p);
        gscNoise(x, 18, c.fs, 3);
        const auto t = run(x40::Gsc, x, c.fs, c.rxOff);
        const bool a = has(t, "Tone page", "954853", "address 1"), b = has(t, "Numeric page", "010362", "555 1234-99"), d = has(t, "Alpha page", "123456", "CALL THE OFFICE AT 9");
        const bool v = has(t, "Voice page", "789012", "address 4"), e = countKind(t, "Voice end") == 1;
        printf("  %-40s tone %d numeric %d alpha %d voice %d end %d | %s\n", c.what, a, b, d, v, e, t.headline.c_str());
        if (!(a && b && d && v && e) || pageRows(t) != 4) dump(t);
        CHECK(a && b && d && v && e, "%s: tone %d numeric %d alpha %d voice %d end %d", c.what, a, b, d, v, e);
        CHECK(pageRows(t) == 4, "%s: %d page rows for 4 pages", c.what, pageRows(t));
    }
    {
        const int pre = wordsOfCode(301234).preamble;
        std::vector<GscPage> bp = {{301234, 1, GscType::Tone, ""}, {301235, 2, GscType::Tone, ""}, {311777, 1, GscType::Numeric, "0123456789"}, {321999, 3, GscType::Alpha, "BATCH MODE"}};
        bool same = true; for (auto& q : bp) same = same && wordsOfCode(q.code).preamble == pre;
        CHECK(same, "batch pages share the preamble");
        auto x = gscSignal(gscBatch(pre, bp), 24000, P(-600, 0, 400, 0, 4500, false)); gscNoise(x, 16, 24000, 9);
        const auto t = run(x40::Gsc, x, 24000);
        const bool ok = has(t, "Tone page", "301234", "batch") && has(t, "Tone page", "301235", "address 2") && has(t, "Numeric page", "311777", "0123456789") && has(t, "Alpha page", "321999", "BATCH MODE");
        printf("  batch of 4 after an inverted preamble: %s | %s\n", ok ? "all read" : "missed", t.headline.c_str());
        if (!ok || pageRows(t) != 4) dump(t);
        CHECK(ok && pageRows(t) == 4, "batch: %d page rows", pageRows(t));
    }
    {
        std::vector<GscPage> np = {{4321, 1, GscType::Tone, ""}, {55500, 2, GscType::Numeric, "911"}};
        auto x = gscSignal(gscNoPreamble(np), 24000, P(1200, 0, -500, 0, 4500, false)); gscNoise(x, 16, 24000, 11);
        const auto t = run(x40::Gsc, x, 24000);
        const bool ok = has(t, "Tone page", "N04321", "no preamble") && has(t, "Numeric page", "N55500", "911");
        printf("  non-battery-saver (75 Hz preamble, no start code): %s\n", ok ? "both read" : "missed");
        if (!ok) dump(t);
        CHECK(ok, "non-battery-saver pages");
    }
    printf("noise sweep (offset +800 Hz, clock +300 ppm): numeric and alphanumeric pages right, signal to noise in 12.5 kHz\n");
    std::vector<std::pair<double, double>> pts, ptsTone;
    for (double snr = -2; snr <= 16.1; snr += 1) {
        int ok = 0, n = 0, okT = 0, nT = 0;
        for (unsigned seed : {1u, 2u, 3u}) {
            std::mt19937 r(seed * 77 + (unsigned)(snr * 10 + 100));
            std::vector<GscPage> pg;
            for (int i = 0; i < 4; i++) {
                int code = 0;
                do { code = (int)(r() % 1000000); } while (!wordsOfCode(code).ok || code % 100000 == 0);
                GscPage q; q.code = code; q.function = 1 + (int)(r() % 4);
                if (i == 0) q.type = GscType::Tone;
                else if (i & 1) { q.type = GscType::Numeric; for (int k = 0; k < 10; k++) q.text += (char)('0' + r() % 10); }
                else { q.type = GscType::Alpha; for (int k = 0; k < 12; k++) q.text += "ABCDEFGHIJKLMNOPQRSTUVWXYZ "[r() % 27]; while (q.text.back() == ' ') q.text.back() = 'X'; }
                pg.push_back(q);
            }
            auto x = gscSignal(gscSingle(pg), 24000, P(800, 0, 300, 0, 4500, false), seed);
            gscNoise(x, snr, 24000, 500 + seed);
            const auto t = run(x40::Gsc, x, 24000);
            for (auto& q : pg) {
                char id[16]; snprintf(id, sizeof id, "%06d", q.code);
                if (q.type == GscType::Tone) { okT += has(t, "Tone page", id, ""); nT++; }
                else { ok += has(t, q.type == GscType::Numeric ? "Numeric page" : "Alpha page", id, q.text); n++; }
            }
        }
        pts.push_back({snr, (double)ok / n}); ptsTone.push_back({snr, (double)okT / nT});
        printf("  %5.1f dB: messages %2d / %d, tone pages %d / %d\n", snr, ok, n, okT, nT);
    }
    printf("  GSC messages: 50 %% at %.1f dB, 90 %% at %.1f dB; tone pages: 50 %% at %.1f dB, 90 %% at %.1f dB (12.5 kHz)\n", gscSnrAt(pts, 0.5), gscSnrAt(pts, 0.9), gscSnrAt(ptsTone, 0.5), gscSnrAt(ptsTone, 0.9));
    CHECK(pts.back().second >= 0.9, "%.2f of the messages at %.0f dB", pts.back().second, pts.back().first);
    CHECK(ptsTone.back().second >= 0.9, "%.2f of the tone pages at %.0f dB", ptsTone.back().second, ptsTone.back().first);
    {
        int falseRows = 0;
        for (unsigned seed = 1; seed <= 3; seed++) {
            std::vector<cf32> x(24000 * 40, cf32(0, 0)); addNoise(x, 1.0, 900 + seed);
            const auto t = run(x40::Gsc, x, 24000); falseRows += pageRows(t) + countKind(t, "Voice end");
        }
        printf("  noise only, 3 x 40 s: %d false pages\n", falseRows);
        CHECK(falseRows == 0, "noise only: %d false pages", falseRows);
        std::vector<cf32> c(24000 * 20, cf32(1, 0)); gscNoise(c, 20, 24000, 5);
        const int fc = pageRows(run(x40::Gsc, c, 24000));
        auto f1 = randomFsk(24000, 30, 1200, 4500, 3); gscNoise(f1, 20, 24000, 6);
        const int ff = pageRows(run(x40::Gsc, f1, 24000));
        auto f2 = randomFsk(24000, 30, 600, 4500, 4); gscNoise(f2, 20, 24000, 7);
        const int f6 = pageRows(run(x40::Gsc, f2, 24000));
        auto f3 = randomFsk(24000, 30, 300, 4500, 8); gscNoise(f3, 20, 24000, 8);
        const int f3n = pageRows(run(x40::Gsc, f3, 24000));
        GscSeg vs; vs.voiceSec = 20; auto vx = gscSignal({vs}, 24000, GscPath()); gscNoise(vx, 20, 24000, 9);
        const int fv = pageRows(run(x40::Gsc, vx, 24000));
        printf("  carrier 20 s: %d, random 1200 bit/s FSK 30 s: %d, random 600 bit/s FSK 30 s: %d, random 300 bit/s FSK 30 s: %d, voice audio 20 s: %d false pages\n", fc, ff, f6, f3n, fv);
        CHECK(fc == 0 && ff == 0 && f6 == 0 && f3n == 0 && fv == 0, "false pages: carrier %d, 1200 %d, 600 %d, 300 %d, voice %d", fc, ff, f6, f3n, fv);
    }
    if (fails) printf("%d FAILED\n", fails); else printf("all GSC checks passed\n");
    return fails ? 1 : 0;
}
