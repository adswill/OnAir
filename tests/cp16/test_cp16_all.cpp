// CP16: fonts and recognition, the generator through the receiver (speeds, spacings, both sidebands, tuning error, clock error, noise), the
// transmitter, and the CP16+ reference recordings (MIT, tests/cp16/data). Simulated signals only, apart from the CP16+ files (which are themselves
// made by the CP16+ software, not received off the air).
// Long sweeps: ONAIR_TEST_LONG=1. A local copy of CRAC's own sample (not in the repository: its licence is not stated): ONAIR_CP16_CRAC_WAV=<path>.
#include "dect2/cp16/cp16_gen.h"
#include "dect2/cp16/cp16_proto.h"
#include "dect2/cp16/cp16_rx.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using namespace dect2;

#ifndef DECT2_SOURCE_DIR
#define DECT2_SOURCE_DIR "."
#endif

static int gFail = 0;
static bool gDebug = false;   // CP16_DEBUG=1: the receiver's log and the picture of a failed case
static void printPicture(const Cp16Telemetry& t, size_t from, size_t n);
#define CHECK(c, ...) do { if (!(c)) { gFail++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::vector<uint8_t> readAll(const std::string& p) {
    std::vector<uint8_t> d;
    FILE* f = std::fopen(p.c_str(), "rb");
    if (!f) return d;
    unsigned char b[65536];
    size_t n;
    while ((n = std::fread(b, 1, sizeof b, f)) > 0) d.insert(d.end(), b, b + n);
    std::fclose(f);
    return d;
}

static bool loadNoto() {
    std::string err;
    const auto d = readAll(std::string(DECT2_SOURCE_DIR) + "/third_party/fonts/noto-cjk/NotoSansCJKsc-Regular.otf");
    if (d.empty()) { std::printf("Noto Sans CJK SC not found under %s\n", DECT2_SOURCE_DIR); return false; }
    const bool ok = cp16::setDefaultFontData(d, "Noto Sans CJK SC", err);
    if (!ok) std::printf("font: %s\n", err.c_str());
    return ok;
}

// characters of b found in order in a (longest common subsequence), spaces ignored
static double readFraction(const std::vector<uint32_t>& want, const std::string& got) {
    std::vector<uint32_t> a, b;
    for (uint32_t c : want) { if (c >= 0xFF01 && c <= 0xFF5E) c = c - 0xFF01 + 0x21; if (c != 0x3000 && c != ' ') a.push_back(c); }
    for (uint32_t c : cp16::utf8ToCodepoints(got)) if (c != ' ') b.push_back(c);
    if (a.empty()) return 0;
    std::vector<std::vector<int>> L(a.size() + 1, std::vector<int>(b.size() + 1, 0));
    for (size_t i = 1; i <= a.size(); i++)
        for (size_t j = 1; j <= b.size(); j++)
            L[i][j] = a[i - 1] == b[j - 1] ? L[i - 1][j - 1] + 1 : std::max(L[i - 1][j], L[i][j - 1]);
    return (double)L[a.size()][b.size()] / (double)a.size();
}

static void testFont() {
    const auto& rep = cp16::gb2312Repertoire();
    CHECK(rep.size() == 7445, "GB2312 repertoire %zu, expected 7445", rep.size());
    CHECK(cp16::gb2312Code(0x4E2D) == 0xD6D0, "GB2312 of 中: %04X", cp16::gb2312Code(0x4E2D));
    const auto cells = cp16::textCells("CQ 中\n7");
    CHECK(cells.size() == 6 && cells[0] == 0xFF23 && cells[2] == 0x3000 && cells[3] == 0x4E2D && cells[4] == 0x3000 && cells[5] == 0xFF17, "text cells");
    CHECK(cp16::messageRows(cells).size() == 6 * cp16::kCellRows, "rows per cell");
    cp16::Glyph g;
    CHECK(cp16::glyphFor(0x4E2D, g) && g.ink() > 20, "glyph of 中 (%d dots)", g.ink());
    // every 37th GB2312 character recognised from its own clean glyph
    int n = 0, ok = 0;
    for (size_t i = 0; i < rep.size(); i += 37) {
        if (!cp16::glyphFor(rep[i], g) || g.ink() < 3) continue;
        float dots[16][16];
        for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) dots[r][c] = (g.row[r] >> (15 - c)) & 1 ? 1.f : 0.f;
        const cp16::Match m = cp16::recognise(dots);
        n++;
        cp16::Glyph g2;
        cp16::glyphFor(m.cp, g2);
        if (m.cp == rep[i] || std::memcmp(g.row, g2.row, sizeof g.row) == 0) ok++;   // identical glyphs (a few punctuation marks) count
    }
    CHECK(n > 150 && ok == n, "clean glyphs recognised %d / %d", ok, n);
    std::printf("font: %s, %d / %d clean glyphs recognised\n", cp16::fontName().c_str(), ok, n);
}

