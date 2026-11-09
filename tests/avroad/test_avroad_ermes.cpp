// ERMES: the specification's codeword examples, then simulated batches. No real recording exists to our knowledge: simulated only.
#include "avroad_gen.h"
#include "dect2/avroad/avroad_proto.h"
#include "dect2/avroad/avroad_rx.h"
#include "dect2/avroad/avroad_tel.h"
#include <cstdio>

using namespace dect2;
using namespace dect2::avroad;
using namespace avgen;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static AvRoadTelemetry run(const std::vector<cf32>& x, double fs) {
    AvRoadReceiver r; r.setVariant(Ermes); r.configure(fs);
    for (size_t i = 0; i < x.size(); i += 4096) r.feed(x.data() + i, std::min<size_t>(4096, x.size() - i));
    std::vector<cf32> tail((size_t)(fs * 0.3)); r.feed(tail.data(), tail.size());
    AvRoadTelemetry t; r.telemetry(t, 0);
    return t;
}

struct Msg { uint32_t addr; int cat; std::vector<uint32_t> data; };
// One batch of codewords
static std::vector<uint32_t> batch(const ErmesSystem& s, std::vector<uint32_t> addrs, const std::vector<Msg>& msgs) {
    std::vector<uint32_t> w;
    w.push_back(kErmesPreamble); w.push_back(kErmesSync);
    uint32_t si0 = (uint32_t)s.country << 11 | (uint32_t)s.oper << 8 | (uint32_t)s.area << 2 | (uint32_t)s.eti << 1 | (uint32_t)s.bai;
    uint32_t si1 = (uint32_t)s.fsi << 13 | (uint32_t)s.cycle << 7 | (uint32_t)s.subseq << 4 | (uint32_t)s.batch;
    uint32_t ssi = (uint32_t)s.ssit << 14 | (uint32_t)s.ssif;
    w.push_back(ermesEncode(si0)); w.push_back(ermesEncode(si1)); w.push_back(ermesEncode(ssi));
    for (uint32_t a : addrs) w.push_back(ermesEncode(a));
    const int I = (int)addrs.size(), J = 9 - (4 + I) % 9;
    for (int i = 0; i < J; i++) w.push_back(kErmesApt);
    // message words in plain order
    std::vector<uint32_t> m;
    m.push_back(kErmesDelim);
    for (auto& g : msgs) {
        m.push_back(ermesEncode(g.addr >> 4));
        m.push_back(ermesEncode((g.addr & 0xF) << 14 | 1u << 9 | (uint32_t)g.cat << 3));
        for (uint32_t d : g.data) m.push_back(ermesEncode(d));
        m.push_back(kErmesDelim);
    }
    const int total = s.batch == 15 ? 190 : 154;
    while ((int)(w.size() + m.size()) < total) m.push_back(kErmesDelim);
    while (w.size() + m.size() > (size_t)total) m.pop_back();
    // interleave nine by thirty
    for (size_t b = 0; b + 9 <= m.size(); b += 9) {
        uint32_t tx[9] = {}; int k = 0;
        for (int col = 0; col < 30; col++) for (int row = 0; row < 9; row++) { if (m[b + (size_t)row] >> (29 - col) & 1) tx[k / 30] |= 1u << (29 - k % 30); k++; }
        for (uint32_t v : tx) w.push_back(v);
    }
    return w;
}
static const float kLevel[4] = {-4687.5f, -1562.5f, 4687.5f, 1562.5f};   // symbol 00, 01, 10, 11
static void addWords(std::vector<cf32>& x, const std::vector<uint32_t>& w, double fs, double ph0, double& phase, double ppm) {
    (void)ph0;
    const double sps = fs / 3125 * (1 + ppm * 1e-6);
    double acc = 0; int cur = 0; int wi = 0, bi = 28;
    float lp = 0; const float a = (float)(1 - std::exp(-2 * kPi * 3000 / fs));
    while (wi < (int)w.size()) {
        cur = (int)(w[(size_t)wi] >> bi & 3);
        lp += a * (kLevel[cur] - lp);
        phase += 2 * kPi * lp / fs;
        x.push_back(cf32((float)std::cos(phase), (float)std::sin(phase)));
        if (++acc >= sps) { acc -= sps; bi -= 2; if (bi < 0) { bi = 28; wi++; } }
    }
}
static std::vector<uint32_t> alpha(const std::string& t) {
    std::vector<uint8_t> b;
    auto put = [&](int v) { for (int i = 6; i >= 0; i--) b.push_back((uint8_t)(v >> i & 1)); };
    for (char c : t) put(c);
    while (b.size() % 18) put(0x11);
    std::vector<uint32_t> w; for (size_t i = 0; i + 18 <= b.size(); i += 18) { uint32_t v = 0; for (int k = 0; k < 18; k++) v = v << 1 | b[i + (size_t)k]; w.push_back(v); }
    return w;
}

