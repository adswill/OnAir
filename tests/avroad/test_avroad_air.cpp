// FANET and OGN: formats against hand-built frames, then simulated air signals with impairments. No real recording of either was available: simulated only.
#include "avroad_gen.h"
#include "dect2/avroad/avroad_proto.h"
#include "dect2/avroad/avroad_rx.h"
#include "dect2/avroad/avroad_tel.h"
#include "dect2/mesh/mesh_lora.h"
#include <cstdio>
#include <cstring>

using namespace dect2;
using namespace dect2::avroad;
using namespace avgen;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static AvRoadTelemetry run(int v, const std::vector<cf32>& x, double fs, double centre = 0, double off = 0) {
    AvRoadReceiver r; r.setVariant(v); r.setCenterHz(centre); r.configure(fs); r.setSignalOffset(off);
    for (size_t i = 0; i < x.size(); i += 8192) r.feed(x.data() + i, std::min<size_t>(8192, x.size() - i));
    std::vector<cf32> tail((size_t)(fs * 0.3)); r.feed(tail.data(), tail.size());
    AvRoadTelemetry t; r.telemetry(t, 0);
    return t;
}

// ---- OGN
static std::vector<uint8_t> ognPacket(uint32_t addr, int acft, double lat, double lon, int altM) {
    std::vector<uint8_t> p(26);
    uint32_t h = addr | 3u << 24;
    if (__builtin_popcount(h & 0x0FFFFFFF) & 1) h ^= 0x08000000;
    int32_t la = (int32_t)std::lround(lat * 600000) >> 3, lo = (int32_t)std::lround(lon * 600000) >> 4;
    uint32_t w[5] = {h, (uint32_t)(la & 0xFFFFFF) | 12u << 24, (uint32_t)(lo & 0xFFFFFF), (uint32_t)altM | 123u << 14, 512u | (uint32_t)acft << 20};
    for (int i = 0; i < 5; i++) memcpy(p.data() + 4 * i, &w[i], 4);
    ognWhiten(p.data());
    ognParity(p.data());
    return p;
}
static std::vector<uint8_t> ognChips(const std::vector<uint8_t>& p, int preamble = 8) {
    std::vector<uint8_t> c;
    auto man = [&](int b) { c.push_back((uint8_t)(b ^ 1)); c.push_back((uint8_t)b); };   // bit 1 is chips 01
    for (int i = 0; i < preamble; i++) man(i & 1);
    uint32_t s = 0x0AF3656C;
    for (int i = 31; i >= 0; i--) man(s >> i & 1);
    for (uint8_t v : p) for (int i = 7; i >= 0; i--) man(v >> i & 1);
    return c;
}
static std::vector<cf32> ognScene(double fs, double chanHz, const std::vector<std::vector<uint8_t>>& pk, double ppm = 0, double gap = 0.02) {
    std::vector<cf32> x((size_t)(fs * 0.01)); double ph = 0;
    for (auto& p : pk) { appendFsk(x, ognChips(p), 100e3, fs, 50e3, ph, 1, ppm); x.resize(x.size() + (size_t)(fs * gap)); }
    mix(x, chanHz, fs);
    return x;
}

