// ATSC 1.0 (8-VSB) in the channels of ATSC A/74:2010 (Receiver Performance Guidelines), through the real AtscReceiver:
//   - the single static echoes of Table 5.6 (pre-echoes to -30 us, post-echoes to +40 us) with a slow 0.05 Hz rotation,
//   - the dynamic ensembles R.1 (two equal echoes at the same delay, one turning at up to 2 Hz: the echo comes and goes, up to 0 dB),
//     R2.1 #1-#4 and R2.2 #1-#3 (Tables 5.10, 5.11: six paths from -1.8 us to +39.8 us, path 5 turning at up to 5 Hz),
//   - burst noise (5.4.5: 165 us of noise at -5 dB every 100 ms), co-channel DTV (D/U +15.5 dB, Table 5.1), the first adjacent DTV
//     channel (D/U -20 and -33 dB, Table 5.2) seen by a 20 Msps radio, a strong carrier in the channel, and the sample rates of real radios
//     (6.5 to 20 Msps, 8-bit), each with a tuning error and a sample clock error.
// For each case the lowest SNR (5.38 MHz noise bandwidth, 1 dB steps) at which at least 90 % of the packets come out good, and no wrong
// ones, is printed as "threshold". Written independently of the receiver: the channel is applied here, to the generator's clean signal.
//   test_atsc_channels             all cases (guards: see kGuards below)
//   test_atsc_channels "a|b"       only the cases whose name contains one of the substrings
//   test_atsc_channels paced       a radio in real time (non-blocking receiver) on a hard channel, with a player that stalls now and then:
//                                  prints dropped fields and the share of good packets (ATSC_OLDDROP=1 ATSC_SYNCCB=1 gives 3.9's behaviour)
#include "dect2/atsc/atsc.h"
#include "dect2/atsc/atsc_gen.h"
#include "dect2/atsc/atsc_rx.h"
#include "dect2/common/stable_random.h"
#include "dect2/dvbt/dvbt_gen.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>
using namespace dect2;

static int fails = 0;
static constexpr double kPktPerSec = 312.0 / (atsc::kFieldSyms / atsc::kSymbolRate);   // 12890 packets a second

// ---- the channel
struct Path { double delayUs, attDb, phaseDeg, dopplerHz; };

static std::vector<cf32> cleanSignal(double rate, double seconds, unsigned seed = 1) {
    atsc::ChannelConfig cc;
    atsc::Generator gen(dvbt::testTsSource(), cc, rate, seed);
    std::vector<cf32> x;
    gen.generate((size_t)(seconds * rate), x);
    return x;
}

// y[n] = sum_k g_k exp(j (phi_k + 2 pi fd_k n / rate)) x(n - d_k), fractional delays by a 24-tap Blackman-windowed sinc. All delays are moved
// so that the earliest path has none (the receiver cannot tell).
static std::vector<cf32> multipath(const std::vector<cf32>& x, double rate, const std::vector<Path>& paths) {
    double dmin = 1e9;
    for (auto& p : paths) dmin = std::min(dmin, p.delayUs);
    const int H = 12;
    std::vector<cf32> y(x.size(), cf32(0, 0));
    for (auto& p : paths) {
        const double d = (p.delayUs - dmin) * 1e-6 * rate;
        const long di = (long)std::floor(d);
        const double fr = d - (double)di;
        float h[2 * H];
        double hs = 0;
        for (int k = 0; k < 2 * H; k++) {
            const double t = (double)(k - H + 1) - fr;   // tap k multiplies x[n - di - (k - H + 1)]
            const double s = std::fabs(t) < 1e-12 ? 1.0 : std::sin(M_PI * t) / (M_PI * t);
            const double u = (double)k / (2 * H - 1);
            const double w = 0.42 - 0.5 * std::cos(2 * M_PI * u) + 0.08 * std::cos(4 * M_PI * u);
            h[k] = (float)(s * w); hs += s * w;
        }
        for (int k = 0; k < 2 * H; k++) h[k] = (float)(h[k] / hs);
        const double g = std::pow(10.0, -p.attDb / 20.0);
        const double w = 2 * M_PI * p.dopplerHz / rate, ph0 = p.phaseDeg * M_PI / 180.0;
        for (size_t n = 0; n < x.size(); n++) {
            std::complex<float> acc(0, 0);
            const long base = (long)n - di + H - 1;
            for (int k = 0; k < 2 * H; k++) {
                const long i = base - k;
                if (i >= 0 && i < (long)x.size()) acc += h[k] * x[(size_t)i];
            }
            const double a = ph0 + w * (double)n;
            y[n] += acc * cf32((float)(g * std::cos(a)), (float)(g * std::sin(a)));
        }
    }
    return y;
}

