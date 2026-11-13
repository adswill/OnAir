// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Crest factor (PAPR / CCDF) meter for the whole captured span: the distribution of the instantaneous power of a signal against its mean, which says how much headroom an amplifier needs
// (an OFDM or multi-carrier transmitter is noise-like: Gaussian statistics give 6.6 dB at 1 %, 8.4 dB at 0.1 %, 9.6 dB at 0.01 %; a constant-envelope signal such as FM or FSK reads 0 dB; a clipped or
// compressed signal reads less than Gaussian). The band is the whole captured span unless it holds a lot of empty channel: tune and set the sample rate so that the signal fills it. Every second: the mean power,
// the peak, the crest factor (peak over RMS, in dB), and the levels at which 10 %, 1 %, 0.1 % and 0.01 % of the samples are exceeded (the CCDF), against the mean power; accumulated over the last 20 s
// (about a million samples) so that the 0.01 % point is meaningful. The numbers include the radio's own noise (a signal 20 dB over the noise reads the noise's Gaussian statistics at a 1 % level) and its limits
// (clipping, ADC range). Receive only.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr int kBlock = 1 << 14, kKeepBlocks = 64;

class PaprDec : public Decoder {
public:
    bool configure(double fs) override { fs_ = fs; reset(); return true; }
    void reset() override { buf_.clear(); hist_.clear(); have_ = false; have2_ = false; res_ = Res(); t_ = 0; lastRow_ = -1e9; nTot_ = 0; }
    void feed(const cf32* x, size_t n) override {
        for (size_t i = 0; i < n; i++) { buf_.push_back(std::norm(x[i])); if ((int)buf_.size() == kBlock) { block(); buf_.clear(); } }
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "collecting"; return; }
        t.headline = fmt("crest factor %.1f dB, CCDF 1 %%: %.1f dB, 0.1 %%: %.1f dB over the mean power (Gaussian: 6.6 and 8.4)", res_.crest, res_.l1, res_.l01);
        t.facts.push_back({"crest factor (peak over RMS)", fmt("%.2f dB (peak %.1f dB, mean %.1f dB re full scale)", res_.crest, res_.peakDb, res_.meanDb)});
        t.facts.push_back({"CCDF 10 %", fmt("%.2f dB over the mean power (Gaussian 3.6)", res_.l10)});
        t.facts.push_back({"CCDF 1 %", fmt("%.2f dB (Gaussian 6.6)", res_.l1)});
        t.facts.push_back({"CCDF 0.1 %", fmt("%.2f dB (Gaussian 8.4)", res_.l01)});
        t.facts.push_back({"CCDF 0.01 %", res_.n >= 1000000 ? fmt("%.2f dB (Gaussian 9.6)", res_.l001) : std::string("needs one million samples")});
        t.facts.push_back({"samples", fmt("%.2f million in the last %d blocks", res_.n / 1e6, (int)hist_.size())});
        t.facts.push_back({"reading", res_.crest < 2 ? "near constant envelope (FM, FSK, a carrier)" : res_.l1 > 5.5 && res_.l1 < 7.5 && res_.l01 > 7.3 ? "Gaussian-like (OFDM, multi-carrier, noise)" : res_.l01 < 7.0 ? "less peaky than Gaussian (single carrier, compressed or clipped)" : "peakier than Gaussian (bursty, or a carrier plus impulses)"});
    }
private:
    struct Res { double crest = 0, peakDb = 0, meanDb = 0, l10 = 0, l1 = 0, l01 = 0, l001 = 0; double n = 0; };
    void block() {
        have_ = true; t_ += (double)kBlock / fs_;
        hist_.push_back(buf_); if (hist_.size() > (size_t)kKeepBlocks) hist_.pop_front();
        if (++nTot_ % 16 != 0 && have2_) return;                  // the sort of a million samples is done every 16 blocks (about a quarter of a second at 1 Msps)
        have2_ = true;
        std::vector<float> all; all.reserve(hist_.size() * kBlock); for (auto& b : hist_) all.insert(all.end(), b.begin(), b.end());
        double mean = 0; float pk = 0; for (float v : all) { mean += v; pk = std::max(pk, v); } mean /= (double)all.size();
        if (mean < 1e-18) return;
        std::sort(all.begin(), all.end());
        auto q = [&](double tail) { const size_t i = std::min(all.size() - 1, (size_t)((1.0 - tail) * (double)all.size())); return 10 * std::log10(std::max((double)all[i], 1e-20) / mean); };
        Res r; r.meanDb = 10 * std::log10(mean); r.peakDb = 10 * std::log10(std::max((double)pk, 1e-20)); r.crest = r.peakDb - r.meanDb; r.l10 = q(0.1); r.l1 = q(0.01); r.l01 = q(0.001); r.l001 = q(0.0001); r.n = (double)all.size();
        res_ = r;
        out->good((float)r.crest, 0);
        if (t_ - lastRow_ >= 10 || lastRow_ < 0) { lastRow_ = t_; out->row("CCDF", fmt("1 %%: %.1f dB", r.l1), fmt("crest %.1f dB; over the mean power 10 %% %.1f, 1 %% %.1f, 0.1 %% %.1f, 0.01 %% %.1f dB (%.2f M samples)", r.crest, r.l10, r.l1, r.l01, r.l001, r.n / 1e6), (float)r.meanDb); }
    }
    double fs_ = 1, t_ = 0, lastRow_ = -1e9; int nTot_ = 0; bool have_ = false, have2_ = false; std::vector<float> buf_; std::deque<std::vector<float>> hist_; Res res_;
};
}

std::unique_ptr<Decoder> makePapr() { return std::make_unique<PaprDec>(); }

} // namespace dect2::x40
