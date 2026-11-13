// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT+ test signal (see ant_gen.h).
#include "dect2/ant/ant_gen.h"
#include "dect2/ant/ant_rx.h"
#include "dect2/dsp/gen_util.h"
#include "dect2/source/source.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;


struct SensorState {
    AntGenSensor s;
    double next = 0;             // signal time of the next frame
    uint64_t count = 0;          // frames sent
    // heart rate strap
    double beatTime = 0;         // seconds of the 64 s cycle
    int beats = 0;
    double hr = 72, hrPhase = 0;
    double prevBeatTime = 0;
    // power meter
    int powerEvents = 0, accumPower = 0;
    // speed and cadence
    double wheelTime = 0, crankTime = 0; int wheelRevs = 0, crankRevs = 0;
    uint64_t opSec = 3600 * 5;
};
} // namespace

struct AntGenerator::Impl {
    AntGenConfig cfg;
    genutil::NoiseSource noise;
    float sigma = 0;
    std::vector<SensorState> st;
    std::vector<cf32> buf;       // samples ready to go out
    size_t bufPos = 0;
    double t = 0;                // signal time of the next sample produced
    double ph = 0;

    explicit Impl(const AntGenConfig& c) : cfg(c), noise(c.seed) {
        double lead = c.leadSec;
        for (const AntGenSensor& s : c.sensors) { SensorState x; x.s = s; x.hr = s.heartRate; x.next = lead; lead += 0.013; st.push_back(x); }
    }

    void hrmPayload(SensorState& x, uint8_t p[8]) {
        // the real strap's page cycle: page 0 (modern straps send pages 1, 2, 3 and 7 as background pages, four of every 65 messages)
        const uint64_t n = x.count;
        int page = 0;
        const uint64_t cyc = n % 65;
        if (cyc == 61) page = 1; else if (cyc == 62) page = 2; else if (cyc == 63) page = 3; else if (cyc == 64) page = 7;
        if (n % 3 == 1 && page == 0 && n > 10) page = 4;   // the previous-beat page now and then
        const bool toggle = (n / 4) & 1;
        memset(p, 0xFF, 8);
        p[0] = (uint8_t)(page | (toggle ? 0x80 : 0));
        // beats: the heart rate gives the beat interval; the beat time is the last beat within a 64 s cycle (1/1024 s)
        const double dtBeat = 60.0 / std::max(30.0, x.hr);
        // advance beats up to now
        while (x.beatTime + dtBeat <= x.next) { x.prevBeatTime = x.beatTime; x.beatTime += dtBeat; x.beats = (x.beats + 1) & 0xFF; }
        const int bt = (int)std::lround(std::fmod(x.beatTime, 64.0) * 1024) & 0xFFFF;
        const int pbt = (int)std::lround(std::fmod(x.prevBeatTime, 64.0) * 1024) & 0xFFFF;
        switch (page) {
        case 1: { const uint32_t ot = (uint32_t)(x.opSec / 2); p[1] = (uint8_t)ot; p[2] = (uint8_t)(ot >> 8); p[3] = (uint8_t)(ot >> 16); break; }
        case 2: p[1] = 1; p[2] = 0x39; p[3] = 0x30; break;              // Garmin, serial 12345
        case 3: p[1] = 2; p[2] = 11; p[3] = 7; break;                    // hw 2, sw 11, model 7
        case 4: p[1] = 0xFF; p[2] = (uint8_t)pbt; p[3] = (uint8_t)(pbt >> 8); break;
        case 7: p[1] = 85; p[2] = 0x80; p[3] = (uint8_t)(2 << 4 | 2); break;   // 85 %, 2.5 V, good
        default: break;
        }
        p[4] = (uint8_t)bt; p[5] = (uint8_t)(bt >> 8); p[6] = (uint8_t)x.beats; p[7] = (uint8_t)std::lround(x.hr);
        if (x.s.sweep) { x.hrPhase += 0.02; x.hr = 79.5 + 19.5 * std::sin(x.hrPhase); }
        x.opSec += 1;
    }

