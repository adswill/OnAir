// DVB-T: channel-state-weighted soft decisions and narrowband-interferer erasure, measured end to end (generator -> channel -> receiver).
// For each channel (AWGN, static multipath, Doppler echo, CW interferers) and each receiver variant (flat noise weights, channel-state
// weights, channel-state weights + carrier erasure) the lowest SNR with at most 5 % damaged packets is found; the gains go to the log
// (and docs/OFDM_CSI.md). Checks: the erasure never costs more than 0.3 dB on a clean/multipath/Doppler channel, and gains >= 1 dB with CW tones.
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/dvbt/dvbt_rx.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;

struct Chan { const char* name; double echoDb; int echoDelay; double dopplerHz; int cwCount; double cwDbc; };
struct Out { bool locked = false; size_t good = 0, bad = 0; };

static Out run(const dvbt::Params& p, int frames, double snrDb, const Chan& ch) {
    const double fn = nativeRateHz(8);
    uint32_t counter = 0;
    dvbt::Generator gen(p, [&](uint8_t* pkt) {
        pkt[0] = 0x47; pkt[1] = 0x01; pkt[2] = 0x00; pkt[3] = 0x10;
        memcpy(pkt + 4, &counter, 4);
        for (int i = 8; i < 188; i++) pkt[i] = (uint8_t)(counter * 31 + i * 7);
        counter++;
    });
    DvbtReceiver rx;
    rx.configure(fn, 8);
    Out r;
    rx.setPacketCallback([&](const uint8_t* pk, size_t n, double) {
        for (size_t i = 0; i < n; i++) {
            const uint8_t* q = pk + i * 188;
            if (q[1] & 0x80) { if (r.good) r.bad++; continue; }
            uint32_t c; memcpy(&c, q + 4, 4);
            bool ok = q[0] == 0x47 && q[1] == 0x01 && q[2] == 0x00;
            for (int j = 8; j < 188 && ok; j++) ok = q[j] == (uint8_t)(c * 31 + j * 7);
            if (ok) r.good++; else if (r.good) r.bad++;
        }
    });
    std::mt19937 rng(11);
    dect2::normal_distribution<float> nd(0, 1);
    const double sigma = std::pow(10.0, -snrDb / 20.0) / std::sqrt(2.0);
    std::vector<cf32> sym, tx;
    for (int s = 0; s < frames * 68; s++) { gen.nextSymbol(sym); tx.insert(tx.end(), sym.begin(), sym.end()); }
    // signal power per sample (for the CW level in dBc)
    double pw = 0; for (auto& v : tx) pw += std::norm(v); pw /= (double)tx.size();
    std::vector<cf32> out;
    for (int i = 0; i < 40000; i++) out.push_back(cf32{nd(rng), nd(rng)} * (float)sigma);
    const int N = p.mode == dvbt::k8K ? 8192 : 2048;
    const double cwAmp = std::sqrt(pw / 1705.0) * std::pow(10.0, ch.cwDbc / 20.0);   // cwDbc: dB above the power of one data carrier
    std::vector<cf32> delayed((size_t)std::max(1, ch.echoDelay), cf32(0, 0));
    const float eg = ch.echoDb > 0 ? (float)std::pow(10.0, -ch.echoDb / 20.0) : 0.f;
    for (size_t n = 0; n < tx.size(); n++) {
        cf32 v = tx[n];
        if (ch.echoDb > 0) {
            const cf32 e = delayed[n % delayed.size()]; delayed[n % delayed.size()] = tx[n];
            const double ph = 2 * M_PI * ch.dopplerHz * (double)n / fn;
            v += e * eg * cf32((float)std::cos(ph), (float)std::sin(ph));
        }
        v += cf32{nd(rng), nd(rng)} * (float)sigma;
        for (int t = 0; t < ch.cwCount; t++) {   // tones exactly on carriers (the worst case for one carrier), spread over the channel
            const double cyc = (double)(40 + 55 * t) / (double)N;   // bin 40 + 55 t, in cycles per sample
            const double ph = 2 * M_PI * cyc * (double)n;
            v += cf32((float)(cwAmp * std::cos(ph)), (float)(cwAmp * std::sin(ph)));
        }
        out.push_back(v);
    }
    for (size_t i = 0; i < out.size(); i += 1 << 14) rx.feed(out.data() + i, std::min<size_t>(1 << 14, out.size() - i));
    rx.flush();
    RxTelemetry t; rx.telemetry(t, 0);
    r.locked = t.state >= 1 && t.dvbt.tpsOk;
    return r;
}

