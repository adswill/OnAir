// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Time signal test signal (see time_gen.h).
#include "dect2/time/time_gen.h"
#include "dect2/time/time_rx.h"
#include "dect2/dsp/gen_util.h"
#include <chrono>
#include <cmath>
#include <map>
#include <vector>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {
using dect2::math::kPi;
using namespace timesig;

double nowUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
} // namespace

struct TimeGenerator::Impl {
    TimeGenConfig c;
    genutil::NoiseSource noise;
    float sigma = 0;
    int64_t n = 0;
    double ph = 0;                       // carrier phase
    double fskPh = 0;
    std::map<int64_t, std::vector<uint8_t>> minutes;
    int64_t chuSec = INT64_MIN;
    std::vector<uint8_t> chuBits;        // the FSK bits of the current CHU second
    int64_t mcSec = INT64_MIN;
    std::vector<uint8_t> mcEl;           // BPM coded: the code elements of the current second

    explicit Impl(const TimeGenConfig& cfg) : c(cfg), noise(cfg.seed) {
        if (c.startUtc <= 0) c.startUtc = nowUtc();
        if (c.snrDb < 99) {
            const double n1k = c.amp * c.amp / std::pow(10.0, c.snrDb / 10);   // in 1 kHz
            sigma = (float)std::sqrt(n1k * c.rate / 1000 / 2);
        }
    }

    const std::vector<uint8_t>& minute(int64_t M) {
        auto it = minutes.find(M);
        if (it != minutes.end()) return it->second;
        if (minutes.size() > 4) minutes.erase(minutes.begin());
        return minutes[M] = encodeMinute(c.station, M, c.opts);
    }

    // LF: the carrier amplitude at second sec, fraction f
    double lfAmp(uint8_t s, double f) const {
        switch (c.station) {
        case Dcf77: return s == SNone ? 1 : f < (s & 1 ? 0.2 : 0.1) ? 0.15 : 1;
        case Msf:
            if (s == SMark) return f < 0.5 ? 0 : 1;
            if (f < 0.1) return 0;
            if (f < 0.2) return s & 1 ? 0 : 1;
            if (f < 0.3) return s & 2 ? 0 : 1;
            return 1;
        case Wwvb: return f < (s == SMark ? 0.8 : s & 1 ? 0.5 : 0.2) ? 0.141 : 1;
        case Bpc: return s == SNone ? 1 : f < 0.1 * ((s & 3) + 1) ? 0.1 : 1;   // low for 0.1 - 0.4 s, none in the frame's first second
        default: return f < (s == SMark ? 0.2 : s & 1 ? 0.5 : 0.8) ? 1 : 0.1;   // JJY
        }
    }

    // RWM: the keyed carrier at transmit time t (time_proto.h: the half hour's schedule, the doubled pulses in the 1 pulse minutes)
    double rwmAmp(double t) {
        const double sf = std::floor(t);
        const int64_t sec = (int64_t)sf;
        const double f = t - sf;
        const int64_t M = sec - (((sec % 60) + 60) % 60);
        const int i = (int)(sec - M);
        const int mh = (int)(((M / 60) % 30 + 30) % 30);
        if (mh < 8) return 1;                               // plain carrier
        if (mh < 10) return 0;                              // off, then the call sign (not sent here)
        if (i == 0) return f < 0.5 ? 1 : 0;
        if (mh < 20) {
            const uint8_t s = minute(M)[(size_t)i];
            return f < 0.1 || (s == S1 && f >= 0.2 && f < 0.3) ? 1 : 0;
        }
        const double g = f * 10 - std::floor(f * 10);       // 10 pulses a second
        return f < 0.04 || g < 0.2 ? 1 : 0;
    }

    void chuPrepare(int64_t sec) {
        chuSec = sec;
        chuBits.clear();
        const int i = (int)(((sec % 60) + 60) % 60);
        if (i < 31 || i > 39) return;
        int y, mo, d;
        const int64_t days = (sec >= 0 ? sec : sec - 86399) / 86400;
        civilFromDays(days, y, mo, d);
        const int64_t sod = sec - days * 86400;
        std::vector<uint8_t> bytes = i == 31 ? chuFrameB(y) : chuFrameA((int)(days - daysFromCivil(y, 1, 1)) + 1, (int)(sod / 3600), (int)(sod / 60 % 60), i);
        for (uint8_t b : bytes) {
            chuBits.push_back(0);
            for (int k = 0; k < 8; k++) chuBits.push_back(b >> k & 1);
            chuBits.push_back(1); chuBits.push_back(1);
        }
    }