static void testOgn() {
    // Format
    {
        auto p = ognPacket(0x123456, 1, 54.68, 25.28, 1500);
        CHECK(ognParityErrors(p.data()) == 0, "parity");
        auto q = p; q[3] ^= 1; CHECK(ognParityErrors(q.data()) > 0, "parity notices");
        const OgnPacket o = ognParse(p.data());
        CHECK(o.address == 0x123456 && o.addrType == 3 && o.acftType == 1 && o.altM == 1500, "fields %06X %d %d", o.address, o.acftType, o.altM);
        CHECK(std::fabs(o.lat - 54.68) < 0.002 && std::fabs(o.lon - 25.28) < 0.002, "position %f %f", o.lat, o.lon);
        CHECK(std::fabs(o.speedMs - 12.3f) < 0.01f && std::fabs(o.headingDeg - 180) < 0.2f, "speed %.1f heading %.1f", o.speedMs, o.headingDeg);
        CHECK(o.addrParityOk, "address parity");
    }
    // Soft repair of a few wrong bits
    {
        auto p = ognPacket(0xABCDEF, 7, -33.9, 151.2, 800);
        float soft[208]; for (int i = 0; i < 208; i++) soft[i] = (p[i / 8] >> (7 - i % 8) & 1) ? 1.f : -1.f;
        for (int nb : {1, 2, 3, 4}) { auto q = p; for (int i = 0; i < nb; i++) { int k = 7 + i * 37; q[k / 8] ^= (uint8_t)(1 << (7 - k % 8)); soft[k] = -soft[k] * 0.1f; } CHECK(ognRepair(q.data(), soft) && !memcmp(q.data(), p.data(), 26), "repair %d bits", nb); for (int i = 0; i < nb; i++) { int k = 7 + i * 37; soft[k] = (p[k / 8] >> (7 - k % 8) & 1) ? 1.f : -1.f; } }
    }
    const double fs = 1000000;
    const auto a = ognPacket(0x111111, 1, 54.0, 25.0, 1000), b = ognPacket(0x222222, 7, 55.0, 24.0, 2000);
    // Both channels, offsets, clock
    for (double chan : {-100e3, 100e3}) for (double cfo : {-20e3, -5e3, 0.0, 15e3}) for (double ppm : {-40.0, 0.0, 40.0}) {
        auto x = ognScene(fs, chan + cfo, {a}, ppm);
        AvRoadTelemetry t = run(Ogn, x, fs);
        CHECK(t.blocksOk == 1, "chan %.0f cfo %.0f ppm %.0f: %llu", chan, cfo, ppm, (unsigned long long)t.blocksOk);
    }
    // Sample rates and a different centre: the capture holds both channels and the decoder finds them from setCenterHz
    for (double r : {1e6, 2e6, 2.4e6}) { auto x = ognScene(r, 100e3, {a, b}); AvRoadTelemetry t = run(Ogn, x, r); CHECK(t.blocksOk == 2, "rate %.0f: %llu", r, (unsigned long long)t.blocksOk); }
    { auto x = ognScene(2e6, 868.2e6 - 868.875e6, {a}); AvRoadTelemetry t = run(Ogn, x, 2e6, 868.875e6); CHECK(t.blocksOk == 1, "centre 868.875: %llu", (unsigned long long)t.blocksOk); }
    // Inverted spectrum
    { auto x = ognScene(fs, 100e3, {a}); for (auto& v : x) v = std::conj(v); CHECK(run(Ogn, x, fs).blocksOk == 1, "inverted spectrum"); }
    // Noise sweep, 200 kHz noise bandwidth
    {
        printf("ogn snr in 200 kHz:");
        for (double snr : {25.0, 18.0, 14.0, 12.0, 10.0, 8.0}) {
            int got = 0;
            for (unsigned s = 1; s <= 8; s++) { auto x = ognScene(fs, 100e3, {a}); addNoise(x, snr + 10 * std::log10(fs / 200e3), 1, s); got += (int)run(Ogn, x, fs).blocksOk; }
            printf(" %.0f dB %d/8", snr, got);
            if (snr >= 14) CHECK(got == 8, "snr %.0f: %d of 8", snr, got);
        }
        printf("\n");
    }
    // Two trackers on one channel, one after the other; on two channels at once
    { auto x = ognScene(fs, 100e3, {a, b}, 0, 0.0005); AvRoadTelemetry t = run(Ogn, x, fs); CHECK(t.blocksOk == 2, "back to back %llu", (unsigned long long)t.blocksOk); }
    { auto x = ognScene(fs, -100e3, {a}); auto y = ognScene(fs, 100e3, {b}); y.resize(x.size()); for (size_t i = 0; i < x.size(); i++) x[i] += y[i]; AvRoadTelemetry t = run(Ogn, x, fs); CHECK(t.blocksOk == 2, "two channels at once %llu", (unsigned long long)t.blocksOk); }
    // A collision: the stronger packet survives
    { auto x = ognScene(fs, 100e3, {a}); auto y = ognScene(fs, 100e3, {b}); y.resize(x.size()); for (size_t i = 0; i < x.size(); i++) x[i] += y[i] * 0.05f; AvRoadTelemetry t = run(Ogn, x, fs); CHECK(t.rows.size() && t.rows[0].id.find("111111") == 0, "collision"); }
    // Noise alone
    { std::vector<cf32> x((size_t)fs * 2); addNoise(x, 0, 1, 3); CHECK(run(Ogn, x, fs).blocksOk == 0, "noise only"); }
}