struct Case {
    const char* name;
    double charSec, spacing, base;
    bool lsb;
    double cfo, ppm, snr;
    int text;          // demo text 0 / 1
    double minRead;    // fraction of the characters that must be read
};

static double runCase(const Case& c, bool verbose) {
    const double rate = 250000;
    Cp16GenOptions o;
    o.params.charSec = c.charSec; o.params.spacingHz = c.spacing; o.params.baseHz = c.base; o.params.lsb = c.lsb;
    o.rate = rate;
    o.dialOffsetHz = -cp16Tuning().tuneOffsetHz;
    o.cfoHz = c.cfo; o.sroPpm = c.ppm; o.snrDb = c.snr;
    o.text = cp16DemoText(c.text);
    o.repeat = true;
    o.seed = 7;
    Cp16Generator gen(o);
    Cp16Receiver rx;
    if (gDebug) rx.setLogCallback([](const std::string& m) { std::printf("  log: %s\n", m.c_str()); });
    rx.configure(rate);
    rx.setSignalOffset(-cp16Tuning().tuneOffsetHz);
    const double sec = gen.messageSec() + o.leadSec + 7;
    std::vector<cf32> buf(25000);
    for (double t = 0; t < sec; t += (double)buf.size() / rate) {
        gen.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
    }
    Cp16Telemetry tel;
    rx.telemetry(tel, 0);
    const double fr = readFraction(gen.cells(), tel.text);
    if (verbose || fr < c.minRead)
        std::printf("%-28s state %d base %.1f spacing %.2f row %.1f ms %s comb %.1f coh %.2f snr %.1f dB  read %.0f %%  \"%s\"\n", c.name, tel.state, tel.baseHz,
                    tel.spacingHz, tel.rowSec * 1000, tel.mirrored ? "mirrored" : "", tel.combScore, tel.timingScore, tel.snrDb, 100 * fr, tel.text.c_str());
    CHECK(fr >= c.minRead, "%s: read %.0f %% (need %.0f %%)", c.name, 100 * fr, 100 * c.minRead);
    if (gDebug && fr < c.minRead) printPicture(tel, 0, 60);
    if (c.snr >= 10) {
        CHECK(std::fabs(tel.spacingHz - c.spacing * (1 + c.ppm * 1e-6)) < 0.15, "%s: spacing %.2f", c.name, tel.spacingHz);
        CHECK(std::fabs(tel.baseHz - (c.base + (c.lsb ? -c.cfo : c.cfo))) < 2.0, "%s: base %.1f", c.name, tel.baseHz);
        CHECK(std::fabs(tel.charSec - c.charSec / (1 + c.ppm * 1e-6)) < 0.03 * c.charSec, "%s: %.3f s a character", c.name, tel.charSec);
    }
    return fr;
}

static void testNoise() {
    // noise alone: nothing read
    Cp16Receiver rx;
    const double rate = 250000;
    rx.configure(rate);
    rx.setSignalOffset(-cp16Tuning().tuneOffsetHz);
    std::vector<cf32> buf(25000);
    uint32_t s = 12345;
    for (int k = 0; k < 20 * 10; k++) {
        for (auto& v : buf) {
            auto u = [&] { s = s * 1664525u + 1013904223u; return ((double)(s >> 8) + 0.5) / 16777216.0; };
            const double r = std::sqrt(-2 * std::log(u())), a = 2 * 3.141592653589793 * u();
            v = cf32((float)(0.02 * r * std::cos(a)), (float)(0.02 * r * std::sin(a)));
        }
        rx.feed(buf.data(), buf.size());
    }
    Cp16Telemetry tel;
    rx.telemetry(tel, 0);
    CHECK(tel.blocksOk == 0, "noise: %llu characters read as sure (\"%s\")", (unsigned long long)tel.blocksOk, tel.text.c_str());
    std::printf("noise alone: state %d, comb %.1f, %zu characters, %llu sure\n", tel.state, tel.combScore, tel.chars.size(), (unsigned long long)tel.blocksOk);
}

