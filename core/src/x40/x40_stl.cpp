// STL link: analogue studio-to-transmitter links (broadcast radio, US Part 74 band 944 - 952 MHz and the like elsewhere) carrying the station's programme as FM: either the whole stereo
// composite (multiplex: mono sum, 19 kHz pilot, 38 kHz stereo difference, RDS on 57 kHz, SCA on 67 / 92 kHz) at about +-60 - 75 kHz deviation in a 250 or 500 kHz channel, or plain mono
// programme audio. The FM receiver of the broadcast mode (FmReceiver: channel filter, discriminator, pilot PLL, stereo matrix, de-emphasis, RDS, SCA) does the demodulation here with the
// settings of a link (stl_cfg.h: deviation, channel width, de-emphasis 75 / 50 / none, stereo or mono, SCA listening). On top of it the mode says what it hears: composite stereo, mono, or a wide signal
// that is not FM at all (a digital STL: QAM or OFDM links with linear PCM or AAC, often encrypted; their frame formats are proprietary and nothing is decoded, the mode only names them and
// measures the channel). The channel filter follows the signal (stl_cfg.h autoWidth): Carson's rule on the measured peak deviation and the content of the multiplex (60 kHz with RDS, 99 kHz with SCA), or the
// occupied width of the capture if that is more, plus a margin; the carrier offset is measured and removed first, so the filter is centred on the link. Level meters of the sound and the readings of the multiplex are shown. Receive only. Simulated signals only (docs/STL-4.1.md).
#include "x40_dsp.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/x40/stl_cfg.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace dect2 {
StlConfig& stlConfig() { static StlConfig c; return c; }
}