// ---- FANET
static std::vector<uint8_t> fanetTrack(int mfg, int dev, double lat, double lon, int alt) {
    std::vector<uint8_t> p = {1, (uint8_t)mfg, (uint8_t)dev, (uint8_t)(dev >> 8)};
    int32_t la = (int32_t)std::lround(lat * 93206), lo = (int32_t)std::lround(lon * 46603);
    for (int i = 0; i < 3; i++) p.push_back((uint8_t)(la >> (8 * i)));
    for (int i = 0; i < 3; i++) p.push_back((uint8_t)(lo >> (8 * i)));
    uint16_t t = (uint16_t)(alt & 0x7FF) | 1 << 12 | 1 << 15;
    p.push_back((uint8_t)t); p.push_back((uint8_t)(t >> 8));
    p.push_back(40); p.push_back(12); p.push_back(64);
    return p;
}
static std::vector<cf32> fanetIq(double fs, const std::vector<std::vector<uint8_t>>& pk, double bw, double freq, double ppm = 0) {
    lora::Params lp; lp.sf = 7; lp.bwHz = bw; lp.cr = 5; lp.preamble = 12; lp.syncWord = 0xF1; lp.crc = true;
    std::vector<cf32> out((size_t)(fs * (0.05 + 0.06 * (double)pk.size())));
    double t = 0.02;
    for (auto& p : pk) { lora::TxFrame f; f.p = lp; f.data = lora::encode(lp, p.data(), p.size()); f.startSec = t; f.freqHz = freq; f.sroPpm = ppm; f.amp = 1; f.render(out.data(), out.size(), 0, fs); t = f.endSec() + 0.01; }
    return out;
}
static void testFanet() {
    {
        auto p = fanetTrack(0x11, 0x2233, 47.5, -8.25, 1200);
        const FanetFrame f = fanetParse(p.data(), p.size());
        CHECK(f.valid && f.type == 1 && f.manufacturer == 0x11 && f.device == 0x2233 && f.altM == 1200 && f.aircraftType == 1, "track fields");
        CHECK(std::fabs(f.lat - 47.5) < 2e-5 && std::fabs(f.lon + 8.25) < 3e-5, "position %f %f", f.lat, f.lon);
        CHECK(std::fabs(f.speedKmh - 20) < 0.01 && std::fabs(f.climbMs - 1.2f) < 0.01 && std::fabs(f.headingDeg - 90) < 0.01, "motion");
        uint8_t n[] = {2, 1, 2, 0, 'B', 'o', 'b'};
        CHECK(fanetParse(n, sizeof n).text == "name: Bob", "name");
        uint8_t e[] = {0x83, 1, 2, 0, 0x20, 9, 8, 7, 0, 'h', 'i'};       // extended header, unicast
        const FanetFrame m = fanetParse(e, sizeof e);
        CHECK(m.unicast && m.destManufacturer == 9 && m.destDevice == 0x0708 && m.text == "message: hi", "unicast message: %s", m.text.c_str());
        uint8_t w[] = {4, 1, 2, 0, 0xE0, 0x10, 0x20, 0x30, 0x10, 0x20, 0x30, 40, 64, 10, 20};   // gateway, temperature, wind with a position
        const FanetFrame s = fanetParse(w, sizeof w);
        CHECK(s.hasPos && s.text.find("internet gateway") != std::string::npos && s.text.find("20.0 C") != std::string::npos, "service: %s", s.text.c_str());
        CHECK(!fanetParse(n, 2).valid, "short frame");
    }
    const double fs = 1000000;
    for (double cfo : {-30e3, 0.0, 25e3}) for (double ppm : {-30.0, 30.0}) {
        auto x = fanetIq(fs, {fanetTrack(1, 77, 54.0, 25.0, 900)}, 250e3, cfo, ppm);
        AvRoadTelemetry t = run(Fanet, x, fs);
        CHECK(t.blocksOk == 1 && t.rows.size() && t.rows[0].hasPos, "cfo %.0f ppm %.0f: %llu", cfo, ppm, (unsigned long long)t.blocksOk);
    }
    { auto x = fanetIq(2e6, {fanetTrack(1, 88, 40.0, -100.0, 900)}, 500e3, 0); AvRoadTelemetry t = run(Fanet, x, 2e6, 920.8e6); CHECK(t.blocksOk == 1, "500 kHz zone: %llu", (unsigned long long)t.blocksOk); }
    {
        printf("fanet snr in 250 kHz:");
        for (double snr : {10.0, 0.0, -4.0, -8.0, -12.0}) {
            int got = 0;
            for (unsigned s = 1; s <= 5; s++) { auto x = fanetIq(fs, {fanetTrack(1, 77, 54.0, 25.0, 900)}, 250e3, 2000); addNoise(x, snr + 10 * std::log10(fs / 250e3), 1, s); got += (int)run(Fanet, x, fs).blocksOk; }
            printf(" %.0f dB %d/5", snr, got);
            if (snr >= -4) CHECK(got == 5, "snr %.0f: %d of 5", snr, got);
        }
        printf("\n");
    }
    { auto x = fanetIq(fs, {fanetTrack(1, 1, 54, 25, 500), fanetTrack(1, 2, 54, 25, 600), fanetTrack(1, 3, 54, 25, 700)}, 250e3, 0); CHECK(run(Fanet, x, fs).blocksOk == 3, "three frames"); }
    { std::vector<cf32> x((size_t)fs * 1); addNoise(x, 0, 1, 3); CHECK(run(Fanet, x, fs).blocksOk == 0, "noise only"); }
}

int main() {
    testOgn(); testFanet();
    printf(fails ? "FAILED %d\n" : "all ok\n", fails);
    return fails != 0;
}