    void powerPayload(SensorState& x, uint8_t p[8]) {
        const uint64_t cyc = x.count % 121;
        memset(p, 0xFF, 8);
        if (cyc == 118) { p[0] = 80; p[1] = 0xFF; p[2] = 0xFF; p[3] = 1; p[4] = 32; p[5] = 0; p[6] = 0x0B; p[7] = 0x00; return; }   // manufacturer Wahoo, model 11
        if (cyc == 119) { p[0] = 81; p[1] = 0xFF; p[2] = 0xFF; p[3] = 3; p[4] = 0xD2; p[5] = 0x04; p[6] = 0; p[7] = 0; return; }        // sw 3, serial 1234
        if (cyc == 120) { p[0] = 82; p[1] = 0xFF; p[2] = 0x40; p[3] = (uint8_t)(3 << 4 | 3); p[4] = 0; p[5] = 0; p[6] = 0; p[7] = 0; return; }
        const int w = x.s.powerW + (int)std::lround(15 * std::sin(x.count * 0.07));
        x.powerEvents = (x.powerEvents + 1) & 0xFF;
        x.accumPower = (x.accumPower + w) & 0xFFFF;
        p[0] = 0x10; p[1] = (uint8_t)x.powerEvents; p[2] = (uint8_t)(0x80 | 52); p[3] = (uint8_t)x.s.cadenceRpm;
        p[4] = (uint8_t)x.accumPower; p[5] = (uint8_t)(x.accumPower >> 8); p[6] = (uint8_t)w; p[7] = (uint8_t)(w >> 8);
    }

    void scPayload(SensorState& x, uint8_t p[8]) {
        // wheel and crank counters advance with the speed and cadence
        const double wheelM = 2.096;
        const double revPerSec = x.s.speedKmh / 3.6 / wheelM;
        while (x.wheelTime + 1 / revPerSec <= x.next) { x.wheelTime += 1 / revPerSec; x.wheelRevs = (x.wheelRevs + 1) & 0xFFFF; }
        const double crankPerSec = x.s.cadenceRpm / 60.0;
        while (x.crankTime + 1 / crankPerSec <= x.next) { x.crankTime += 1 / crankPerSec; x.crankRevs = (x.crankRevs + 1) & 0xFFFF; }
        const int ct = (int)std::lround(std::fmod(x.crankTime, 64.0) * 1024) & 0xFFFF, wt = (int)std::lround(std::fmod(x.wheelTime, 64.0) * 1024) & 0xFFFF;
        p[0] = (uint8_t)ct; p[1] = (uint8_t)(ct >> 8); p[2] = (uint8_t)x.crankRevs; p[3] = (uint8_t)(x.crankRevs >> 8);
        p[4] = (uint8_t)wt; p[5] = (uint8_t)(wt >> 8); p[6] = (uint8_t)x.wheelRevs; p[7] = (uint8_t)(x.wheelRevs >> 8);
    }

    void frameOf(SensorState& x, std::vector<uint8_t>& bits) {
        ant::Frame f;
        f.marker = x.s.marker; f.deviceNumber = x.s.deviceNumber; f.deviceType = (uint8_t)x.s.type; f.transType = x.s.transType;
        if (x.s.type == ant::HeartRate) hrmPayload(x, f.payload);
        else if (x.s.type == ant::BikePower) powerPayload(x, f.payload);
        else if (x.s.type == ant::BikeSpeedCadence) scPayload(x, f.payload);
        else { memset(f.payload, 0, 8); f.payload[0] = 1; f.payload[7] = (uint8_t)x.count; }
        ant::frameBits(f, bits);
        if (x.s.badCrcEvery > 0 && (x.count % (uint64_t)x.s.badCrcEvery) == (uint64_t)(x.s.badCrcEvery - 1)) bits.back() ^= 1;
        x.count++;
    }

