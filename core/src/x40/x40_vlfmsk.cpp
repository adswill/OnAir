// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// VLF MSK transmitter monitor (10 - 30 kHz, the long-wave naval communication stations): a minimum-shift-keyed carrier has two tones, a quarter of the bit rate above and below its
// centre. Squaring the signal turns them into two lines whose spacing is the bit rate and whose middle is twice the carrier. The mode measures that, over 4 s at a time: the carrier
// frequency (and its offset from the tuned frequency), the bit rate (compared with 50, 75, 100, 150, 200 and 300 bit/s), the level, and whether the station is on the air; it logs when a
// station starts, stops or changes rate. The traffic of these stations is encrypted: no message is decoded and none is shown. A table of public carrier frequencies names the likely
// station when the measured carrier is within 60 Hz of one (a hint to check, not a proof). Receive only. Needs a radio that reaches the VLF band (direct sampling or an LF capable SDR).
#include "x40_dsp.h"

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 4000, kCut = 1700, kReportSec = 4.0;
constexpr int kFft = 4096;

struct Known { double khz; const char* name; };
const Known kStations[] = {{24.0, "NAA, Cutler, USA"}, {24.8, "NLK, Jim Creek, USA"}, {25.2, "NML, LaMoure, USA"}, {21.4, "NPM, Lualualei, USA"}, {19.8, "NWC, Exmouth, Australia"},
                           {18.3, "HWU, Rosnay, France"}, {22.1, "GQD, Anthorn, UK"}, {23.4, "DHO38, Rhauderfehn, Germany"}, {20.27, "ICV, Tavolara, Italy"}, {22.2, "JJI, Ebino, Japan"}};

class MskDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, kCut)) return false;
        rate_ = dec_.outRate(); win_.assign(kFft, 0.f);
        for (int i = 0; i < kFft; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft));
        reset(); return true;
    }
    void reset() override {
        dec_.reset(); buf_.clear(); acc_.assign(kFft, 0.0); accX_.assign(kFft, 0.0); segs_ = 0; n_ = 0; have_ = false; on_ = false; cnt_ = 0; rateBd_ = 0; fcHz_ = 0; snrDb_ = 0; levelDb_ = -120; station_.clear(); cls_.clear(); lastRate_ = 0;
        floorLvl_ = -1e9; offSince_ = 0;
    }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) {
            buf_.push_back(a); n_++;
            if ((int)buf_.size() == kFft) { segment(); buf_.erase(buf_.begin(), buf_.begin() + kFft / 2); }
            if (n_ >= (uint64_t)(kReportSec * rate_)) { n_ = 0; report(); }
        });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        t.headline = on_ ? fmt("%s, carrier %+.1f Hz from the tuned frequency%s%s", cls_.c_str(), fcHz_, station_.empty() ? "" : ", likely ", station_.c_str()) : "no MSK signal";
        t.facts.push_back({"state", on_ ? cls_ : std::string("off air or too weak")});
        if (on_) {
            t.facts.push_back({"carrier", fmt("%+.2f Hz from the tuned frequency", fcHz_)});
            t.facts.push_back({"bit rate", rateBd_ > 0 ? fmt("%.1f bit/s (the two lines are %.1f Hz apart)", rateBd_, rateBd_) : std::string("not modulated, or under 25 bit/s")});
            t.facts.push_back({"level over the noise", fmt("%.1f dB", snrDb_)});
            if (!station_.empty()) t.facts.push_back({"carrier near the listed", station_});
        }
    }
