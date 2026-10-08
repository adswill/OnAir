#include "dect2/marine_gen.h"
#include "dect2/gen_util.h"
#include "dect2/marine_dsc.h"
#include "dect2/marine_fax.h"
#include "dect2/marine_navtex.h"
#include "dect2/marine_rx.h"
#include <algorithm>
#include <cmath>
#include <deque>

namespace dect2 {

using namespace marine;

namespace {

const double kPi = 3.14159265358979323846;
constexpr double kBase = 24000.0;

// ---- sources at 24 kHz, complex baseband with the carrier at 0 Hz
class BaseSource {
public:
    virtual ~BaseSource() = default;
    virtual void next(cf32* out, size_t n) = 0;
    virtual double power() const = 0;          // mean power of the signal while it is on
};

// Continuous phase FSK from a list of segments. Bit 1 = the higher tone for NAVTEX, the lower for DSC (the caller maps).
class FskSource : public BaseSource {
public:
    // each segment: bits to send at `baud`, or silence (bits empty, silenceSec > 0)
    struct Segment { std::vector<uint8_t> bits; double silenceSec = 0; };
    FskSource(std::vector<Segment> segs, double baud, double shiftHz, bool highIsOne) : segs_(std::move(segs)), baud_(baud), half_(shiftHz / 2), highIsOne_(highIsOne) {
        spb_ = kBase / baud_;
        loadSegment();
    }
    double power() const override { return 1.0; }
    void next(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            if (silent_) {
                out[i] = cf32(0, 0);
                if (--silentLeft_ <= 0) advance();
                continue;
            }
            const int bit = segs_[seg_].bits[bitIdx_];
            const double f = ((bit != 0) == highIsOne_) ? half_ : -half_;
            ph_ += 2 * kPi * f / kBase;
            if (ph_ > kPi) ph_ -= 2 * kPi; else if (ph_ < -kPi) ph_ += 2 * kPi;
            out[i] = cf32((float)std::cos(ph_), (float)std::sin(ph_));
            acc_ += 1.0;
            if (acc_ >= spb_) {
                acc_ -= spb_;
                if (++bitIdx_ >= segs_[seg_].bits.size()) advance();
            }
        }
    }
private:
    void advance() { seg_ = (seg_ + 1) % segs_.size(); loadSegment(); }
    void loadSegment() {
        bitIdx_ = 0; acc_ = 0;
        const auto& s = segs_[seg_];
        if (s.bits.empty()) { silent_ = true; silentLeft_ = (long)std::lround(s.silenceSec * kBase); if (silentLeft_ < 1) silentLeft_ = 1; }
        else silent_ = false;
    }
    std::vector<Segment> segs_;
    double baud_, half_, spb_;
    bool highIsOne_;
    size_t seg_ = 0, bitIdx_ = 0;
    double acc_ = 0, ph_ = 0;
    bool silent_ = false; long silentLeft_ = 0;
};

// VHF channel 70: AFSK 1300 (Y = 1) / 2100 Hz (B = 0), 1200 baud, frequency modulation
class VhfSource : public BaseSource {
public:
    VhfSource(std::vector<FskSource::Segment> segs) : segs_(std::move(segs)) { load(); }
    double power() const override { return 1.0; }
    void next(cf32* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            if (silent_) { out[i] = cf32(0, 0); if (--left_ <= 0) advance(); continue; }
            const int bit = segs_[seg_].bits[bit_];
            const double f = bit ? 1300.0 : 2100.0;
            aph_ += 2 * kPi * f / kBase;
            if (aph_ > kPi) aph_ -= 2 * kPi;
            ph_ += 2 * kPi * 3000.0 * std::sin(aph_) / kBase;          // deviation 3 kHz
            if (ph_ > kPi) ph_ -= 2 * kPi; else if (ph_ < -kPi) ph_ += 2 * kPi;
            out[i] = cf32((float)std::cos(ph_), (float)std::sin(ph_));
            if (++cnt_ >= 20) { cnt_ = 0; if (++bit_ >= segs_[seg_].bits.size()) advance(); }
        }
    }
private:
    void advance() { seg_ = (seg_ + 1) % segs_.size(); load(); }
    void load() { bit_ = 0; cnt_ = 0; const auto& s = segs_[seg_]; silent_ = s.bits.empty(); left_ = std::max(1L, (long)std::lround(s.silenceSec * kBase)); }
    std::vector<FskSource::Segment> segs_;
    size_t seg_ = 0, bit_ = 0;
    int cnt_ = 0;
    double aph_ = 0, ph_ = 0;
    bool silent_ = false; long left_ = 0;
};

