// DECT NR+ physical control channel (core/src/xlink/nrplus.cpp) against its own generator: turbo code and rate matching round trip, both control field types,
// the CRC masks, carrier offsets up to 40 kHz, timing, two-path channel, noise sweep, packets back to back, noise only. Simulated signals only.
#include "dect2/xlink/nrplus.h"
#include "dect2/xlink/xlink_rx.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace dect2;
using namespace dect2::xlink::nrplus;

static Header hdr(int type, int tx, int net) {
    Header h; h.type = type; h.lengthType = 0; h.length = 3; h.networkId = net; h.txId = tx; h.power = 7; h.mcs = 2;
    if (type == 2) { h.format = 0; h.rxId = 0xFFFF; h.streams = 0; h.rv = 1; h.ndi = 1; h.harq = 5; h.fbFormat = 1; h.fbInfo = 0x123; }
    return h;
}

static void impair(std::vector<cf32>& x, double cfo, double sigma, unsigned seed) {
    std::mt19937 g(seed);
    std::normal_distribution<float> n(0, (float)sigma);
    double ph = 1.1;
    for (auto& v : x) { v = v * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(n(g), n(g)); ph += 2 * 3.141592653589793 * cfo / kRate; }
}

static std::vector<Header> run(const std::vector<cf32>& x) {
    Receiver rx;
    std::vector<Header> out;
    rx.setCallback([&](const Header& h) { out.push_back(h); });
    for (size_t i = 0; i < x.size(); i += 1000) rx.feed(x.data() + i, std::min<size_t>(1000, x.size() - i));
    return out;
}

static bool same(const Header& a, const Header& b) { return headerBits(a) == headerBits(b) && a.type == b.type; }

