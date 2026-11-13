// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 802.11p, DSRC framing, GBAS bursts. The BSM layout is checked against a published example; signals are simulated only.
#include "avroad_gen.h"
#include "dect2/avroad/avroad_proto.h"
#include "dect2/avroad/avroad_rx.h"
#include "dect2/avroad/avroad_tel.h"
#include "dect2/wifi/wifi_ofdm.h"
#include <cstdio>

using namespace dect2;
using namespace dect2::avroad;
using namespace avgen;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static AvRoadTelemetry run(int v, const std::vector<cf32>& x, double fs) {
    AvRoadReceiver r; r.setVariant(v); r.configure(fs);
    for (size_t i = 0; i < x.size(); i += 65536) r.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
    std::vector<cf32> tail((size_t)(fs * 0.3)); r.feed(tail.data(), tail.size());
    AvRoadTelemetry t; r.telemetry(t, 0);
    return t;
}
static std::vector<uint8_t> unhex(const char* s) { std::vector<uint8_t> v; for (; s[0] && s[1]; s += 2) { unsigned b; sscanf(s, "%2x", &b); v.push_back((uint8_t)b); } return v; }

namespace ofdm = dect2::wifi::ofdm;

// Discrete Fourier synthesis of one OFDM symbol (64 points, unitary as in the wifi code)
static void synth(std::vector<cf32>& out, const std::vector<cf32>& X) {
    std::vector<cf32> t(64);
    for (int n = 0; n < 64; n++) { std::complex<double> acc(0); for (int k = 0; k < 64; k++) acc += std::complex<double>(X[(size_t)k]) * std::polar(1.0, 2 * kPi * k * n / 64); t[(size_t)n] = cf32(acc / std::sqrt(52.0)); }
    for (int i = 48; i < 64; i++) out.push_back(t[(size_t)i]);
    out.insert(out.end(), t.begin(), t.end());
}
static int car(int k) { return k < 0 ? k + 64 : k; }