// Fax audio -> analytic signal (USB) with a Hilbert FIR
class FaxSource : public BaseSource {
public:
    FaxSource(int ioc, int lpm, int lines, uint32_t seed, double phasingSec) : src_(kBase, ioc, lpm, lines, seed) {
        if (phasingSec > 0) src_.setTiming(5.0, phasingSec, 5.0, 10.0);
        h_.resize(kN);
        const int m = kN / 2;
        for (int i = 0; i < kN; i++) {
            const int t = i - m;
            double v = 0;
            if (t % 2 != 0) {
                const double r = (double)t / (m + 1);
                double w = 0, x = 7.0 * std::sqrt(std::max(0.0, 1 - r * r)), s = 1, term = 1;
                for (int k = 1; k < 40; k++) { term *= (x / (2.0 * k)) * (x / (2.0 * k)); s += term; }
                double s0 = 1, t0 = 1; for (int k = 1; k < 40; k++) { t0 *= (7.0 / (2.0 * k)) * (7.0 / (2.0 * k)); s0 += t0; }
                w = s / s0;
                v = 2.0 / (kPi * t) * w;
            }
            h_[(size_t)i] = (float)v;
        }
        ring_.assign(kN, 0.f);
    }
    double power() const override { return 0.25; }
    void next(cf32* out, size_t n) override {
        std::vector<float> a(n);
        src_.generate(a.data(), n);
        const int m = kN / 2;
        for (size_t i = 0; i < n; i++) {
            ring_[(size_t)pos_] = a[i];
            if (++pos_ >= kN) pos_ = 0;
            float im = 0;
            int idx = pos_;                          // oldest first
            for (int k = kN - 1; k >= 0; k--) { im += h_[(size_t)k] * ring_[(size_t)idx]; if (++idx >= kN) idx = 0; }
            // the real part is the audio delayed by m samples
            const float re = ring_[(size_t)((pos_ + kN - 1 - m + kN) % kN)];
            out[i] = cf32(re, im);
        }
    }
private:
    static constexpr int kN = 255;
    FaxAudioSource src_;
    std::vector<float> h_, ring_;
    int pos_ = 0;
};

// two Rayleigh paths, 1 ms apart, 0.5 Hz Doppler spread
class Fader {
public:
    explicit Fader(uint32_t seed) : rng_(seed * 2654435761u + 7) {
        for (int p = 0; p < 2; p++) { g_[p] = cf32((float)gauss(), (float)gauss()); prev_[p] = g_[p]; }
        hist_.assign(kDelay + 1, cf32(0, 0));
    }
    void process(cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            if (cnt_ == 0) {
                for (int p = 0; p < 2; p++) {
                    prev_[p] = g_[p];
                    const float rho = 0.9695f, s = std::sqrt(1.f - rho * rho);
                    g_[p] = rho * g_[p] + s * cf32((float)gauss(), (float)gauss());
                }
            }
            const float a = (float)cnt_ / kStep;
            const cf32 g1 = prev_[0] + a * (g_[0] - prev_[0]), g2 = prev_[1] + a * (g_[1] - prev_[1]);
            if (++cnt_ >= kStep) cnt_ = 0;
            hist_[(size_t)hp_] = x[i];
            const cf32 d = hist_[(size_t)((hp_ + kDelay + 1 - kDelay) % (kDelay + 1))];   // 24 samples = 1 ms ago
            hp_ = (hp_ + 1) % (kDelay + 1);
            x[i] = 0.70710678f * (g1 * x[i] + g2 * d);
        }
    }
private:
    double gauss() {                                           // N(0, 0.5): the complex gain has unit power
        auto u = [&] { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5; return ((double)(rng_ & 0xFFFFFF) + 0.5) / 16777216.0; };
        return std::sqrt(-2.0 * std::log(u())) * std::cos(2 * kPi * u()) * 0.70710678;
    }
    static constexpr int kDelay = 24, kStep = 240;
    uint32_t rng_;
    cf32 g_[2], prev_[2];
    std::vector<cf32> hist_;
    int hp_ = 0, cnt_ = 0;
};