    void modulate(const std::vector<uint8_t>& bits, double off, double amp, std::vector<cf32>& out) {
        const int nb = (int)bits.size();
        const double pad = 4;                      // bits of plain carrier before and after (the radio's turn-on)
        const double bitsPerSample = 1e6 * (1 + cfg.clockPpm * 1e-6) / cfg.rate;
        const size_t ns = (size_t)std::ceil((nb + 2 * pad) / bitsPerSample);
        out.resize(ns);
        for (size_t i = 0; i < ns; i++) {
            const double u = i * bitsPerSample - pad;
            double f = 0;
            const int k0 = (int)std::floor(u);
            for (int k = k0 - 2; k <= k0 + 2; k++) {
                if (k < 0 || k >= nb) continue;
                f += (bits[(size_t)k] ? 1.0 : -1.0) * genutil::gfskPulse(u - k - 0.5, 0.5);
            }
            ph = std::remainder(ph + 2 * kPi * (f * ant::kDevHz + off) / cfg.rate, 2 * kPi);
            out[i] = cf32((float)(amp * std::cos(ph)), (float)(amp * std::sin(ph)));
        }
    }

    // fill `out` with the next n samples: silence, with each sensor's frame mixed in at its time
    void produce(cf32* out, size_t n) {
        for (size_t i = 0; i < n; i++) out[i] = cf32(0.f, 0.f);
        // what earlier frames left for this block
        std::vector<std::pair<int64_t, cf32>> keep;
        for (auto& c : carry) { if (c.first < (int64_t)n) out[c.first] += c.second; else keep.push_back({c.first - (int64_t)n, c.second}); }
        carry.swap(keep);
        const double t0 = t, t1 = t + n / cfg.rate;
        if (!cfg.noiseOnly) {
            for (SensorState& x : st) {
                while (x.next < t1) {
                    std::vector<uint8_t> bits;
                    std::vector<cf32> w;
                    frameOf(x, bits);
                    modulate(bits, cfg.dialOffsetHz + cfg.cfoHz + x.s.cfoHz, cfg.amp * x.s.ampScale, w);
                    // the frame starts at x.next: it may begin in this block and run past it (kept in `carry`)
                    const int64_t start = (int64_t)std::llround((x.next - t0) * cfg.rate);
                    for (size_t k = 0; k < w.size(); k++) {
                        const int64_t idx = start + (int64_t)k;
                        if (idx < 0) continue;
                        if (idx < (int64_t)n) out[idx] += w[k];
                        else carry.push_back({idx - (int64_t)n, w[k]});
                    }
                    x.next += x.s.periodSec;
                }
            }
        }
        t = t1;
    }
    std::vector<std::pair<int64_t, cf32>> carry;
};

double AntGenerator::noiseSigma(const AntGenConfig& c) {
    if (c.snrDb >= 99) return 0;
    const double nb = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // noise power in 1 MHz
    return std::sqrt(nb * c.rate / 1e6 / 2);
}

AntGenerator::AntGenerator(const AntGenConfig& c) : p_(std::make_unique<Impl>(c)) { p_->sigma = (float)noiseSigma(c); }
AntGenerator::~AntGenerator() = default;

void AntGenerator::generate(cf32* out, size_t n) {
    p_->produce(out, n);
    if (p_->sigma > 0) p_->noise.add(out, n, p_->sigma);
}

std::vector<AntGenSensor> antTestSensors(int option) {
    std::vector<AntGenSensor> v;
    AntGenSensor hrm;
    hrm.deviceNumber = 0x6941; hrm.type = ant::HeartRate; hrm.transType = 1; hrm.heartRate = 72; hrm.sweep = true;
    v.push_back(hrm);
    if (option == 1) return v;
    AntGenSensor pwr;
    pwr.deviceNumber = 12345; pwr.type = ant::BikePower; pwr.transType = 5; pwr.periodSec = 8182.0 / 32768; pwr.cfoHz = 12e3; pwr.ampScale = 0.7;
    v.push_back(pwr);
    AntGenSensor sc;
    sc.deviceNumber = 777; sc.type = ant::BikeSpeedCadence; sc.transType = 1; sc.periodSec = 8086.0 / 32768; sc.cfoHz = -20e3; sc.ampScale = 0.5;
    v.push_back(sc);
    return v;
}

namespace {
class AntSynth : public ModeSynth {
public:
    AntSynth(const SynthConfig& s, double rate) : rate_(rate) {
        AntGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -antTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        const int o = s.modeOpt[0];
        c.noiseOnly = o == 2;
        c.sensors = antTestSensors(o == 1 ? 1 : 0);
        gen_ = std::make_unique<AntGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<AntGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeAntSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<AntSynth>(cfg, sampleRate);
}

} // namespace dect2
