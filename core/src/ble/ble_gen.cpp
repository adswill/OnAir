// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Bluetooth LE advertising test signal (see ble_gen.h).
#include "dect2/ble/ble_gen.h"
#include "dect2/ble/ble_proto.h"
#include "dect2/ble/ble_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/dsp/fast_sincos.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;

} // namespace

namespace { constexpr int kPulseSpan = 6, kPulseRes = 512; }   // the pulse table: +-3 bits at 512 points per bit

struct BleGenerator::Impl {
    BleGenConfig cfg;
    genutil::NoiseSource noise;
    float sigma = 0;
    std::vector<cf32> q;   // samples made and not yet handed out, from qPos on (a deque per sample was a large share of the time)
    size_t qPos = 0;
    size_t idx = 0;
    bool done = false;
    double ph = 0;

    explicit Impl(const BleGenConfig& c) : cfg(c), noise(c.seed) {}

    void silence(double sec) { const size_t n = (size_t)std::llround(sec * cfg.rate); q.insert(q.end(), n, cf32(0.f, 0.f)); }

    void refill() {
        if (qPos > 0) { q.erase(q.begin(), q.begin() + (ptrdiff_t)qPos); qPos = 0; }
        if (done || cfg.pkts.empty()) { silence(0.001); return; }
        const BleGenPacket& p = cfg.pkts[idx];
        std::vector<uint8_t> bits;
        ble::packetBits(p.pdu, cfg.channel ? cfg.channel : p.channel, bits);
        if (p.badCrc) bits.back() ^= 1;
        const int nb = (int)bits.size();
        const double pad = 3;                      // bits of plain carrier before and after
        const double bitsPerSample = 1e6 * (1 + cfg.clockPpm * 1e-6) / cfg.rate;
        const size_t ns = (size_t)std::ceil((nb + 2 * pad) / bitsPerSample);
        const double off = cfg.dialOffsetHz + cfg.cfoHz + p.cfoHz;
        const double amp = cfg.amp * p.ampScale;
        // the Gaussian pulse from a table (10 erf() per sample were most of the time), and a 2 us raised-cosine ramp up and down inside the
        // plain-carrier padding, so that the burst edges do not splatter (the data bits are untouched)
        static const std::vector<double> pulse = [] {
            std::vector<double> t(kPulseSpan * kPulseRes + 2);
            for (size_t j = 0; j < t.size(); j++) t[j] = genutil::gfskPulse((double)j / kPulseRes - kPulseSpan / 2.0, 0.5);
            return t;
        }();
        auto g = [&](double tau) {   // tau in bits from the middle of the bit, |tau| < kPulseSpan / 2
            const double x = (tau + kPulseSpan / 2.0) * kPulseRes;
            const size_t j = (size_t)x;
            if (j + 1 >= pulse.size()) return 0.0;
            return pulse[j] + (pulse[j + 1] - pulse[j]) * (x - (double)j);
        };
        const double rampBits = 2;
        const FastSinCos& st = FastSinCos::get();   // table sin / cos (below -130 dB): std::cos and std::sin in double were the rest of the cost
        q.reserve(q.size() + ns);
        for (size_t i = 0; i < ns; i++) {
            const double u = i * bitsPerSample - pad;   // bit time from the first bit's start
            double f = 0;
            const int k0 = (int)std::floor(u);
            for (int k = k0 - 2; k <= k0 + 2; k++) {
                if (k < 0 || k >= nb) continue;
                f += (bits[(size_t)k] ? 1.0 : -1.0) * g(u - k - 0.5);
            }
            ph += 2 * kPi * (f * ble::kDevHz + off) / cfg.rate;
            while (ph > kPi) ph -= 2 * kPi;
            while (ph < -kPi) ph += 2 * kPi;
            const double edge = std::min(u + pad, nb + pad - u) / rampBits;   // bits from the burst's nearer end, in ramp lengths
            const double a = edge >= 1 ? amp : edge <= 0 ? 0.0 : amp * (0.5 - 0.5 * std::cos(kPi * edge));
            q.push_back(st.polar((float)a, ph));
        }
        silence(p.gapSec);
        if (++idx >= cfg.pkts.size()) { idx = 0; if (!cfg.loop) done = true; }
    }
};

double BleGenerator::noiseSigma(const BleGenConfig& c) {
    if (c.snrDb >= 99) return 0;
    const double nb = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // noise power in 1 MHz
    return std::sqrt(nb * c.rate / 1e6 / 2);
}

BleGenerator::BleGenerator(const BleGenConfig& c) : p_(std::make_unique<Impl>(c)) {
    p_->sigma = (float)noiseSigma(c);
    p_->silence(c.leadSec);
}
BleGenerator::~BleGenerator() = default;

void BleGenerator::generate(cf32* out, size_t n) {
    Impl& m = *p_;
    size_t i = 0;
    while (i < n) {
        if (m.qPos >= m.q.size()) { m.refill(); continue; }
        const size_t k = std::min(n - i, m.q.size() - m.qPos);
        std::copy(m.q.begin() + (ptrdiff_t)m.qPos, m.q.begin() + (ptrdiff_t)(m.qPos + k), out + i);
        m.qPos += k;
        i += k;
    }
    if (m.sigma > 0) m.noise.add(out, n, m.sigma);
}

