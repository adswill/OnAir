// DJI DroneID (dji_proto.h): the building blocks and one burst at 15.36 Msps through noise, a carrier offset and a timing offset.
//
// VERIFIED AGAINST PUBLISHED MATERIAL: the structure constants (FFT 1024, 600 carriers, CP 80/72/.../80, 9880 samples = 643 us, ZC symbols 4 and 6 with roots
// 600 and 147, ZC formula and carrier mapping of the proto17 scripts), the 7200-bit frame, K = 1408 with a QPP interleaver that is a permutation, the CRC-24A
// polynomial, the packet layout offsets (paper appendix, 0x5b bytes).
// SYNTHETIC ONLY (our own encoder and decoder agree, no real DJI capture was available): the LTE turbo encoder and rate matching details (tail-bit mapping,
// sub-block interleaver, rv 0), the scrambler seed convention, the QPSK orientation, the QPP parameters (43, 88), the field scales (altitude 0.01 m).
#include "dect2/dji/dji_proto.h"
#include "dect2/common/stable_random.h"
#include "dect2/dsp/fftutil.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::dji;
#include "../common/test_util.h"

static DroneId sampleId(int n) {
    DroneId d;
    d.seq = 878 + n; d.state = 8179; d.serial = "SecureStorage?";
    d.lonRaw = (int32_t)(7.267960786 / 57.29577951308232 * 1e7); d.latRaw = (int32_t)(51.446866781 / 57.29577951308232 * 1e7);
    d.altRaw = 3932; d.heightRaw = 549; d.vNorth = 0; d.vEast = -7; d.vUp = 0; d.yaw = 16900;
    d.gpsMs = 1650894901980ull;
    d.appLatRaw = (int32_t)(43.268264 / 57.29577951308232 * 1e7); d.appLonRaw = (int32_t)(6.640125 / 57.29577951308232 * 1e7);
    d.homeLonRaw = (int32_t)(7.267943 / 57.29577951308232 * 1e7); d.homeLatRaw = (int32_t)(51.446883 / 57.29577951308232 * 1e7);
    d.deviceType = 60; d.uuidLen = 0;
    return d;
}

static std::vector<uint8_t> airBits(const DroneId& d, int variant) {
    std::vector<uint8_t> e = turboEncode(packDroneId(d));
    const std::vector<uint8_t> s = scramblerBits(kBurstBits, variant);
    for (int i = 0; i < kBurstBits; i++) e[(size_t)i] ^= s[(size_t)i];
    return e;
}

