// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DJI drone test signal: see dji_gen.h.
#include "dect2/dji/dji_gen.h"
#include "dect2/dji/dji_dsp.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/dsp/gen_util.h"
#include <algorithm>
#include <cmath>

namespace dect2 {

namespace {

constexpr double kLevel = 0.16;      // rms of one signal while it is on

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed * 2654435761u + 12345u) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    double uni() { return (next() >> 8) * (1.0 / 16777216.0); }
};

// random-QPSK OFDM symbols with cyclic prefix at native rate: nfft x 15 kHz
std::vector<cf32> ofdmNative(Rng& rng, int nSym, int nfft, int cp, int carriers, int repeatFirst) {
    Fft fft(nfft);
    std::vector<cf32> out;
    std::vector<cf32> first;
    const float sc = 1.f / std::sqrt((float)carriers);
    for (int s = 0; s < nSym; s++) {
        std::vector<cf32> f((size_t)nfft, cf32(0, 0));
        if (s < repeatFirst && !first.empty()) f = first;
        else {
            for (int k = -carriers / 2; k <= carriers / 2; k++) {
                if (k == 0) continue;
                f[(size_t)((k + nfft) % nfft)] = dji::qpsk((int)(rng.next() & 1), (int)((rng.next() >> 3) & 1));
            }
            if (s == 0) first = f;
        }
        std::vector<cf32> t = f;
        fft.inverse(t.data());
        for (auto& v : t) v *= sc;
        out.insert(out.end(), t.end() - cp, t.end());
        out.insert(out.end(), t.begin(), t.end());
    }
    return out;
}

class DjiSynth : public ModeSynth {
public:
    DjiSynth(const SynthConfig& cfg, double rate) : rate_(rate), cfg_(cfg), rng_(7), noise_(5) {
        scen_ = std::max(0, std::min(5, cfg.modeOpt[0]));
        eight_ = cfg.modeOpt[1] == 1;
        wide_ = cfg.modeOpt[2] == 1;
        period_ = cfg.modeVal[0] > 0.01 ? cfg.modeVal[0] : 0.64;
        // the noise table's own level, so that snrDb is what it says
        double p = 0;
        genutil::NoiseSource ns(9);
        std::vector<cf32> t(20000, cf32(0, 0));
        ns.add(t.data(), t.size(), 1.f);
        for (auto& v : t) p += std::norm(v);
        noiseUnit_ = (float)std::sqrt(p / t.size());
        // noise power relative to a signal of kLevel rms
        sigma_ = (float)(kLevel / std::pow(10.0, cfg.snrDb / 20.0)) / noiseUnit_;
        if (scen_ == 0 || scen_ == 1 || scen_ == 5) { Src s; s.kind = 0; s.next = 0.05; srcs_.push_back(s); }
        if (scen_ == 0 || scen_ == 2) { Src s; s.kind = 1; s.next = 0.01; srcs_.push_back(s); prerender(1); }
        if (scen_ == 3) { Src s; s.kind = 2; s.next = 0.01; srcs_.push_back(s); prerender(2); }
        if (scen_ == 0 || scen_ == 4 || scen_ == 5) { Src s; s.kind = 3; s.next = 0.02; srcs_.push_back(s); prerender(3); }
    }
    double sampleRate() const override { return rate_; }

    void generate(cf32* out, size_t n) override {
        std::fill(out, out + n, cf32(0, 0));
        const double t1 = (double)(pos_ + n) / rate_;
        for (Src& s : srcs_) {
            while (s.next < t1) {
                schedule(s);
            }
        }
        for (size_t i = 0; i < pending_.size();) {
            Pending& p = pending_[i];
            const long a = p.start - (long)pos_;
            const long b = a + (long)p.w.size();
            const long lo = std::max(0L, a), hi = std::min((long)n, b);
            for (long k = lo; k < hi; k++) out[k] += p.w[(size_t)(k - a)];
            if (b <= (long)n) pending_.erase(pending_.begin() + (long)i); else i++;
        }
        if (cfg_.cfoHz != 0) dji::mixInPlace(out, n, cfg_.cfoHz, rate_, (double)pos_);
        noise_.add(out, n, sigma_);
        pos_ += n;
    }

private:
    struct Src { int kind = 0; double next = 0; int count = 0; };
    struct Pending { long start; std::vector<cf32> w; };

    void prerender(int kind) {
        Rng r(100 + (uint32_t)kind);
        if (kind == 1 || kind == 2) {
            for (int v = 0; v < 3; v++) {
                std::vector<cf32> native;
                double nrate;
                if (kind == 1 && wide_) { native = ofdmNative(r, 14, 2048, 144, 1200, 0); nrate = 30.72e6; }
                else if (kind == 1) { native = ofdmNative(r, 14, 1024, 72, 600, 0); nrate = 15.36e6; }
                else { native = ofdmNative(r, 10, 1024, 72, 600, 2); nrate = 15.36e6; }
                const double cut = std::min(nrate, rate_) * 0.5 * 0.93;
                dji::Resampler rs(nrate, rate_, cut);
                const size_t no = (size_t)(native.size() * rate_ / nrate);
                tmpl_[kind].push_back(rs.run(native, 0.0, no));
            }
        } else {
            for (int v = 0; v < 8; v++) {
                const double us = 100 + r.uni() * 1400;
                std::vector<cf32> w((size_t)(us * 1e-6 * rate_));
                genutil::NoiseSource ns(20 + (uint32_t)v);
                ns.add(w.data(), w.size(), 1.f / noiseUnit_);
                tmpl_[3].push_back(std::move(w));
            }
        }
    }

