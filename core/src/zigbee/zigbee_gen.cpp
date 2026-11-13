// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Zigbee / IEEE 802.15.4 test signal (see zigbee_gen.h).
#include "dect2/zigbee/zigbee_gen.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include "dect2/zigbee/zigbee_proto.h"
#include "dect2/zigbee/zigbee_rx.h"
#include <cmath>
#include <deque>
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr double kChipRate = 2e6;

// the baseband of chips at fractional chip time u (chip n occupies [n, n + 2]: a half-sine pulse, even chips on I, odd on Q)
inline cf32 oqpsk(const std::vector<uint8_t>& chips, double u) {
    const int n1 = (int)std::floor(u);
    float re = 0, im = 0;
    for (int n = n1 - 1; n <= n1; n++) {
        if (n < 0 || n >= (int)chips.size()) continue;
        const double x = u - n;
        if (x < 0 || x > 2) continue;
        const float a = (chips[(size_t)n] ? 1.f : -1.f) * (float)std::sin(kPi * x / 2);
        if (n & 1) im += a; else re += a;
    }
    return cf32(re, im);
}
} // namespace

std::vector<uint8_t> zigbeeChips(const std::vector<uint8_t>& psdu, int preambleSyms) {
    std::vector<uint8_t> syms;
    for (int i = 0; i < preambleSyms; i++) syms.push_back(0);
    syms.push_back(0x7); syms.push_back(0xA);   // SFD 0xA7, low nibble first
    const size_t len = psdu.size() > 127 ? 127 : psdu.size();
    syms.push_back((uint8_t)(len & 15)); syms.push_back((uint8_t)(len >> 4 & 7));
    for (size_t i = 0; i < len; i++) { syms.push_back(psdu[i] & 15); syms.push_back(psdu[i] >> 4); }
    std::vector<uint8_t> chips;
    chips.reserve(syms.size() * 32);
    for (uint8_t s : syms) for (int c = 0; c < 32; c++) chips.push_back((uint8_t)(zigbee::kChipTable[s] >> (31 - c) & 1));
    return chips;
}

struct ZigbeeGenerator::Impl {
    ZigbeeGenConfig cfg;
    genutil::NoiseSource noise;
    std::mt19937 rng;
    float sigma = 0;
    std::deque<cf32> q;
    size_t idx = 0;
    bool done = false;
    double ph = 0;
    // the interferer: a continuous stream of random chips
    std::vector<uint8_t> ichips;
    double iu = 0, iph = 0;

    explicit Impl(const ZigbeeGenConfig& c) : cfg(c), noise(c.seed), rng(c.seed * 7919u + 13u) {}

    void silence(double sec) { const size_t n = (size_t)std::llround(sec * cfg.rate); for (size_t i = 0; i < n; i++) q.push_back(cf32(0.f, 0.f)); }

    void refill() {
        if (done || cfg.frames.empty()) { silence(0.001); return; }
        const ZigbeeGenFrame& f = cfg.frames[idx];
        std::vector<uint8_t> chips = zigbeeChips(f.psdu, f.preambleSyms);
        if (f.badFcs) {   // the last symbol sent as another one
            const size_t last = chips.size() - 32;
            const int sym = f.psdu.back() >> 4 & 15, bad = sym ^ 3;
            for (int c = 0; c < 32; c++) chips[last + (size_t)c] = (uint8_t)(zigbee::kChipTable[bad] >> (31 - c) & 1);
        }
        const double chipsPerSample = kChipRate * (1 + cfg.clockPpm * 1e-6) / cfg.rate;
        const double frac = (double)(rng() & 0xFFFF) / 65536.0;     // the start between samples
        const double padChips = 3;
        const size_t ns = (size_t)std::ceil(((double)chips.size() + 2 + 2 * padChips) / chipsPerSample);
        const double off = cfg.dialOffsetHz + cfg.cfoHz + f.cfoHz;
        const double amp = cfg.amp * f.ampScale;
        for (size_t i = 0; i < ns; i++) {
            const double u = ((double)i - frac) * chipsPerSample - padChips;
            const cf32 s = oqpsk(chips, u);
            ph = std::remainder(ph + 2 * kPi * off / cfg.rate, 2 * kPi);
            q.push_back((float)amp * s * cf32((float)std::cos(ph), (float)std::sin(ph)));
        }
        silence(f.gapSec);
        if (++idx >= cfg.frames.size()) { idx = 0; if (!cfg.loop) done = true; }
    }