std::vector<BleGenPacket> bleTestSchedule(int channel) {
    using namespace ble;
    std::vector<BleGenPacket> v;
    int rr = 0;
    auto add = [&](std::vector<uint8_t> pdu, double gap, double cfo = 0) {
        BleGenPacket p;
        p.pdu = std::move(pdu);
        p.channel = channel ? channel : 37 + (rr++ % 3);
        p.gapSec = gap;
        p.cfoHz = cfo;
        v.push_back(std::move(p));
    };
    const uint8_t phone[6] = {0x56, 0x34, 0x12, 0xEE, 0xFF, 0xC0};      // C0:FF:EE:12:34:56, random static
    const uint8_t beacon[6] = {0x01, 0x00, 0x5E, 0xA1, 0x0B, 0x00};     // public
    const uint8_t eddy[6] = {0x02, 0x22, 0x33, 0x44, 0x55, 0x3A};       // non-resolvable private
    const uint8_t rpa[6] = {0x9A, 0x78, 0x56, 0x34, 0x12, 0x5B};        // resolvable private
    const uint8_t scanner[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};    // resolvable private (0x66 >> 6 = 1)
    const uint8_t uuid[16] = {0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2, 0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0};
    const uint8_t ns[10] = {0xED, 0xD1, 0xEB, 0xEA, 0xC0, 0x4E, 0x5D, 0xEF, 0xA0, 0x17};
    const uint8_t inst[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    const uint8_t rpi[16] = {0x3C, 0x1A, 0x09, 0x77, 0x5E, 0x42, 0x81, 0x0D, 0xC6, 0x2B, 0x94, 0xF0, 0x6E, 0x13, 0xA5, 0x58};
    const uint8_t meta[4] = {0x40, 0x08, 0x00, 0x00};

    add(makePdu(AdvInd, true, advPayload(phone, AdBuilder().flags(0x06).name("OnAir").txPower(4).uuid16({0x180F, 0x180D}).mfr(0xFFFF, {0x01, 0x02, 0x03, 0x04}).b)), 0.003);
    add(makePdu(AdvNonconnInd, false, advPayload(beacon, AdBuilder().flags(0x04).ibeacon(uuid, 100, 7, -59).b)), 0.004, 20e3);
    add(makePdu(ScanReq, true, [&] { std::vector<uint8_t> p(scanner, scanner + 6); p.insert(p.end(), phone, phone + 6); return p; }(), true), 0.0002, -15e3);
    add(makePdu(ScanRsp, true, advPayload(phone, AdBuilder().name("OnAir test phone").b)), 0.003);
    add(makePdu(AdvNonconnInd, true, advPayload(eddy, AdBuilder().flags(0x06).eddystoneUrl(-20, 3, "example\x07").b)), 0.003, -30e3);
    add(makePdu(AdvNonconnInd, true, advPayload(eddy, AdBuilder().eddystoneTlm(3000, 21.5, 1234, 56789).b)), 0.003, -30e3);
    add(makePdu(AdvNonconnInd, true, advPayload(eddy, AdBuilder().eddystoneUid(-20, ns, inst).b)), 0.003, -30e3);
    add(makePdu(AdvNonconnInd, true, advPayload(rpa, AdBuilder().exposure(rpi, meta).b)), 0.004, 10e3);
    add(makePdu(AdvScanInd, true, advPayload(rpa, AdBuilder().flags(0x1A).mfr(0x004C, {0x10, 0x05, 0x01, 0x18, 0x44, 0x22, 0x11}).b)), 0.003, 10e3);
    {   // CONNECT_IND: InitA, AdvA, LLData (access address, CRC init, window, interval, latency, timeout, channel map, hop)
        std::vector<uint8_t> p(scanner, scanner + 6);
        p.insert(p.end(), phone, phone + 6);
        const uint8_t ll[22] = {0x71, 0x2A, 0x3B, 0x50, 0x12, 0x34, 0x56, 0x02, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x48, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F, 0x05};
        p.insert(p.end(), ll, ll + 22);
        add(makePdu(ConnectInd, true, p, true), 0.004, -15e3);
    }
    {   // ADV_EXT_IND: ADI and an AuxPtr to a secondary channel (not followed)
        const uint16_t adi = (uint16_t)(2 << 12 | 0x123), aux = (uint16_t)(100 | 1 << 13);
        std::vector<uint8_t> p = {6, 0x18, (uint8_t)adi, (uint8_t)(adi >> 8), 12, (uint8_t)aux, (uint8_t)(aux >> 8)};
        add(makePdu(AdvExtInd, false, p), 0.005);
    }
    return v;
}

namespace {
class BleSynth : public ModeSynth {
public:
    BleSynth(const SynthConfig& s, double rate) : rate_(rate) {
        BleGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -bleTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        const int o = s.modeOpt[0];
        c.pkts = bleTestSchedule(o >= 1 && o <= 3 ? 36 + o : 0);
        gen_ = std::make_unique<BleGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<BleGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeBleSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<BleSynth>(cfg, sampleRate);
}

} // namespace dect2