std::string navtexTexts(int i) {
    static const char* t[3] = {
        "ZCZC AA01\r\n260630 UTC OCT 26\r\nNAVAREA I WARNING 331\r\nBRITISH ISLES. ENGLISH CHANNEL.\r\nOFF DOVER TRAFFIC SEPARATION SCHEME\r\n"
        "DERELICT FISHING VESSEL ADRIFT IN 50-52.3N 001-24.8E.\r\nMARINERS ARE REQUESTED TO KEEP A SHARP LOOKOUT.\r\nCANCEL THIS MESSAGE 270600 UTC OCT 26\r\nNNNN\r\n",
        "ZCZC AB02\r\n260700 UTC OCT 26\r\nGALE WARNING 118\r\nSHIPPING FORECAST AREAS DOVER WIGHT PORTLAND\r\nSOUTHWEST GALE FORCE 8 EXPECTED, SEVERE GALE FORCE 9\r\n"
        "LATER TODAY. WIND BACKING SOUTHEAST 7 TO 9 WITH\r\nRAIN AND POOR VISIBILITY.\r\nNNNN\r\n",
        "ZCZC AE03\r\n260900 UTC OCT 26\r\nINSHORE WATERS FORECAST FOR 24 HOURS\r\nDOVER STRAIT: SOUTHWEST 6 TO GALE 8, OCCASIONALLY SEVERE\r\n"
        "GALE 9. RAIN. MODERATE OR POOR. SEA STATE ROUGH OR VERY ROUGH.\r\nOUTLOOK: WEST 5 TO 7, VISIBILITY GOOD.\r\nNNNN\r\n"};
    static const char* shortMsg = "ZCZC AA09\r\n010000 UTC JAN 26\r\nTEST MESSAGE FROM STATION A. 518 KHZ.\r\nNNNN\r\n";
    return i == 3 ? shortMsg : t[i % 3];
}

std::vector<FskSource::Segment> navtexSegments(double idleSec, int which) {
    std::vector<FskSource::Segment> segs;
    const int idleTokens = std::max(20, (int)std::lround(idleSec * 100.0 / 14.0));       // one DX token = two characters of 70 ms
    for (int mm = 0; mm < (which == 0 ? 3 : 1); mm++) {
        const int m = which == 0 ? mm : which - 1;
        std::vector<int> tok(idleTokens, -1);
        for (uint8_t c : navtexEncode(navtexTexts(m))) tok.push_back(c);
        for (int i = 0; i < 12; i++) tok.push_back(-1);
        FskSource::Segment s;
        s.bits = codesToBits(sitorBSlots(tok));
        segs.push_back(std::move(s));
    }
    return segs;
}

std::vector<FskSource::Segment> dscSegments(bool vhf) {
    std::vector<FskSource::Segment> segs;
    const int dotHf = 200, dotShort = 20;
    const double gap = vhf ? 1.2 : 2.5;
    auto add = [&](const std::vector<int>& body, int eos, int dots) {
        FskSource::Segment s; s.bits = dscFrameBits(body, eos, dots); segs.push_back(std::move(s));
        FskSource::Segment g; g.silenceSec = gap; segs.push_back(std::move(g));
    };
    const int dotsDist = vhf ? dotShort : dotHf;
    add(dscBuildDistress("232123456", 103, 50.85, -1.3, 12, 34, vhf ? 100 : 109), 127, dotsDist);                                 // grounding, 50d51'N 001d18'W
    add(dscBuildAllShips("235012345", 108, vhf ? 100 : 109, 126, vhf ? 900006 : 82910, -1), 127, dotsDist);                       // safety, working channel / frequency
    add(dscBuildIndividual("002320064", 100, "232123456", vhf ? 100 : 109, 126, vhf ? 900006 : 82910, vhf ? 900006 : 82910), 117, dotShort);   // routine to a coast station
    add(dscBuildDistressAck("002320064", "232123456", 103, 50.85, -1.3, 12, 34, vhf ? 100 : 109), 127, dotsDist);                // acknowledgement of the distress alert
    return segs;
}

class MarineSynth : public ModeSynth {
public:
    MarineSynth(const SynthConfig& cfg, double rate, double channelOffsetHz) : cfg_(cfg), rate_(rate), off_(channelOffsetHz), noise_(17) {
        build();
    }
    double sampleRate() const override { return rate_; }

