// Qi receiver-to-transmitter packets: every named message with its content, bit clock 4 % off, power-signal offset, an amplitude wander of the power signal, a noise sweep with the
// false-packet count and 15 minutes of noise. Simulated signals only.
#include "x40_gen2.h"
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<Bits> packets() {
    return {qiPacket({0x01, 0xA0}), qiPacket({0x02, 0x01}), qiPacket({0x03, 0xF6}), qiPacket({0x04, 0x50}), qiPacket({0x05, 0x64}), qiPacket({0x06, 0x0A}),
            qiPacket({0x51, 0x0C, 0x00, 0x00, 0x00, 0x00}), qiPacket({0x71, 0x11, 0x00, 0x1D, 0x12, 0x34, 0x56, 0x78}), qiPacket({0x04, 0x12}, 8)};
}
static size_t n(const X40Telemetry& t) { return t.rows.size(); }
static bool find(const X40Telemetry& t, const char* id, const char* frag) { for (auto& r : t.rows) if (r.id == id && r.text.find(frag) != std::string::npos) return true; return false; }

int main() {
    const double fs = 250000;
    {
        auto t = run(x40::Qi, qiSignal(Qi{}, packets(), fs), fs);
        CHECK(n(t) == 9, "9 packets, got %zu", n(t));
        CHECK(find(t, "Signal strength", "160"), "strength");
        CHECK(find(t, "End power transfer", "charge complete"), "end power");
        CHECK(find(t, "Control error", "-10"), "control error");
        CHECK(find(t, "Received power", "80"), "received power");
        CHECK(find(t, "Charge status", "100 %"), "charge");
        CHECK(find(t, "Power control hold-off", "10 ms"), "hold-off");
        CHECK(find(t, "Configuration", "power class 0, maximum power value 12"), "configuration");
        CHECK(find(t, "Identification", "version 1.1") && find(t, "Identification", "manufacturer code 001D") && find(t, "Identification", "12345678"), "identification");
    }
    for (double ppm : {40000.0, -40000.0}) { Qi q; q.clockPpm = ppm; CHECK(n(run(x40::Qi, qiSignal(q, packets(), fs), fs)) == 9, "clock %.0f ppm", ppm); }
    for (double depth : {0.10, 0.03}) { Qi q; q.depth = depth; CHECK(n(run(x40::Qi, qiSignal(q, packets(), fs), fs)) == 9, "depth %.2f", depth); }
    for (double off : {0.0, 3000.0, -3000.0}) { auto x = qiSignal(Qi{}, packets(), fs); mix(x, off, off == 0 ? 600 : 0, fs); CHECK(n(run(x40::Qi, x, fs)) == 9, "offset %.0f", off); }
    { auto x = qiSignal(Qi{}, packets(), fs); mix(x, 30000, 0, fs); CHECK(n(run(x40::Qi, x, fs, 30000)) == 9, "off-centre"); }
    { Qi q; q.wanderHz = 0.5; q.wanderDepth = 0.04; CHECK(n(run(x40::Qi, qiSignal(q, packets(), fs), fs)) == 9, "wander 0.5 Hz, 4 %%"); }
    // a wrong checksum and a wrong parity: nothing
    { auto p = qiPacket({0x04, 0x30}); p[p.size() - 3] ^= 1; auto p2 = qiPacket({0x04, 0x31}); p2[12 + 9] ^= 1; auto t = run(x40::Qi, qiSignal(Qi{}, {p, p2}, fs), fs); CHECK(n(t) == 0, "bad packets listed: %zu", n(t)); }
    printf("noise sweep, 5 %% depth, carrier to noise in 10 kHz, 3 runs of 9 packets:\n");
    for (double cnr : {70.0, 60.0, 55.0, 50.0, 45.0, 40.0}) {
        int ok = 0, tot = 0, extra = 0;
        for (unsigned s = 1; s <= 3; s++) {
            auto x = qiSignal(Qi{}, packets(), fs); addNoise(x, noiseFor(cnr, 10000, fs), s);
            const size_t k = n(run(x40::Qi, x, fs)); ok += (int)std::min<size_t>(k, 9); tot += 9; if (k > 9) extra += (int)k - 9;
        }
        printf("  %.0f dB: %d of %d, %d extra\n", cnr, ok, tot, extra);
        if (cnr >= 55) CHECK(ok == tot && extra == 0, "%.0f dB: %d of %d, %d extra", cnr, ok, tot, extra);
    }
    {
        size_t falseRows = 0;
        for (int k = 0; k < 15; k++) {
            std::vector<cf32> x((size_t)(fs * 60), cf32(k % 2 ? 1.f : 0.f, 0)); addNoise(x, noiseFor(45, 10000, fs), 200 + k);
            falseRows += n(run(x40::Qi, x, fs));
        }
        printf("noise alone or under a plain power signal, 15 minutes: %zu false packets\n", falseRows);
        CHECK(falseRows == 0, "false packets %zu", falseRows);
    }
    printf(fails ? "x40 qi: %d FAILED\n" : "x40 qi: ok\n", fails);
    return fails ? 1 : 0;
}