int main() {
    // structure
    CHECK(kBurstSamples == 9880 && std::abs(kBurstUs - 643.2) < 0.1, "burst length %d", kBurstSamples);
    CHECK(bodyStart(3) - bodyStart(1) == 2 * (1024 + 72), "symbol spacing");
    CHECK(carrierBin(0) == 724 && carrierBin(299) == 1023 && carrierBin(300) == 1 && carrierBin(599) == 300, "carrier bins");
    for (int r : kZcRoot) {
        const std::vector<cf32> z = zcFreq(r);
        bool unit = z.size() == 600;
        for (auto& v : z) unit = unit && std::abs(std::abs(v) - 1.f) < 1e-4f;
        CHECK(unit, "ZC root %d is not unit amplitude", r);
        const std::vector<cf32> t = zcTime(r);
        cf32 c0(0, 0); double e = 0;
        for (auto& v : t) e += std::norm(v);
        for (int n = 0; n < 1024; n++) c0 += t[(size_t)n] * std::conj(t[(size_t)((n + 7) % 1024)]);
        CHECK(std::abs(c0) / e < 0.05, "ZC autocorrelation at lag 7 is %.3f", std::abs(c0) / e);
    }
    // the interleaver is a permutation
    {
        std::vector<int> p = qppInterleaver(), seen(1408, 0);
        for (int v : p) seen[(size_t)v]++;
        bool perm = p.size() == 1408;
        for (int v : seen) perm = perm && v == 1;
        CHECK(perm, "QPP (43, 88) is not a permutation of 1408");
    }
    // CRC-24A: a block followed by its CRC gives 0; the packet round trip
    {
        const std::vector<uint8_t> f = packDroneId(sampleId(0));
        CHECK(crc24a(f.data(), f.size()) == 0, "frame CRC");
        DroneId d;
        CHECK(parseDroneId(f, d) && d.serial == "SecureStorage?" && d.seq == 878 && d.altRaw == 3932 && d.crc16Ok && d.gpsMs == 1650894901980ull, "packet round trip");
        CHECK(std::abs(d.lon() - 7.267960786) < 1e-5 && std::abs(d.lat() - 51.446866781) < 1e-5, "coordinates %.7f %.7f", d.lon(), d.lat());
    }
    // the scrambler is balanced and differs between the two seed conventions
    {
        const auto a = scramblerBits(7200, 0), b = scramblerBits(7200, 1);
        int ones = 0, diff = 0;
        for (size_t i = 0; i < a.size(); i++) { ones += a[i]; diff += a[i] != b[i]; }
        CHECK(std::abs(ones - 3600) < 200 && diff > 3000, "scrambler: %d ones, %d differences", ones, diff);
    }
    // turbo code on a Gaussian channel: BPSK at Es/N0 = -2 dB per transmitted bit (the code rate is 0.2)
    {
        std::mt19937 rng(5);
        dect2::normal_distribution<float> nd(0.f, 1.f);
        int ok = 0;
        const int trials = 12;
        for (int t = 0; t < trials; t++) {
            const std::vector<uint8_t> e = turboEncode(packDroneId(sampleId(t)));
            const float sigma = std::sqrt(0.5f / std::pow(10.f, -2.f / 10));
            std::vector<float> llr(e.size());
            for (size_t i = 0; i < e.size(); i++) llr[i] = (e[i] ? -1.f : 1.f) + sigma * nd(rng);
            for (auto& v : llr) v *= 2.f / (sigma * sigma);
            std::vector<uint8_t> out;
            if (turboDecode(llr, out) && out == packDroneId(sampleId(t))) ok++;
        }
        printf("  turbo at -2 dB Es/N0: %d / %d frames\n", ok, trials);
        CHECK(ok >= trials - 1, "turbo decoder: %d of %d", ok, trials);
        // noise only never passes the CRC
        int fp = 0;
        for (int t = 0; t < 30; t++) {
            std::vector<float> llr(7200);
            for (auto& v : llr) v = 4.f * nd(rng);
            std::vector<uint8_t> out;
            fp += turboDecode(llr, out);
        }
        CHECK(fp == 0, "%d false CRCs from noise", fp);
    }
    // a burst at 15.36 Msps: noise, offsets (carrier offset up to 3 subcarriers and a bit, timing), nine and eight symbols
    {
        std::mt19937 rng(11);
        dect2::normal_distribution<float> nd(0.f, 1.f);
        struct Case { double snr, cfo; int pad; bool first; int variant; } cases[] = {
            {20, 0, 300, true, 0}, {12, 23456, 417, true, 0}, {12, -41234, 250, true, 0}, {8, 8000, 333, true, 0}, {12, 15000, 300, false, 0}, {12, -7000, 500, true, 1}};
        for (const Case& c : cases) {
            const DroneId id = sampleId((int)(&c - cases));
            const std::vector<cf32> b = buildBurst(airBits(id, c.variant), c.first);
            std::vector<cf32> x((size_t)(c.pad + b.size() + 500), cf32(0, 0));
            for (size_t i = 0; i < b.size(); i++) x[(size_t)c.pad + i] = b[i];
            const float sigma = std::sqrt(0.5f / std::pow(10.f, (float)c.snr / 10));
            for (size_t n = 0; n < x.size(); n++) {
                const double ph = 2 * 3.14159265358979 * c.cfo * (double)n / kFs;
                x[n] = x[n] * cf32((float)std::cos(ph), (float)std::sin(ph)) + sigma * cf32{nd(rng), nd(rng)};
            }
            const BurstResult r = receiveBurst(x);
            printf("  pad %d timing %d snr %4.1f cfo %8.0f: zc %.2f %.2f cfo %8.0f snr %.1f crc %d variant %d rot %d\n", c.pad, r.timing, c.snr, c.cfo, r.zc1, r.zc2, r.cfoHz, r.snrDb, r.crcOk, r.variant, r.rotation);
            CHECK(r.zc && std::abs(r.cfoHz - c.cfo) < 300, "ZC / CFO");
            CHECK(r.crcOk && r.id.serial == "SecureStorage?" && r.id.seq == id.seq && r.id.lonRaw == id.lonRaw, "decode");
            CHECK(r.variant == c.variant, "scrambler convention found: %d", r.variant);
        }
        // noise-free bursts: the ZC measure must not read past the 600 carriers (it produced NaN on a clean burst)
        for (int cfo : {0, 5000, -12000}) {
            const DroneId id = sampleId(7);
            const std::vector<cf32> b = buildBurst(airBits(id, 0), true);
            std::vector<cf32> x((size_t)(300 + b.size() + 500), cf32(0, 0));
            for (size_t i = 0; i < b.size(); i++) x[300 + i] = b[i];
            for (size_t n = 0; n < x.size(); n++) {
                const double ph = 2 * 3.14159265358979 * cfo * (double)n / kFs;
                x[n] *= cf32((float)std::cos(ph), (float)std::sin(ph));
            }
            const BurstResult r = receiveBurst(x);
            printf("  noise-free cfo %6d: zc %.4f %.4f crc %d\n", cfo, r.zc1, r.zc2, r.crcOk);
            CHECK(std::isfinite(r.zc1) && std::isfinite(r.zc2), "noise-free ZC is not finite (%f %f)", r.zc1, r.zc2);
            CHECK(r.zc1 > 0.9f && r.zc2 > 0.9f, "noise-free ZC %.3f %.3f", r.zc1, r.zc2);
            CHECK(r.zc && r.crcOk && r.id.seq == id.seq, "noise-free burst (cfo %d) not decoded", cfo);
        }
        // noise alone: no ZC
        int zc = 0;
        for (int t = 0; t < 10; t++) {
            std::vector<cf32> x(12000);
            for (auto& v : x) v = cf32{nd(rng), nd(rng)};
            zc += receiveBurst(x).zc;
        }
        CHECK(zc == 0, "%d ZC detections in noise", zc);
        // another OFDM burst (random QPSK, same cyclic prefixes, no ZC symbols): the cyclic prefix matches but the ZC check says no
        std::vector<cf32> x(12000, cf32(0, 0));
        {
            std::vector<uint8_t> bits(7200);
            for (auto& v : bits) v = (uint8_t)(rng() & 1);
            std::vector<cf32> b = buildBurst(bits, true);
            // overwrite the ZC symbols with data by rebuilding with different roots is not possible: swap the symbols' content with the neighbours'
            for (size_t i = 0; i < b.size(); i++) x[200 + i] = b[i] * 0.f;
            std::vector<cf32> o(kFft);
            for (int s = 0; s < 9; s++) {   // plain random-QPSK symbols with the same prefixes
                std::vector<cf32> f(kFft, cf32(0, 0));
                for (int k = 0; k < 600; k++) f[(size_t)carrierBin(k)] = qpsk((int)(rng() & 1), (int)(rng() & 1));
                Fft fft(kFft);
                fft.inverse(f.data());
                size_t at = 200;
                for (int j = 0; j < s; j++) at += (size_t)(cpLen(j) + kFft);
                for (int i = 0; i < cpLen(s); i++) x[at + (size_t)i] = f[(size_t)(kFft - cpLen(s) + i)] * 0.04f;
                for (int i = 0; i < kFft; i++) x[at + (size_t)cpLen(s) + (size_t)i] = f[(size_t)i] * 0.04f;
            }
            for (auto& v : x) v += 0.02f * cf32{nd(rng), nd(rng)};
            const BurstResult r = receiveBurst(x);
            CHECK(!r.zc && !r.crcOk, "an OFDM burst without the ZC symbols was taken for DroneID (zc %.2f %.2f)", r.zc1, r.zc2);
        }
    }
    printf(fails ? "dji proto: %d FAILED\n" : "dji proto: all passed\n", fails);
    return fails ? 1 : 0;
}