static double power(const std::vector<cf32>& x) {
    double p = 0;
    for (auto& v : x) p += std::norm(v);
    return p / (double)std::max<size_t>(1, x.size());
}

// white noise for an SNR over the 5.38 MHz noise bandwidth of the 8-VSB channel (A/74: the signal power is that of all paths together)
static void addNoise(std::vector<cf32>& x, double rate, double snrDb, double sigPower, uint32_t seed) {
    std::mt19937 rng(seed);
    const double var = sigPower * std::pow(10.0, -snrDb / 10.0) * rate / 5.381e6;
    dect2::normal_distribution<float> g(0.f, (float)std::sqrt(var / 2));
    for (auto& v : x) v += cf32(g(rng), g(rng));
}

static void shiftHz(std::vector<cf32>& x, double hz, double rate) {
    const double w = 2 * M_PI * hz / rate;
    for (size_t n = 0; n < x.size(); n++) { const double a = w * (double)n; x[n] *= cf32((float)std::cos(a), (float)std::sin(a)); }
}

// the sample clock of the radio `ppm` fast: windowed sinc, 32 taps
static std::vector<cf32> clockErr(const std::vector<cf32>& x, double ppm) {
    const double step = 1.0 / (1.0 + ppm * 1e-6);
    const int H = 16;
    std::vector<cf32> y;
    y.reserve((size_t)((double)x.size() / step) + 1);
    for (double t = H; t < (double)x.size() - H - 1; t += step) {
        const long i0 = (long)std::floor(t);
        const double fr = t - (double)i0;
        std::complex<double> a = 0;
        double ws = 0;
        for (int k = -H + 1; k <= H; k++) {
            const double u = (double)k - fr;
            const double s = std::fabs(u) < 1e-12 ? 1.0 : std::sin(M_PI * u) / (M_PI * u);
            const double w = 0.5 + 0.5 * std::cos(M_PI * u / (H + 0.5));
            a += std::complex<double>(x[(size_t)(i0 + k)]) * (s * w); ws += s * w;
        }
        y.push_back(cf32(a / ws));
    }
    return y;
}

static void quant8(std::vector<cf32>& x, double rmsTarget) {
    const double g = rmsTarget / std::sqrt(power(x));
    auto q = [](double v) { v = std::max(-1.0, std::min(1.0, v)); return (float)(std::round(v * 127.0) / 127.0); };
    for (auto& v : x) v = cf32(q(v.real() * g), q(v.imag() * g));
}

// ---- the receiver
struct Res { double good = 0; size_t packets = 0, goodN = 0, flagged = 0, wrong = 0; uint64_t dropped = 0; double load = 0; };

static const std::set<std::string>& refPackets() {
    static std::set<std::string> ref = [] {
        std::set<std::string> r;
        auto src = dvbt::testTsSource();
        for (int i = 0; i < 80000; i++) { uint8_t p[188]; src(p); p[0] = 0x47; r.insert(std::string((char*)p, 188)); }
        return r;
    }();
    return ref;
}

// good: packets that are right, over the packets the signal carries after its first 0.4 s (lock-up)
static Res receive(const std::vector<cf32>& x, double rate) {
    AtscReceiver rx;
    rx.configure(rate);
    rx.setBlocking(true);
    std::vector<uint8_t> got;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { got.insert(got.end(), p, p + n * 188); });
    for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
    rx.flush();
    const auto& ref = refPackets();
    Res r;
    r.packets = got.size() / 188;
    for (size_t k = 0; k < r.packets; k++) {
        if (got[k * 188 + 1] & 0x80) { r.flagged++; continue; }
        if (ref.count(std::string((char*)&got[k * 188], 188))) r.goodN++; else r.wrong++;
    }
    const double possible = ((double)x.size() / rate - 0.4) * kPktPerSec;
    r.good = std::min(1.0, (double)r.goodN / std::max(1.0, possible));
    AtscTelemetry t;
    if (rx.telemetry(t, 0)) { r.dropped = t.fieldsDropped; r.load = t.decodeLoad; }
    return r;
}

// ---- cases
struct Case {
    std::string name;
    double rate, seconds;
    std::function<std::vector<cf32>(double rate, double seconds)> make;   // the signal without the noise
    bool bits8 = false;
};