// 802.11a framing at either rate; samples at 20 Msps
static std::vector<cf32> ppdu(const std::vector<uint8_t>& mpdu, bool qpsk) {
    uint32_t c = 0xFFFFFFFF; for (uint8_t b : mpdu) { c ^= b; for (int i = 0; i < 8; i++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1; } c = ~c;
    std::vector<uint8_t> f = mpdu; for (int i = 0; i < 4; i++) f.push_back((uint8_t)(c >> (8 * i)));
    if (!qpsk) return ofdm::txPpdu(f);
    // start from the 6 Mbit/s PPDU for the preamble, then replace SIGNAL and DATA
    std::vector<cf32> head = ofdm::txPpdu(f); head.resize(ofdm::kStfLen + ofdm::kLtfLen);
    static const int kP[4] = {-21, -7, 7, 21}, kPb[4] = {1, 1, 1, -1};
    std::vector<int> dc; for (int k = -26; k <= 26; k++) if (k && k != -21 && k != -7 && k != 7 && k != 21) dc.push_back(k);
    auto put = [&](std::vector<cf32>& o, const uint8_t* bits, int ncbps, int pol) {
        std::vector<cf32> X(64, cf32(0, 0));
        const int nb = ncbps / 48;
        for (int k = 0; k < ncbps; k++) {
            const int j = ncbps / 16 * (k % 16) + k / 16;
            int s = j / nb;
            float v = bits[k] ? 1.f : -1.f;
            cf32& d = X[(size_t)car(dc[(size_t)s])];
            if (nb == 1) d = cf32(v, 0); else if (j % 2 == 0) d += cf32(v * 0.70711f, 0); else d += cf32(0, v * 0.70711f);
        }
        for (int i = 0; i < 4; i++) X[(size_t)car(kP[i])] = cf32((float)(kPb[i] * pol), 0);
        synth(o, X);
    };
    std::vector<uint8_t> sig = {0, 1, 0, 1, 0};
    int len = (int)f.size();
    for (int i = 0; i < 12; i++) sig.push_back((uint8_t)(len >> i & 1));
    uint8_t par = 0; for (uint8_t b : sig) par ^= b; sig.push_back(par);
    for (int i = 0; i < 6; i++) sig.push_back(0);
    auto sc = ofdm::convEncode(sig);
    put(head, sc.data(), 48, ofdm::pilotPolarity(0));
    std::vector<uint8_t> d(16, 0);
    for (uint8_t b : f) for (int k = 0; k < 8; k++) d.push_back((uint8_t)(b >> k & 1));
    size_t tailAt = d.size();
    d.resize(((tailAt + 6 + 47) / 48) * 48, 0);
    ofdm::scramble(d, 0x5D);
    for (size_t i = 0; i < 6; i++) d[tailAt + i] = 0;
    auto coded = ofdm::convEncode(d);
    for (size_t s = 0; s * 96 < coded.size(); s++) put(head, coded.data() + s * 96, 96, ofdm::pilotPolarity((int)s + 1));
    return head;
}

static std::vector<uint8_t> dataFrame(uint16_t eth, const std::vector<uint8_t>& body) {
    std::vector<uint8_t> f(26, 0); f[0] = 0x88;
    for (int i = 0; i < 6; i++) { f[4 + (size_t)i] = 0xFF; f[10 + (size_t)i] = (uint8_t)(0x10 + i); f[16 + (size_t)i] = 0xFF; }
    for (uint8_t b : {0xAA, 0xAA, 0x03, 0, 0, 0}) f.push_back(b);
    f.push_back((uint8_t)(eth >> 8)); f.push_back((uint8_t)eth);
    f.insert(f.end(), body.begin(), body.end());
    return f;
}
static std::vector<uint8_t> cam(uint32_t station, double lat, double lon, int proto = 2) {
    std::vector<uint8_t> v = {0x07, 0xD1, 0, 0, (uint8_t)proto, 2, (uint8_t)(station >> 24), (uint8_t)(station >> 16), (uint8_t)(station >> 8), (uint8_t)station, 0x12, 0x34};
    uint64_t la = (uint64_t)(int64_t)std::llround(lat * 1e7 + 900000000), lo = (uint64_t)(int64_t)std::llround(lon * 1e7 + 1800000000);
    std::vector<uint8_t> bits;
    auto put = [&](uint64_t x, int n) { for (int i = n - 1; i >= 0; i--) bits.push_back((uint8_t)(x >> i & 1)); };
    put(0, 3); put(0, 1); put(5, 8); put(la, 31); put(lo, 32); put(0, 200);
    for (size_t i = 0; i + 8 <= bits.size(); i += 8) { uint8_t b = 0; for (int k = 0; k < 8; k++) b = (uint8_t)(b << 1 | bits[i + (size_t)k]); v.push_back(b); }
    return v;
}
// The 20 Msps PPDU played as a 10 MHz channel: its samples are at 10 Msps
static std::vector<cf32> itsScene(double fs, const std::vector<std::vector<uint8_t>>& mpdus, bool qpsk, double cfo, double snr, unsigned seed) {
    std::vector<cf32> x((size_t)(fs * 0.001));
    for (auto& m : mpdus) { auto p = ppdu(m, qpsk); x.insert(x.end(), p.begin(), p.end()); x.resize(x.size() + (size_t)(fs * 0.0008)); }
    mix(x, cfo, fs);
    double power = 0; size_t c = 0; for (auto& v : x) if (std::norm(v) > 0) { power += std::norm(v); c++; }
    addNoise(x, snr, c ? power / (double)c : 1, seed);
    return x;
}

static void testIts() {
    const std::vector<uint8_t> real = unhex("0014251200000A252F24E6D03FEF9CCC88740D5F7FFFFFFFF098D5F0FDFA1FA1007FFF00004B1090");
    Bsm b; CHECK(bsmParse(real.data(), real.size(), b), "the published BSM example parses");
    CHECK(b.msgCnt == 72 && b.id == 0x2894 && b.lat == 402364127 && b.lon == -833667351 && b.elev == 2750, "published BSM: count %d id %X lat %d lon %d elev %d", b.msgCnt, b.id, b.lat, b.lon, b.elev);
    CHECK(b.heading == 22000, "published BSM heading %d", b.heading);
    std::vector<uint8_t> wsm = {0x03, 0x00, 0x20, 0x00, (uint8_t)(real.size())};  // version 3, TPID, PSID, a length
    wsm.insert(wsm.end(), real.begin(), real.end());
    const auto bsmF = dataFrame(0x88DC, wsm);
    const auto camF = dataFrame(0x8947, [] { std::vector<uint8_t> g(40, 0x11); auto c = cam(0xCAFE01, 54.68, 25.28); g.insert(g.end(), c.begin(), c.end()); return g; }());
    { Cam c; auto v = cam(77, -12.3, 45.6); CHECK(camParse(v.data() + 4, v.size() - 4, c) && c.station == 77 && std::abs(c.lat - (-123000000)) <= 2 && std::abs(c.lon - 456000000) <= 2, "CAM parse %u %d %d", c.station, c.lat, c.lon); }
    for (bool qpsk : {true, false}) for (double fs : {10e6, 20e6}) for (double cfo : {0.0, 40e3}) {
        auto x = itsScene(10e6, {bsmF, camF}, qpsk, cfo, 30, 3);
        if (fs != 10e6) { std::vector<cf32> up(x.size() * 2); for (size_t i = 0; i < x.size(); i++) { up[2 * i] = x[i]; up[2 * i + 1] = x[i]; } x = up; }
        AvRoadTelemetry t = run(ItsG5, x, fs);
        CHECK(t.blocksOk == 2 && t.rows.size() == 2, "qpsk %d fs %.0f cfo %.0f: %llu", qpsk, fs, cfo, (unsigned long long)t.blocksOk);
        if (t.rows.size() == 2) {
            CHECK(t.rows[1].kind == "BSM" && t.rows[1].id == "00002894" && t.rows[1].hasPos && std::fabs(t.rows[1].lat - 40.2364127) < 1e-6, "BSM row %s %s %f", t.rows[1].kind.c_str(), t.rows[1].id.c_str(), t.rows[1].lat);
            CHECK(t.rows[0].kind == "CAM" && t.rows[0].id == std::to_string(0xCAFE01) && std::fabs(t.rows[0].lat - 54.68) < 1e-6, "CAM row %s %f", t.rows[0].kind.c_str(), t.rows[0].lat);
        }
    }
    {
        printf("its snr in 10 MHz (QPSK 1/2):");
        for (double snr : {20.0, 12.0, 8.0, 6.0, 4.0, 2.0}) {
            int got = 0;
            for (unsigned s = 1; s <= 8; s++) got += (int)run(ItsG5, itsScene(10e6, {bsmF}, true, 2000, snr, s), 10e6).blocksOk;
            printf(" %.0f dB %d/8", snr, got);
            if (snr >= 8) CHECK(got == 8, "snr %.0f: %d/8", snr, got);
        }
        printf("\n");
    }
    { std::vector<cf32> x(10000000 / 2); addNoise(x, 0, 1, 2); CHECK(run(ItsG5, x, 10e6).blocksOk == 0, "noise"); }
}

// ---- DSRC
static void fm0(std::vector<uint8_t>& lvl, int bit, int& cur) {
    cur ^= 1; lvl.push_back((uint8_t)cur);
    if (!bit) cur ^= 1;
    lvl.push_back((uint8_t)cur);
}
static std::vector<cf32> dsrcScene(double fs, const std::vector<std::vector<uint8_t>>& frames, double snr, double cfo, unsigned seed, double depth = 1.0) {
    std::vector<uint8_t> half; int cur = 0;
    for (int i = 0; i < 64; i++) fm0(half, 1, cur);            // run-in
    auto send = [&](uint8_t b) { for (int i = 0; i < 8; i++) fm0(half, b >> i & 1, cur); };
    for (auto& fr : frames) {
        std::vector<uint8_t> f = fr; const uint16_t c = dsrcFcs(f.data(), f.size()); f.push_back((uint8_t)c); f.push_back((uint8_t)(c >> 8));
        send(0x7E);
        int ones = 0;
        for (uint8_t b : f) for (int i = 0; i < 8; i++) { int v = b >> i & 1; fm0(half, v, cur); if (v) { if (++ones == 5) { fm0(half, 0, cur); ones = 0; } } else ones = 0; }
        send(0x7E);
        for (int i = 0; i < 40; i++) fm0(half, 1, cur);
    }
    const double sph = fs / 1e6;
    std::vector<cf32> x((size_t)(fs * 0.0005));
    double p = 0; const double d = 2 * kPi * cfo / fs;
    for (uint8_t l : half) { const size_t e = x.size() + (size_t)sph; while (x.size() < e) { x.push_back(std::polar(l ? 1.f : (float)(1 - depth), (float)p)); p += d; } }
    x.resize(x.size() + (size_t)(fs * 0.0005));
    addNoise(x, snr, 0.5, seed);
    return x;
}
static void testDsrc() {
    std::vector<uint8_t> bcast = {0xFF, 0xFF, 0xFF, 0xFF, 0x03, 0x50, 0x12, 0x34, 0x56, 0xF8, 0xF8, 0xF8, 0x01};
    std::vector<uint8_t> priv = {0x10, 0x22, 0x31, 0x03, 0xAB, 0xCD, 0x7E, 0xFE, 0xFF, 0xFF};      // data that needs stuffing and a flag pattern inside
    { DsrcFrame d; std::vector<uint8_t> v = priv; uint16_t c = dsrcFcs(v.data(), v.size()); v.push_back((uint8_t)c); v.push_back((uint8_t)(c >> 8)); CHECK(dsrcFrame(v.data(), v.size(), d) && !d.broadcast && d.bytes == priv.size(), "frame"); v[2] ^= 1; CHECK(!dsrcFrame(v.data(), v.size(), d), "FCS catches a bit"); }
    for (double fs : {4e6, 8e6, 5e6}) for (double cfo : {0.0, 50e3}) {
        auto x = dsrcScene(fs, {bcast, priv}, 25, cfo, 2);
        AvRoadTelemetry t = run(Dsrc, x, fs);
        CHECK(t.blocksOk == 2 && t.rows.size() == 2, "fs %.0f cfo %.0f: %llu", fs, cfo, (unsigned long long)t.blocksOk);
        if (t.rows.size() == 2) { CHECK(t.rows[1].kind == "Beacon broadcast" && t.rows[0].kind == "Private link", "kinds"); CHECK(t.rows[0].text.find("31") == std::string::npos || t.rows[0].text.find("private") != std::string::npos, "no identifier shown: %s", t.rows[0].text.c_str()); }
    }
    {
        printf("dsrc snr (carrier to noise in the sample rate):");
        for (double snr : {25.0, 18.0, 14.0, 12.0, 10.0}) { int got = 0; for (unsigned s = 1; s <= 8; s++) got += (int)run(Dsrc, dsrcScene(4e6, {bcast}, snr, 2000, s), 4e6).blocksOk; printf(" %.0f dB %d/8", snr, got); if (snr >= 18) CHECK(got == 8, "snr %.0f %d", snr, got); }
        printf("\n");
    }
    { std::vector<cf32> x(4000000); addNoise(x, 0, 1, 7); CHECK(run(Dsrc, x, 4e6).blocksOk == 0, "noise"); }
    // Low modulation depth (EN 12253 allows less than full)
    { auto x = dsrcScene(4e6, {bcast}, 30, 0, 2, 0.5); CHECK(run(Dsrc, x, 4e6).blocksOk == 1, "modulation depth 50%%"); }
}

// ---- GBAS: D8PSK bursts of about 50 ms in chosen slots, RC shaped
static std::vector<cf32> gbasScene(double fs, const std::vector<int>& slots, int frames, double cfo, double snr, unsigned seed) {
    std::mt19937 r(seed); std::uniform_int_distribution<int> d(0, 7);
    std::vector<cf32> x((size_t)(fs * (0.1 + 0.5 * frames)));
    double phase = 0;
    for (int f = 0; f < frames; f++) for (int s : slots) {
        const double t0 = 0.05 + 0.5 * f + 0.0625 * s;
        const int syms = (int)(0.05 * 10500);
        const double sps = fs / 10500;
        double state = 0; std::vector<cf32> sy; for (int i = 0; i < syms; i++) { state += d(r) * kPi / 4; sy.push_back(std::polar(1.f, (float)state)); }
        const size_t a = (size_t)(t0 * fs);
        for (int i = 0; i < syms; i++) { for (int k = 0; k < (int)sps; k++) { double u = (k + 0.5) / sps; double w = std::sin(kPi * u); x[a + (size_t)(i * sps) + (size_t)k] += sy[(size_t)i] * (float)w; } }
    }
    mix(x, cfo, fs); (void)phase;
    addNoise(x, snr, 0.5, seed);
    return x;
}
static void testGbas() {
    double fs = 250000;
    auto x = gbasScene(fs, {1, 3, 5}, 10, 500, 20, 2);
    AvRoadTelemetry t = run(Gbas, x, fs);
    CHECK(t.blocksOk >= 25, "bursts %llu", (unsigned long long)t.blocksOk);
    CHECK(t.headline.find("3 of 8") == 0, "%s", t.headline.c_str());
    CHECK(t.headline.find("10 500") != std::string::npos, "symbol rate: %s", t.headline.c_str());
    x = gbasScene(fs, {0, 1, 2, 3, 4, 5, 6, 7}, 6, 0, 15, 3); t = run(Gbas, x, fs);
    CHECK(t.headline.find("8 of 8") == 0, "%s", t.headline.c_str());
    { std::vector<cf32> nn((size_t)(fs * 3)); addNoise(nn, 0, 1, 4); CHECK(run(Gbas, nn, fs).blocksOk == 0, "noise"); }
}

int main() {
    testIts(); testDsrc(); testGbas();
    printf(fails ? "FAILED %d\n" : "all ok\n", fails);
    return fails != 0;
}