    void addInterferer(cf32* out, size_t n) {
        const double chipsPerSample = kChipRate / cfg.rate;
        const double a = cfg.amp * std::pow(10.0, cfg.interfererDb / 20);
        for (size_t i = 0; i < n; i++) {
            const int need = (int)std::floor(iu) + 2;
            while ((int)ichips.size() < need + 4) ichips.push_back((uint8_t)(rng() >> 7 & 1));
            if (ichips.size() > 4096) {   // keep the list short (an even count: I and Q stay where they are)
                const int drop = ((int)std::floor(iu) - 8) & ~1;
                if (drop > 0) { ichips.erase(ichips.begin(), ichips.begin() + drop); iu -= drop; }
            }
            const cf32 s = oqpsk(ichips, iu);
            iu += chipsPerSample;
            iph = std::remainder(iph + 2 * kPi * (cfg.interfererHz + cfg.dialOffsetHz) / cfg.rate, 2 * kPi);
            out[i] += (float)a * s * cf32((float)std::cos(iph), (float)std::sin(iph));
        }
    }
};

double ZigbeeGenerator::noiseSigma(const ZigbeeGenConfig& c) {
    if (c.snrDb >= 99) return 0;
    const double nb = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // noise power in 2 MHz
    return std::sqrt(nb * c.rate / 2e6 / 2);
}

ZigbeeGenerator::ZigbeeGenerator(const ZigbeeGenConfig& c) : p_(std::make_unique<Impl>(c)) {
    p_->sigma = (float)noiseSigma(c);
    p_->silence(c.leadSec);
}
ZigbeeGenerator::~ZigbeeGenerator() = default;

void ZigbeeGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    for (size_t i = 0; i < n; i++) {
        if (m.q.empty()) m.refill();
        out[i] = m.q.front();
        m.q.pop_front();
    }
    if (m.cfg.interfererDb > -100) m.addInterferer(out, n);
    if (m.sigma > 0) m.noise.add(out, n, m.sigma);
}

