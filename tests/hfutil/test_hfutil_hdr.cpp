// MIL-STD-188-110C Appendix C / STANAG 4539 high-rate modem (hfutil/hfu_hdr.h): the tables against the text of the standard, the coding of every
// rate and interleaver, and generated transmissions through the receiver with frequency error, drift, a sample clock error, noise in 3 kHz,
// an echo, AGC blocks, chunk sizes, two messages, joining an ongoing transmission and noise only. Simulated signals only.
// ONAIR_TEST_LONG=1 also sends all 30 rate / interleaver combinations through the waveform.
#include "dect2/hfutil/hfu_hdr.h"
#include "dect2/hfutil/hfutil_rx.h"
#include "dect2/hfutil/hfutil_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/impair.h"

namespace {

const int kRates[] = {3200, 4800, 6400, 8000, 9600};
const int kIntl[] = {1, 3, 9, 18, 36, 72};

std::string message(size_t n, unsigned seed = 1) {
    static const char* words[] = {"HF", "MODEM", "TEST", "OF", "THE", "HIGH", "RATE", "WAVEFORM", "STANAG", "4539", "QSL", "1234567890"};
    std::mt19937 r(seed);
    std::string s;
    while (s.size() < n) { s += words[r() % 12]; s += ' '; }
    s.resize(n);
    return s;
}

// noise for an SNR measured in 3 kHz (C.6.1), the signal power over the transmission
void noise3k(std::vector<cf32>& x, double snrDb, size_t from, size_t to, unsigned seed = 3) {
    double p = 0;
    for (size_t i = from; i < to; i++) p += std::norm(x[i]);
    p /= (double)std::max<size_t>(1, to - from);
    const double n0 = p / std::pow(10.0, snrDb / 10) * (24000.0 / 3000.0);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, (float)std::sqrt(n0 / 2));
    for (auto& v : x) v += cf32(g(rng), g(rng));
}

void drift(std::vector<cf32>& x, double hzPerSec) {
    double ph = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const double f = hzPerSec * (double)i / 24000.0;
        ph += 2 * M_PI * f / 24000.0;
        x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph));
    }
}

struct Out { std::string text; std::vector<std::string> ev; hfu::HdrStatus st; };

Out run(const std::vector<cf32>& x, size_t chunk = 4096) {
    Out o;
    hfu::HighRateRx rx;
    rx.setByteSink([&](uint8_t b) { o.text.push_back((char)b); });
    rx.setEventSink([&](const hfu::SerialEvent& e) { o.ev.push_back(e.text); });
    for (size_t i = 0; i < x.size(); i += chunk) rx.push(x.data() + i, std::min(chunk, x.size() - i));
    o.st = rx.status();
    return o;
}

bool hasEv(const Out& o, const char* s) { for (auto& e : o.ev) if (e.find(s) != std::string::npos) return true; return false; }

struct Case { int bps = 9600, frames = 1, agc = 0; double cfo = 0, driftHz = 0, ppm = 0, snr = 99, echoDb = -99; size_t chunk = 4096, len = 300; };

void wave(const char* name, const Case& c, bool exact = true) {
    const std::string msg = message(c.len, (unsigned)c.bps + (unsigned)c.frames);
    hfu::HdrGenCfg g;
    g.bps = c.bps; g.frames = c.frames; g.agcBlocks = c.agc; g.leadSec = 0.3; g.tailSec = 0.3;
    std::vector<cf32> x = hfu::genHighRate(g, hfu::hdrBytesToBits(msg));
    const size_t a = (size_t)(0.3 * 24000), b = x.size() - (size_t)(0.3 * 24000);
    if (c.echoDb > -90) impair::echo(x, 24, c.echoDb);   // 1 ms
    if (c.ppm != 0) x = impair::clock(x, c.ppm);
    if (c.cfo != 0) impair::shift(x, c.cfo, 24000);
    if (c.driftHz != 0) drift(x, c.driftHz);
    if (c.snr < 90) noise3k(x, c.snr, a, b);
    const Out o = run(x, c.chunk);
    const bool ok = o.text == msg;
    size_t same = 0;
    for (size_t i = 0; i < std::min(o.text.size(), msg.size()); i++) same += o.text[i] == msg[i];
    printf("%-40s %5d bit/s %2d fr: %s  %zu/%zu bytes right, cfo %+.1f Hz, SNR %.1f dB, EOM %llu\n", name, c.bps, c.frames, ok ? "ok " : "BAD", same, msg.size(),
           o.st.cfoHz, o.st.snrDb, (unsigned long long)o.st.eoms);
    if (exact) {
        CHECK(ok, "%s: got %zu bytes \"%.60s\"", name, o.text.size(), o.text.c_str());
        CHECK(o.st.eoms == 1, "%s: end of message %llu", name, (unsigned long long)o.st.eoms);
        CHECK(o.st.bps == c.bps && o.st.frames == c.frames, "%s: read %d / %d", name, o.st.bps, o.st.frames);
    }
}

}  // namespace