static void testTx() {
    TxRequest r;
    r.text = "BG1ABC 你好";
    r.opt["cp16.char"] = 1.5;
    r.opt["cp16.spacing"] = 12;
    r.opt["cp16.base"] = 1200;
    const double rate = 250000;
    std::string err;
    auto g = makeCp16TxGenerator(r, rate, err);
    CHECK(g != nullptr, "tx: %s", err.c_str());
    if (!g) return;
    // the transmitter's band through the receiver (the receiver is tuned to the dial: no offset)
    Cp16Receiver rx;
    if (gDebug) rx.setLogCallback([](const std::string& m) { std::printf("  log: %s\n", m.c_str()); });
    rx.configure(rate);
    rx.setSignalOffset(0);
    std::vector<cf32> buf(25000);
    double peak = 0;
    int blocks = 0;
    while (!g->finished() && blocks < 4000) { g->generate(buf.data(), buf.size()); for (auto& v : buf) peak = std::max(peak, (double)std::abs(v)); rx.feed(buf.data(), buf.size()); blocks++; }
    CHECK(g->finished(), "tx: the message does not end");
    std::fill(buf.begin(), buf.end(), cf32(0, 0));
    for (int k = 0; k < 70; k++) rx.feed(buf.data(), buf.size());
    Cp16Telemetry tel;
    rx.telemetry(tel, 0);
    const double fr = readFraction(cp16::textCells(r.text), tel.text);
    std::printf("tx: %.1f s, peak %.2f, read \"%s\" (%.0f %%)\n", blocks * 0.1, peak, tel.text.c_str(), 100 * fr);
    CHECK(peak < 1.0, "tx peak %.2f", peak);
    CHECK(fr >= 0.99, "tx: read %.0f %%", 100 * fr);
    if (gDebug && fr < 0.99) { std::printf("tx levels: state %d base %.1f spacing %.2f row %.1f ms snr %.1f\n", tel.state, tel.baseHz, tel.spacingHz, tel.rowSec * 1000, tel.snrDb); printPicture(tel, 0, 60); }
    TxRequest bad = r;
    bad.opt["cp16.spacing"] = 50;
    CHECK(makeCp16TxGenerator(bad, rate, err) == nullptr, "tx: 50 Hz spacing accepted");
}

// a mono WAV as the upper-sideband complex signal at 250 kHz (analytic signal by FFT, then linear interpolation)
static bool wavToIq(const std::string& path, std::vector<cf32>& out, double rate) {
    const auto d = readAll(path);
    if (d.size() < 44) return false;
    size_t off = 12, dataOff = 0, dataLen = 0;
    int ch = 1, fs = 8000, bits = 16;
    while (off + 8 <= d.size()) {
        const uint32_t len = d[off + 4] | d[off + 5] << 8 | d[off + 6] << 16 | (uint32_t)d[off + 7] << 24;
        if (!std::memcmp(&d[off], "fmt ", 4)) { ch = d[off + 10] | d[off + 11] << 8; fs = (int)(d[off + 12] | d[off + 13] << 8 | d[off + 14] << 16); bits = d[off + 22]; }
        if (!std::memcmp(&d[off], "data", 4)) { dataOff = off + 8; dataLen = std::min<size_t>(len, d.size() - dataOff); break; }
        off += 8 + len + (len & 1);
    }
    if (!dataOff || bits != 16) return false;
    const size_t n = dataLen / (2 * (size_t)ch);
    // down to 8 kHz first when the file is faster (box average: the CP16 carriers are below 2 kHz)
    const int dec = std::max(1, fs / 8000);
    const double fa = (double)fs / dec;
    std::vector<float> a(n / (size_t)dec);
    for (size_t i = 0; i < a.size(); i++) {
        double s = 0;
        for (int k = 0; k < dec; k++) s += (int16_t)(d[dataOff + ((i * dec + k) * ch) * 2] | d[dataOff + ((i * dec + k) * ch) * 2 + 1] << 8);
        a[i] = (float)(s / dec / 32768.0);
    }
    // analytic signal, a block FFT of the whole file (power of two)
    size_t N = 1;
    while (N < a.size()) N <<= 1;
    std::vector<std::complex<double>> x(N);
    for (size_t i = 0; i < a.size(); i++) x[i] = a[i];
    auto fft = [&](bool inv) {
        for (size_t i = 1, j = 0; i < N; i++) { size_t bit = N >> 1; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit; if (i < j) std::swap(x[i], x[j]); }
        for (size_t len = 2; len <= N; len <<= 1) {
            const double ang = 2 * 3.141592653589793 / (double)len * (inv ? 1 : -1);
            const std::complex<double> wl(std::cos(ang), std::sin(ang));
            for (size_t i = 0; i < N; i += len) { std::complex<double> w(1); for (size_t j = 0; j < len / 2; j++) { auto u = x[i + j], v = x[i + j + len / 2] * w; x[i + j] = u + v; x[i + j + len / 2] = u - v; w *= wl; } }
        }
    };
    fft(false);
    for (size_t k = N / 2 + 1; k < N; k++) x[k] = 0;
    for (size_t k = 1; k < N / 2; k++) x[k] *= 2;
    fft(true);
    const size_t m = (size_t)((double)a.size() * rate / fa);
    out.resize(m);
    for (size_t i = 0; i < m; i++) {
        const double t = (double)i * fa / rate;
        const size_t k = (size_t)t;
        const double u = t - (double)k;
        const auto v = k + 1 < a.size() ? x[k] * (1 - u) + x[k + 1] * u : x[std::min(k, a.size() - 1)];
        out[i] = cf32((float)(v.real() / (double)N * 0.3), (float)(v.imag() / (double)N * 0.3));
    }
    return true;
}