std::vector<ZigbeeGenFrame> zigbeeTestSchedule() {
    using namespace zigbee;
    std::vector<ZigbeeGenFrame> v;
    auto add = [&](std::vector<uint8_t> psdu, double gap, double cfo = 0) {
        ZigbeeGenFrame f;
        f.psdu = std::move(psdu);
        f.gapSec = gap;
        f.cfoHz = cfo;
        v.push_back(std::move(f));
    };
    const uint64_t coordExt = 0x00124B0001020304ull, hueExt = 0x001788010A0B0C0Dull, lumiExt = 0x00158DFFFE123456ull, ikeaExt = 0x14B457FFFE654321ull;
    const uint64_t extPan = 0xDDDDDDDDDDDD1A62ull;
    uint8_t seq = 0x40;
    // a device scans: beacon request (command 7) to the broadcast address
    {
        MacBuild b; b.type = Command; b.destMode = 2; b.srcMode = 0; b.destPan = 0xFFFF; b.destShort = 0xFFFF; b.seq = seq++; b.payload = {7};
        add(buildMac(b), 0.002, 8e3);
    }
    // the coordinator answers with its Zigbee beacon
    {
        MacBuild b; b.type = Beacon; b.destMode = 0; b.srcMode = 2; b.srcPan = 0x1A62; b.srcShort = 0x0000; b.seq = seq++;
        b.payload = {0xFF, 0xCF, 0x00, 0x00};   // superframe: BO 15, SO 15, final CAP 15, PAN coordinator, association permit; no GTS, no pending
        const auto zp = zbBeaconPayload(2, 2, true, true, 0, extPan, 3);
        b.payload.insert(b.payload.end(), zp.begin(), zp.end());
        add(buildMac(b), 0.003, -12e3);
    }
    // association: request (extended source), response, data request
    {
        MacBuild b; b.type = Command; b.ackReq = true; b.destMode = 2; b.srcMode = 3; b.destPan = 0x1A62; b.destShort = 0x0000; b.srcPan = 0xFFFF; b.srcExt = hueExt; b.seq = seq++;
        b.payload = {1, 0x8E};   // alternate coordinator no, FFD, mains, rx on idle, allocate address
        add(buildMac(b), 0.002, 25e3);
    }
    {
        MacBuild b; b.type = Command; b.ackReq = true; b.destMode = 3; b.srcMode = 3; b.destPan = 0x1A62; b.destExt = hueExt; b.srcExt = coordExt; b.seq = seq++;
        b.payload = {2, 0x34, 0x12, 0x00};   // assigned address 0x1234, success
        add(buildMac(b), 0.002, -12e3);
    }
    {
        MacBuild b; b.type = Command; b.ackReq = true; b.destMode = 2; b.srcMode = 2; b.destPan = 0x1A62; b.destShort = 0x0000; b.srcShort = 0x5A5A; b.seq = seq++; b.payload = {4};
        add(buildMac(b), 0.002, 25e3);
    }
    {
        MacBuild b; b.type = Ack; b.destMode = 0; b.srcMode = 0; b.seq = (uint8_t)(seq - 1);
        add(buildMac(b), 0.002, -12e3);
    }
    // NWK data in the clear: ZDO Device_annce from the Hue bulb (0x1234): cluster 0x0013, profile 0, endpoint 0
    {
        NwkBuild n; n.type = 0; n.dest = 0xFFFD; n.src = 0x1234; n.radius = 30; n.seq = 7;
        n.payload = {0x08, 0x00, 0x13, 0x00, 0x00, 0x00, 0x00, 0x09,       // APS: broadcast data, endpoint 0, cluster 0x0013, profile 0, source endpoint 0, counter 9
                     0x07, 0x34, 0x12, 0x0D, 0x0C, 0x0B, 0x0A, 0x01, 0x88, 0x17, 0x00, 0x8E};   // ZDO: sequence, short address, IEEE address, capability
        MacBuild b; b.type = Data; b.destPan = 0x1A62; b.destShort = 0xFFFF; b.srcShort = 0x1234; b.seq = seq++; b.payload = buildNwk(n);
        add(buildMac(b), 0.002, 25e3);
    }
    // a Zigbee link status command (clear) from the coordinator
    {
        NwkBuild n; n.type = 1; n.dest = 0xFFFC; n.src = 0x0000; n.radius = 1; n.seq = 21; n.route = false;
        n.payload = {0x08, 0xC0, 0x34, 0x12, 0x33, 0x00};   // link status, first and last frame, one entry
        MacBuild b; b.type = Data; b.destPan = 0x1A62; b.destShort = 0xFFFF; b.srcShort = 0x0000; b.seq = seq++; b.payload = buildNwk(n);
        add(buildMac(b), 0.002, -12e3);
    }
    // encrypted NWK payloads (the security bit set, an auxiliary header and ciphertext): a sensor reporting, a light being switched
    for (int k = 0; k < 3; k++) {
        NwkBuild n; n.type = 0; n.secured = true; n.dest = k == 1 ? 0x1234 : 0x0000; n.src = k == 1 ? 0x0000 : (k == 0 ? 0x7A12 : 0x1234); n.radius = 30; n.seq = 40 + k;
        n.srcExt = k == 0; n.srcExtAddr = lumiExt;
        std::vector<uint8_t> aux = {0x28, (uint8_t)(0x10 + k), 0x00, 0x00, 0x00};
        const uint64_t src = k == 0 ? lumiExt : coordExt;
        for (int i = 0; i < 8; i++) aux.push_back((uint8_t)(src >> (8 * i)));
        aux.push_back(0x00);
        n.payload = aux;
        for (int i = 0; i < 14 + 3 * k; i++) n.payload.push_back((uint8_t)(0x9D * i + 0x31 * k + 7));   // ciphertext and MIC
        MacBuild b; b.type = Data; b.ackReq = true; b.destPan = 0x1A62; b.destShort = n.dest; b.srcShort = n.src; b.seq = seq++; b.payload = buildNwk(n);
        add(buildMac(b), 0.002, k == 0 ? 40e3 : -30e3);
    }
    // a second network: a device beaconing on its own PAN, and a data frame with extended addresses
    {
        MacBuild b; b.type = Beacon; b.destMode = 0; b.srcMode = 2; b.srcPan = 0x3C2D; b.srcShort = 0x0000; b.seq = seq++;
        b.payload = {0xFF, 0x4F, 0x00, 0x00};   // PAN coordinator, join closed
        const auto zp = zbBeaconPayload(2, 2, false, true, 0, 0xA1B2C3D4E5F60718ull, 0);
        b.payload.insert(b.payload.end(), zp.begin(), zp.end());
        add(buildMac(b), 0.003, 15e3);
    }
    {
        MacBuild b; b.type = Data; b.destMode = 3; b.srcMode = 3; b.destPan = 0x3C2D; b.destExt = coordExt; b.srcExt = ikeaExt; b.seq = seq++;
        b.payload = {0xDE, 0xAD, 0xBE, 0xEF};
        add(buildMac(b), 0.003, 15e3);
    }
    return v;
}

namespace {
class ZigbeeSynth : public ModeSynth {
public:
    ZigbeeSynth(const SynthConfig& s, double rate) : rate_(rate) {
        ZigbeeGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -zigbeeTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.frames = zigbeeTestSchedule();
        gen_ = std::make_unique<ZigbeeGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<ZigbeeGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeZigbeeSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<ZigbeeSynth>(cfg, sampleRate);
}

} // namespace dect2
