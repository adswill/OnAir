// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FM broadcast modulation monitor (87.5 - 108 MHz): the measurements a broadcast engineer reads from a modulation monitor, taken from the FM-demodulated multiplex (MPX) of a
// station: peak and RMS deviation, the carrier offset, the 19 kHz pilot (nominal 8 - 10 % of 75 kHz, i.e. 6 - 7.5 kHz) and stereo, the 57 kHz RDS injection (nominal 1 - 7.5 kHz),
// SCA subcarriers (67 and 92 kHz), the audio (L+R) level and the MPX power in dB against a sine of 19 kHz deviation (the ITU-R BS.412 reference; the 60 s averaging and the
// weighting of BS.412 are not applied: this is the plain mean-square value of the multiplex over each second). It measures what arrives, after the radio and the path:
// it is not a type-approval instrument. Over-deviation (above 80 kHz) and changes of pilot, stereo, RDS and SCA are logged. Receive only.
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 250000, kCut = 100000, kReportSec = 1.0;
constexpr float kOverKhz = 80.f;

class FmMonitor : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate();
        n_ = 1; while (n_ * 2 <= rate_ / 30.0) n_ *= 2;       // bins of 30 - 60 Hz
        wl_.configure(rate_, n_); reset();
        return true;
    }
    void reset() override {
        dec_.reset(); prev_ = cf32(1, 0); mpx_.clear(); peak_ = 0; secN_ = 0; t_ = 0; sumSq_ = 0; sum_ = 0; blockPeak_ = 0; blockN_ = 0; runMean_ = 0; top3_[0] = top3_[1] = top3_[2] = 0;
        haveMeas_ = false; pilotOn_ = false; rdsOn_ = false; sca67_ = sca92_ = false; stereo_ = false; overUntil_ = -1; first_ = true; pend_ = {};
        peakKhz_ = rmsKhz_ = offKhz_ = pilotKhz_ = rdsKhz_ = audioKhz_ = mpxDb_ = lmrKhz_ = 0; sca67Db_ = sca92Db_ = -99; nOver_ = 0; levelDb_ = -120; wl_.clear();
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            const cf32 d = a * std::conj(prev_); prev_ = a;
            const float f = (float)(std::atan2(d.imag(), d.real()) * rate_ / (2 * kPi));     // Hz
            t_ += 1.0 / rate_;
            if (t_ < 0.1) return;                                   // the filter and the demodulator start from nothing
            mpx_.push_back(f);
            sumSq_ += (double)f * f; sum_ += f; secN_++;
            // peak deviation: in blocks of 5 ms the third largest excursion from the running mean (a click of the noise is one or two samples, a real peak lasts longer)
            const float dv = std::fabs(f - runMean_); runMean_ += 2e-5f * (f - runMean_);
            if (dv > top3_[0]) { top3_[2] = top3_[1]; top3_[1] = top3_[0]; top3_[0] = dv; } else if (dv > top3_[1]) { top3_[2] = top3_[1]; top3_[1] = dv; } else if (dv > top3_[2]) top3_[2] = dv;
            if (++blockN_ >= (int)(0.005 * rate_)) { peak_ = std::max(peak_, top3_[2]); top3_[0] = top3_[1] = top3_[2] = 0; blockN_ = 0; }
            lvlAcc_ += std::norm(a); lvlN_++;
            if (secN_ >= (size_t)(kReportSec * rate_)) report();
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!haveMeas_) { t.headline = "waiting for data"; return; }
        t.headline = fmt("%s%s peak %.1f kHz, pilot %.1f kHz, RDS %.1f kHz", pilotOn_ ? "stereo" : "mono", pilotOn_ ? "" : " (no pilot)", peakKhz_, pilotKhz_, rdsKhz_);
        t.facts.push_back({"peak deviation", fmt("%.1f kHz (limit 75 kHz)", peakKhz_)});
        t.facts.push_back({"RMS deviation", fmt("%.1f kHz", rmsKhz_)});
        t.facts.push_back({"MPX power", fmt("%.1f dBr (0 dBr: a 19 kHz deviation sine)", mpxDb_)});
        t.facts.push_back({"carrier offset", fmt("%+.2f kHz", offKhz_)});
        t.facts.push_back({"audio (L+R)", fmt("%.1f kHz peak deviation", audioKhz_)});
        t.facts.push_back({"pilot 19 kHz", fmt("%.2f kHz (%.1f %% of 75 kHz): %s", pilotKhz_, 100 * pilotKhz_ / 75, pilotOn_ ? (pilotKhz_ >= 5.5 && pilotKhz_ <= 8.2 ? "in the usual range" : "outside 8 - 10 %") : "absent")});
        t.facts.push_back({"stereo (L-R) band", fmt("%.1f kHz peak equivalent", lmrKhz_)});
        t.facts.push_back({"RDS 57 kHz", fmt("%.2f kHz injection: %s", rdsKhz_, rdsOn_ ? "present" : "absent")});
        t.facts.push_back({"SCA 67 / 92 kHz", fmt("%s / %s", sca67_ ? fmt("%.0f dB over the floor", sca67Db_).c_str() : "absent", sca92_ ? fmt("%.0f dB over the floor", sca92Db_).c_str() : "absent")});
        t.facts.push_back({"over-deviation", fmt("%d seconds above %.0f kHz", nOver_, kOverKhz)});
    }