private:
    void segment() {
        std::vector<cf32> a(kFft), b(kFft);
        for (int i = 0; i < kFft; i++) { const cf32 v = buf_[i] * win_[i]; a[i] = v * v; b[i] = v; }       // the square, and the signal itself
        fftx(a); fftx(b);
        for (int k = 0; k < kFft; k++) { acc_[k] += std::norm(a[k]); accX_[k] += std::norm(b[k]); }
        segs_++;
    }
    void report() {
        have_ = true;
        if (segs_ < 3) return;
        const double bin = rate_ / kFft;
        // noise level: median of the signal spectrum
        std::vector<double> s(accX_); std::sort(s.begin(), s.end()); const double med = std::max(s[kFft / 2] / segs_, 1e-30);
        double totalSig = 0; for (int k = 0; k < kFft; k++) totalSig += accX_[k] / segs_;
        const double noiseTot = med / 0.693 * kFft * 0.9;     // averaged bins sit near the mean
        const double snr = 10 * std::log10(std::max(totalSig / std::max(noiseTot, 1e-30) - 1.0, 1e-6));
        // lines of the squared signal: strongest bin, then the strongest at 20 - 400 Hz from it
        std::vector<double> q(kFft); for (int k = 0; k < kFft; k++) q[k] = acc_[k] / segs_;
        std::vector<double> qs(q); std::sort(qs.begin(), qs.end()); const double qmed = std::max(qs[kFft / 2], 1e-30);
        auto freqOf = [&](int k) { return (k < kFft / 2 ? k : k - kFft) * bin; };
        // the squared band holds frequencies up to twice the signal's: search only within +-1200 Hz of the origin
        int p1 = -1; double v1 = 0; for (int k = 0; k < kFft; k++) { const double f = freqOf(k); if (std::fabs(f) > 1200) continue; if (q[k] > v1) { v1 = q[k]; p1 = k; } }
        bool sig = p1 >= 0 && v1 > 9.0 * qmed;      // a line 9.5 dB over the typical bin of the squared spectrum
        double rate = 0, fc = 0; std::string cls = "carrier only";
        if (sig) {
            // sub-bin position of the first line (parabolic), and the second line
            auto pos = [&](int k) { const double l = q[(k + kFft - 1) % kFft], c = q[k], r = q[(k + 1) % kFft]; const double den = l - 2 * c + r; const double d = std::fabs(den) > 1e-30 ? 0.5 * (l - r) / den : 0.0; return freqOf(k) + d * bin; };
            int p2 = -1; double v2 = 0;
            for (int k = 0; k < kFft; k++) { const double f = freqOf(k); if (std::fabs(f) > 1200) continue; const double dist = std::fabs(f - freqOf(p1)); if (dist < 20 || dist > 420) continue; if (q[k] > v2) { v2 = q[k]; p2 = k; } }
            const double f1 = pos(p1);
            if (p2 >= 0 && v2 > 0.2 * v1 && v2 > 9.0 * qmed) {
                const double f2 = pos(p2); rate = std::fabs(f2 - f1); fc = (f1 + f2) / 4; cls = "MSK";
            } else fc = f1 / 2;
        }
        // on / off with a two-report hold
        const bool now = sig;
        if (now != on_) { if (++cnt_ >= 2 || (!on_ && now && cnt_ >= 1 && false)) { on_ = now; cnt_ = 0; if (on_) { rateBd_ = rate; fcHz_ = fc; log("on", rate, fc, snr); } else out->row("Transmitter", "off", "the signal has gone", levelDb_); } }
        else cnt_ = 0;
        if (on_ && sig) {
            fcHz_ = fc; rateBd_ = rate; snrDb_ = (float)snr; levelDb_ = (float)(snr);
            std::string c;
            if (rate > 0) {
                static const double std_[] = {50, 75, 100, 150, 200, 300}; double best = 0; for (double r : std_) if (std::fabs(rate - r) < 0.08 * r) best = r;
                c = best > 0 ? fmt("MSK %.0f bit/s", best) : fmt("MSK about %.0f bit/s", rate);
            } else c = "carrier only";
            if (c != cls_) { const bool had = !cls_.empty(); cls_ = c; if (had && lastRate_ > 0 && rate > 0 && std::fabs(rate - lastRate_) > 0.08 * lastRate_) log("changed", rate, fc, snr); }
            if (rate > 0) lastRate_ = rate;
            station_.clear();
            const double absKhz = tunedKhz_ + fc / 1000;
            if (tunedKhz_ > 0) for (auto& k : kStations) if (std::fabs(absKhz - k.khz) < 0.06) station_ = k.name;
            out->good((float)snr, fc);
        }
        acc_.assign(kFft, 0.0); accX_.assign(kFft, 0.0); segs_ = 0;
    }
    void log(const char* what, double rate, double fc, double snr) {
        std::string c = rate > 0 ? fmt("%.0f bit/s", rate) : "carrier only";
        const double absKhz = tunedKhz_ + fc / 1000; std::string st;
        if (tunedKhz_ > 0) for (auto& k : kStations) if (std::fabs(absKhz - k.khz) < 0.06) st = fmt(", near the listed frequency of %s", k.name);
        out->row("Transmitter", what, fmt("%s, carrier %+.2f Hz from the tuned frequency, %.1f dB over the noise%s", c.c_str(), fc, snr, st.c_str()), (float)snr);
    }
public:
    void setCenterHz(double hz) override { tunedKhz_ = hz / 1000; }
private:
    Decimator dec_; std::vector<cf32> buf_; std::vector<float> win_; std::vector<double> acc_, accX_; int segs_ = 0; uint64_t n_ = 0; double rate_ = 1, tunedKhz_ = 0, lastRate_ = 0, floorLvl_ = 0, offSince_ = 0;
    bool have_ = false, on_ = false; int cnt_ = 0; double rateBd_ = 0, fcHz_ = 0; float snrDb_ = 0, levelDb_ = -120; std::string station_, cls_;
};
}

std::unique_ptr<Decoder> makeVlfMsk() { return std::make_unique<MskDec>(); }

} // namespace dect2::x40
