// MIL-STD-188-110 serial tone and STANAG 4285: the receiver recovers the generator's bits over every rate and interleaver, with frequency offset, noise,
// a second path and the sample clock off. The generator follows the published documents; no over-the-air recording was available (simulated only).
#include "dect2/hfutil/hfu_serial.h"
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::hfu;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); fails++; } } while (0)

static std::vector<cf32> channel(const std::vector<cf32>& in, double cfo, double snrDb, double path2ms, double path2db, double ppm, uint32_t seed) {
    const double fs = kBaseRate;
    std::vector<cf32> v = in;
    if (path2ms > 0) {
        const size_t d = (size_t)(path2ms * 1e-3 * fs);
        const float a = (float)std::pow(10.0, path2db / 20);
        v.resize(v.size() + d, cf32(0, 0));
        for (size_t i = v.size(); i-- > d;) v[i] += (i - d < in.size() ? in[i - d] : cf32(0, 0)) * a;
    }
    if (ppm != 0) {
        std::vector<cf32> o;
        const double r = 1.0 + ppm * 1e-6;
        for (double t = 0; t + 1 < (double)v.size(); t += r) { const size_t i = (size_t)t; const float f = (float)(t - (double)i); o.push_back(v[i] * (1 - f) + v[i + 1] * f); }
        v.swap(o);
    }
    double ph = 0;
    const double st = 2 * M_PI * cfo / fs;
    double p = 0;
    for (auto& s : v) { s *= cf32((float)std::cos(ph), (float)std::sin(ph)); ph += st; p += std::norm(s); }
    p /= (double)v.size();
    if (snrDb < 90) {
        // signal to noise in 3 kHz: noise density scales up to the 24 kHz simulation bandwidth
        const double n = p / std::pow(10.0, snrDb / 10) * fs / 3000.0;
        std::mt19937 g(seed);
        std::normal_distribution<float> d(0, (float)std::sqrt(n / 2));
        for (auto& s : v) s += cf32(d(g), d(g));
    }
    return v;
}

static std::vector<uint8_t> prbs(size_t n, uint32_t seed) {
    std::mt19937 g(seed);
    std::vector<uint8_t> b(n);
    for (auto& x : b) x = (uint8_t)(g() & 1);
    return b;
}

static size_t findBest(const std::vector<uint8_t>& rx, const std::vector<uint8_t>& ref, size_t* bestErr) {
    size_t bo = 0, be = (size_t)-1;
    for (size_t o = 0; o + ref.size() <= rx.size() && o < 4096; o++) {
        size_t e = 0;
        for (size_t i = 0; i < ref.size() && e < be; i++) if (rx[o + i] != ref[i]) e++;
        if (e < be) { be = e; bo = o; }
    }
    *bestErr = be;
    return bo;
}

struct Out { std::vector<uint8_t> bits; std::vector<D5066> d; SerialStatus st; };
static Out runRx(SerialStd std, const std::vector<cf32>& sig, int force = -1) {
    SerialRx rx(std);
    if (force != -1) rx.forceProfile(force);
    Out o;
    rx.setBitSink([&](uint8_t b) { o.bits.push_back(b); });
    rx.setD5066Sink([&](const D5066& d) { o.d.push_back(d); });
    const size_t step = 1024;
    for (size_t i = 0; i < sig.size(); i += step) rx.push(sig.data() + i, std::min(step, sig.size() - i));
    { std::vector<cf32> z(48000, cf32(0, 0)); rx.push(z.data(), z.size()); }
    o.st = rx.status();
    return o;
}

// each rate at its own target SNR, with +40 Hz offset; after the leading blocks (the receiver's first decoded bits are clean anyway) the bits match
static void test110Rates() {
    struct T { int bps; bool lng; double snr; } tt[] = {{4800, false, 25}, {2400, false, 18}, {2400, true, 18}, {1200, false, 12}, {1200, true, 12},
                                                        {600, false, 8}, {300, false, 5}, {150, false, 3}, {75, false, 1}, {75, true, 1}};
    for (const auto& t : tt) {
        SerialGenCfg c;
        c.std = SerialStd::Mil110; c.bps = t.bps; c.longIntl = t.lng;
        size_t nbits = (size_t)(t.bps * (t.lng ? 12.0 : 5.0));
        const auto ref = prbs(nbits, 7 + t.bps);
        const auto sig = channel(genSerial(c, ref), 40.0, t.snr, 0, 0, 0, 3);
        auto o = runRx(SerialStd::Mil110, sig);
        size_t err = (size_t)-1;
        size_t at = o.bits.size() >= ref.size() ? findBest(o.bits, ref, &err) : 0;
        CHECK(err == 0, "110 %d bit/s long=%d snr %.0f: errors %zu of %zu bits, got %zu bits, offset %zu, mode '%s'", t.bps, t.lng, t.snr, err, ref.size(), o.bits.size(), at, o.st.mode.c_str());
        std::printf("110 %5d bit/s %s: %zu bits, %zu errors, SNR %.0f dB\n", t.bps, t.lng ? "long" : "short", ref.size(), err, t.snr);
    }
}

