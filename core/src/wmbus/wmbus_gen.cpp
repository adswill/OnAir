// Wireless M-Bus test signal (see wmbus_gen.h).
#include "dect2/wmbus/wmbus_gen.h"
#include "dect2/wmbus/wmbus_live.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/common/mathconst.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {
using dect2::math::kPi;
using namespace wmbus;

void bcd(std::vector<uint8_t>& o, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; i++) { o.push_back((uint8_t)(((v / 10) % 10) << 4 | (v % 10))); v /= 100; }
}
void le(std::vector<uint8_t>& o, uint64_t v, int bytes) { for (int i = 0; i < bytes; i++) { o.push_back((uint8_t)v); v >>= 8; } }

struct Meter { const char* mfr; uint32_t id; uint8_t ver, type; Phy phy; bool enc; bool wize = false; };
const Meter kMeters[] = {
    {"KAM", 12345678, 0x1B, 0x07, Phy::S1, false},     // water
    {"ELV", 40112233, 0x2A, 0x04, Phy::T1, false},     // heat
    {"EMH", 50667788, 0x01, 0x02, Phy::T1, true},      // electricity, encrypted
    {"GWF", 61000042, 0x03, 0x03, Phy::C1A, false},    // gas
    {"SEN", 72004711, 0x0C, 0x16, Phy::C1B, false},    // cold water, a long frame in format B
    {"QDS", 83001234, 0x05, 0x08, Phy::R2, false},     // heat cost allocator
    {"ITW", 94005566, 0x02, 0x07, Phy::N1, false},     // water on 169 MHz
    {"SAG", 21436587, 0x01, 0x03, Phy::N24, true, true},   // Wize gas meter, 2.4 kbit/s, format B, L7 encrypted (3.9)
    {"ITW", 13572468, 0x01, 0x07, Phy::N1, false, true},   // Wize water meter, 4.8 kbit/s, format B, key selector 0 (L7 in clear)
};
}

int WmbusGenerator::meterCount() { return (int)(sizeof kMeters / sizeof kMeters[0]); }

std::vector<uint8_t> WmbusGenerator::meterFrame(int mi, uint32_t n, Phy& phy, bool encrypted) {
    const Meter& m = kMeters[mi % meterCount()];
    phy = m.phy;
    std::vector<uint8_t> b;
    if (m.wize) {   // Wize LAN v1.x DATA message: C = SND-NR, CI 0x20, PRES-EXCHANGE layer
        uint8_t id4[4];
        uint32_t v = m.id;
        for (int i = 0; i < 4; i++) { id4[i] = (uint8_t)((v % 10) | ((v / 10) % 10) << 4); v /= 100; }
        std::vector<uint8_t> l7;
        uint32_t x = m.id * 2654435761u + n;
        for (int i = 0; i < 20; i++) { x = x * 1664525u + 1013904223u; l7.push_back((uint8_t)(x >> 24)); }
        return buildFrame(wizeBody(mfrStringToCode(m.mfr), id4, m.ver, m.type, 0x44, m.enc && encrypted ? 1 : 0, 0x0A, (int)(n & 0xFFFF), 0xFE, l7,
                                   0xA1B2C3D4u ^ n, (int)((1794300000u + 60 * n) & 0xFFFF), (uint16_t)(0x5A5A ^ n)), true);
    }
    b.push_back(0x44);                                  // C: SND-NR
    le(b, mfrStringToCode(m.mfr), 2);
    bcd(b, m.id, 4);
    b.push_back(m.ver); b.push_back(m.type);
    b.push_back(0x7A);                                  // CI: short header
    b.push_back((uint8_t)n);                            // access number
    b.push_back(0x00);                                  // status
    const bool enc = m.enc && encrypted;
    b.push_back(0x00); b.push_back(enc ? 0x05 : 0x00);  // configuration word: encryption mode 5 in bits 8..12
    if (enc) {
        uint32_t x = m.id * 2654435761u + n;
        for (int i = 0; i < 32; i++) { x = x * 1664525u + 1013904223u; b.push_back((uint8_t)(x >> 24)); }
        return buildFrame(b, false);
    }
    switch (m.type) {
    case 0x07: case 0x16:   // water: volume (litres, 8 digits), flow, due date, volume at the due date
        b.push_back(0x0C); b.push_back(0x13); bcd(b, 123456 + 3ull * n, 4);
        b.push_back(0x4C); b.push_back(0x13); bcd(b, 120000, 4);
        b.push_back(0x42); b.push_back(0x6C); le(b, 0x3A21, 2);   // 2026-... due date (type G)
        b.push_back(0x02); b.push_back(0x3B); le(b, 40 + n % 17, 2);
        b.push_back(0x01); b.push_back(0xFD); b.push_back(0x17); b.push_back(0x00);   // error flags
        if (m.phy == Phy::C1B) {   // pad with monthly values so the frame has two blocks
            for (int k = 1; k <= 24; k++) { b.push_back(0xCC); b.push_back((uint8_t)(k & 15)); b.push_back(0x13); bcd(b, 100000 + 100ull * k, 4); }
        }
        break;
    case 0x04:              // heat: energy kWh, volume, power, flow / return temperature
        b.push_back(0x0C); b.push_back(0x06); bcd(b, 8123456 + n, 4);
        b.push_back(0x0C); b.push_back(0x13); bcd(b, 450000 + 7ull * n, 4);
        b.push_back(0x0B); b.push_back(0x2D); bcd(b, 342 + n % 9, 3);
        b.push_back(0x0A); b.push_back(0x5A); bcd(b, 652 + n % 5, 2);
        b.push_back(0x0A); b.push_back(0x5E); bcd(b, 412, 2);
        break;
    case 0x03:              // gas: volume in 0.1 m3
        b.push_back(0x0C); b.push_back(0x15); bcd(b, 2345678 + n, 4);
        b.push_back(0x02); b.push_back(0x6C); le(b, 0x3A21, 2);
        break;
    case 0x08:              // heat cost allocator: units, date
        b.push_back(0x0C); b.push_back(0x6E); bcd(b, 1723 + n, 4);
        b.push_back(0x42); b.push_back(0x6C); le(b, 0x3A21, 2);
        break;
    default:
        b.push_back(0x04); b.push_back(0x03); le(b, 1000 + n, 4);
        break;
    }
    return buildFrame(b, m.phy == Phy::C1B);
}

