// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CubeSat telemetry test signal: see cubesat_gen.h.
#include "dect2/cubesat/cubesat_gen.h"
#include "dect2/cubesat/cubesat_rx.h"
#include "dect2/cubesat/cubesat_more.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/packet/packet_ax25.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include "dect2/common/mathconst.h"

namespace dect2 {

using namespace cubesat;

namespace {

using dect2::math::kPi;
constexpr double kWork = 96000.0;
constexpr double kLeadSec = 0.6;
constexpr double kChanBw = 25000.0;

ax25::Address addr(const std::string& s) {
    ax25::Address a;
    const size_t d = s.find('-');
    a.call = s.substr(0, d);
    a.ssid = d == std::string::npos ? 0 : atoi(s.c_str() + d + 1);
    return a;
}

std::vector<uint8_t> ax25Bytes(const std::string& from, const std::string& to, const std::string& info) {
    ax25::Frame f;
    f.from = addr(from); f.to = addr(to);
    f.info = info;
    return ax25::buildFrame(f);       // FCS included
}

std::vector<uint8_t> bytesOf(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<uint8_t> msbBits(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> b;
    for (uint8_t v : bytes) for (int k = 7; k >= 0; k--) b.push_back((uint8_t)((v >> k) & 1));
    return b;
}

struct Mod {
    std::vector<cf32>* out;
    double theta = 0;
    void silence(double sec) { out->insert(out->end(), (size_t)std::lround(sec * kWork), cf32(0.f, 0.f)); }
    void push(double devHz) {
        theta += 2 * kPi * devHz / kWork;
        if (theta > kPi) theta -= 2 * kPi;
        out->push_back(cf32((float)std::cos(theta), (float)std::sin(theta)));
    }
};

void sendAfsk(Mod& m, const std::vector<uint8_t>& levels) {
    double ph = 0;
    for (uint8_t lv : levels)
        for (int i = 0; i < 80; i++) {
            ph += 2 * kPi * (lv ? 1200.0 : 2200.0) / kWork;
            if (ph > 2 * kPi) ph -= 2 * kPi;
            m.push(2800.0 * std::sin(ph));
        }
}

// audio FSK on FM (GOMspace U482C radios): the tones afCarrier +- dev, continuous phase, at `baud`, 2.8 kHz FM deviation
void sendAudioFsk(Mod& m, const std::vector<uint8_t>& levels, int baud, double afCarrier, double dev) {
    double ph = 0;
    const double spb = kWork / baud;
    double acc = 0;
    for (uint8_t lv : levels) {
        acc += spb;
        const int n = (int)std::floor(acc);
        acc -= n;
        for (int i = 0; i < n; i++) {
            ph += 2 * kPi * (afCarrier + (lv ? dev : -dev)) / kWork;
            if (ph > 2 * kPi) ph -= 2 * kPi;
            m.push(2800.0 * std::sin(ph));
        }
    }
}

// levels -> +-1, Gaussian filter BT 0.5, frequency deviation devHz
void sendFsk(Mod& m, const std::vector<uint8_t>& levels, int baud, double devHz) {
    const int sps = (int)std::lround(kWork / baud);
    const double sigma = std::sqrt(std::log(2.0)) / (2 * kPi * 0.5 * baud) * kWork;
    const int half = (int)std::ceil(3 * sigma);
    std::vector<double> g((size_t)(2 * half + 1));
    double sum = 0;
    for (int i = -half; i <= half; i++) { g[(size_t)(i + half)] = std::exp(-0.5 * i * i / (sigma * sigma)); sum += g[(size_t)(i + half)]; }
    for (double& x : g) x /= sum;
    const long n = (long)levels.size() * sps;
    auto nrz = [&](long i) { i = std::max(0L, std::min(n - 1, i)); return levels[(size_t)(i / sps)] ? 1.0 : -1.0; };
    for (long i = 0; i < n; i++) {
        double acc = 0;
        for (int k = -half; k <= half; k++) acc += g[(size_t)(k + half)] * nrz(i + k);
        m.push(devHz * acc);
    }
}

struct Item { CubeGenFrame f; std::vector<uint8_t> air, bits; double afCarrier = 0; };   // air: the bytes to send (HDLC / ASM framing applied below); bits: code symbols sent as they are

std::vector<Item> items() {
    std::vector<Item> v;
    auto csp = [](int src, int dst, int dport, int sport, const std::string& p) {
        CspHeader h; h.prio = 2; h.src = src; h.dst = dst; h.dport = dport; h.sport = sport;
        return buildCsp(h, bytesOf(p));
    };
    {   // AX.25 AFSK 1200
        Item it; it.f.framing = Ax25Afsk1200; it.f.baud = 1200; it.f.from = "RS0ISS";
        it.air = ax25Bytes("RS0ISS", "CQ", ">CubeSat test: 1200 baud AFSK packet");
        it.f.data.assign(it.air.begin(), it.air.end() - 2);
        v.push_back(it);
    }
    {   // AX.25 G3RUH 9600
        Item it; it.f.framing = Ax25G3ruh; it.f.baud = 9600;
        it.air = ax25Bytes("TSTSAT-1", "BEACON", "TLM bat=7.92V temp=+12.5C mode=nominal uptime=123456");
        it.f.from = "TSTSAT-1";
        it.f.data.assign(it.air.begin(), it.air.end() - 2);
        v.push_back(it);
    }
    {   // AX100 Mode 5, GMSK 9600
        Item it; it.f.framing = Ax100Mode5; it.f.baud = 9600;
        it.f.data = csp(1, 10, 31, 0, "AX100 mode 5 beacon: obc ok, eps 8.1 V, 4 boots");
        it.air = ax100Mode5Encode(it.f.data);
        v.push_back(it);
    }
    {   // AX100 Mode 6, G3RUH 9600
        Item it; it.f.framing = Ax100Mode6; it.f.baud = 9600;
        it.f.data = csp(5, 10, 8, 1, "AX100 mode 6: RS inside HDLC, G3RUH scrambled");
        it.air = ax100Mode6Encode(it.f.data);
        v.push_back(it);
    }
    {   // CCSDS RS, GMSK 9600
        Item it; it.f.framing = CcsdsRs; it.f.baud = 9600;
        std::vector<uint8_t> d = bytesOf("CCSDS frame: virtual channel 0, housekeeping");
        d.resize(223, 0);
        it.f.data = d;
        it.air = ccsdsEncode(d, 255);
        v.push_back(it);
    }
    {   // AX100 Mode 5, GMSK 4800
        Item it; it.f.framing = Ax100Mode5; it.f.baud = 4800;
        it.f.data = csp(1, 10, 31, 0, "AX100 at 4800 baud");
        it.air = ax100Mode5Encode(it.f.data);
        v.push_back(it);
    }
    // 3.9 framings (cubesat_more.h)
    {   // GOMspace U482C: AFSK 4800 baud on a 3600 Hz audio carrier (as GOMX-1, AISAT, AU02 ...), Reed-Solomon + scrambler
        Item it; it.f.framing = U482c; it.f.baud = 4800; it.afCarrier = 3600;
        it.f.data = csp(3, 10, 31, 0, "U482C beacon: AFSK 4800 on 3600 Hz, RS + scrambler");
        it.air = u482cEncode(it.f.data, kU482cRs | kU482cScrambler);
        v.push_back(it);
    }
    {   // CCSDS concatenated, GMSK 9600: 223 byte frame, CCSDS convolutional code, dual basis RS
        Item it; it.f.framing = CcsdsConcat; it.f.baud = 9600;
        std::vector<uint8_t> d = bytesOf("CCSDS concatenated: Viterbi + Reed-Solomon, VC 1 housekeeping");
        d.resize(223, 0x5A);
        it.f.data = d;
        it.bits = ccsdsConcatEncode(d, ConcatParams());
        v.push_back(it);
    }
    {   // USP (SPUTNIX), FSK 9600: an AX.25 frame inside
        Item it; it.f.framing = Usp; it.f.baud = 9600;
        std::vector<uint8_t> ax = ax25Bytes("RS20S", "CQ", "USP test frame: PLS, CCSDS conv + RS, AX.25 inside");
        ax.resize(ax.size() - 2);                 // without its FCS (the RS code protects it)
        it.f.data = ax; it.f.from = "RS20S";
        it.bits = uspEncode(ax);
        v.push_back(it);
    }
    {   // NGHam, FSK 4800
        Item it; it.f.framing = Ngham; it.f.baud = 4800;
        it.f.data = bytesOf("NGHam test payload: size tag, RS(47,31), CRC-16");
        it.air = nghamEncode(it.f.data);
        v.push_back(it);
    }
    {   // Mobitex-NX (BEESAT / TECHNOSAT style), GMSK 4800: 4 blocks
        Item it; it.f.framing = Mobitex; it.f.baud = 4800; it.f.from = "DP0TST";
        MobitexFrame mf;
        mf.control[0] = 0x20; mf.control[1] = 0x01; mf.callsign = "DP0TST";
        for (int b = 0; b < 4; b++) { std::vector<uint8_t> d = bytesOf("Mobitex block " + std::to_string(b) + " data"); d.resize(18, (uint8_t)b); mf.blocks.push_back(d); }
        it.air = mobitexEncode(mf, true, MobitexDefault);
        MobitexFrame chk;
        if (mobitexDecode(it.air.data() + 2, it.air.size() - 2, MobitexDefault, chk)) it.f.data = chk.reframed;   // what the receiver reports
        v.push_back(it);
    }
    return v;
}

struct Cycle {
    std::vector<CubeGenFrame> frames;
    std::vector<cf32> bb;
};

const Cycle& cycleFor(int mode) {
    static Cycle cache[1 + kNumFramings];
    static bool built[1 + kNumFramings] = {};
    const int k = mode >= 1 && mode <= kNumFramings ? mode : 0;
    if (built[k]) return cache[k];
    Cycle& c = cache[k];
    Mod m;
    m.out = &c.bb;
    m.silence(kLeadSec);
    uint32_t scr = 0;
    for (const Item& it : items()) {
        if (k && it.f.framing != k - 1) continue;
        c.frames.push_back(it.f);
        switch (it.f.framing) {
        case Ax25Afsk1200:
            sendAfsk(m, ax25::nrziEncode(ax25::hdlcBits(it.air, 24, 4)));
            m.silence(0.25);
            break;
        case Ax25G3ruh:
        case Ax100Mode6:
            sendFsk(m, ax25::scramble(ax25::nrziEncode(ax25::hdlcBits(it.air, 60, 6)), scr), it.f.baud, 3000.0);
            m.silence(0.15);
            break;
        default: {                       // the sync word framings: a 0101 preamble, the frame, a few bytes of tail
            std::vector<uint8_t> bytes(48, 0x55);
            bytes.insert(bytes.end(), it.air.begin(), it.air.end());
            bytes.insert(bytes.end(), 4, 0x55);
            std::vector<uint8_t> lv = msbBits(bytes);
            if (!it.bits.empty()) {      // code symbols: preamble, symbols, tail
                lv = msbBits(std::vector<uint8_t>(48, 0x55));
                lv.insert(lv.end(), it.bits.begin(), it.bits.end());
                const std::vector<uint8_t> tail = msbBits(std::vector<uint8_t>(8, 0x55));
                lv.insert(lv.end(), tail.begin(), tail.end());
            }
            if (it.afCarrier > 0) { sendAudioFsk(m, lv, it.f.baud, it.afCarrier, 1200.0); m.silence(0.15); break; }
            sendFsk(m, lv, it.f.baud, it.f.baud / 4.0 * 1.25);
            m.silence(0.15);
            break;
        }
        }
    }
    built[k] = true;
    return c;
}

class CubesatSynth : public ModeSynth {
public:
    CubesatSynth(const SynthConfig& cfg, double rate) : rate_(rate), cyc_(cycleFor(cfg.modeOpt[0])), noise_(11) {
        const double pn = rate / kChanBw / std::pow(10.0, cfg.snrDb / 10.0);
        sigma_ = cfg.snrDb < -100 || cfg.snrDb >= 99 ? 0.f : (float)std::sqrt(pn / 2);   // >= 99: the transmitter's clean signal (TxSession), at 0.4 instead of full scale
        scale_ = sigma_ > 0.15f ? 0.15f / sigma_ : 1.f;
        if (sigma_ == 0.f) scale_ = 0.4f;
        const double w = 2 * kPi * (cfg.cfoHz - cubesatTuning().tuneOffsetHz) / rate;
        step_ = cf32((float)std::cos(w), (float)std::sin(w));
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        const std::vector<cf32>& bb = cyc_.bb;
        const double adv = kWork / rate_;
        const size_t len = bb.size();
        for (size_t i = 0; i < n; i++) {
            const size_t i0 = (size_t)pos_;
            const float fr = (float)(pos_ - (double)i0);
            const cf32 a = bb[i0 % len], b = bb[(i0 + 1) % len];
            out[i] = (a + (b - a) * fr) * rot_ * scale_;
            rot_ *= step_;
            pos_ += adv;
            if (pos_ >= (double)len) pos_ -= (double)len;
        }
        rot_ /= std::abs(rot_);
        if (sigma_ > 0.f) noise_.add(out, n, sigma_ * scale_);
    }
private:
    double rate_;
    const Cycle& cyc_;
    genutil::NoiseSource noise_;
    float sigma_ = 0, scale_ = 1;
    cf32 step_, rot_ = cf32(1.f, 0.f);
    double pos_ = 0;
};

} // namespace

std::vector<CubeGenFrame> cubesatGenCycle(int mode) { return cycleFor(mode).frames; }
double cubesatGenCycleSec(int mode) { return (double)cycleFor(mode).bb.size() / kWork; }

std::unique_ptr<ModeSynth> makeCubesatSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate < cubesatTuning().minSampleRate - 1) return nullptr;
    return std::make_unique<CubesatSynth>(cfg, sampleRate);
}

} // namespace dect2