static std::vector<cf32> withPaths(double rate, double secs, const std::vector<Path>& p) { return multipath(cleanSignal(rate, secs), rate, p); }

// the lowest SNR in 1 dB steps between lo and hi at which the case gives >= 90 % good packets and no wrong ones (-1: not even at hi)
static double threshold(const Case& c, double lo, double hi, Res* atHi) {
    const auto base = c.make(c.rate, c.seconds);
    const double sp = power(base);
    auto okAt = [&](double snr, Res* out) {
        auto x = base;
        addNoise(x, c.rate, snr, sp, 7 + (uint32_t)snr);
        if (c.bits8) quant8(x, 0.25);
        const Res r = receive(x, c.rate);
        if (out) *out = r;
        return r.good >= 0.9 && r.wrong <= 3;
    };
    Res top;
    if (!okAt(hi, &top)) { if (atHi) *atHi = top; return -1; }
    if (atHi) *atHi = top;
    // bisection on the 1 dB grid (the share of good packets falls with the SNR)
    int a = (int)lo, b = (int)hi;   // b passes
    if (okAt(a, nullptr)) return a;
    while (b - a > 1) { const int m = (a + b) / 2; if (okAt(m, nullptr)) b = m; else a = m; }
    return b;
}

static std::vector<Case> cases() {
    std::vector<Case> v;
    const double R = 10e6;
    v.push_back({"AWGN, 10 Msps", R, 1.6, [](double r, double s) { return cleanSignal(r, s); }});
    // Table 5.6: single static echoes at their guideline level, turning slowly (0.05 Hz)
    const double t56[][2] = {{-30, -7}, {-15, -5}, {-10, -3}, {-5, -0.5}, {5, -0.5}, {10, -1}, {15, -1}, {20, -2}, {30, -3}, {40, -4}};
    for (auto& e : t56) {
        char n[96];
        snprintf(n, sizeof n, "A/74 5.6 echo %+.0f us %.1f dB", e[0], e[1]);
        const double d = e[0], a = -e[1];
        v.push_back({n, R, 1.6, [d, a](double r, double s) { return withPaths(r, s, {{0, 0, 0, 0}, {d, a, 30, 0.05}}); }});
    }
    // R.1: two paths at the same delay and level, one fixed (0.05 Hz), one turning (here 2 Hz): their sum comes and goes (0 dB max)
    for (double dl : {0.5, 2.0})
        for (double att : {7.0, 3.0, 0.0}) {
            char n[96];
            snprintf(n, sizeof n, "A/74 R.1 %.1f us, paths 2+3 at %.0f dB, 2 Hz", dl, att);
            const double a = att + 6.02;   // each path half the voltage: together up to `att`
            v.push_back({n, R, 2.0, [dl, a](double r, double s) { return withPaths(r, s, {{0, 0, 0, 0}, {dl, a, 0, 0.05}, {dl, a, 0, 2.0}}); }});
        }
    // R2.1 (Table 5.10): path 5 at 5.7 us turning at 5 Hz, set here to 10 dB below the main path
    const double r21[4][4] = {{20, 20, 10, 18}, {17, 17, 7, 15}, {14, 14, 4, 12}, {11, 11, 1, 9}};
    for (int e = 0; e < 4; e++) {
        char n[96];
        snprintf(n, sizeof n, "A/74 R2.1 #%d, path 5 -10 dB at 5 Hz", e + 1);
        const auto& q = r21[e];
        v.push_back({n, R, 1.6, [q](double r, double s) {
            return withPaths(r, s, {{0, 0, 0, 0}, {-1.8, q[0], 125, 0}, {0.15, q[1], 80, 0}, {1.8, q[2], 45, 0}, {5.7, 10, 0, 5.0}, {35.0, q[3], 90, 0}}); }});
    }
    // R2.2 (Table 5.11): five echoes, path 6 at 39.8 us, path 5 turning at 5 Hz; with noise (the guideline asks for 18 dB)
    const double r22[3][5] = {{15, 15, 7, 7, 15}, {8, 3, 4, 3, 12}, {3, 1, 1, 3, 9}};
    for (int e = 0; e < 3; e++) {
        char n[96];
        snprintf(n, sizeof n, "A/74 R2.2 #%d, path 5 at 5 Hz", e + 1);
        const auto& q = r22[e];
        v.push_back({n, R, 1.6, [q](double r, double s) {
            return withPaths(r, s, {{0, 0, 0, 0}, {-1.8, q[0], 125, 0}, {0.15, q[1], 80, 0}, {1.8, q[2], 45, 0}, {5.7, q[3], 0, 5.0}, {39.8, q[4], 90, 0}}); }});
    }
    // an airplane: a -12 dB echo at 3 us whose phase turns at 60 Hz (A/74 5.5.3.4: flutter at 50 to 150 Hz)
    v.push_back({"airplane flutter, -12 dB at 3 us, 60 Hz", R, 1.6, [](double r, double s) { return withPaths(r, s, {{0, 0, 0, 0}, {3.0, 12, 0, 60.0}}); }});
    // burst noise (5.4.5): 165 us of noise at -5 dB (6 MHz) every 100 ms
    v.push_back({"burst noise 165 us at -5 dB, 10 Hz", R, 1.6, [](double r, double s) {
        auto x = cleanSignal(r, s);
        const double p = power(x), var = p * std::pow(10.0, -0.5) * r / 6e6;
        std::mt19937 rng(5);
        dect2::normal_distribution<float> g(0.f, (float)std::sqrt(var / 2));
        const size_t per = (size_t)(0.1 * r), len = (size_t)(165e-6 * r);
        for (size_t b = (size_t)(0.033 * r); b + len < x.size(); b += per)
            for (size_t i = b; i < b + len; i++) x[i] += cf32(g(rng), g(rng));
        return x; }});
    // co-channel DTV (Table 5.1, D/U +15.5 dB): another station on the same channel, 1.7 kHz and a few ms apart
    v.push_back({"co-channel DTV at D/U 15.5 dB", R, 1.6, [](double r, double s) {
        auto x = cleanSignal(r, s);
        auto u = cleanSignal(r, s + 0.05, 9);
        u.erase(u.begin(), u.begin() + (ptrdiff_t)(0.013 * r));
        shiftHz(u, 1700, r);
        const double g = std::sqrt(power(x) / power(u) * std::pow(10.0, -1.55));
        for (size_t i = 0; i < x.size(); i++) x[i] += (float)g * u[i];
        return x; }});
    // the upper adjacent DTV channel 20 dB stronger (Table 5.2, strong desired), 20 Msps radio, 8-bit
    for (double du : {-20.0, -33.0}) {
        char n[96];
        snprintf(n, sizeof n, "adjacent DTV +6 MHz at D/U %.0f dB, 20 Msps%s", du, du > -30 ? " 8-bit" : "");
        Case c{n, 20e6, 1.6, [du](double r, double s) {
            auto x = cleanSignal(r, s);
            auto u = cleanSignal(r, s + 0.05, 9);
            u.erase(u.begin(), u.begin() + (ptrdiff_t)(0.007 * r));
            shiftHz(u, 6e6, r);
            const double g = std::sqrt(power(x) / power(u) * std::pow(10.0, -du / 10.0));
            for (size_t i = 0; i < x.size(); i++) x[i] += (float)g * u[i];
            return x; }};
        c.bits8 = du > -30;
        v.push_back(c);
    }
    v.push_back({"carrier 10 dB below the signal at +1.2 MHz", R, 1.6, [](double r, double s) {
        auto x = cleanSignal(r, s);
        const double a = std::sqrt(power(x) * 0.1), w = 2 * M_PI * 1.2e6 / r;
        for (size_t n = 0; n < x.size(); n++) x[n] += cf32((float)(a * std::cos(w * n)), (float)(a * std::sin(w * n)));
        return x; }});
    // the sample rates of real radios, 8-bit, with a tuning error of +38 kHz and the radio's clock 30 ppm fast
    for (double rr : {6.5e6, 7.5e6, 8e6, 64e6 / 7, 10e6, 12.5e6, 20e6}) {
        char n[96];
        snprintf(n, sizeof n, "%.3f Msps 8-bit, +38 kHz, +30 ppm", rr / 1e6);
        Case c{n, rr, 1.6, [](double r, double s) { auto x = clockErr(cleanSignal(r, s), 30); shiftHz(x, 38e3, r); return x; }};
        c.bits8 = true;
        v.push_back(c);
    }
    // a hard real-world mix: R2.2 #2 plus tuning error, clock error and 8 bits
    {
        Case c{"R2.2 #2 + 38 kHz + 30 ppm, 8-bit", R, 1.6, [](double r, double s) {
            auto x = clockErr(withPaths(r, s, {{0, 0, 0, 0}, {-1.8, 8, 125, 0}, {0.15, 3, 80, 0}, {1.8, 4, 45, 0}, {5.7, 3, 0, 5.0}, {39.8, 12, 90, 0}}), 30);
            shiftHz(x, 38e3, r); return x; }};
        c.bits8 = true;
        v.push_back(c);
    }
    return v;
}