struct WmbusGenerator::Impl {
    WmbusGenConfig c;
    genutil::NoiseSource noise;
    float sigma = 0;
    int64_t n = 0;
    struct Burst {
        std::vector<uint8_t> chips; double chipRate, dev, offHz, ph = 0, f1 = 0, f2 = 0; int64_t start, end;
        double amp;
    };
    std::vector<Burst> active;
    int64_t nextAt = 0;
    int next = 0;
    uint32_t counts[16]{};
    uint32_t rng;
    explicit Impl(const WmbusGenConfig& cfg) : c(cfg), noise(cfg.seed), rng(cfg.seed * 2654435761u + 7) {
        if (c.snrDb < 99) {
            const double n250 = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);
            sigma = (float)std::sqrt(n250 * c.rate / 250e3 / 2);
        }
        nextAt = (int64_t)(0.1 * c.rate);
    }
    void schedule() {
        for (int tries = 0; tries < 16; tries++) {
            const int mi = next++ % meterCount();
            Phy ph;
            auto air = WmbusGenerator::meterFrame(mi, counts[mi], ph, c.encryptedMeter);
            const bool is169 = ph == Phy::N1 || ph == Phy::N24;
            if (is169 != (c.plan == 1)) continue;
            if (c.onlyPhy >= 0 && (int)ph != c.onlyPhy) continue;
            counts[mi]++;
            const PhySpec& sp = phySpec(ph);
            double freq = 0;
            switch (ph) {
            case Phy::S1: freq = 868.3e6; break;
            case Phy::T1: case Phy::C1A: case Phy::C1B: freq = 868.95e6; break;
            case Phy::R2: freq = 868.09e6; break;
            default: freq = 169.43125e6; break;
            }
            Burst b;
            b.chips = encodeChips(ph, air, 20, kMeters[mi % meterCount()].wize ? kSyncNFormatB : 0);
            b.chipRate = sp.chipRate; b.dev = sp.devHz; b.offHz = freq - c.centreHz + c.cfoHz;
            b.start = nextAt; b.end = nextAt + (int64_t)((double)b.chips.size() / sp.chipRate * c.rate) + (int64_t)(4 * c.rate / sp.chipRate);
            b.amp = c.amp;
            active.push_back(std::move(b));
            nextAt = active.back().end + (int64_t)(c.gapSec * c.rate);
            return;
        }
    }
};

WmbusGenerator::WmbusGenerator(const WmbusGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
WmbusGenerator::~WmbusGenerator() = default;

void WmbusGenerator::generate(cf32* out, size_t cnt) {
    Impl& m = *p_;
    for (size_t k = 0; k < cnt; k++) {
        const int64_t i = m.n + (int64_t)k;
        while (!m.c.silent && m.nextAt <= i + (int64_t)m.c.rate && m.active.size() < 4) m.schedule();
        double re = 0, im = 0;
        for (auto it = m.active.begin(); it != m.active.end();) {
            auto& b = *it;
            if (i >= b.end) { it = m.active.erase(it); continue; }
            if (i >= b.start) {
                const int64_t ci = (int64_t)((double)(i - b.start) * b.chipRate / m.c.rate);
                const double target = ci < (int64_t)b.chips.size() ? (b.chips[(size_t)ci] ? 1.0 : -1.0) : 0.0;
                // Gaussian-like pulse shaping: two one-pole filters of about 0.4 chip each
                const double a = 1.0 - std::exp(-b.chipRate / m.c.rate / 0.32);
                b.f1 += a * (target - b.f1);
                b.f2 += a * (b.f1 - b.f2);
                b.ph += 2 * kPi * (b.offHz + b.dev * b.f2) / m.c.rate;
                if (b.ph > kPi) b.ph -= 2 * kPi; else if (b.ph < -kPi) b.ph += 2 * kPi;
                re += b.amp * std::cos(b.ph); im += b.amp * std::sin(b.ph);
            }
            ++it;
        }
        out[k] = cf32((float)re, (float)im);
    }
    m.n += (int64_t)cnt;
    if (m.sigma > 0) m.noise.add(out, cnt, m.sigma);
}

namespace {
class WmbusSynth : public ModeSynth {
public:
    WmbusSynth(const SynthConfig& s, double rate) : rate_(rate) {
        WmbusGenConfig c;
        c.rate = rate;
        c.plan = s.modeOpt[0] == 1 ? 1 : 0;
        c.centreHz = c.plan ? 169.4375e6 : wmbusTuning().defMhz * 1e6;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        gen_ = std::make_unique<WmbusGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<WmbusGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeWmbusSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<WmbusSynth>(cfg, sampleRate);
}

} // namespace dect2