    double audio(double t) {   // HF: the audio at transmit time t, about -1 .. 1
        const double sf = std::floor(t);
        const int64_t sec = (int64_t)sf;
        const double f = t - sf;
        const int64_t M = sec - (((sec % 60) + 60) % 60);
        const int i = (int)(sec - M);
        double a = 0;
        if (c.station == Wwv) {
            const double tf = c.wwvh ? 1200 : 1000;
            const bool tick = (i == 0 && f < 0.8) || (f < 0.005 && i != 29 && i != 59);
            if (tick) a += 0.5 * std::sin(2 * kPi * tf * f);
            const uint8_t s = minute(M)[(size_t)i];
            double lvl = 0.18;
            if (s != SNone) {
                const double w = s == SMark ? 0.8 : s & 1 ? 0.5 : 0.2;
                if (f < w) lvl = 1;
            }
            if (f < 0.03 || f > 0.99) lvl = 0;
            a += 0.4 * lvl * std::sin(2 * kPi * 100 * f);
            return a;
        }
        if (c.station == Bpm) {
            // UT1 minutes: the seconds follow UT1 = UTC + dut1 / 10 (the code is the same, the seconds are early by dut1 tenths)
            const double tu = c.opts.ut1 ? t + c.opts.dut1 * 0.1 : t;
            const double suf = std::floor(tu);
            const int64_t su = (int64_t)suf;
            const double fu = tu - suf;
            const int64_t Mu = su - (((su % 60) + 60) % 60);
            const int iu = (int)(su - Mu);
            if (fu < 0.01) a += 0.5 * std::sin(2 * kPi * 1000 * fu);
            const uint8_t s = minute(Mu)[(size_t)iu];
            const double w = s == SMark ? 0.3 : s & 1 ? 0.2 : 0.1;
            if (fu >= 0.1 && fu < 0.1 + w) a += 0.5 * std::sin(2 * kPi * 1000 * fu);
            return a;
        }
        if (c.station == Hla) {
            const uint8_t s = minute(M)[(size_t)i];
            const double tf = 1800;
            if ((i == 0 && f < 0.768) || (i == 59 && f >= 0.968)) return 0.4 * std::sin(2 * kPi * tf * f);   // the vacant bit 0: a 0.8 s tone from the tick
            if (f >= 0.968 && f < 0.974) a += 0.5 * std::sin(2 * kPi * tf * f);
            if (s != SNone) {
                const double w = s == SMark ? 0.77 : s & 1 ? 0.47 : 0.17;
                if (f < w) a += 0.4 * std::sin(2 * kPi * 100 * f);
            }
            return a;
        }
        if (c.station == BpmC) {
            if (sec != mcSec) {
                mcSec = sec;
                if (i == 0) mcEl = encodeBpmcMinute((int)std::lround(std::fabs(c.opts.dut1Ms) * 10) % 10, c.opts.leapCount, c.opts.leapAtMonthEnd);
                else mcEl = encodeBpmcSecond(sec, c.opts.dut1Ms);
            }
            if (i == 0 && f < 0.3) return 0.5 * std::sin(2 * kPi * 1000 * f);
            const int k = (int)(f * 100);
            if (k >= 0 && (size_t)k < mcEl.size() && (i != 0 || k >= 32) && mcEl[(size_t)k] && f - k * 0.01 < 0.005) return 0.5 * std::sin(2 * kPi * 1000 * f);
            return 0;
        }
        // CHU
        double tl = 0.3;
        if (i == 0) tl = 0.5; else if (i == 29) tl = 0; else if ((i >= 31 && i <= 39) || i >= 51) tl = 0.01;
        if (f < tl) return 0.8 * std::sin(2 * kPi * 1000 * f);
        if (i >= 31 && i <= 39 && f >= 0.01 && f < 0.5) {
            if (sec != chuSec) chuPrepare(sec);
            const double bt = (f - 0.01) * 300 - 2;   // two bits of mark first
            int bit = 1;
            if (bt >= 0 && (size_t)bt < chuBits.size()) bit = chuBits[(size_t)bt];
            fskPh = std::remainder(fskPh + 2 * kPi * (bit ? 2225.0 : 2025.0) / c.rate, 2 * kPi);
            return 0.8 * std::sin(fskPh);
        }
        return 0;
    }
};

TimeGenerator::TimeGenerator(const TimeGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
TimeGenerator::~TimeGenerator() = default;

void TimeGenerator::generate(cf32* out, size_t cnt) {
    Impl& m = *p_;
    const double dph = 2 * kPi * (m.c.dialOffsetHz + m.c.cfoHz) / m.c.rate;
    const bool hf = !stationKeysCarrier(m.c.station);
    for (size_t k = 0; k < cnt; k++) {
        // the radio's sample n is taken at true time n / (rate (1 + ppm)); what arrives then left the station delayMs earlier
        const double t = m.c.startUtc + (double)m.n / (m.c.rate * (1 + m.c.clockPpm * 1e-6)) - m.c.delayMs * 1e-3;
        double a;
        if (hf) {
            a = 1 + m.audio(t);
        } else if (m.c.station == Rwm) {
            a = m.rwmAmp(t);
        } else {
            const double sf = std::floor(t);
            const int64_t sec = (int64_t)sf;
            const int64_t M = sec - (((sec % 60) + 60) % 60);
            a = m.lfAmp(m.minute(M)[(size_t)(sec - M)], t - sf);
        }
        m.ph = std::remainder(m.ph + dph, 2 * kPi);
        out[k] = cf32((float)(m.c.amp * a * std::cos(m.ph)), (float)(m.c.amp * a * std::sin(m.ph)));
        m.n++;
    }
    if (m.sigma > 0) m.noise.add(out, cnt, m.sigma);
}

namespace {
class TimeSynth : public ModeSynth {
public:
    TimeSynth(const SynthConfig& s, double rate) : rate_(rate) {
        TimeGenConfig c;
        c.station = s.modeOpt[0] > 0 && s.modeOpt[0] <= kNumStations ? s.modeOpt[0] - 1 : Dcf77;
        c.wwvh = s.modeOpt[1] == 1;
        c.rate = rate;
        c.dialOffsetHz = -timeTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        gen_ = std::make_unique<TimeGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<TimeGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeTimeSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<TimeSynth>(cfg, sampleRate);
}

} // namespace dect2