int main() {
    {
        CHECK(ermesEncode(1) == 0b000000000000000001101011001101, "spec example, message 1");
        CHECK(ermesEncode(0x3FFFF) == 0b111111111111111111110001001011, "spec example, all ones");
        uint32_t i; int f = 0;
        CHECK(ermesDecode(ermesEncode(0x12345) ^ (1u << 3) ^ (1u << 20), i, &f) && i == 0x12345 && f == 2, "two errors");
        CHECK(!ermesDecode(ermesEncode(0x12345) ^ (1u << 3) ^ (1u << 20) ^ (1u << 28), i, &f), "three errors detected");
        CHECK(ermesMessageText(2, alpha("HELLO")) == "HELLO", "alpha text");
    }
    ErmesSystem s; s.country = 40; s.oper = 2; s.area = 5; s.cycle = 12; s.subseq = 3; s.batch = 4; s.ssit = 0; s.ssif = 3 << 11 | 18 << 6 | 21 << 1;
    Msg m1{0x2ABCD5 & 0x3FFFFF, 2, alpha("HELLO WORLD")};
    auto digits = [](const std::string& t) { std::vector<uint8_t> b; for (char c : t) { const int v = c - '0'; for (int i = 3; i >= 0; i--) b.push_back((uint8_t)(v >> i & 1)); } while (b.size() % 18) b.push_back((b.size() % 4 == 1 || b.size() % 4 == 0) ? 1 : 0); std::vector<uint32_t> w; for (size_t i = 0; i + 18 <= b.size(); i += 18) { uint32_t v = 0; for (int k = 0; k < 18; k++) v = v << 1 | b[i + (size_t)k]; w.push_back(v); } return w; };
    Msg m2{0x123454, 1, digits("1234567890")};
    auto mk = [&](double fs, double ppm, double cfo) { std::vector<cf32> x((size_t)(fs * 0.05)); double p = 0; addWords(x, batch(s, {1000, 200, 30}, {m1, m2}), fs, 0, p, ppm); x.resize(x.size() + (size_t)(fs * 0.1)); mix(x, cfo, fs); return x; };
    for (double fs : {48000.0, 250000.0, 1000000.0}) for (double cfo : {-1500.0, 0.0, 1200.0}) for (double ppm : {-50.0, 50.0}) {
        AvRoadTelemetry t = run(mk(fs, ppm, cfo), fs);
        bool ok = t.blocksOk == 1 && t.rows.size() == 3;
        CHECK(ok, "fs %.0f cfo %.0f ppm %.0f: batches %llu rows %zu", fs, cfo, ppm, (unsigned long long)t.blocksOk, t.rows.size());
        if (ok) {
            CHECK(t.rows[1].text == "HELLO WORLD" && t.rows[1].id == std::to_string(m1.addr), "message 1: %s %s", t.rows[1].id.c_str(), t.rows[1].text.c_str());
            CHECK(t.rows[0].text.rfind("1234567890", 0) == 0, "message 2: %s", t.rows[0].text.c_str());
            CHECK(t.rows[2].text == "1000 200 30 ", "addresses: %s", t.rows[2].text.c_str());
        }
        bool net = false; for (auto& f : t.facts) if (f.second.find("country 40 operator 2 area 5, cycle 12 subsequence 3 batch E, zone 3 hour 18 day 21") != std::string::npos) net = true;
        CHECK(net, "system information");
    }
    // Last batch of a subsequence (190 codewords), no addresses and empty message partition
    { ErmesSystem z = s; z.batch = 15; std::vector<cf32> x(4800); double p = 0; addWords(x, batch(z, {}, {}), 48000, 0, p, 0); x.resize(x.size() + 4800); AvRoadTelemetry t = run(x, 48000); CHECK(t.blocksOk == 1 && t.rows.empty(), "empty last batch: %llu %zu", (unsigned long long)t.blocksOk, t.rows.size()); }
    // Noise sweep in 25 kHz
    {
        printf("ermes snr in 25 kHz:");
        for (double snr : {35.0, 25.0, 20.0, 16.0, 13.0, 10.0}) {
            int got = 0;
            for (unsigned se = 1; se <= 5; se++) { auto x = mk(48000, 0, 300); addNoise(x, snr + 10 * std::log10(48000 / 25000.0), 1, se); got += run(x, 48000).blocksOk == 1; }
            printf(" %.0f dB %d/5", snr, got);
            if (snr >= 20) CHECK(got == 5, "snr %.0f: %d of 5", snr, got);
        }
        printf("\n");
    }
    { std::vector<cf32> x(48000 * 4); addNoise(x, 0, 1, 2); CHECK(run(x, 48000).blocksOk == 0, "noise only"); }
    printf(fails ? "FAILED %d\n" : "all ok\n", fails);
    return fails != 0;
}
