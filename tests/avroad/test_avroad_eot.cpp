// AAR end of train: the real recording from PyEOT (demo3eot.wav), then simulated frames with impairments.
#include "avroad_gen.h"
#include "dect2/avroad/avroad_proto.h"
#include "dect2/avroad/avroad_rx.h"
#include "dect2/avroad/avroad_tel.h"
#include <cstdio>
#include <fstream>
#include <map>

using namespace dect2;
using namespace avgen;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static AvRoadTelemetry run(const std::vector<cf32>& x, double fs, double offset = 0) {
    AvRoadReceiver r; r.setVariant(avroad::EotHot); r.configure(fs); r.setSignalOffset(offset);
    for (size_t i = 0; i < x.size(); i += 4096) r.feed(x.data() + i, std::min<size_t>(4096, x.size() - i));
    std::vector<cf32> tail((size_t)(fs * 0.3)); r.feed(tail.data(), tail.size());
    AvRoadTelemetry t; r.telemetry(t, 0);
    return t;
}

static std::vector<cf32> scene(double fs, std::vector<EotFields> frames, double gapSec, double ppm = 0) {
    std::vector<cf32> x((size_t)(fs * 0.05)); double tp = 0, cp = 0;
    for (auto& f : frames) { appendAfskFm(x, eotFrameBits(eotDataBits(f)), 1200, fs, 1800, 1200, 3000, tp, cp, ppm); x.resize(x.size() + (size_t)(fs * gapSec)); }
    return x;
}

int main(int argc, char** argv) {
    // The real recording, FM modulated back onto a carrier at the file's rate
    {
        const std::string dir = argc > 1 ? argv[1] : "tests/avroad/data";
        std::ifstream f(dir + "/demo3eot.wav", std::ios::binary);
        CHECK((bool)f, "open the recording");
        std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
        std::vector<cf32> x; double p = 0;
        for (size_t i = 44; i < b.size(); i++) { const float a = ((uint8_t)b[i] - 128.f) / 128.f; p += 2 * kPi * 3000 * a / 48000; x.push_back(cf32((float)std::cos(p), (float)std::sin(p))); }
        AvRoadTelemetry t = run(x, 48000);
        printf("real recording: %llu good, %llu bad\n", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
        for (auto& r : t.rows) printf("  %.2f %s %s\n", r.timeSec, r.id.c_str(), r.text.c_str());
        CHECK(t.blocksOk >= 3, "real recording decoded %llu frames", (unsigned long long)t.blocksOk);
    }
    const double fs = 1000000;
    // Every field value reaches the output
    for (int psi : {0, 1, 45, 90, 127}) for (int type : {0, 7}) for (int motion : {0, 1}) {
        EotFields f; f.psi = psi; f.type = type; f.motion = motion; f.unit = 1 + (uint32_t)psi * 1000; f.conf = type == 7;
        AvRoadTelemetry t = run(scene(48000, {f}, 0.05), 48000);
        CHECK(t.blocksOk == 1, "psi %d type %d motion %d: %llu", psi, type, motion, (unsigned long long)t.blocksOk);
        if (t.rows.size()) { CHECK(t.rows[0].text.find(std::to_string(psi) + " psi") != std::string::npos, "%s", t.rows[0].text.c_str()); CHECK(t.rows[0].id == [&] { char s[16]; snprintf(s, 16, "%05u", f.unit); return std::string(s); }(), "id %s", t.rows[0].id.c_str()); }
    }
    // Carrier offset and clock error, several input rates
    for (double r : {48000.0, 250000.0, 1000000.0, 2400000.0}) for (double cfo : {-4000.0, -1000.0, 0.0, 2500.0}) for (double ppm : {-200.0, 200.0}) {
        auto x = scene(r, {EotFields{}}, 0.05, ppm); mix(x, cfo, r);
        AvRoadTelemetry t = run(x, r);
        CHECK(t.blocksOk == 1, "rate %.0f cfo %.0f ppm %.0f: %llu", r, cfo, ppm, (unsigned long long)t.blocksOk);
    }
    // Signal away from the centre, the offset set
    { auto x = scene(fs, {EotFields{}}, 0.05); mix(x, 100000, fs); CHECK(run(x, fs, 100000).blocksOk == 1, "off-centre"); }
    // Noise sweep at 12.5 kHz noise bandwidth; the frame count should fall smoothly
    {
        printf("eot snr in 12.5 kHz:");
        for (double snr : {30.0, 20.0, 15.0, 12.0, 10.0, 8.0, 6.0}) {
            int got = 0;
            for (unsigned s = 1; s <= 10; s++) { auto x = scene(48000, {EotFields{}}, 0.05); addNoise(x, snr + 10 * std::log10(48000 / 12500.0), 1, s); got += (int)run(x, 48000).blocksOk; }
            printf(" %.0f dB %d/10", snr, got);
            if (snr >= 12) CHECK(got == 10, "snr %.0f: %d of 10", snr, got);
        }
        printf("\n");
    }
    // Back to back frames with different units
    { std::vector<EotFields> v(4); for (int i = 0; i < 4; i++) v[i].unit = 100 + i; AvRoadTelemetry t = run(scene(48000, v, 0.01), 48000); CHECK(t.blocksOk == 4, "back to back %llu", (unsigned long long)t.blocksOk); }
    // Two wrong bits repaired; three refused
    {
        auto bits = eotFrameBits(eotDataBits(EotFields{})); const size_t s = 69 + 11;
        auto body = std::vector<uint8_t>(bits.begin() + (long)s, bits.end());
        auto b2 = body; b2[3] ^= 1; b2[50] ^= 1; int fx = 0;
        CHECK(avroad::eotCheck(b2.data(), &fx) && fx == 2 && std::equal(b2.begin(), b2.begin() + 45, body.begin()), "two bits repaired");
        auto b3 = body; b3[3] ^= 1; b3[20] ^= 1; b3[50] ^= 1; uint8_t before[63]; std::copy(b3.begin(), b3.end(), before);
        const bool acc = avroad::eotCheck(b3.data(), &fx);
        if (acc) printf("note: a three bit error mis-corrected to another codeword\n"); else printf("three bit error refused\n");
    }
    // Noise alone gives nothing
    { std::vector<cf32> x(48000 * 5); addNoise(x, 0, 1, 7); CHECK(run(x, 48000).blocksOk == 0, "noise only"); }
    printf(fails ? "FAILED %d\n" : "all ok\n", fails);
    return fails != 0;
}
