// STANAG 4539 / MIL-STD-188-110C Appendix C high-rate modem through Watterson-model HF channels (tests/common/watterson.h): the
// decision-feedback equaliser of hfutil/hfu_hdr against the conditions of C.6 (ITU-R Poor: two equal Rayleigh paths 2 ms apart with a
// 1 Hz fading spread; ITU-R Moderate: 1 ms, 0.5 Hz; the Doppler shift test of C.6.3 at +-75 Hz), with noise measured in 3 kHz.
// Bit errors are counted on the decoded bytes. Simulated signals only.
// ONAIR_TEST_LONG=1 also measures the BER of every rate with the very long interleaver at the SNRs of table C-XV and prints the table.
#include "dect2/hfutil/hfu_hdr.h"
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
#include "../common/watterson.h"
#include "../common/test_long.h"

namespace {

std::string payload(size_t n, unsigned seed) {
    std::mt19937 r(seed);
    std::string s(n, '\0');
    for (auto& c : s) c = (char)(r() & 0xFF);
    return s;
}

enum class Ch { Awgn, Moderate, Poor };

struct Trial {
    int bps = 3200, frames = 9;
    Ch ch = Ch::Poor;
    double snr = 20, cfo = 0, ppm = 0, driftHz = 0;
    bool sweep = false;     // C.6.4: a triangle of 3.5 Hz/s between -75 and +75 Hz
    double sec = 5;          // message length in seconds of user data
    unsigned seed = 1;
    bool eq = true;
};
struct Result {
    size_t bits = 0, errs = 0, missedBytes = 0;
    uint64_t messages = 0;
    bool lost = false, joined = false;
    double ber() const { return bits ? (double)errs / (double)bits : 1.0; }
};

Result trial(const Trial& t) {
    const size_t nBytes = std::max<size_t>(16, (size_t)(t.sec * t.bps / 8));
    const std::string msg = payload(nBytes, t.seed * 7919u + (unsigned)t.bps);
    hfu::HdrGenCfg g;
    g.bps = t.bps; g.frames = t.frames; g.leadSec = 0.3; g.tailSec = 0.5;
    std::vector<cf32> x = hfu::genHighRate(g, hfu::hdrBytesToBits(msg));
    const size_t a = (size_t)(0.3 * 24000), b = x.size() - (size_t)(0.5 * 24000);
    const double ps = watterson::power(x, a, b);
    if (t.ch != Ch::Awgn) x = watterson::apply(x, t.ch == Ch::Poor ? watterson::ituPoor(t.seed) : watterson::ituModerate(t.seed));
    if (t.ppm != 0) x = impair::clock(x, t.ppm);
    if (t.cfo != 0) impair::shift(x, t.cfo, 24000);
    if (t.sweep) {
        double ph = 0, f = 0, dir = 1;
        for (size_t i = 0; i < x.size(); i++) {
            f += dir * 3.5 / 24000.0;
            if (f > 75) { f = 75; dir = -1; } else if (f < -75) { f = -75; dir = 1; }
            ph += 2 * M_PI * f / 24000.0;
            x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph));
        }
    }
    if (t.driftHz != 0) {
        double ph = 0;
        for (size_t i = 0; i < x.size(); i++) { ph += 2 * M_PI * (t.driftHz * (double)i / 24000.0) / 24000.0; x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
    }
    watterson::noise3k(x, t.snr, ps, 24000, t.seed + 100);
    hfu::HighRateRx rx;
    rx.setEqualizer(t.eq);
    std::string got;
    Result r;
    rx.setByteSink([&](uint8_t v) { got.push_back((char)v); });
    rx.setEventSink([&](const hfu::SerialEvent& e) {
        if (e.text.find("signal lost") != std::string::npos) r.lost = true;
        if (e.text.find("joined") != std::string::npos) r.joined = true;
    });
    for (size_t i = 0; i < x.size(); i += 4096) rx.push(x.data() + i, std::min<size_t>(4096, x.size() - i));
    r.messages = rx.status().messages;
    // bit errors over the decoded part. When the start was missed (a deep fade on the preamble) the receiver joins at a reinserted
    // preamble and delivers the rest of the message: those bytes are compared with the end of the message, the rest is counted as missed.
    // Bytes not delivered at the end count as wrong (half their bits).
    const size_t off = r.joined && got.size() < msg.size() ? msg.size() - got.size() : 0;
    r.missedBytes = off;
    r.bits = (msg.size() - off) * 8;
    for (size_t i = off; i < msg.size(); i++) {
        if (i - off < got.size()) r.errs += (size_t)__builtin_popcount((unsigned)(uint8_t)(msg[i] ^ got[i - off]));
        else r.errs += 4;
    }
    return r;
}