static Cp16Telemetry runWav(const std::string& path, double padSec, double base = 0, double spacing = 0, double charSec = 0) {
    Cp16Telemetry tel;
    std::vector<cf32> iq;
    const double rate = 250000;
    if (!wavToIq(path, iq, rate)) { std::printf("cannot read %s\n", path.c_str()); return tel; }
    Cp16Receiver rx;
    rx.configure(rate);
    rx.setSignalOffset(0);
    if (base > 0) rx.setManual(base, spacing, charSec, 1);
    for (size_t i = 0; i < iq.size(); i += 25000) rx.feed(iq.data() + i, std::min<size_t>(25000, iq.size() - i));
    std::vector<cf32> z(25000, cf32(0, 0));
    for (int k = 0; k < (int)(padSec * 10); k++) rx.feed(z.data(), z.size());
    rx.telemetry(tel, 0);
    return tel;
}

static void printPicture(const Cp16Telemetry& t, size_t from, size_t n) {
    for (size_t r = from; r < std::min(t.rows.size(), from + n); r++) {
        char b[20] = {};
        for (int c = 0; c < 16; c++) b[c] = t.rows[r][(size_t)c] >= 128 ? '#' : '.';
        std::printf("%4zu %s\n", r, b);
    }
}

// the dots of the first cells against CP16+'s own truth files (GT16: 16-byte header, then rows x cols bytes a cell), at the best row offset
static double dotAccuracy(const Cp16Telemetry& t, const std::vector<uint8_t>& truth, int cells) {
    if (truth.size() < 16) return 0;
    const int n = std::min<int>(truth[4] | truth[5] << 8, cells);
    double best = 0;
    for (size_t off = 0; off + (size_t)n * 17 <= t.rows.size(); off++) {
        size_t ok = 0, all = 0;
        for (int c = 0; c < n; c++)
            for (int r = 0; r < 16; r++)
                for (int k = 0; k < 16; k++) {
                    const bool got = t.rows[off + (size_t)c * 17 + 1 + (size_t)r][(size_t)k] >= 128;
                    const bool want = truth[16 + (size_t)c * 256 + (size_t)r * 16 + (size_t)k] != 0;
                    ok += got == want; all++;
                }
        best = std::max(best, (double)ok / (double)all);
    }
    return best;
}