static bool passes(const Out& o) { return o.locked && o.good > 80 && o.bad * 20 <= o.good + o.bad; }

// lowest SNR (0.5 dB steps, descending from `start`) at which the packets still come out; 99 if none
static double threshold(const dvbt::Params& p, int frames, double start, const Chan& ch) {
    double last = 99;
    int fails = 0;
    for (double snr = start; snr > 0; snr -= 0.5) {
        if (passes(run(p, frames, snr, ch))) { last = snr; fails = 0; } else if (++fails >= 2) break;
    }
    return last;
}

int main() {
    dvbt::Params p; p.mode = dvbt::k2K; p.guard = dvbt::kGi4; p.mod = dvbt::k16Qam; p.crHp = dvbt::kR23; p.crLp = dvbt::kR23;
    const int frames = 14;
    const Chan chans[] = {
        {"AWGN", 0, 0, 0, 0, 0},
        {"static echo -2 dB @ 40 samples (deep notches)", 2, 40, 0, 0, 0},
        {"Doppler echo -3 dB @ 30 samples, 120 Hz", 3, 30, 120, 0, 0},
        {"24 CW tones, each 6 dB above a carrier", 0, 0, 0, 24, 6},
        {"24 CW tones, each 3 dB above a carrier", 0, 0, 0, 24, 3},
        {"echo -3 dB @ 30 + 24 CW tones at +3 dB", 3, 30, 60, 24, 3},
    };
    struct V { const char* name; const char* flat; const char* noerase; } vars[] = {
        {"flat n0", "1", "1"}, {"channel-state", nullptr, "1"}, {"channel-state + erase", nullptr, nullptr}};
    int fails = 0;
    printf("%-48s %12s %15s %22s   gain csi  gain erase\n", "channel (2K 16-QAM 2/3 GI 1/4, threshold = SNR dB for <=5%% damaged packets)", vars[0].name, vars[1].name, vars[2].name);
    for (const Chan& ch : chans) {
        double th[3];
        for (int v = 0; v < 3; v++) {
            if (vars[v].flat) setenv("DECT2_OFDM_FLAT_N0", vars[v].flat, 1); else unsetenv("DECT2_OFDM_FLAT_N0");
            if (vars[v].noerase) setenv("DECT2_OFDM_NO_ERASE", vars[v].noerase, 1); else unsetenv("DECT2_OFDM_NO_ERASE");
            th[v] = threshold(p, frames, ch.cwCount ? 30 : 26, ch);
        }
        printf("%-48s %12.1f %15.1f %22.1f   %+.1f dB   %+.1f dB\n", ch.name, th[0], th[1], th[2], th[0] - th[1], th[1] - th[2]);
        fflush(stdout);
        if (th[1] > 98 && th[0] > 98) { /* the channel is too hard for either variant: no claim */ }
        if (!ch.cwCount) { if (th[2] > th[1] + 0.3) { printf("FAIL: erasure costs %.1f dB on %s\n", th[2] - th[1], ch.name); fails++; } }
        else if (th[1] - th[2] < 1.0 && th[1] < 98) { printf("FAIL: erasure gains only %.1f dB on %s\n", th[1] - th[2], ch.name); fails++; }
        if (th[1] < 98 && th[1] > th[0] + 0.3) { printf("FAIL: channel-state weighting costs %.1f dB on %s\n", th[1] - th[0], ch.name); fails++; }
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