// two-path multipath and a clock error: 2400 bit/s (the demanding rate) on a realistic bad channel
static void test110Multipath() {
    SerialGenCfg c;
    c.std = SerialStd::Mil110; c.bps = 1200;
    const auto ref = prbs(6000, 11);
    const auto sig = channel(genSerial(c, ref), -35.0, 20, 2.0, -3, 20, 5);
    auto o = runRx(SerialStd::Mil110, sig);
    size_t err = (size_t)-1;
    if (o.bits.size() >= ref.size()) findBest(o.bits, ref, &err);
    CHECK(err == 0, "110 1200 bit/s 2 ms path, 20 ppm: errors %zu, %zu bits", err, o.bits.size());
    std::printf("110 multipath: errors %zu\n", err);
}

static void test4285Rates() {
    struct T { int bps; bool coded, lng; double snr; int force; } tt[] = {{2400, true, false, 18, -1}, {2400, true, true, 18, -1}, {1200, true, false, 12, -1}, {1200, true, true, 12, -1},
                                                               {600, true, false, 8, -1}, {300, true, false, 5, -1}, {150, true, false, 3, -1}, {75, true, false, 2, -1},
                                                               {3600, false, false, 28, 6}, {2400, false, false, 22, 7}, {1200, false, false, 16, 8}};
    for (const auto& t : tt) {
        SerialGenCfg c;
        c.std = SerialStd::Stanag4285; c.bps = t.bps; c.coded = t.coded; c.longIntl = t.lng;
        size_t nbits = (size_t)(t.bps * (t.lng ? 15.0 : 6.0));
        const auto ref = prbs(nbits, 9 + t.bps);
        const auto sig = channel(genSerial(c, ref), 60.0, t.snr, 0, 0, 0, 13);
        auto o = runRx(SerialStd::Stanag4285, sig, t.force);
        size_t err = (size_t)-1;
        size_t at = o.bits.size() >= ref.size() ? findBest(o.bits, ref, &err) : 0;
        if (t.coded) CHECK(err == 0, "4285 %d coded long=%d snr %.0f: errors %zu of %zu, got %zu, offset %zu, mode '%s'", t.bps, t.lng, t.snr, err, ref.size(), o.bits.size(), at, o.st.mode.c_str());
        else CHECK(err < ref.size() / 200, "4285 %d uncoded: errors %zu of %zu, got %zu", t.bps, err, ref.size(), o.bits.size());
        std::printf("4285 %5d bit/s %s %s: %zu bits, %zu errors, SNR %.0f dB, mode '%s'\n", t.bps, t.coded ? "coded" : "uncoded", t.lng ? "long" : "short", ref.size(), err, t.snr, o.st.mode.c_str());
    }
}

static void test5066() {
    std::vector<uint8_t> payload;
    for (int i = 0; i < 50; i++) payload.push_back((uint8_t)("OnAir test of STANAG 5066 D_PDU framing, one two."[i % 49]));
    auto pdu = make5066NonArq(payload, 0x1234, 0x5678);
    // bits, least significant first
    std::vector<uint8_t> bits(64, 0);
    for (auto b : pdu) for (int i = 0; i < 8; i++) bits.push_back((b >> i) & 1);
    // on both modems
    for (int which = 0; which < 2; which++) {
        SerialGenCfg c;
        c.std = which ? SerialStd::Stanag4285 : SerialStd::Mil110; c.bps = 600;
        const auto sig = channel(genSerial(c, bits), 15.0, 12, 0, 0, 0, 21);
        auto o = runRx(c.std, sig);
        CHECK(o.d.size() >= 1, "5066 over %s: no D_PDU", which ? "4285" : "110");
        if (!o.d.empty()) {
            CHECK(o.d[0].typeName == "NON_ARQ_DATA" && o.d[0].headerCrcOk && o.d[0].dataCrcOk && o.d[0].payload == 50, "5066 fields type %s crc %d %d size %d", o.d[0].typeName.c_str(), o.d[0].headerCrcOk, o.d[0].dataCrcOk, o.d[0].payload);
            CHECK(o.d[0].src == "1234" && o.d[0].dst == "5678", "5066 addresses %s %s", o.d[0].src.c_str(), o.d[0].dst.c_str());
        }
    }
    std::printf("5066 D_PDU over 110 and 4285 ok\n");
}

// a wrong rate of the same family must not produce confident output: 4285 auto stays unlocked on pure noise
static void testNoLockOnNoise() {
    std::vector<cf32> z(240000, cf32(0, 0));
    std::mt19937 g(5);
    std::normal_distribution<float> d(0, 0.3f);
    for (auto& s : z) s = cf32(d(g), d(g));
    auto a = runRx(SerialStd::Stanag4285, z);
    auto b = runRx(SerialStd::Mil110, z);
    CHECK(a.bits.empty() && b.bits.empty(), "noise produced bits: %zu %zu", a.bits.size(), b.bits.size());
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    test110Rates();
    test110Multipath();
    test4285Rates();
    test5066();
    testNoLockOnNoise();
    if (fails) std::printf("%d FAILED\n", fails); else std::printf("OK\n");
    return fails ? 1 : 0;
}
