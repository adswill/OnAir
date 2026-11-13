// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-SUN FAN test signal (see wisun_gen.h).
#include "dect2/wisun/wisun_gen.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/wisun/wisun_proto.h"
#include "dect2/wisun/wisun_rx.h"
#include <cmath>
#include "dect2/dsp/fast_sincos.h"
#include <random>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
constexpr int kPulseRes = 64;     // pulse table points per bit
constexpr int kPulseSpan = 3;     // bits on each side the Gaussian pulse reaches

// The frequency pulse of one bit of 2-GFSK (BT 0.5): a rectangle of one bit through the Gaussian filter, from -kPulseSpan to +kPulseSpan bits
struct Pulse {
    float v[2 * kPulseSpan * kPulseRes + 1];
    Pulse() {
        const double bt = 0.5;
        const double k = 2 * kPi * bt / std::sqrt(std::log(2.0)) / std::sqrt(2.0);   // the Gaussian's scale, per bit, inside erf
        for (int i = 0; i <= 2 * kPulseSpan * kPulseRes; i++) {
            const double t = (double)i / kPulseRes - kPulseSpan;   // bits from the bit's centre
            v[i] = (float)(0.5 * (std::erf(k * (t + 0.5)) - std::erf(k * (t - 0.5))));
        }
    }
};
}

std::vector<uint8_t> wisunPpduBits(const WisunGenFrame& f) {
    std::vector<uint8_t> psdu = f.mpdu;
    wisun::addFcs(psdu, f.fcs2);
    if (f.badFcs) psdu.back() ^= 0x5A;
    wisun::Phr phr;
    phr.fcs2 = f.fcs2; phr.whitened = f.whiten; phr.length = (int)psdu.size();
    if (f.whiten) wisun::whiten(psdu.data(), psdu.size());
    std::vector<uint8_t> bits;
    for (int i = 0; i < f.preambleOctets * 8; i++) bits.push_back((uint8_t)(i & 1));   // 0101...: 0x55 sent least significant bit first
    for (int i = 15; i >= 0; i--) bits.push_back((uint8_t)(wisun::kSfdUncoded >> i & 1));
    const uint16_t p = (uint16_t)(wisun::buildPhr(phr) | (f.msError ? 0x8000 : 0));
    for (int i = 15; i >= 0; i--) bits.push_back((uint8_t)(p >> i & 1));
    for (uint8_t o : psdu) for (int j = 0; j < 8; j++) bits.push_back((uint8_t)(o >> j & 1));
    return bits;
}

struct WisunGenerator::Impl {
    WisunGenConfig cfg;
    genutil::NoiseSource noise;
    float sigma = 0;
    std::vector<cf32> q;   // the samples of the current frame / gap; qpos is the next one out (a vector, not a deque: no per-sample pop)
    size_t qpos = 0;
    size_t idx = 0;
    bool done = false;
    double ph = 0;
    Pulse pulse;

    explicit Impl(const WisunGenConfig& c) : cfg(c), noise(c.seed) {
        if (cfg.snrDb < 99) {
            const double p = cfg.amp * cfg.amp / std::pow(10.0, cfg.snrDb / 10) * cfg.rate / 400e3;   // the noise over the whole rate
            sigma = (float)std::sqrt(p / 2);
        }
        silence(cfg.leadSec);
    }

    void silence(double sec) { const size_t n = (size_t)std::llround(sec * cfg.rate); q.insert(q.end(), n, cf32(0.f, 0.f)); }

    void frame(const WisunGenFrame& f) {
        const std::vector<uint8_t> bits = wisunPpduBits(f);
        const double br = f.bitRate * (1 + cfg.clockPpm * 1e-6);
        const double dev = f.modIndex * f.bitRate / 2;
        const double off = (f.mhz - cfg.centerMhz) * 1e6 + cfg.cfoHz + f.cfoHz;
        const size_t n = (size_t)std::ceil((bits.size() + 1) / br * cfg.rate);
        const float a = (float)(cfg.amp * f.ampScale);
        for (size_t s = 0; s < n; s++) {
            const double b = (double)s / cfg.rate * br - 0.5;   // bits from the first bit's centre
            const int c = (int)std::floor(b + 0.5);
            double fr = 0;
            for (int j = c - kPulseSpan; j <= c + kPulseSpan; j++) {
                if (j < 0 || j >= (int)bits.size()) continue;
                const double t = b - j;
                const int k = (int)std::lround((t + kPulseSpan) * kPulseRes);
                if (k < 0 || k > 2 * kPulseSpan * kPulseRes) continue;
                fr += (bits[(size_t)j] ? 1.0 : -1.0) * pulse.v[k];
            }
            // ramp the carrier in and out over a bit
            const double edge = std::min(1.0, std::min((double)s / cfg.rate * br, (double)(n - s) / cfg.rate * br));
            ph += 2 * kPi * (off + dev * fr) / cfg.rate;
            if (ph > kPi) ph -= 2 * kPi * std::floor((ph + kPi) / (2 * kPi));
            q.push_back(FastSinCos::get().polar((float)(a * edge), ph));   // a table, not cos and sin in double per sample
        }
    }