static void testCp16Plus() {
    // CP16+ (BD8DQS, MIT): not CP16, but the same picture: 16 carriers 25 Hz apart from 800 Hz, 80 ms rows (M2), a margin row before each character, a fixed
    // font of its own. Automatic search must find the carriers and rows; with them fixed, the dots must match the truth files of the recordings.
    struct F { const char* file; const char* truth; int cells; };
    const F files[] = {{"m2_snrp6_M2.wav", "m2_snrp6_M2.truth", 8}, {"m1_snrp6_M2.wav", "m1_snrp6_M2.truth", 8}};
    for (const F& f : files) {
        const std::string dir = std::string(DECT2_SOURCE_DIR) + "/tests/cp16/data/";
        const Cp16Telemetry a = runWav(dir + f.file, 8);
        std::printf("CP16+ %s automatic: lowest carrier %.1f Hz spacing %.2f row %.1f ms\n", f.file, a.baseHz, a.spacingHz, a.rowSec * 1000);
        CHECK(a.state == 2, "CP16+ %s: rows not found", f.file);
        CHECK(std::fabs(a.spacingHz - 25) < 0.3, "CP16+ %s: spacing %.2f", f.file, a.spacingHz);
        CHECK(std::fabs(a.rowSec - 0.080) < 0.003, "CP16+ %s: row %.1f ms", f.file, a.rowSec * 1000);
        const Cp16Telemetry t = runWav(dir + f.file, 8, 800, 25, 17 * 0.080);
        const double acc = dotAccuracy(t, readAll(dir + f.truth), f.cells);
        std::printf("CP16+ %s fixed: dots right %.1f %%\n", f.file, 100 * acc);
        CHECK(acc >= 0.97, "CP16+ %s: dots right %.1f %%", f.file, 100 * acc);
        if (gDebug && acc < 0.97) { std::printf("fixed: base %.1f spacing %.2f row %.2f ms\n", t.baseHz, t.spacingHz, t.rowSec * 1000); printPicture(t, 0, 80); }
    }
}

int main() {
    const bool longRun = std::getenv("ONAIR_TEST_LONG") != nullptr;
    gDebug = std::getenv("CP16_DEBUG") != nullptr;
    if (!loadNoto()) { std::printf("FAIL: no font\n"); return 1; }
    testFont();
    static std::deque<std::string> names;   // the cases point at these
    std::vector<Case> cases = {
        {"2 s 15 Hz USB", 2.0, 15, 1000, false, 0, 0, 30, 0, 0.95},
        {"1 s 20 Hz LSB +40 Hz +80 ppm", 1.0, 20, 1500, true, 40, 80, 20, 1, 0.95},
        {"3 s 11 Hz -73 Hz -120 ppm", 3.0, 11, 700, false, -73, -120, 15, 0, 0.9},
        {"2 s 15 Hz SNR -8 dB", 2.0, 15, 1000, false, 12, 30, -8, 0, 0.7},
    };
    if (longRun) {
        for (double cs : cp16::kCharSecChoices)
            for (double sp : cp16::kSpacingChoices)
                for (int side = 0; side < 2; side++) {
                    char b[64];
                    snprintf(b, sizeof b, "%.1f s %.0f Hz %s", cs, sp, side ? "LSB" : "USB");
                    names.push_back(b);
                    cases.push_back({names.back().c_str(), cs, sp, 900 + 37 * sp, side == 1, 25, -60, 20, 1, sp * cs / 17 >= 1.0 ? 0.9 : 0.6});
                }
        for (double snr : {10.0, 0.0, -5.0, -10.0, -13.0}) {
            char b[64];
            snprintf(b, sizeof b, "SNR %.0f dB", snr);
            names.push_back(b);
            cases.push_back({names.back().c_str(), 2.0, 15, 1000, false, 0, 0, snr, 0, snr >= -5 ? 0.85 : 0.0});
        }
    }
    for (const Case& c : cases) runCase(c, true);
    testNoise();
    testTx();
    testCp16Plus();
    if (const char* crac = std::getenv("ONAIR_CP16_CRAC_WAV")) {
        const Cp16Telemetry t = runWav(crac, 8);
        std::printf("CRAC sample: base %.1f Hz spacing %.2f row %.1f ms (%.2f s a character) %s, read \"%s\"\n", t.baseHz, t.spacingHz, t.rowSec * 1000, t.charSec,
                    t.mirrored ? "mirrored" : "", t.text.c_str());
        printPicture(t, 0, 130);
    }
    if (gFail) { std::printf("%d failures\n", gFail); return 1; }
    std::printf("all CP16 checks passed\n");
    return 0;
}
