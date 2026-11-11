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
    double snr = 20, cfo = 0;
    double sec = 5;          // message length in seconds of user data
    unsigned seed = 1;
    bool eq = true;
};
struct Result { size_t bits = 0, errs = 0; uint64_t messages = 0; bool lost = false; double ber() const { return bits ? (double)errs / (double)bits : 1.0; } };

Result trial(const Trial& t) {
    const size_t nBytes = std::max<size_t>(16, (size_t)(t.sec * t.bps / 8));
    const std::string msg = payload(nBytes, t.seed * 7919u + (unsigned)t.bps);
    hfu::HdrGenCfg g;
    g.bps = t.bps; g.frames = t.frames; g.leadSec = 0.3; g.tailSec = 0.5;
    std::vector<cf32> x = hfu::genHighRate(g, hfu::hdrBytesToBits(msg));
    const size_t a = (size_t)(0.3 * 24000), b = x.size() - (size_t)(0.5 * 24000);
    const double ps = watterson::power(x, a, b);
    if (t.ch != Ch::Awgn) x = watterson::apply(x, t.ch == Ch::Poor ? watterson::ituPoor(t.seed) : watterson::ituModerate(t.seed));
    if (t.cfo != 0) impair::shift(x, t.cfo, 24000);
    watterson::noise3k(x, t.snr, ps, 24000, t.seed + 100);
    hfu::HighRateRx rx;
    rx.setEqualizer(t.eq);
    std::string got;
    Result r;
    rx.setByteSink([&](uint8_t v) { got.push_back((char)v); });
    rx.setEventSink([&](const hfu::SerialEvent& e) { if (e.text.find("signal lost") != std::string::npos) r.lost = true; });
    for (size_t i = 0; i < x.size(); i += 4096) rx.push(x.data() + i, std::min<size_t>(4096, x.size() - i));
    r.messages = rx.status().messages;
    // bit errors over the message; bytes not delivered count as all wrong (half of their bits on average)
    r.bits = msg.size() * 8;
    for (size_t i = 0; i < msg.size(); i++) {
        if (i < got.size()) r.errs += (size_t)__builtin_popcount((unsigned)(uint8_t)(msg[i] ^ got[i]));
        else r.errs += 4;
    }
    return r;
}

const char* chName(Ch c) { return c == Ch::Awgn ? "AWGN" : c == Ch::Poor ? "ITU-R Poor" : "ITU-R Moderate"; }

Result report(const char* what, const Trial& t) {
    const Result r = trial(t);
    printf("  %-28s %5d bit/s %-11s %-14s %4.0f dB %+4.0f Hz eq %-3s: BER %.2e (%zu / %zu bits), messages %llu%s\n", what, t.bps, hfu::hdrInterleaverName(t.frames),
           chName(t.ch), t.snr, t.cfo, t.eq ? "on" : "off", r.ber(), r.errs, r.bits, (unsigned long long)r.messages, r.lost ? ", lost" : "");
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
    {
        Trial t; t.bps = 3200; t.frames = 9; t.ch = Ch::Poor; t.snr = 18; t.sec = 6; t.seed = 1;
        const Result on = report("poor, equaliser", t);
        t.eq = false;
        const Result off = report("poor, no equaliser", t);
        check(on.ber() < 2e-3 && on.messages == 1, "3200 bit/s ITU-R Poor 18 dB with the equaliser");
        check(off.ber() > 10 * std::max(on.ber(), 1e-4), "the equaliser improves the ITU-R Poor channel");
    }
    {
        Trial t; t.bps = 9600; t.frames = 9; t.ch = Ch::Poor; t.snr = 36; t.sec = 4; t.seed = 2;
        check(report("poor, 64QAM", t).ber() < 2e-3, "9600 bit/s ITU-R Poor 36 dB");
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
    // C.6.3 Doppler shift test (shortened from 5 minutes): 9600 bit/s, +-75 Hz, 2 ms, 1 Hz, 30 dB; synchronisation is kept
    for (double f : {75.0, -75.0}) {
        Trial t; t.bps = 9600; t.frames = 9; t.ch = Ch::Poor; t.snr = 30; t.cfo = f; t.sec = 10; t.seed = f > 0 ? 4u : 5u;
        const Result r = report("C.6.3 Doppler shift", t);
        check(!r.lost && r.messages == 1, "C.6.3: synchronisation kept at +-75 Hz through a reinserted preamble");
        check(r.ber() < 2e-2, "C.6.3: the data still decodes");
    }

    if (testLong()) {
        // table C-XV: very long interleaver (72 frames, 8.64 s), BER <= 1e-5 required. Measured here over a few interleaver blocks.
        printf("Table C-XV, very long interleaver (simulated, %s):\n", "a few blocks per rate, not the 5 hours of the standard");
        const int rates[5] = {3200, 4800, 6400, 8000, 9600};
        const double awgn[5] = {9, 13, 16, 19, 21}, poor[5] = {15, 20, 24, 28, 33};
        for (int i = 0; i < 5; i++) {
            Trial t; t.bps = rates[i]; t.frames = 72; t.ch = Ch::Awgn; t.snr = awgn[i]; t.sec = 3 * 8.64; t.seed = 11 + (unsigned)i;
            report("C-XV AWGN", t);
            size_t e = 0, n = 0;
            for (unsigned s = 0; s < 3; s++) {
                Trial p = t; p.ch = Ch::Poor; p.snr = poor[i]; p.seed = 21 + 3 * (unsigned)i + s;
                const Result r = report("C-XV ITU-R Poor", p);
                e += r.errs; n += r.bits;
            }
            printf("  -> %d bit/s ITU-R Poor at %.0f dB: BER %.2e over %zu bits\n", rates[i], poor[i], n ? (double)e / (double)n : 1.0, n);
            check((double)e / (double)std::max<size_t>(1, n) < 1e-2, "C-XV Poor channel within 1e-2");
        }
        // 3 dB above table C-XV
        for (int i = 0; i < 5; i++) {
            Trial p; p.bps = rates[i]; p.frames = 72; p.ch = Ch::Poor; p.snr = poor[i] + 3; p.sec = 2 * 8.64; p.seed = 41 + (unsigned)i;
            report("C-XV Poor + 3 dB", p);
        }
    }

    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("all passed\n");
    return 0;
}