const char* chName(Ch c) { return c == Ch::Awgn ? "AWGN" : c == Ch::Poor ? "ITU-R Poor" : "ITU-R Moderate"; }

Result report(const char* what, const Trial& t) {
    const Result r = trial(t);
    printf("  %-24s %5d bit/s %-11s %-14s %4.0f dB %+4.0f Hz eq %-3s: BER %.2e (%zu / %zu bits)%s%s\n", what, t.bps, hfu::hdrInterleaverName(t.frames),
           chName(t.ch), t.snr, t.cfo, t.eq ? "on" : "off", r.ber(), r.errs, r.bits, r.lost ? ", lost" : "", r.missedBytes ? ", start missed" : "");
    return r;
}

}  // namespace

int main() {
    int fails = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { printf("FAIL: %s\n", what); fails++; } };

    // the simulator itself: two equal paths, average power kept, Rayleigh statistics (about 63 % of samples below the mean power)
    {
        std::vector<cf32> one(24000 * 20, cf32(1, 0));
        const auto y = watterson::apply(one, watterson::ituPoor(5));
        const double p = watterson::power(y, 2000, y.size());
        size_t below = 0;
        for (size_t i = 2000; i < y.size(); i++) below += std::norm(y[i]) < p;
        const double frac = (double)below / (double)(y.size() - 2000);
        printf("Watterson: average power %.2f, %.0f %% of samples below it\n", p, frac * 100);
        check(p > 0.6 && p < 1.5 && frac > 0.5 && frac < 0.75, "Watterson channel statistics");
        // the fading rate: the tap's autocorrelation falls to about exp(-1/2) (sigma = 0.5 Hz: 1 / (2 pi 0.5)) after about 0.32 s
        std::vector<cf32> imp(24000 * 60, cf32(1, 0));
        watterson::Cfg c1; c1.paths = {{0, 0}}; c1.spreadHz = 1.0; c1.seed = 9;
        const auto g = watterson::apply(imp, c1);
        auto acf = [&](size_t lag) { std::complex<double> s = 0; double e = 0; for (size_t i = 0; i + lag < g.size(); i += 7) { s += std::complex<double>(g[i + lag]) * std::conj(std::complex<double>(g[i])); e += std::norm(g[i]); } return std::abs(s) / e; };
        const double r32 = acf((size_t)(0.32 * 24000));
        printf("Watterson: tap autocorrelation at 0.32 s %.2f (Gaussian spectrum, 1 Hz two-sigma: 0.61)\n", r32);
        check(r32 > 0.4 && r32 < 0.8, "Watterson fading rate");
    }

    printf("STANAG 4539 through Watterson channels:\n");
    // ITU-R Poor: the equaliser decodes where the one-tap channel interpolation cannot
    {   // table C-XV's ITU-R Poor level for 3200 bit/s, very long interleaver, three interleaver blocks
        Trial t; t.bps = 3200; t.frames = 72; t.ch = Ch::Poor; t.snr = 15; t.sec = 3 * 8.64 - 1; t.seed = 1;
        const Result on = report("poor, equaliser", t);
        t.eq = false;
        const Result off = report("poor, no equaliser", t);
        check(on.ber() < 1e-3 && on.messages == 1, "3200 bit/s ITU-R Poor 15 dB with the equaliser");
        check(off.ber() > 0.05, "without the equaliser the ITU-R Poor channel does not decode");
    }
    {
        Trial t; t.bps = 9600; t.frames = 9; t.ch = Ch::Poor; t.snr = 36; t.sec = 6; t.seed = 3;
        check(report("poor, 64QAM", t).ber() < 1e-2, "9600 bit/s ITU-R Poor 36 dB");
    }
    {
        Trial t; t.bps = 6400; t.frames = 9; t.ch = Ch::Moderate; t.snr = 26; t.sec = 4; t.seed = 3;
        check(report("moderate, 16QAM", t).ber() < 1e-3, "6400 bit/s ITU-R Moderate 26 dB");
    }
    {   // AWGN with the equaliser at the 3200 / 9600 levels the one-tap receiver reached
        Trial t; t.bps = 3200; t.frames = 9; t.ch = Ch::Awgn; t.snr = 11; t.sec = 3;
        check(report("AWGN, 8PSK", t).errs == 0, "3200 bit/s AWGN 11 dB");
        t.bps = 9600; t.snr = 24;
        check(report("AWGN, 64QAM", t).errs == 0, "9600 bit/s AWGN 24 dB");
    }
    // C.6.3 Doppler shift test (shortened from 5 minutes to 20 s): 9600 bit/s, +-75 Hz, 2 ms, 1 Hz, 30 dB; synchronisation is kept
    for (double f : {75.0, -75.0}) {
        Trial t; t.bps = 9600; t.frames = 72; t.ch = Ch::Poor; t.snr = 30; t.cfo = f; t.sec = 20; t.seed = f > 0 ? 4u : 6u;
        const Result r = report("C.6.3 Doppler shift", t);
        check(!r.lost && r.messages == 1, "C.6.3: synchronisation kept at +-75 Hz through two reinserted preambles");   // C.6.3 asks for no BER
    }
    // a sample clock 40 ppm off in ITU-R Poor fading (the channel window follows the drift)
    {
        Trial t; t.bps = 6400; t.frames = 9; t.ch = Ch::Poor; t.snr = 30; t.ppm = 40; t.sec = 20; t.seed = 7;
        const Result r = report("poor, clock +40 ppm", t);
        check(!r.lost && r.ber() < 1e-2, "ITU-R Poor with a 40 ppm clock");
    }
    // C.6.4 Doppler sweep (shortened from 1 hour to 30 s): 9600 bit/s very long, AWGN 24 dB, the frequency a triangle of 3.5 Hz/s
    // between -75 and +75 Hz (starting at 0 and rising)
    {
        Trial t; t.bps = 9600; t.frames = 72; t.ch = Ch::Awgn; t.snr = 24; t.sweep = true; t.sec = 30; t.seed = 9;
        const Result r = report("C.6.4 Doppler sweep", t);
        check(!r.lost && r.ber() < 1e-4, "C.6.4: 3.5 Hz/s triangle sweep");
    }

    if (testLong()) {
        // table C-XV: very long interleaver (72 frames, 8.64 s), BER <= 1e-5 required over 5 hours. Measured here over 10 fading
        // realisations of 3 interleaver blocks each (26 s) per rate.
        printf("Table C-XV, very long interleaver (simulated: 10 x 26 s per rate, not the 5 hours of the standard):\n");
        const int rates[5] = {3200, 4800, 6400, 8000, 9600};
        const double awgn[5] = {9, 13, 16, 19, 21}, poor[5] = {15, 20, 24, 28, 33};
        for (int i = 0; i < 5; i++) {
            Trial t; t.bps = rates[i]; t.frames = 72; t.ch = Ch::Awgn; t.snr = awgn[i]; t.sec = 3 * 8.64 - 1; t.seed = 11 + (unsigned)i;
            const Result a = report("C-XV AWGN", t);
            for (double extra : {0.0, 3.0}) {
                size_t e = 0, n = 0, missed = 0;
                for (unsigned sd = 1; sd <= 10; sd++) {
                    Trial p = t; p.ch = Ch::Poor; p.snr = poor[i] + extra; p.seed = sd;
                    char nm[32]; snprintf(nm, sizeof nm, "  seed %u", sd);
                    const Result r = getenv("HDREQ_VERBOSE") ? report(nm, p) : trial(p);
                    e += r.errs; n += r.bits; missed += r.missedBytes > 0;
                }
                printf("  -> %d bit/s ITU-R Poor %.0f dB: BER %.2e over %zu bits, start missed in %zu of 10%s\n", rates[i], poor[i] + extra, n ? (double)e / (double)n : 1.0, n,
                       missed, extra > 0 ? " (3 dB above the table)" : "");
                if (extra == 0) check((double)e / (double)std::max<size_t>(1, n) < 1e-2, "C-XV Poor channel within 1e-2");
            }
            check(a.ber() < 1e-4, "C-XV AWGN");
        }
    }

    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("all passed\n");
    return 0;
}