    void refill() {
        if (done) { silence(0.01); return; }
        if (idx >= cfg.frames.size()) {
            if (!cfg.loop || cfg.frames.empty()) { done = true; silence(0.01); return; }
            idx = 0;
        }
        const WisunGenFrame& f = cfg.frames[idx++];
        frame(f);
        silence(f.gapSec);
    }

    void generate(cf32* out, size_t n) {
        for (size_t i = 0; i < n; i++) {
            while (qpos >= q.size()) { q.clear(); qpos = 0; refill(); }
            out[i] = q[qpos++];
        }
        if (sigma > 0) noise.add(out, n, sigma);
    }
};

WisunGenerator::WisunGenerator(const WisunGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
WisunGenerator::~WisunGenerator() = default;
void WisunGenerator::generate(cf32* out, size_t n) { p_->generate(out, n); }

std::vector<WisunGenFrame> wisunTestSchedule(double centerMhz, double spanMhz) {
    using namespace wisun;
    std::vector<WisunGenFrame> v;
    const ChannelPlan p2 = plansOf("ANZ")[1], p1 = plansOf("ANZ")[0];
    // the class 2 channels (400 kHz, 150 kbit/s) inside the span, visited in a scrambled order
    std::vector<double> chs;
    for (int c = 0; c < p2.channels; c++) {
        const double f = channelMhz(p2, c);
        if (std::fabs(f - centerMhz) <= spanMhz) chs.push_back(f);
    }
    if (chs.empty()) chs.push_back(centerMhz);
    size_t hop = 0;
    auto next = [&]() { hop = (hop * 7 + 3) % chs.size(); return chs[hop]; };
    const uint64_t br = 0x00124BFFFE1A2B3Cull;                         // a border router (Texas Instruments OUI)
    const uint64_t router = 0x000D6FFFFE445566ull;                     // a router (Silicon Labs OUI)
    const uint64_t meter = 0x02A1B2FFFEC3D4E5ull;                      // a meter joining (locally administered address)
    const uint16_t pan = 0x7A31;
    auto add = [&](const Build& b, int rate = 150000, double gap = 0.004, double amp = 1, double cfo = 0) {
        WisunGenFrame f;
        f.mpdu = buildMpdu(b); f.bitRate = rate; f.modIndex = rate == 50000 ? 1.0 : 0.5; f.gapSec = gap; f.ampScale = amp; f.cfoHz = cfo;
        f.mhz = rate == 50000 ? channelMhz(p1, channelOf(p1, next()) + 1) : next();
        v.push_back(f);
    };
    Schedule us;
    us.present = true; us.dwellMs = 255; us.chPlan = 0; us.regDomain = 0x08; us.opClass = 2; us.chFunction = 2;
    Schedule bs = us;
    bs.bcastIntervalMs = 1020; bs.bsi = 0x3995; bs.dwellMs = 255;
    uint32_t fcBr = 417967, fcRouter = 736547, fcMeter = 10;
    // the border router's PAN advert (routing cost 0) and the router's (cost 128)
    {
        Build b; b.utt = PA; b.src = br; b.srcPanPresent = true; b.pan = pan; b.ufsi = 0x1234;
        b.us = us; b.panIe = true; b.panSize = 3; b.routingCost = 0; b.tpsVersion = 2; b.netName = "OnAir-Test-FAN"; b.phyModes = {0x03, 0x05};
        add(b, 150000, 0.004, 1.0, 3e3);
    }
    {
        Build b; b.utt = PA; b.src = router; b.srcPanPresent = true; b.pan = pan; b.ufsi = 0x4321;
        b.us = us; b.panIe = true; b.panSize = 3; b.routingCost = 128; b.tpsVersion = 2; b.netName = "OnAir-Test-FAN";
        add(b, 150000, 0.004, 0.6, -5e3);
    }
    // the meter: a PAN advert solicit (with its own schedule), then the EAPOL exchange through the router (authenticator: the border router)
    {
        Build b; b.utt = PAS; b.src = meter; b.srcPanPresent = true; b.pan = 0xFFFF; b.us = us; b.netName = "OnAir-Test-FAN";
        add(b, 150000, 0.004, 0.4, 8e3);
    }
    {
        Build b; b.utt = Eapol; b.src = meter; b.dst = router; b.seq = 1; b.ea = br; b.mpxId = 0x0001; b.kmpId = 1;
        b.payload = {0x03, 0x00, 0x00, 0x05, 0x02, 0x01, 0x00, 0x05, 0x01};   // EAP response, identity
        add(b, 150000, 0.004, 0.4, 8e3);
    }
    {
        Build b; b.utt = Eapol; b.src = router; b.dst = meter; b.seq = 2; b.ea = br; b.mpxId = 0x0001; b.kmpId = 1;
        b.payload = {0x03, 0x00, 0x00, 0x06, 0x01, 0x02, 0x00, 0x06, 0x0D, 0x20};   // EAP request, TLS start
        add(b, 150000, 0.004, 0.6, -5e3);
    }
    // a PAN configuration solicit and the PAN configurations (encrypted)
    {
        Build b; b.utt = PCS; b.src = meter; b.srcPanPresent = true; b.pan = pan;
        add(b, 150000, 0.004, 0.4, 8e3);
    }
    for (int k = 0; k < 2; k++) {
        Build b; b.utt = PC; b.secured = true; b.frameCounter = k ? fcRouter++ : fcBr++; b.src = k ? router : br; b.srcPanPresent = true; b.pan = pan;
        b.bt = true; b.bsn = 0x3995 + k; b.bio = 0xB5; b.lbc = true;
        for (int i = 0; i < 77; i++) b.payload.push_back((uint8_t)(0x6B * i + 0x11 * k + 3));
        add(b, 150000, 0.004, k ? 0.6 : 1.0, k ? -5e3 : 3e3);
    }
    // unicast data meter > router (encrypted) and the router's enhanced ack with the level it measured
    for (int k = 0; k < 3; k++) {
        Build b; b.utt = DataFrame; b.secured = true; b.frameCounter = fcMeter++; b.src = meter; b.dst = router; b.seq = 10 + k; b.ackReq = true; b.ufsi = 0x10000 * k;
        for (int i = 0; i < 40 + 10 * k; i++) b.payload.push_back((uint8_t)(0x3D * i + k));
        add(b, 150000, 0.002, 0.4, 8e3);
        Build a; a.utt = AckFrame; a.src = router; a.dst = meter; a.seq = 10 + k; a.rslDbm = -88 + k;
        add(a, 150000, 0.004, 0.6, -5e3);
    }
    // the border router's broadcast data (encrypted, broadcast timing)
    {
        Build b; b.utt = DataFrame; b.secured = true; b.frameCounter = fcBr++; b.src = br; b.seq = 0x55; b.bt = true; b.bsn = 0x3996; b.bio = 0x16;
        for (int i = 0; i < 90; i++) b.payload.push_back((uint8_t)(0x29 * i + 5));
        add(b, 150000, 0.004, 1.0, 3e3);
    }
    // a 50 kbit/s frame (modulation index 1) on the 200 kHz plan: the router's PAN config solicit answered
    {
        Build b; b.utt = PCS; b.src = router; b.srcPanPresent = true; b.pan = pan;
        add(b, 50000, 0.006, 0.6, -5e3);
    }
    return v;
}

namespace {
class WisunSynth : public ModeSynth {
public:
    WisunSynth(const SynthConfig& s, double rate) : rate_(rate) {
        WisunGenConfig c;
        c.rate = rate;
        c.centerMhz = wisunTuning().defMhz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.frames = wisunTestSchedule(c.centerMhz, std::min(6.0, 0.4 * rate / 1e6));
        gen_ = std::make_unique<WisunGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<WisunGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeWisunSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<WisunSynth>(cfg, sampleRate);
}

} // namespace dect2