// thresholds the receiver must keep (dB, 1 dB grid; -1 = must decode at the top SNR at all). Measured on the CI runner, see docs/ATSC-4.0.md.
struct Guard { const char* name; double maxThreshold; };
static const Guard kGuards[] = {
    {"AWGN, 10 Msps", 16},
};

// ---- a radio in real time: the receiver does not block, the feed is paced to the signal, and the player stalls for 150 ms every 2 s
static int paced() {
    const double rate = 10e6, secs = 6.0;
    auto x = withPaths(rate, secs, {{0, 0, 0, 0}, {-1.8, 8, 125, 0}, {0.15, 3, 80, 0}, {1.8, 4, 45, 0}, {5.7, 3, 0, 5.0}, {39.8, 12, 90, 0}});
    addNoise(x, rate, 26, power(x), 3);
    AtscReceiver rx;
    rx.configure(rate);
    rx.setBlocking(false);
    const auto& ref = refPackets();
    std::atomic<size_t> good{0}, flagged{0}, calls{0};
    auto t0 = std::chrono::steady_clock::now();
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) {
        for (size_t k = 0; k < n; k++) {
            if (p[k * 188 + 1] & 0x80) flagged++;
            else if (ref.count(std::string((const char*)p + k * 188, 188))) good++;
        }
        if (++calls % 80 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(150));   // the player busy (a GOP, the UI, a disk)
    });
    const size_t blk = 65536;
    for (size_t i = 0; i < x.size(); i += blk) {
        rx.feed(x.data() + i, std::min(blk, x.size() - i));
        std::this_thread::sleep_until(t0 + std::chrono::duration<double>((double)(i + blk) / rate));
    }
    rx.flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    AtscTelemetry t;
    rx.telemetry(t, 0);
    const double possible = (secs - 0.4) * kPktPerSec;
    printf("paced, R2.2 #2 at 26 dB, player stalls 150 ms every 80 calls: good %.1f %% (%zu), flagged %zu, fields dropped %llu, packets dropped %llu, "
           "lock losses %llu, decoder load %.2f%s%s\n",
           100.0 * (double)good / possible, (size_t)good, (size_t)flagged, (unsigned long long)t.fieldsDropped, (unsigned long long)t.packetsDropped,
           (unsigned long long)t.lockLosses, t.decodeLoad, getenv("ATSC_OLDDROP") ? "  [ATSC_OLDDROP]" : "", getenv("ATSC_SYNCCB") ? "  [ATSC_SYNCCB]" : "");
    return 0;
}