int main() {
    // coding round trip with clean soft values
    for (int type : {1, 2}) for (int mask : {0, 0x5555, 0xAAAA}) {
        const Header h = hdr(type, 0x1234, 0x56);
        const auto e = pccEncode(headerBits(h), mask);
        std::vector<float> s(e.size());
        for (size_t i = 0; i < e.size(); i++) s[i] = e[i] ? 4.f : -4.f;
        std::vector<uint8_t> b; int m = -1;
        if (!pccDecode(s, b, m) || b != headerBits(h) || m != mask) { printf("FAIL coding type %d mask %x\n", type, mask); return 1; }
    }
    // packets with offsets and noise
    int k = 0;
    for (int type : {1, 2}) for (double cfo : {0.0, 12e3, -40e3}) for (int mask : {0, 0xAAAA}) {
        const Header h = hdr(type, 0x100 + k, 0x20 + k);
        std::vector<cf32> x(700, cf32(0, 0));
        auto p = txPacket(headerBits(h), 10, (unsigned)k, mask);
        x.insert(x.end(), p.begin(), p.end());
        x.resize(x.size() + 900, cf32(0, 0));
        impair(x, cfo, 0.05, (unsigned)k);
        const auto got = run(x);
        if (got.size() != 1 || !same(got[0], h) || got[0].crcMask != mask) { printf("FAIL type %d cfo %.0f mask %x: %zu headers\n", type, cfo, mask, got.size()); return 1; }
        if (std::fabs(got[0].cfoHz - cfo) > 500) { printf("FAIL cfo est %.0f want %.0f\n", got[0].cfoHz, cfo); return 1; }
        k++;
    }
    printf("types, masks, offsets ok (%s)\n", describe(hdr(2, 1, 2)).c_str());
    // two-path channel inside the cyclic prefix, packets back to back
    {
        std::vector<cf32> x(500, cf32(0, 0));
        std::vector<Header> want;
        for (int i = 0; i < 4; i++) {
            const Header h = hdr(1 + i % 2, 0xA000 + i, 9);
            want.push_back(h);
            auto p = txPacket(headerBits(h), 10, 50u + (unsigned)i);
            std::vector<cf32> q(p.size() + 3, cf32(0, 0));
            for (size_t j = 0; j < p.size(); j++) { q[j] += p[j]; q[j + 3] += p[j] * cf32(0.4f, 0.3f); }
            x.insert(x.end(), q.begin(), q.end());
            x.resize(x.size() + 40, cf32(0, 0));
        }
        x.resize(x.size() + 800, cf32(0, 0));
        impair(x, 5e3, 0.04, 77);
        const auto got = run(x);
        int ok = 0;
        for (size_t i = 0; i < got.size() && i < want.size(); i++) if (same(got[i], want[i])) ok++;
        if (ok != 4 || got.size() != 4) { printf("FAIL back to back: %zu headers, %d right\n", got.size(), ok); return 1; }
    }
    // noise sweep (SNR per sample on the unit-power signal)
    for (double snr : {0.0, 3.0, 6.0, 10.0}) {
        int ok = 0;
        for (int t = 0; t < 20; t++) {
            const Header h = hdr(1, 0x2000 + t, 3);
            std::vector<cf32> x(600, cf32(0, 0));
            auto p = txPacket(headerBits(h), 10, 900u + (unsigned)t);
            x.insert(x.end(), p.begin(), p.end());
            x.resize(x.size() + 600, cf32(0, 0));
            impair(x, 3e3 * (t % 5 - 2), std::pow(10.0, -snr / 20) / std::sqrt(2.0), 1000u + (unsigned)t);
            for (const auto& g : run(x)) if (same(g, h)) { ok++; break; }
        }
        printf("SNR %.0f dB: %d/20\n", snr, ok);
        if (snr >= 6 && ok < 19) { printf("FAIL noise sweep\n"); return 1; }
    }
    // noise only
    {
        std::vector<cf32> x(2000000, cf32(0, 0));
        impair(x, 0, 0.3, 4);
        Receiver rx; int n = 0;
        rx.setCallback([&](const Header&) { n++; });
        rx.feed(x.data(), x.size());
        if (n) { printf("FAIL noise gave %d headers\n", n); return 1; }
        printf("noise only: %llu STF candidates, no header\n", (unsigned long long)rx.syncs());
    }
    // through XlinkReceiver from a 4 Msps capture with the channel 300 kHz above the middle
    {
        const Header h = hdr(2, 0xBEEF, 0x42);
        auto p = txPacket(headerBits(h), 10, 5);
        // up-sample to 4 Msps by linear interpolation of a 1.728 Msps signal is too crude; build the 4 Msps signal by band-limited interpolation instead
        const double fs = 4e6;
        std::vector<cf32> x((size_t)(fs * 0.004), cf32(0, 0));
        const double off = 300e3, t0 = 0.0008;
        for (size_t i = 0; i < x.size(); i++) {
            const double t = (double)i / fs - t0;
            const double u = t * kRate;
            const long c = (long)std::floor(u);
            std::complex<double> a(0, 0);
            for (long j = c - 24; j <= c + 24; j++) {
                if (j < 0 || j >= (long)p.size()) continue;
                const double d = u - (double)j;
                const double sinc = std::fabs(d) < 1e-9 ? 1.0 : std::sin(3.141592653589793 * d) / (3.141592653589793 * d);
                const double win = 0.5 + 0.5 * std::cos(3.141592653589793 * d / 25);
                a += std::complex<double>(p[(size_t)j]) * sinc * win;
            }
            a *= std::polar(1.0, 2 * 3.141592653589793 * off * (double)i / fs);
            x[i] = cf32((float)a.real(), (float)a.imag());
        }
        impair(x, 0, 0.02, 9);
        XlinkReceiver rx;
        rx.setProtocols(xlink::NrPlus);
        rx.configure(fs);
        rx.setSignalOffset(off);
        rx.feed(x.data(), x.size());
        XlinkTelemetry t;
        rx.telemetry(t, ~0ull);
        if (t.frames.size() != 1 || t.frames[0].id != "network 42, RD BEEF") { printf("FAIL receiver path: %zu\n", t.frames.size()); return 1; }
        printf("receiver path: %s | %s\n", t.frames[0].id.c_str(), t.frames[0].detail.c_str());
    }
    printf("nrplus ok\n");
    return 0;
}