    void generate(cf32* out, size_t n) override {
        const double step = kBase / rate_ * (1.0 + cfg_.sroPpm * 1e-6);
        for (size_t i = 0; i < n; i++) {
            // need base samples up to index floor(pos_) + 2
            while ((size_t)pos_ + 3 >= base_.size()) refill();
            const size_t i0 = (size_t)pos_;
            const float t = (float)(pos_ - (double)i0);
            const cf32 p0 = base_[i0 - 1], p1 = base_[i0], p2 = base_[i0 + 1], p3 = base_[i0 + 2];
            // cubic (Catmull-Rom) interpolation
            const float a0 = -0.5f * t * t * t + t * t - 0.5f * t;
            const float a1 = 1.5f * t * t * t - 2.5f * t * t + 1.f;
            const float a2 = -1.5f * t * t * t + 2.f * t * t + 0.5f * t;
            const float a3 = 0.5f * t * t * t - 0.5f * t * t;
            const cf32 y = a0 * p0 + a1 * p1 + a2 * p2 + a3 * p3;
            out[i] = y * (float)amp_ * cf32((float)rr_, (float)ri_);
            // the output oscillator: the channel offset plus the carrier offset
            const double nr = rr_ * sr_ - ri_ * si_; ri_ = rr_ * si_ + ri_ * sr_; rr_ = nr;
            pos_ += step;
            if (++rcount_ >= 4096) { rcount_ = 0; const double g = 1.0 / std::sqrt(rr_ * rr_ + ri_ * ri_); rr_ *= g; ri_ *= g; }
        }
        noise_.add(out, n, (float)sigma_);
        // drop what has been used
        if ((size_t)pos_ > 8192) { const size_t drop = (size_t)pos_ - 4; base_.erase(base_.begin(), base_.begin() + (long)drop); pos_ -= (double)drop; }
    }

private:
    void build() {
        const int svc = cfg_.modeOpt[0];
        const double mist = cfg_.modeVal[0];
        const double idle = cfg_.modeVal[1] > 0 ? cfg_.modeVal[1] : 6.0;
        if (svc == 2) src_ = std::make_unique<FskSource>(dscSegments(false), 100.0, 170.0, false);       // M.493: the higher tone is B = 0
        else if (svc == 4) src_ = std::make_unique<VhfSource>(dscSegments(true));
        else if (svc == 3) src_ = std::make_unique<FaxSource>(cfg_.modeOpt[4] == 288 ? 288 : 576, cfg_.modeOpt[3] > 0 ? cfg_.modeOpt[3] : 120, cfg_.modeOpt[5] > 0 ? cfg_.modeOpt[5] : 800, 5, cfg_.modeVal[2]);
        else src_ = std::make_unique<FskSource>(navtexSegments(idle, cfg_.modeOpt[2]), 100.0, 170.0, true);
        if (cfg_.modeOpt[1] == 1 && svc != 4) fader_ = std::make_unique<Fader>(3);
        double snr = std::pow(10.0, cfg_.snrDb / 10.0);
        const double ratio = rate_ / (svc == 4 ? 12500.0 : 3000.0) / snr;     // noise power over signal power, whole band (reference band: 3 kHz, VHF channel 12.5 kHz)
        const double ps = src_->power();
        const double total = 0.04;                                      // rms 0.2
        amp_ = std::sqrt(total / (ps * (1.0 + ratio)));
        sigma_ = amp_ * std::sqrt(ps * ratio / 2.0);                    // per real component
        const double w = 2 * kPi * (off_ + cfg_.cfoHz + mist) / rate_;
        sr_ = std::cos(w); si_ = std::sin(w);
        base_.assign(4, cf32(0, 0));
        pos_ = 2;
    }
    void refill() {
        const size_t blk = 480;
        std::vector<cf32> t(blk);
        src_->next(t.data(), blk);
        if (fader_) fader_->process(t.data(), blk);
        base_.insert(base_.end(), t.begin(), t.end());
    }
    SynthConfig cfg_;
    double rate_, off_;
    std::unique_ptr<BaseSource> src_;
    std::unique_ptr<Fader> fader_;
    std::vector<cf32> base_;
    double pos_ = 2, amp_ = 0.2, sigma_ = 0;
    double rr_ = 1, ri_ = 0, sr_ = 1, si_ = 0;
    int rcount_ = 0;
    genutil::NoiseSource noise_;
};

} // namespace

std::unique_ptr<ModeSynth> makeMarineSynthAt(const SynthConfig& cfg, double sampleRate, double channelOffsetHz) {
    if (sampleRate < 250000) return nullptr;
    return std::make_unique<MarineSynth>(cfg, sampleRate, channelOffsetHz);
}

std::unique_ptr<ModeSynth> makeMarineSynth(const SynthConfig& cfg, double sampleRate) {
    return makeMarineSynthAt(cfg, sampleRate, -marineTuning().tuneOffsetHz);
}

} // namespace dect2
