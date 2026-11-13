// STL link: analogue studio-to-transmitter links (broadcast radio, US Part 74 band 944 - 952 MHz and the like elsewhere) carrying the station's programme as FM: either the whole stereo
// composite (multiplex: mono sum, 19 kHz pilot, 38 kHz stereo difference, RDS on 57 kHz, SCA on 67 / 92 kHz) at about +-60 - 75 kHz deviation in a 250 or 500 kHz channel, or plain mono
// programme audio. The FM receiver of the broadcast mode (FmReceiver: channel filter, discriminator, pilot PLL, stereo matrix, de-emphasis, RDS, SCA) does the demodulation here with the
// settings of a link (stl_cfg.h: deviation, channel width, de-emphasis 75 / 50 / none, stereo or mono, SCA listening). On top of it the mode says what it hears: composite stereo, mono, or a wide signal
// that is not FM at all (a digital STL: QAM or OFDM links with linear PCM or AAC, often encrypted; their frame formats are proprietary and nothing is decoded, the mode only names them and
// measures the channel). Level meters of the sound and the readings of the multiplex are shown. Receive only. Simulated signals only (docs/STL-4.1.md).
#include "x40_dsp.h"
#include "dect2/fm/fm_rx.h"
#include "dect2/x40/stl_cfg.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dect2 {
StlConfig& stlConfig() { static StlConfig c; return c; }
}

namespace dect2::x40 {

namespace {
constexpr int kFft = 1024;
constexpr double kReportSec = 0.5;
constexpr float kDigitalRatioDb = 6.f;     // the channel against the floor beside it, for a signal that is not FM

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
    }
    void feed(const cf32* x, size_t n) override {
        apply();
        fm_->feed(x, n);
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
        t.facts.push_back({"carrier offset", fmt("%+.2f kHz", m.cfoHz / 1000)});
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
        const double dev = c.deviationHz.load(), w = c.channelWidthHz.load(); const int de = c.deemphasisUs.load(), mode = c.mode.load(), sca = c.scaListen.load(), reg = c.rdsRegion.load();
        const float vol = c.volume.load(); const int mu = c.muted.load() ? 1 : 0;
        if (dev != applied_.dev) { fm_->setReferenceDeviation(dev); applied_.dev = dev; }
        if (w != applied_.width) { fm_->setChannelWidth(w > 0 ? w : FmReceiver::kDefaultChannelWidthHz); applied_.width = w; }
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
    // The channel against the floor beside it, from the spectrum of the capture (index kFft / 2 = 0 Hz after the shift)
    void spectrum() {
        spectralOk_ = false; ratioDb_ = 0; flatDb_ = 0; widthKhz_ = 0;
        if (nSeg_ < 8) return;
        const double cw = stlConfig().channelWidthHz.load();
        const double bin = fs_ / kFft, w = std::max(20000.0, cw > 0 ? cw : 220000.0);
        std::vector<double> p(kFft);
        for (int k = 0; k < kFft; k++) p[(k + kFft / 2) % kFft] = psd_[k] / nSeg_;
        std::vector<double> in, edge;
        for (int i = 0; i < kFft; i++) {
            const double a = std::fabs((i - kFft / 2) * bin);
            if (a < 0.4 * w) in.push_back(p[i]);
            else if (a > 0.75 * w && a < 0.4 * fs_) edge.push_back(p[i]);
        }
        if (in.size() < 8 || edge.size() < 24) return;     // the capture is not much wider than the channel: no floor to compare with
        std::sort(edge.begin(), edge.end());
        const double floorP = std::max(edge[edge.size() / 2], 1e-30);
        double mean = 0; for (double v : in) mean += v;
        mean /= (double)in.size();
        std::sort(in.begin(), in.end());
        const double p10 = in[in.size() / 10];
        ratioDb_ = (float)(10 * std::log10(std::max(mean, 1e-30) / floorP));
        flatDb_ = (float)(10 * std::log10(std::max(p10, 1e-30) / std::max(mean, 1e-30)));     // 0 dB: flat; a line spectrum or an FM bell dips far below
        // the occupied width: the bins half way (in power) between the floor and the plateau, counted from the centre outwards
        const double thr = std::sqrt(floorP * std::max(mean, floorP));
        int lo = kFft / 2, hi = kFft / 2;
        while (lo > 1 && p[lo - 1] > thr) lo--;
        while (hi < kFft - 2 && p[hi + 1] > thr) hi++;
        widthKhz_ = (float)((hi - lo + 1) * bin / 1000);
        spectralOk_ = true;
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
        Kind now = kNone;
        if (t.carrier) now = t.pilotPct > 0 ? kStereo : kMono;
        else if (spectralOk_ && ratioDb_ >= kDigitalRatioDb) now = flatDb_ > -3.f && widthKhz_ >= 60.f ? kDigital : kOther;
        // a change must hold for two reports
        if (first_) { kind_ = now; first_ = false; if (now != kNone) announce(); }
        else if (now == kind_) pendN_ = 0;
        else if (now == pending_) { if (++pendN_ >= 2) { kind_ = now; pendN_ = 0; announce(); } }
        else { pending_ = now; pendN_ = 1; }
        haveMeas_ = true;
        if (kind_ == kStereo || kind_ == kMono) out->good(t.snrDb, t.cfoHz);
        else if (kind_ == kDigital || kind_ == kOther) out->energy();
    }
    void announce() {
        const FmTelemetry& t = tel_;
        switch (kind_) {
        case kStereo: out->row("Link", "composite stereo", fmt("pilot %.1f kHz, peak deviation %.1f kHz, offset %+.1f kHz", t.pilotPct * 0.75f, t.devKhz, t.cfoHz / 1000), ratioDb_); break;
        case kMono: out->row("Link", "mono", fmt("no pilot, peak deviation %.1f kHz, offset %+.1f kHz", t.devKhz, t.cfoHz / 1000), ratioDb_); break;
        case kDigital: out->row("Link", "digital-unknown", fmt("not FM, %.0f dB over the floor, about %.0f kHz wide; not decoded", ratioDb_, widthKhz_), ratioDb_); break;
        case kOther: out->row("Link", "unknown", fmt("not FM, %.0f dB over the floor", ratioDb_), ratioDb_); break;
        default: out->row("Link", "lost", "no signal", ratioDb_); break;
        }
    }

    std::unique_ptr<FmReceiver> fm_;
    Applied applied_;
    double fs_ = 0; uint64_t samples_ = 0, fmSeq_ = 0;
    std::vector<float> win_; std::vector<double> psd_ = std::vector<double>(kFft, 0.0); std::vector<cf32> blk_; int nSeg_ = 0, fill_ = 0, skip_ = 0;
    FmTelemetry tel_;
    Kind kind_ = kNone, pending_ = kNone; int pendN_ = 0; bool haveMeas_ = false, first_ = true, spectralOk_ = false;
    double sumSq_ = 0; size_t sumN_ = 0; float peakL_ = 0, peakR_ = 0, rmsDb_ = -120.f, peakDb_ = -120.f, ratioDb_ = 0, flatDb_ = 0, widthKhz_ = 0;
};
}

std::unique_ptr<Decoder> makeStl() { return std::make_unique<StlLink>(); }

} // namespace dect2::x40