namespace dect2::x40 {

namespace {
constexpr int kFft = 1024;
constexpr double kReportSec = 0.5;
constexpr float kDigitalRatioDb = 6.f;
constexpr double kStartWidth = 400000;     // the automatic channel width before anything is measured: Carson for +-75 kHz with SCA is 348 kHz
using cf64 = std::complex<double>;     // the channel against the floor beside it, for a signal that is not FM

enum Kind { kNone, kStereo, kMono, kDigital, kOther };

class StlLink : public Decoder {
public:
    bool configure(double fs) override {
        fs_ = fs;
        fm_ = std::make_unique<FmReceiver>();
        StlConfig& c = stlConfig();
        fm_->setSilent(c.silent.load());
        fm_->setStationFinder(false);                // a live link: the channel is where it was tuned
        fm_->setNeighbourCancel(false);
        applied_ = Applied{};
        apply();
        fm_->setAudioTap([this](const float* l, const float* r, size_t n) { audioIn(l, r, n); });
        fm_->configure(fs);
        if (!fm_->ready()) return false;
        win_.resize(kFft);
        for (int i = 0; i < kFft; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft));
        reset();
        return true;
    }
    void reset() override {
        if (fm_) fm_->reset();
        psd_.assign(kFft, 0.0); nSeg_ = 0; fill_ = 0; blk_.assign(kFft, cf32());
        samples_ = 0; skip_ = 0; kind_ = kNone; pending_ = kNone; pendN_ = 0; haveMeas_ = false; first_ = true;
        sumSq_ = 0; peakL_ = peakR_ = 0; sumN_ = 0; ratioDb_ = 0; flatDb_ = 0; widthKhz_ = 0; spectralOk_ = false; fmSeq_ = 0; tel_ = FmTelemetry{};
        rmsDb_ = peakDb_ = -120.f;
        shift_ = 0; osc_ = cf64(1, 0); rot_ = cf64(1, 0); autoW_ = kStartWidth; devHold_ = 0; shrinkN_ = 0; settled_ = false; hasRegion_ = false; occKhz_ = 0; centroidHz_ = 0; applied_.width = -1;
    }
    void feed(const cf32* x, size_t n) override {
        apply();
        if (shift_ != 0) {                                       // the carrier to 0 Hz, phase continuous (oscillator by recurrence, renormalised per block)
            mix_.resize(n);
            for (size_t i = 0; i < n; i++) { mix_[i] = x[i] * cf32((float)osc_.real(), (float)osc_.imag()); osc_ *= rot_; }
            osc_ /= std::abs(osc_);
            fm_->feed(mix_.data(), n);
        } else fm_->feed(x, n);
        for (size_t i = 0; i < n; i++) {                         // the spectrum of the capture: every fourth block of 1024 samples
            blk_[fill_++] = x[i];
            if (fill_ == kFft) { fill_ = 0; if ((skip_++ & 3) == 0) segment(); }
        }
        samples_ += n;
        if (samples_ >= (uint64_t)(kReportSec * fs_)) { samples_ = 0; report(); }
    }
    void fill(X40Telemetry& t) const override {
        if (!haveMeas_) { t.headline = "waiting for data"; return; }
        const FmTelemetry& m = tel_;
        switch (kind_) {
        case kStereo: t.headline = fmt("composite stereo, peak %.1f kHz, pilot %.1f kHz, %s", m.devKhz, m.pilotPct * 0.75f, m.rdsSync ? (m.psName.empty() ? "RDS synced" : m.psName.c_str()) : "no RDS"); break;
        case kMono: t.headline = fmt("mono (no pilot), peak %.1f kHz, audio SNR %.0f dB", m.devKhz, m.snrDb); break;
        case kDigital: t.headline = fmt("digital or unknown link (not FM), %.0f dB over the floor, about %.0f kHz wide: not decoded", ratioDb_, widthKhz_); break;
        case kOther: t.headline = fmt("unknown signal (not FM), %.0f dB over the floor", ratioDb_); break;
        default: t.headline = "no link"; break;
        }
        const char* what = kind_ == kStereo ? "composite (stereo multiplex)" : kind_ == kMono ? "mono programme audio (or a mono multiplex)" : kind_ == kDigital ? "digital-unknown" : kind_ == kOther ? "unknown, not FM" : "nothing";
        t.facts.push_back({"what is this", what});
        t.facts.push_back({"peak deviation", fmt("%.1f kHz (100 %% = %.0f kHz)", m.devKhz, stlConfig().deviationHz.load() / 1000)});
        t.facts.push_back({"carrier offset", fmt("%+.2f kHz", (m.cfoHz + shift_) / 1000)});
        t.facts.push_back({"channel width", fmt("%.0f kHz (%s)", applied_.width > 0 ? applied_.width / 1000 : 0.0, stlConfig().autoWidth.load() ? "automatic: Carson + margin" : "manual")});
        t.facts.push_back({"audio SNR", fmt("%.0f dB", m.snrDb)});
        t.facts.push_back({"pilot 19 kHz", kind_ == kStereo ? fmt("locked, %.1f kHz", m.pilotPct * 0.75f) : std::string("absent")});
        t.facts.push_back({"RDS", m.rdsSync ? fmt("synced, %.0f %% good blocks, PS \"%s\"", m.rdsBlockOkPct, m.psName.c_str()) : std::string("none")});
        const std::string s67 = m.sub.sca[0].present ? fmt("%.0f dB", m.sub.sca[0].snrDb) : std::string("absent"), s92 = m.sub.sca[1].present ? fmt("%.0f dB", m.sub.sca[1].snrDb) : std::string("absent");
        t.facts.push_back({"SCA 67 / 92 kHz", s67 + " / " + s92});
        t.facts.push_back({"sound level", fmt("peak %.0f dBFS, RMS %.0f dBFS", peakDb_, rmsDb_)});
        if (spectralOk_) t.facts.push_back({"channel over its floor", fmt("%.1f dB (flatness %.1f dB)", ratioDb_, flatDb_)});
        else t.facts.push_back({"channel over its floor", "not measurable: the channel is as wide as the capture"});
    }
private:
    struct Applied { double dev = -1, width = -1; int de = -1, mode = -1, sca = -2, region = -1; float vol = -1; int muted = -1; };