int main() {
    // ---- the tables of the standard ----
    {
        int d[3];
        hfu::hdrDValues(3200, 1, d); CHECK(d[0] == 0 && d[1] == 0 && d[2] == 4, "C-VIII 3200 US");
        hfu::hdrDValues(4800, 1, d); CHECK(d[0] == 0 && d[1] == 6 && d[2] == 2, "C-VIII 4800 US");
        hfu::hdrDValues(8000, 18, d); CHECK(d[0] == 4 && d[1] == 0 && d[2] == 0, "C-VIII 8000 M");
        hfu::hdrDValues(9600, 72, d); CHECK(d[0] == 4 && d[1] == 2 && d[2] == 6, "C-VIII 9600 VL");
        hfu::hdrDValues(6400, 9, d); CHECK(d[0] == 0 && d[1] == 4 && d[2] == 4, "C-VIII 6400 S");
        hfu::hdrDValues(12800, 1, d); CHECK(d[0] == 6 && d[1] == 6 && d[2] == 2, "C-VIII 12800");
        int n = 0;
        for (int r : {3200, 4800, 6400, 8000, 9600, 12800}) for (int f : kIntl) {
            if (!hfu::hdrValid(r, f)) continue;
            n++;
            hfu::hdrDValues(r, f, d);
            int r2 = 0, f2 = 0;
            CHECK(hfu::hdrFromD(d, r2, f2) && r2 == r && f2 == f, "D round trip %d %d", r, f);
            CHECK(hfu::hdrInputBits(r, f) * (r == 12800 ? 1 : 4) == hfu::hdrInterleaverBits(r, f) * (r == 12800 ? 1 : 3), "C-XII vs C-XIII %d %d", r, f);
        }
        CHECK(n == 31, "31 combinations, %d", n);
        CHECK(hfu::hdrInterleaverBits(3200, 1) == 512 && hfu::hdrInterleaverBits(9600, 72) == 110592 && hfu::hdrInputBits(8000, 36) == 34560, "C-XII / C-XIII");
        CHECK(hfu::hdrIncrement(3200, 1) == 97 && hfu::hdrIncrement(9600, 72) == 17329 && hfu::hdrIncrement(6400, 36) == 6985, "C-XIV");
        // C.5.3.3.2: the first load locations for 3200 bps, one frame
        const int want[8] = {0, 97, 194, 291, 388, 485, 70, 167};
        for (int i = 0; i < 8; i++) CHECK((i * hfu::hdrIncrement(3200, 1)) % 512 == want[i], "load location %d", i);
        // the scrambler: first value from the initial 00000001, period 511 bits
        CHECK(hfu::hdrScrambleValue(0, 3) == 1 && hfu::hdrScrambleValue(0, 6) == 1, "scrambler start");
        std::vector<int> seen(64, 0);
        for (int i = 0; i < 256; i++) seen[(size_t)hfu::hdrScrambleValue(i, 6)]++;
        int distinct = 0; for (int v : seen) distinct += v > 0;
        CHECK(distinct >= 60, "scrambler values spread, %d of 64", distinct);
    }
    // ---- coding and interleaving of every combination: clean, and with 0.5% of the bits wrong (hard decisions) ----
    {
        std::mt19937 r(5);
        for (int bps : {3200, 4800, 6400, 8000, 9600, 12800}) for (int f : kIntl) {
            if (!hfu::hdrValid(bps, f)) continue;
            const int K = hfu::hdrInputBits(bps, f);
            std::vector<uint8_t> in((size_t)K);
            for (auto& b : in) b = (uint8_t)(r() & 1);
            const auto tx = hfu::hdrEncodeBlock(in, bps, f);
            CHECK((int)tx.size() == hfu::hdrInterleaverBits(bps, f), "block size %d %d", bps, f);
            std::vector<float> soft(tx.size());
            int flipped = 0;
            for (size_t i = 0; i < tx.size(); i++) {
                bool b = tx[i];
                if (bps != 12800 && r() % 1000 < 5) { b = !b; flipped++; }
                soft[i] = b ? -1.f : 1.f;
            }
            const auto out = hfu::hdrDecodeBlock(soft, bps, f);
            int err = 0;
            for (int i = 0; i < K; i++) err += out[(size_t)i] != in[(size_t)i];
            CHECK(err == 0, "decode %d bit/s %d frames: %d errors (%d flipped)", bps, f, err, flipped);
        }
    }
    // ---- the waveform ----
    for (int bps : kRates) { Case c; c.bps = bps; c.cfo = 37; wave("clean, +37 Hz", c); }
    { Case c; c.bps = 12800; c.snr = 35; wave("uncoded 12800, 35 dB", c); }
    { Case c; c.bps = 9600; c.frames = 9; c.cfo = -75; c.snr = 30; wave("9600 S, -75 Hz, 30 dB", c); }
    { Case c; c.bps = 4800; c.frames = 3; c.cfo = 75; c.snr = 20; wave("4800 VS, +75 Hz, 20 dB", c); }
    { Case c; c.bps = 9600; c.frames = 3; c.driftHz = 3.5; c.cfo = -20; c.snr = 30; c.len = 2000; wave("9600 VS, drift 3.5 Hz/s", c); }
    { Case c; c.bps = 8000; c.frames = 3; c.ppm = 40; c.snr = 30; c.len = 2000; wave("8000 VS, clock +40 ppm", c); }
    { Case c; c.bps = 6400; c.frames = 3; c.ppm = -40; c.cfo = 12; c.snr = 25; c.len = 2000; wave("6400 VS, clock -40 ppm", c); }
    // AWGN a few dB above the levels of table C-XV
    { Case c; c.bps = 3200; c.frames = 9; c.snr = 11; wave("3200 S, 11 dB", c); }
    { Case c; c.bps = 4800; c.frames = 9; c.snr = 15; wave("4800 S, 15 dB", c); }
    { Case c; c.bps = 6400; c.frames = 9; c.snr = 19; wave("6400 S, 19 dB", c); }
    { Case c; c.bps = 8000; c.frames = 9; c.snr = 22; wave("8000 S, 22 dB", c); }
    { Case c; c.bps = 9600; c.frames = 9; c.snr = 24; wave("9600 S, 24 dB", c); }
    { Case c; c.bps = 9600; c.frames = 72; c.snr = 28; c.len = 1200; wave("9600 VL, 28 dB", c); }
    { Case c; c.bps = 4800; c.agc = 3; c.snr = 25; wave("4800 US, 3 AGC blocks", c); }
    { Case c; c.bps = 3200; c.frames = 3; c.echoDb = -12; c.snr = 25; wave("3200 VS, 1 ms echo -12 dB", c); }
    for (size_t ch : {size_t(7), size_t(100000)}) { Case c; c.bps = 6400; c.snr = 30; c.chunk = ch; char n[40]; snprintf(n, sizeof n, "chunks of %zu", ch); wave(n, c); }
    // two messages with a gap; each one read with its own rate
    {
        hfu::HdrGenCfg g; g.bps = 4800; g.frames = 3;
        std::vector<cf32> x = hfu::genHighRate(g, hfu::hdrBytesToBits("FIRST MESSAGE 111"));
        g.bps = 8000; g.frames = 1;
        const auto y = hfu::genHighRate(g, hfu::hdrBytesToBits("SECOND MESSAGE 222"));
        x.insert(x.end(), 12000, cf32(0, 0));
        x.insert(x.end(), y.begin(), y.end());
        impair::shift(x, -30, 24000);
        noise3k(x, 30, 0, x.size());
        const Out o = run(x);
        printf("two messages: \"%s\", %llu messages\n", o.text.c_str(), (unsigned long long)o.st.messages);
        CHECK(o.text == "FIRST MESSAGE 111SECOND MESSAGE 222", "two messages: \"%s\"", o.text.c_str());
        CHECK(o.st.messages == 2 && o.st.eoms == 2, "two messages: %llu / %llu", (unsigned long long)o.st.messages, (unsigned long long)o.st.eoms);
    }
    // joining an ongoing transmission: the first 3 s are missed, the receiver starts at the reinserted preamble (8.64 s in)
    {
        hfu::HdrGenCfg g; g.bps = 3200; g.frames = 1; g.leadSec = 0;
        const std::string msg = message(4600, 9);
        std::vector<cf32> x = hfu::genHighRate(g, hfu::hdrBytesToBits(msg));
        impair::skip(x, 3 * 24000);
        impair::shift(x, 21, 24000);
        noise3k(x, 25, 0, x.size());
        const Out o = run(x);
        // the receiver aligns its bytes at the reinserted preamble: superframe 1 holds 72 * 384 bits = 3456 bytes
        const std::string tailWant = msg.substr(3456);
        printf("joined late: %zu bytes, events: %s\n", o.text.size(), o.ev.empty() ? "" : o.ev[0].c_str());
        CHECK(hasEv(o, "joined at a reinserted preamble"), "late join event");
        CHECK(o.text == tailWant, "late join: %zu bytes, want %zu", o.text.size(), tailWant.size());
    }
    // noise only: nothing
    {
        std::vector<cf32> x(24000 * 6, cf32(0, 0));
        noise3k(x, 0, 0, 0);
        for (auto& v : x) v += cf32(1e-3f, 0);
        impair::noise(x, -20, 11);
        const Out o = run(x);
        CHECK(o.text.empty() && o.st.messages == 0, "noise: %zu bytes, %llu locks", o.text.size(), (unsigned long long)o.st.messages);
    }
    // through the HF utility receiver at 250 ksps, 20 kHz off the tuning (the way the engine feeds it)
    {
        hfu::HdrGenCfg g; g.bps = 6400; g.frames = 3;
        const std::string msg = "ONAIR STANAG 4539 THROUGH THE RECEIVER";
        const auto b = hfu::genHighRate(g, hfu::hdrBytesToBits(msg));
        const double rate = 250000;
        std::vector<cf32> x((size_t)((double)b.size() * rate / 24000.0));
        for (size_t i = 0; i < x.size(); i++) {
            const double p = (double)i * 24000.0 / rate;
            const size_t k = (size_t)p;
            const float fr = (float)(p - (double)k);
            const cf32 v = k + 1 < b.size() ? b[k] * (1 - fr) + b[k + 1] * fr : cf32(0, 0);
            const double a = 2 * M_PI * -20000.0 * (double)i / rate;
            x[i] = v * cf32((float)std::cos(a), (float)std::sin(a)) * 0.1f;
        }
        HfUtilReceiver rx(kHfuKindHdr);
        rx.configure(rate);
        rx.setSignalOffset(-20000);
        for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        HfUtilTelemetry t;
        rx.telemetry(t, 0);
        printf("receiver: \"%s\" mode \"%s\"\n", t.text.c_str(), t.mode.c_str());
        CHECK(t.text.find(msg) != std::string::npos, "receiver text \"%s\"", t.text.c_str());
        CHECK(hfUtilTuning(kHfuKindHdr).stdMode == kHfuHdrFamily + 2, "engine standard");
    }
    if (std::getenv("ONAIR_TEST_LONG")) {
        for (int r : kRates) for (int f : kIntl) { Case c; c.bps = r; c.frames = f; c.cfo = 15; c.snr = 30; c.len = f >= 36 ? 1500 : 400; wave("all combinations", c); }
    }
    return testSummary("hfutil_hdr");
}