    void put(std::vector<cf32> w, double sec, double offMhz) {
        double p = 0;
        for (auto& v : w) p += std::norm(v);
        const float g = (float)(kLevel / std::sqrt(p / std::max<size_t>(1, w.size())));
        for (auto& v : w) v *= g;
        if (offMhz != 0) dji::mixInPlace(w.data(), w.size(), offMhz * 1e6, rate_, 0);
        Pending pe;
        pe.start = (long)std::floor(sec * rate_);
        pe.w = std::move(w);
        pending_.push_back(std::move(pe));
    }

    void schedule(Src& s) {
        const double t = s.next;
        if (s.kind == 0) {          // DroneID
            dji::DroneId id;
            id.seq = 1000 + s.count; id.state = 8179; id.serial = kDjiGenSerial;
            const double lat = 51.4468 + 0.00002 * s.count, lon = 7.2679 + 0.00003 * s.count;
            id.latRaw = (int32_t)(lat / 57.29577951308232 * 1e7); id.lonRaw = (int32_t)(lon / 57.29577951308232 * 1e7);
            id.altRaw = 4054 + 3 * s.count; id.heightRaw = 305 + s.count; id.vNorth = 29; id.vEast = 84; id.vUp = 0; id.yaw = 127;
            id.gpsMs = 1650894651295ull + (uint64_t)(t * 1000);
            id.appLatRaw = (int32_t)(43.2682071 / 57.29577951308232 * 1e7); id.appLonRaw = (int32_t)(6.6401597 / 57.29577951308232 * 1e7);
            id.homeLatRaw = (int32_t)(51.4468438 / 57.29577951308232 * 1e7); id.homeLonRaw = (int32_t)(7.2679321 / 57.29577951308232 * 1e7);
            id.deviceType = 60;
            std::vector<uint8_t> bits = dji::turboEncode(dji::packDroneId(id));
            const std::vector<uint8_t> sc = dji::scramblerBits(dji::kBurstBits, 0);
            for (int i = 0; i < dji::kBurstBits; i++) bits[(size_t)i] ^= sc[(size_t)i];
            const std::vector<cf32> b = dji::buildBurst(bits, !eight_);
            const double cut = std::min(dji::kFs, rate_) * 0.5 * 0.93;
            dji::Resampler rs(dji::kFs, rate_, cut);
            const double frac = rng_.uni();   // where the burst starts between two output samples
            std::vector<cf32> w = rs.run(b, -frac * dji::kFs / rate_, (size_t)(b.size() * rate_ / dji::kFs) + 2);
            put(std::move(w), t, kDjiGenDroneIdMhz);
            if (scen_ == 5) {   // a wideband burst that starts a little before or after the DroneID burst: they overlap
                const auto& v = tmpl_[3][(size_t)(rng_.next() % tmpl_[3].size())];
                put(v, t + (rng_.uni() * 1.0e-3 - 0.3e-3), 0.0);
            }
            s.next = t + period_;
        } else if (s.kind == 1) {   // OcuSync-like: 1 ms on, 1 ms off
            const auto& v = tmpl_[1][(size_t)s.count % tmpl_[1].size()];
            put(v, t, wide_ ? 0.0 : kDjiGenTrainMhz);
            s.next = t + 0.002;
        } else if (s.kind == 2) {   // Lightbridge-like: 0.7 ms every 1 ms
            const auto& v = tmpl_[2][(size_t)s.count % tmpl_[2].size()];
            put(v, t, kDjiGenTrainMhz);
            s.next = t + 0.001;
        } else {                    // Wi-Fi-like wideband bursts at random times
            const auto& v = tmpl_[3][(size_t)(rng_.next() % tmpl_[3].size())];
            put(v, t, 0.0);
            s.next = t + (scen_ == 5 ? 0.04 + 0.1 * rng_.uni() : scen_ == 0 ? 0.02 + 0.04 * rng_.uni() : 0.0005 + 0.006 * rng_.uni());
        }
        s.count++;
    }

    double rate_;
    SynthConfig cfg_;
    int scen_ = 0;
    bool eight_ = false, wide_ = false;
    double period_ = 0.64;
    Rng rng_;
    genutil::NoiseSource noise_;
    float noiseUnit_ = 1, sigma_ = 0;
    uint64_t pos_ = 0;
    std::vector<Src> srcs_;
    std::vector<Pending> pending_;
    std::vector<std::vector<cf32>> tmpl_[4];
};

} // namespace

std::unique_ptr<ModeSynth> makeDjiSynth(const SynthConfig& cfg, double sampleRate) { return std::make_unique<DjiSynth>(cfg, sampleRate); }

} // namespace dect2