    void apply() {
        StlConfig& c = stlConfig();
        const double dev = c.deviationHz.load(), w = c.autoWidth.load() ? autoW_ : c.channelWidthHz.load(); const int de = c.deemphasisUs.load(), mode = c.mode.load(), sca = c.scaListen.load(), reg = c.rdsRegion.load();
        const float vol = c.volume.load(); const int mu = c.muted.load() ? 1 : 0;
        if (dev != applied_.dev) { fm_->setReferenceDeviation(dev); applied_.dev = dev; }
        if (w != applied_.width) { fm_->setChannelWidth(w > 0 ? w : FmReceiver::kDefaultChannelWidthHz); applied_.width = w; c.widthNowHz = w > 0 ? w : FmReceiver::kDefaultChannelWidthHz; }
        if (de != applied_.de) { fm_->setDeemphasis(de); applied_.de = de; }
        if (mode != applied_.mode) { fm_->setStereoMode(mode == 1 ? 2 : mode == 2 ? 1 : 0); applied_.mode = mode; }
        if (sca != applied_.sca) { fm_->setScaListen(sca); applied_.sca = sca; }
        if (reg != applied_.region) { fm_->setRdsRegion(reg, 0); applied_.region = reg; }
        if (vol != applied_.vol) { fm_->setVolume(vol); applied_.vol = vol; }
        if (mu != applied_.muted) { fm_->setMuted(mu != 0); applied_.muted = mu; }
    }
    void audioIn(const float* l, const float* r, size_t n) {
        for (size_t i = 0; i < n; i++) { sumSq_ += 0.5 * ((double)l[i] * l[i] + (double)r[i] * r[i]); peakL_ = std::max(peakL_, std::fabs(l[i])); peakR_ = std::max(peakR_, std::fabs(r[i])); }
        sumN_ += n;
        const auto& tap = stlConfig().audioTap; if (tap) tap(l, r, n);
    }
    void segment() {
        std::vector<cf32> a(kFft);
        for (int i = 0; i < kFft; i++) a[i] = blk_[i] * win_[i];
        fftx(a);
        for (int k = 0; k < kFft; k++) psd_[k] += (double)std::norm(a[k]);
        nSeg_++;
    }
    // The signal against the floor beside it, from the spectrum of the capture (index kFft / 2 = 0 Hz after the shift): the occupied region around the strongest bin
    // (smoothed bins 3 dB over the floor), its width and centroid, its level over the floor and how flat it is
    void spectrum() {
        spectralOk_ = false; hasRegion_ = false; ratioDb_ = 0; flatDb_ = 0; widthKhz_ = 0; occKhz_ = 0;
        if (nSeg_ < 8) return;
        const double bin = fs_ / kFft;
        std::vector<double> p(kFft), sm(kFft);
        for (int k = 0; k < kFft; k++) p[(k + kFft / 2) % kFft] = psd_[k] / nSeg_;
        const double wRef = std::max(100000.0, autoW_);
        std::vector<double> edge;
        for (int i = 0; i < kFft; i++) { const double a = std::fabs((i - kFft / 2) * bin); if (a > 0.75 * wRef && a < 0.4 * fs_) edge.push_back(p[i]); }
        if (edge.size() < 24) return;                       // the capture is not much wider than the channel: no floor to compare with
        std::sort(edge.begin(), edge.end());
        const double floorP = std::max(edge[edge.size() / 2], 1e-30);
        for (int i = 0; i < kFft; i++) { double a = 0; int c = 0; for (int k = std::max(0, i - 4); k <= std::min(kFft - 1, i + 4); k++) { a += p[k]; c++; } sm[i] = a / c; }
        const int lim = (int)(0.4 * fs_ / bin);
        int pk = kFft / 2;
        for (int i = kFft / 2 - lim; i <= kFft / 2 + lim; i++) if (i > 0 && i < kFft - 1 && sm[i] > sm[pk]) pk = i;
        spectralOk_ = true;
        ratioDb_ = (float)(10 * std::log10(std::max(sm[pk], 1e-30) / floorP));
        if (sm[pk] < 2 * floorP) return;                    // nothing stands out
        int lo = pk, hi = pk;
        while (lo > kFft / 2 - lim && sm[lo - 1] > 2 * floorP) lo--;
        while (hi < kFft / 2 + lim && sm[hi + 1] > 2 * floorP) hi++;
        const int cnt = hi - lo + 1;
        if (cnt < 8) return;
        occKhz_ = (float)(cnt * bin / 1000); widthKhz_ = occKhz_;
        double sw = 0, sf = 0;
        for (int i = lo; i <= hi; i++) { const double e = std::max(0.0, p[i] - floorP); sw += e; sf += e * (i - kFft / 2) * bin; }
        centroidHz_ = sw > 0 ? sf / sw : 0;
        const int c0 = lo + cnt / 10, c1 = hi - cnt / 10;
        std::vector<double> in(p.begin() + c0, p.begin() + c1 + 1);
        double mean = 0; for (double v : in) mean += v;
        mean /= (double)in.size();
        std::sort(in.begin(), in.end());
        ratioDb_ = (float)(10 * std::log10(std::max(mean, 1e-30) / floorP));
        flatDb_ = (float)(10 * std::log10(std::max(in[in.size() / 10], 1e-30) / std::max(mean, 1e-30)));   // 0 dB: flat; a line spectrum or an FM bell dips far below
        hasRegion_ = true;
    }
    // Channel width and centre follow the link: Carson's rule 2 (peak deviation + highest multiplex frequency) on the measured deviation and the content (RDS 60 kHz, SCA 99 kHz),
    // or the occupied width of the capture, whichever is more, plus 35 % and 10 kHz. Wider at once, narrower only after 3 s of asking for it (the filter starts empty at every change).
    void follow(const FmTelemetry& t) {
        if (!t.carrier) { devHold_ *= 0.9; return; }
        devHold_ = std::max(devHold_ * 0.9, (double)t.devKhz * 1000);
        const bool sca = t.sub.sca[0].present || t.sub.sca[1].present;
        const double fmax = sca ? 99000.0 : 60000.0;
        double need = 2 * (std::max(devHold_, 20000.0) + fmax);
        if (hasRegion_) need = std::max(need, (double)occKhz_ * 1000);
        need = need * 1.35 + 10000;   // Carson holds about 98 % of the power: the tails beyond it, cut by the filter, were a distortion floor of -46 dB in the tests (a margin of 10 % was not enough)
        const double top = 0.9 * fs_;
        need = std::min(std::max(need, 100000.0), top);
        if (need > autoW_ * 1.05) { autoW_ = need; shrinkN_ = 0; }
        else if (need < autoW_ * 0.8) {      // narrower: once early, when the readings have settled (1.5 s), later only after 30 s of asking (every change leaves the filter empty: a click in the sound)
            if (++shrinkN_ >= (settled_ ? 60 : 3)) { autoW_ = need; shrinkN_ = 0; settled_ = true; }
        }
        else shrinkN_ = 0;
        // the centre: the occupied region's centroid first, then the carrier's own offset after the shift
        // (no feedback from the receiver's own offset reading: that loop hunted and put steps into the sound; the readout adds the receiver's residual to the shift)
        if (hasRegion_ && std::fabs(centroidHz_ - shift_) > 4000) shift_ = centroidHz_;
        rot_ = cf64(std::cos(-2 * kPi * shift_ / fs_), std::sin(-2 * kPi * shift_ / fs_));
    }
    void report() {
        spectrum();
        std::fill(psd_.begin(), psd_.end(), 0.0); nSeg_ = 0;
        FmTelemetry m;
        if (fm_->telemetry(m, fmSeq_)) { fmSeq_ = m.seq; tel_ = m; }
        const FmTelemetry& t = tel_;
        if (sumN_) {
            const double rms = std::sqrt(sumSq_ / (double)sumN_);
            rmsDb_ = (float)(20 * std::log10(std::max(rms, 1e-6))); peakDb_ = (float)(20 * std::log10(std::max((double)std::max(peakL_, peakR_), 1e-6)));
        } else rmsDb_ = peakDb_ = -120.f;
        sumSq_ = 0; sumN_ = 0; peakL_ = peakR_ = 0;
        stlConfig().peakDbfs = peakDb_; stlConfig().rmsDbfs = rmsDb_;
        if (stlConfig().autoWidth.load()) follow(t);
        Kind now = kNone;
        if (t.carrier) now = t.pilotPct > 0 ? kStereo : kMono;
        else if (hasRegion_ && ratioDb_ >= kDigitalRatioDb) now = flatDb_ > -3.f && widthKhz_ >= 60.f ? kDigital : kOther;
        // a change must hold for two reports
        if (first_) { kind_ = now; first_ = false; if (now != kNone) announce(); }
        else if (now == kind_) pendN_ = 0;
        else if (now == pending_) { if (++pendN_ >= 2) { kind_ = now; pendN_ = 0; announce(); } }
        else { pending_ = now; pendN_ = 1; }
        haveMeas_ = true;
        if (kind_ == kStereo || kind_ == kMono) out->good(t.snrDb, t.cfoHz + shift_);
        else if (kind_ == kDigital || kind_ == kOther) out->energy();
    }
    void announce() {
        const FmTelemetry& t = tel_;
        switch (kind_) {
        case kStereo: out->row("Link", "composite stereo", fmt("pilot %.1f kHz, peak deviation %.1f kHz, offset %+.1f kHz", t.pilotPct * 0.75f, t.devKhz, (t.cfoHz + shift_) / 1000), ratioDb_); break;
        case kMono: out->row("Link", "mono", fmt("no pilot, peak deviation %.1f kHz, offset %+.1f kHz", t.devKhz, (t.cfoHz + shift_) / 1000), ratioDb_); break;
        case kDigital: out->row("Link", "digital-unknown", fmt("not FM, %.0f dB over the floor, about %.0f kHz wide; not decoded", ratioDb_, widthKhz_), ratioDb_); break;
        case kOther: out->row("Link", "unknown", fmt("not FM, %.0f dB over the floor", ratioDb_), ratioDb_); break;
        default: out->row("Link", "lost", "no signal", ratioDb_); break;
        }
    }

    std::unique_ptr<FmReceiver> fm_;
    Applied applied_;
    double fs_ = 0; uint64_t samples_ = 0, fmSeq_ = 0;
    double shift_ = 0, autoW_ = kStartWidth, devHold_ = 0, centroidHz_ = 0; cf64 osc_{1, 0}, rot_{1, 0}; int shrinkN_ = 0; bool settled_ = false; bool hasRegion_ = false; float occKhz_ = 0; std::vector<cf32> mix_;
    std::vector<float> win_; std::vector<double> psd_ = std::vector<double>(kFft, 0.0); std::vector<cf32> blk_; int nSeg_ = 0, fill_ = 0, skip_ = 0;
    FmTelemetry tel_;
    Kind kind_ = kNone, pending_ = kNone; int pendN_ = 0; bool haveMeas_ = false, first_ = true, spectralOk_ = false;
    double sumSq_ = 0; size_t sumN_ = 0; float peakL_ = 0, peakR_ = 0, rmsDb_ = -120.f, peakDb_ = -120.f, ratioDb_ = 0, flatDb_ = 0, widthKhz_ = 0;
};
}

std::unique_ptr<Decoder> makeStl() { return std::make_unique<StlLink>(); }

} // namespace dect2::x40