private:
    static bool settle(int& cnt, bool now, bool& cur) {       // a change must hold for two reports
        if (now == cur) { cnt = 0; return false; }
        if (++cnt >= 2) { cur = now; cnt = 0; return true; }
        return false;
    }
    void report() {
        const size_t N = mpx_.size();
        // spectra: segments of n_ with 50 % overlap
        const double meanHz = sum_ / secN_; for (auto& v : mpx_) v -= (float)meanHz;
        wl_.clear();
        for (size_t o = 0; o + n_ <= N; o += n_ / 2) wl_.add(mpx_.data() + o);
        auto A = [&](double f0, double f1) { return std::sqrt(2.0 * wl_.band(f0, f1)); };    // peak equivalent of a sine in the band, Hz
        const double bw = 3 * wl_.binHz();
        const double mean = sum_ / secN_;
        offKhz_ = (float)(mean / 1000);
        rmsKhz_ = (float)(std::sqrt(std::max(0.0, sumSq_ / secN_ - mean * mean)) / 1000);
        mpxDb_ = (float)(10 * std::log10(std::max(1e-12, (sumSq_ / secN_ - mean * mean) / (19000.0 * 19000.0 / 2))));
        peakKhz_ = (float)(peak_ / 1000);
        pilotKhz_ = (float)(A(19000 - bw, 19000 + bw) / 1000);
        audioKhz_ = (float)(A(30, 15000) / 1000);
        lmrKhz_ = (float)(A(23000, 53000) / 1000);
        rdsKhz_ = (float)(A(57000 - 2400, 57000 + 2400) / 1000);
        // floor: the median power of 2 kHz bands between 58 and 100 kHz (SCA and RDS lie on a few of them)
        std::vector<double> fl; const double top = std::min(100000.0, rate_ / 2 - 2000);
        for (double f = 60000; f + 2000 <= top; f += 2000) fl.push_back(wl_.band(f, f + 2000));
        double floorP = 1e-9; if (!fl.empty()) { std::sort(fl.begin(), fl.end()); floorP = std::max(fl[fl.size() / 2], 1e-9); }
        auto db = [&](double f0, double f1) { return 10 * std::log10(std::max(wl_.band(f0, f1), 1e-12) / (floorP * (f1 - f0) / 2000.0)); };
        sca67Db_ = (float)db(63000, 71000); sca92Db_ = rate_ / 2 > 97000 ? (float)db(87000, 97000) : -99.f;
        level();
        // the pilot is a line: over the bands either side of it (the discriminator's noise alone gave 4 - 5 kHz of "pilot" in the QA sweep)
        const bool pilotNow = pilotKhz_ > 2.0f && pilotKhz_ > 3 * (float)(0.5 * (A(19000 - 4 * bw, 19000 - 2 * bw) + A(19000 + 2 * bw, 19000 + 4 * bw)) / 1000);
        const bool rdsNow = rdsKhz_ > 0.3f && rdsKhz_ > 3 * (float)(A(60000, 62500) / 1000);   // above its neighbourhood
        const bool s67 = sca67Db_ > 10.f && A(63000, 71000) > 300, s92 = sca92Db_ > 10.f && A(87000, 97000) > 300;
        const double t = t_;
        if (first_) { pilotOn_ = pilotNow; rdsOn_ = rdsNow; sca67_ = s67; sca92_ = s92; first_ = false; row("Pilot", pilotOn_ ? "present" : "absent", fmt("19 kHz pilot %.2f kHz", pilotKhz_)); if (pilotOn_) out->good(0, offKhz_ * 1000); }
        else {
            if (settle(cPilot_, pilotNow, pilotOn_)) row("Pilot", pilotOn_ ? "present" : "lost", fmt("19 kHz pilot %.2f kHz", pilotKhz_));
            if (settle(cRds_, rdsNow, rdsOn_)) row("RDS", rdsOn_ ? "present" : "lost", fmt("57 kHz injection %.2f kHz", rdsKhz_));
            if (settle(c67_, s67, sca67_)) row("SCA", sca67_ ? "67 kHz on" : "67 kHz off", fmt("%.0f dB over the floor", sca67Db_));
            if (settle(c92_, s92, sca92_)) row("SCA", sca92_ ? "92 kHz on" : "92 kHz off", fmt("%.0f dB over the floor", sca92Db_));
        }
        if (peakKhz_ > kOverKhz) { nOver_++; row("Over-deviation", fmt("%.1f kHz", peakKhz_), fmt("peak deviation %.1f kHz over %.0f kHz, MPX power %.1f dBr", peakKhz_, kOverKhz, mpxDb_)); }
        stereo_ = pilotOn_ && lmrKhz_ > 0.5f;
        haveMeas_ = true; (void)t;
        mpx_.clear(); peak_ = 0; secN_ = 0; sumSq_ = 0; sum_ = 0;
    }
    void level() { levelDb_ = lvlN_ ? toDb((float)(lvlAcc_ / lvlN_)) : -120.f; lvlAcc_ = 0; lvlN_ = 0; }
    void row(const char* kind, const std::string& id, const std::string& text) { out->row(kind, id, text, levelDb_); }
    Decimator dec_; RealWelch wl_; double rate_ = 1, t_ = 0, sumSq_ = 0, sum_ = 0, lvlAcc_ = 0; int n_ = 1; cf32 prev_{1, 0}; std::vector<float> mpx_;
    float peak_ = 0, blockPeak_ = 0, runMean_ = 0, top3_[3] = {0, 0, 0}, peakKhz_ = 0, rmsKhz_ = 0, offKhz_ = 0, pilotKhz_ = 0, rdsKhz_ = 0, audioKhz_ = 0, mpxDb_ = 0, lmrKhz_ = 0, sca67Db_ = -99, sca92Db_ = -99, levelDb_ = -120;
    size_t secN_ = 0, lvlN_ = 0; int blockN_ = 0, nOver_ = 0, cPilot_ = 0, cRds_ = 0, c67_ = 0, c92_ = 0; bool haveMeas_ = false, pilotOn_ = false, rdsOn_ = false, sca67_ = false, sca92_ = false, stereo_ = false, first_ = true;
    double overUntil_ = -1; struct P {} pend_;
};
}

std::unique_ptr<Decoder> makeFmMon() { return std::make_unique<FmMonitor>(); }

} // namespace dect2::x40
