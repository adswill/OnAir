// ISDB-T: the same measurement as test_ofdm_csi (flat noise weights / channel-state weights / + narrowband-interferer erasure), end to end
// with the generator and tests/common/impair.h. Threshold = lowest SNR at which 90 % of the layer's packets arrive.
#include "dect2/isdbt/isdbt_gen.h"
#include "dect2/isdbt/isdbt_rx.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
using namespace dect2;
using namespace dect2::isdbt;

struct Chan { const char* name; double echoDb; int echoDelay; int cwCount; double cwDb; };

static bool run(const Params& p, const std::vector<cf32>& base, double snr, const Chan& ch, int nFrames) {
    std::vector<cf32> x = base;
    if (ch.echoDb > 0) impair::echo(x, ch.echoDelay, -ch.echoDb);
    if (ch.cwCount) {   // the tones are the same for every run of a channel: built once
        static std::vector<cf32> cw; static const Chan* built = nullptr;
        if (built != &ch) {
            double pw = 0; for (auto& v : x) pw += std::norm(v); pw /= (double)x.size();
            const double amp = std::sqrt(pw / 5617.0) * std::pow(10.0, ch.cwDb / 20.0);
            cw.assign(x.size(), cf32(0, 0));
            for (int t = 0; t < ch.cwCount; t++) {
                const double w = 2 * M_PI * (double)(100 + 110 * t) / 8192.0;
                const cf32 rot((float)std::cos(w), (float)std::sin(w));
                std::complex<double> ph(amp, 0), rd(std::cos(w), std::sin(w));
                for (size_t n = 0; n < x.size(); n++) { cw[n] += cf32((float)ph.real(), (float)ph.imag()); ph *= rd; if ((n & 0xffff) == 0xffff) ph *= amp / std::abs(ph); }
                (void)rot;
            }
            built = &ch;
        }
        for (size_t n = 0; n < x.size(); n++) x[n] += cw[n];
    }
    impair::noise(x, snr);
    IsdbtReceiver rx;
    rx.configure(kSampleRate);
    long good = 0;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) { int layer; unsigned c; if (!(pk[i * 188 + 1] & 0x80) && checkCountingPacket(pk + i * 188, &layer, &c)) good++; }
    });
    for (size_t i = 0; i < x.size(); i += 37000) rx.feed(x.data() + i, std::min<size_t>(37000, x.size() - i));
    return good >= (long)(0.9 * packetsPerFrame(p.mode, p.layer[0]) * (nFrames - 6));
}

static double threshold(const Params& p, const std::vector<cf32>& base, const Chan& ch, int nFrames) {
    double last = 99; int fails = 0;
    for (double snr = ch.cwCount ? 30 : 26; snr > 0; snr -= 1.0) {
        if (run(p, base, snr, ch, nFrames)) { last = snr; fails = 0; } else if (++fails >= 2) break;
    }
    return last;
}

int main() {
    Params p; p.mode = 3; p.guard = kGi8; p.layer[0].segments = 13; p.layer[0].mod = k16Qam; p.layer[0].rate = kR23; p.layer[0].ti = 0;
    const int nFrames = 14;
    Generator gen(p, countingSource(5), 1);
    std::vector<cf32> base, f;
    for (int i = 0; i < nFrames; i++) { gen.nextFrame(f); base.insert(base.end(), f.begin(), f.end()); }
    const Chan chans[] = {
        {"AWGN", 0, 0, 0, 0},
        {"static echo -2 dB @ 100 samples (deep notches)", 2, 100, 0, 0},
        {"24 CW tones, each 6 dB above a carrier", 0, 0, 24, 6},
        {"echo -3 dB + 24 CW tones at +6 dB", 3, 60, 24, 6},
    };
    struct V { const char* flat; const char* noerase; } vars[] = {{"1", "1"}, {nullptr, "1"}, {nullptr, nullptr}};
    int fails = 0;
    printf("ISDB-T mode 3 GI 1/8 16-QAM 2/3, 13 segments; threshold = SNR dB for 90%% of packets\n%-48s %9s %14s %20s\n", "channel", "flat n0", "channel-state", "channel-state+erase");
    for (const Chan& ch : chans) {
        double th[3];
        for (int v = 0; v < 3; v++) {
            if (vars[v].flat) setenv("DECT2_OFDM_FLAT_N0", vars[v].flat, 1); else unsetenv("DECT2_OFDM_FLAT_N0");
            if (vars[v].noerase) setenv("DECT2_OFDM_NO_ERASE", vars[v].noerase, 1); else unsetenv("DECT2_OFDM_NO_ERASE");
            th[v] = threshold(p, base, ch, nFrames);
        }
        printf("%-48s %9.1f %14.1f %20.1f   (csi %+.1f dB, erase %+.1f dB)\n", ch.name, th[0], th[1], th[2], th[0] - th[1], th[1] - th[2]);
        fflush(stdout);
        if (th[2] > th[1] + 1.01) { printf("FAIL: erasure costs %.1f dB on %s\n", th[2] - th[1], ch.name); fails++; }
        if (ch.cwCount && th[1] < 98 && th[1] - th[2] < 1.0) { printf("FAIL: erasure gains only %.1f dB on %s\n", th[1] - th[2], ch.name); fails++; }
        if (th[1] < 98 && th[1] > th[0] + 1.01) { printf("FAIL: channel-state weighting costs %.1f dB on %s\n", th[1] - th[0], ch.name); fails++; }
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