int main(int argc, char** argv) {
    const std::string only = argc > 1 ? argv[1] : "";
    if (only == "paced") return paced();
    const auto t0 = std::chrono::steady_clock::now();
    for (auto& c : cases()) {
        if (!only.empty()) {
            bool hit = false;
            size_t p0 = 0;
            for (;;) {
                const size_t e = only.find('|', p0);
                const std::string sub = only.substr(p0, e == std::string::npos ? std::string::npos : e - p0);
                if (!sub.empty() && c.name.find(sub) != std::string::npos) hit = true;
                if (e == std::string::npos) break;
                p0 = e + 1;
            }
            if (!hit) continue;
        }
        Res top;
        const auto c0 = std::chrono::steady_clock::now();
        const double th = threshold(c, 12, 32, &top);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count();
        char ts[16];
        if (th < 0) snprintf(ts, sizeof ts, "-"); else snprintf(ts, sizeof ts, "%.0f dB", th);
        printf("%-52s threshold %6s   at 32 dB: good %5.1f %% flagged %5zu wrong %3zu   (%.0f s)\n", c.name.c_str(), ts, 100 * top.good, top.flagged, top.wrong, sec);
        fflush(stdout);
        for (auto& g : kGuards)
            if (c.name == g.name && (th < 0 || th > g.maxThreshold)) { printf("FAIL: %s: threshold above %.0f dB\n", g.name, g.maxThreshold); fails++; }
        if (top.wrong > 3) { printf("FAIL: %s: %zu wrong packets passed as good\n", c.name.c_str(), top.wrong); fails++; }
    }
    printf("total %.0f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    printf(fails ? "ATSC channel tests FAILED\n" : "ATSC channel tests passed\n");
    return fails ? 1 : 0;
}
