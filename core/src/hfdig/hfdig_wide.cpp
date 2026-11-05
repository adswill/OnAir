// Whole-span decoding of the HF digital receiver (see hfdig_wide.h).
#include "dect2/hfdig/hfdig_wide.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/marine/marine_dsp.h"
#include "dect2/engine/widespan.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include "dect2/common/mathconst.h"

namespace dect2 {

namespace {

const FtxSlot kSlots[] = {
    // FT8 (WSJT-X default dial frequencies)
    {1840000, 0}, {3573000, 0}, {5357000, 0}, {7074000, 0}, {10136000, 0}, {14074000, 0}, {18100000, 0}, {21074000, 0}, {24915000, 0},
    {28074000, 0}, {50313000, 0},
    // FT2 (WSJT-X Improved, data/frequencies/hf.tsv)
    {14084000, 2},
    // FT4
    {3575000, 1}, {7047500, 1}, {10140000, 1}, {14080000, 1}, {18104000, 1}, {21140000, 1}, {24919000, 1}, {28180000, 1}, {50318000, 1},
    // WSPR (dial = centre of the 200 Hz sub-band minus 1500 Hz)
    {1836600, 3}, {3568600, 3}, {5364700, 3}, {7038600, 3}, {10138700, 3}, {14095600, 3}, {18104600, 3}, {21094600, 3}, {24924600, 3},
    {28124600, 3}, {50293000, 3},
};

using dect2::math::kPi;
constexpr double kFs = kHfdigAudioRate;
constexpr int kFft = 2048, kHop = 1024;
constexpr double kBinHz = kFs / kFft;
constexpr int kBinLo = 77, kBinHi = 844;   // 300 .. 3300 Hz
constexpr double kLockRatio = 251.0, kKeepRatio = 100.0;
constexpr double kBauds[3] = {45.45, 50.0, 75.0};
constexpr int kBaudIdx[3] = {0, 1, 2};
constexpr int kShiftIdx[3] = {0, 2, 4};      // 170, 425, 850 Hz
constexpr double kShiftHz[3] = {170.0, 425.0, 850.0};
constexpr double kRttyStep = 2150.0;         // channel spacing: a pair of 850 Hz shift always lies inside one 3000 Hz window

// ---------------------------------------------------------------------------------------------------------------------------------
// One RTTY signal: a pair of tones and the decoders that try its possible speeds and polarities
struct Track {
    double lo = 0;
    int shiftSel = 0;
    std::vector<std::unique_ptr<HfdigRtty>> dec;     // 3 bauds x 2 polarities until resolved
    std::vector<std::pair<int, bool>> cfg;           // baud index, reverse
    int best = -1;                                   // the settled decoder
    double age = 0;                                  // audio seconds
    int weak = 0;
    uint64_t lastCharsAll = 0;
    double lastSec = 0;
};

class RttyBank : public HfdigDecoder {
public:
    RttyBank() : fft_(kFft), win_(kFft), ring_(kFft, 0.f), pw_(kFft / 2, 0.f), buf_(kFft) {
        for (int i = 0; i < kFft; i++) win_[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft));
    }
    void reset() override {
        retireAll();
        std::fill(ring_.begin(), ring_.end(), 0.f);
        std::fill(pw_.begin(), pw_.end(), 0.f);
        ringPos_ = since_ = frames_ = 0;
        cands_.clear(); hold_.clear();
    }
    void feedAudio(const float* x, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            ring_[(size_t)ringPos_] = x[i];
            if (++ringPos_ >= kFft) ringPos_ = 0;
            if (++since_ >= kHop) { since_ = 0; frame(); }
        }
        for (Track& t : tracks_)
            for (auto& d : t.dec) if (d) d->feedAudio(x, n);
        sec_ += (double)n / kFs;
        for (Track& t : tracks_) { t.age += (double)n / kFs; }
        resolve();
    }
    void snapshot(double centre, std::vector<RttyWideSignal>& out) const {
        for (const Track& t : tracks_) out.push_back(make(t, centre, true));
        for (const RttyWideSignal& s : gone_) { RttyWideSignal g = s; g.lowHz += centre; out.push_back(std::move(g)); }
    }
    void clearText() { gone_.clear(); for (Track& t : tracks_) for (auto& d : t.dec) if (d) d->clearText(); }
private:
    Fft fft_;
    std::vector<float> win_, ring_, pw_;
    std::vector<cf32> buf_;
    int ringPos_ = 0, since_ = 0, frames_ = 0;
    double sec_ = 0;
    std::vector<Track> tracks_;
    std::vector<RttyWideSignal> gone_;               // finished signals, lowHz from this channel's dial (centre added in snapshot)
    struct Cand { double lo; int shift; int frames; };
    std::vector<Cand> cands_;
    std::vector<std::pair<double, double>> hold_;    // (lowHz, until sec): a pair that was no RTTY, not tried again for a while

    double peak(int k) const { return std::max({pw_[(size_t)k - 1], pw_[(size_t)k], pw_[(size_t)k + 1]}); }
    double centroid(int k, double fl) const {
        int kk = k;
        for (int d = -1; d <= 1; d++) if (pw_[(size_t)(k + d)] > pw_[(size_t)kk]) kk = k + d;
        double sw = 0, sk = 0;
        for (int d = -2; d <= 2; d++) { const double w = std::max(0.0, (double)pw_[(size_t)(kk + d)] - fl); sw += w; sk += w * (kk + d); }
        return (sw > 0 ? sk / sw : (double)kk) * kBinHz;
    }

    RttyWideSignal make(const Track& t, double centre, bool active) const {
        RttyWideSignal s;
        s.lowHz = t.lo + centre; s.shiftHz = kShiftHz[t.shiftSel]; s.active = active;
        s.resolved = t.best >= 0;
        const int bi = t.best >= 0 ? t.best : bestGuess(t);
        if (bi >= 0 && bi < (int)t.dec.size() && t.dec[(size_t)bi]) {
            HfdigRttyTelemetry rt;
            t.dec[(size_t)bi]->telemetry(rt);
            s.baud = rt.baud; s.reverse = rt.reverse; s.chars = rt.chars; s.framingErrors = rt.framingErrors; s.text = rt.text;
            s.quality = rt.quality;
        }
        s.lastSec = t.lastSec;
        return s;
    }
    static double score(const HfdigRttyTelemetry& r) { return (double)r.chars - 4.0 * (double)r.framingErrors; }
    int bestGuess(const Track& t) const {
        int bi = -1; double bs = -1e9;
        for (size_t i = 0; i < t.dec.size(); i++) {
            if (!t.dec[i]) continue;
            HfdigRttyTelemetry r; t.dec[i]->telemetry(r);
            if (score(r) > bs) { bs = score(r); bi = (int)i; }
        }
        return bi;
    }

    void retire(Track& t) {
        RttyWideSignal s = make(t, 0.0, false);
        if (s.chars >= 6) {
            gone_.push_back(std::move(s));
            if (gone_.size() > 40) gone_.erase(gone_.begin());
        }
    }
    void retireAll() { for (Track& t : tracks_) retire(t); tracks_.clear(); }

    // after a few seconds: the decoder that reads sensible text stays, the others go; no text for long: it was no RTTY
    void resolve() {
        for (size_t ti = 0; ti < tracks_.size();) {
            Track& t = tracks_[ti];
            uint64_t all = 0;
            for (auto& d : t.dec) if (d) { HfdigRttyTelemetry r; d->telemetry(r); all += r.chars; }
            if (all != t.lastCharsAll) { t.lastCharsAll = all; t.lastSec = sec_; }
            if (t.best < 0 && t.age > 6.0) {
                int bi = -1; double bs = 0;
                for (size_t i = 0; i < t.dec.size(); i++) {
                    HfdigRttyTelemetry r; t.dec[i]->telemetry(r);
                    if (score(r) > bs) { bs = score(r); bi = (int)i; }
                }
                if (bi >= 0 && bs >= 8) {
                    for (size_t i = 0; i < t.dec.size(); i++) if ((int)i != bi) t.dec[i].reset();
                    t.best = bi;
                } else if (t.age > 20.0) {
                    hold_.push_back({t.lo, sec_ + 30.0});
                    tracks_.erase(tracks_.begin() + (long)ti);
                    continue;
                }
            }
            ti++;
        }
    }

    void frame() {
        for (int i = 0; i < kFft; i++) buf_[(size_t)i] = cf32(ring_[(size_t)((ringPos_ + i) % kFft)] * win_[(size_t)i], 0.f);
        fft_.forward(buf_.data());
        for (int k = 0; k < kFft / 2; k++) {
            const float p = std::norm(buf_[(size_t)k]);
            pw_[(size_t)k] = frames_ == 0 ? p : 0.65f * pw_[(size_t)k] + 0.35f * p;
        }
        if (++frames_ < 3) return;
        std::vector<float> sc(pw_.begin() + kBinLo, pw_.begin() + kBinHi + 1);
        std::nth_element(sc.begin(), sc.begin() + (long)sc.size() / 2, sc.end());
        const double fl = std::max(1e-12, (double)sc[sc.size() / 2]);
        // pairs that are being followed: still there?
        for (size_t ti = 0; ti < tracks_.size();) {
            Track& t = tracks_[ti];
            const int ds = (int)std::lround(kShiftHz[t.shiftSel] / kBinHz);
            const int kl = (int)std::lround(t.lo / kBinHz);
            double cur = 0;
            if (kl >= kBinLo && kl + ds <= kBinHi) { cur = std::sqrt(peak(kl)) + std::sqrt(peak(kl + ds)); cur = cur * cur / fl; }
            if (cur < kKeepRatio) { if (++t.weak >= 24) { retire(t); tracks_.erase(tracks_.begin() + (long)ti); continue; } }
            else t.weak = 0;
            ti++;
        }
        // new pairs: both tones present, 24 dB over the floor, on three frames in a row
        std::vector<Cand> now;
        for (int si = 0; si < 3; si++) {
            const int ds = (int)std::lround(kShiftHz[si] / kBinHz);
            std::vector<std::pair<double, int>> sco;
            for (int k = kBinLo; k + ds <= kBinHi; k++) {
                const double a = peak(k), b = peak(k + ds);
                if (std::min(a, b) < 0.1 * std::max(a, b)) continue;
                double s = std::sqrt(a) + std::sqrt(b);
                s *= s;
                if (s / fl >= kLockRatio && pw_[(size_t)k] >= pw_[(size_t)k - 1] && pw_[(size_t)k] >= pw_[(size_t)k + 1]) sco.push_back({s, k});
            }
            std::sort(sco.begin(), sco.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
            std::vector<int> taken;
            for (const auto& c : sco) {
                bool dup = false;
                for (int k : taken) if (std::abs(c.second - k) <= ds + 6 && std::abs(c.second - k) >= 0) dup = true;
                if (dup) continue;
                taken.push_back(c.second);
                if (taken.size() >= 3) break;
                const double fa = centroid(c.second, fl), fb = centroid(c.second + ds, fl);
                now.push_back({0.5 * (fa + (fb - kShiftHz[si])), si, 1});
            }
        }
        std::vector<Cand> next;
        for (Cand c : now) {
            bool busy = false;
            for (const Track& t : tracks_) {
                const double th = t.lo + kShiftHz[t.shiftSel], ch = c.lo + kShiftHz[c.shift];
                if (std::fabs(t.lo - c.lo) < 40 || std::fabs(th - ch) < 40 || std::fabs(t.lo - ch) < 40 || std::fabs(th - c.lo) < 40) busy = true;
            }
            for (const auto& h : hold_) if (std::fabs(h.first - c.lo) < 40 && h.second > sec_) busy = true;
            if (busy) continue;
            for (const Cand& o : cands_) if (o.shift == c.shift && std::fabs(o.lo - c.lo) < 12) { c.frames = o.frames + 1; break; }
            if (c.frames >= 3) { addTrack(c.lo, c.shift); continue; }
            next.push_back(c);
        }
        cands_ = std::move(next);
        hold_.erase(std::remove_if(hold_.begin(), hold_.end(), [&](const auto& h) { return h.second <= sec_; }), hold_.end());
    }

    void addTrack(double lo, int shiftSel) {
        if (tracks_.size() >= 6) return;
        Track t;
        t.lo = lo; t.shiftSel = shiftSel; t.lastSec = sec_;
        for (int b = 0; b < 3; b++)
            for (int r = 0; r < 2; r++) {
                auto d = makeRttyDecoder();
                d->setBaudIndex(kBaudIdx[b]); d->setShiftIndex(kShiftIdx[shiftSel]); d->setReverse(r == 1);
                d->setFixedPair(lo);
                t.dec.push_back(std::move(d));
                t.cfg.push_back({b, r == 1});
            }
        tracks_.push_back(std::move(t));
    }
};

// ---------------------------------------------------------------------------------------------------------------------------------
// A channel: the dial frequency of its own, 24 kHz front end, upper sideband audio, one decoder
struct Chain {
    double centre = 0;                  // from the radio's dial frequency
    marine::Front front;
    hfdig::UsbAudio usb;
    std::vector<cf32> base;
    std::vector<float> audio;
    double audioPower = 0;
    std::unique_ptr<HfdigFtx> ftx;
    std::unique_ptr<RttyBank> bank;
    void init(double rate, double c) { centre = c; front.configure(rate); front.setOffsetHz(c); }
    void feed(const cf32* x, size_t n) {
        base.clear(); audio.clear();
        front.process(x, n, base);
        usb.process(base.data(), base.size(), audio, audioPower);
        if (audio.empty()) return;
        if (ftx) ftx->feedAudio(audio.data(), audio.size());
        if (bank) bank->feedAudio(audio.data(), audio.size());
    }
};

} // namespace

const FtxSlot* ftxSlots(int& count) { count = (int)(sizeof kSlots / sizeof *kSlots); return kSlots; }

struct HfdigWide::Impl {
    HfdigFtx& mainFtx;
    WideDecimator dec;
    std::vector<std::unique_ptr<Chain>> ftxChains, rttyChains;
    std::vector<cf32> buf;
    std::vector<int> chanOf;     // channelised input: the channel of each ftx chain
    double span = 0;
    explicit Impl(HfdigFtx& f) : mainFtx(f) {}
};

HfdigWide::HfdigWide(HfdigFtx& f) : p_(std::make_unique<Impl>(f)) {}
HfdigWide::~HfdigWide() {
    for (auto& c : p_->ftxChains) if (c->ftx) c->ftx->setForwardTo(nullptr);
}

void HfdigWide::configure(double rate, double spanHz, double dialHz, double offsetHz, bool ftx, bool rtty) {
    Impl& m = *p_;
    for (auto& c : m.ftxChains) if (c->ftx) c->ftx->setForwardTo(nullptr);
    m.ftxChains.clear(); m.rttyChains.clear();
    m.dec.configure(rate, spanHz);
    m.dec.setOffsetHz(offsetHz);
    m.span = m.dec.spanHz();
    const double r2 = m.dec.outRate();
    if (ftx && dialHz > 0) {
        int n; const FtxSlot* sl = ftxSlots(n);
        const double half = m.span / 2 - 3300;    // the 3 kHz of the slot's audio must lie inside the span
        std::vector<std::pair<double, int>> dials;   // dial, mode bits
        for (int i = 0; i < n; i++) {
            const double d = sl[i].dialHz - dialHz;
            if (std::fabs(d) < 150 || std::fabs(d) > half) continue;   // the dial itself is the main decoder's
            bool merged = false;
            for (auto& x : dials) if (std::fabs(x.first - d) < 100) { x.second |= 1 << sl[i].mode; merged = true; }
            if (!merged) dials.push_back({d, 1 << sl[i].mode});
        }
        for (const auto& x : dials) {
            auto c = std::make_unique<Chain>();
            c->init(r2, x.first);
            c->ftx = makeFtxDecoder();
            for (int mo = 0; mo < kFtxModes; mo++) c->ftx->setEnabled(mo, (x.second >> mo) & 1);
            c->ftx->setDialHz(dialHz + x.first);
            c->ftx->setSpotShiftHz(x.first);
            c->ftx->setForwardTo(&m.mainFtx);
            c->ftx->copyClockFrom(m.mainFtx);
            m.ftxChains.push_back(std::move(c));
        }
    }
    if (rtty) {
        const double half = std::min(m.span, kRttyWideMaxSpanHz * wideSpanCapHz() / kWideMaxSpanHz) / 2;
        for (int k = (int)std::floor(-half / kRttyStep); k <= (int)std::ceil(half / kRttyStep); k++) {
            auto c = std::make_unique<Chain>();
            c->init(r2, k * kRttyStep);
            c->bank = std::make_unique<RttyBank>();
            m.rttyChains.push_back(std::move(c));
        }
    }
}

void HfdigWide::configureSlots(double rate, double spanHz, double dialHz, double offsetHz, const std::vector<FtxSlot>& slots, double reportDialHz) {
    Impl& m = *p_;
    for (auto& c : m.ftxChains) if (c->ftx) c->ftx->setForwardTo(nullptr);
    m.ftxChains.clear(); m.rttyChains.clear();
    m.dec.configure(rate, spanHz);
    m.dec.setOffsetHz(offsetHz);
    m.span = m.dec.spanHz();
    const double r2 = m.dec.outRate();
    const double half = m.span / 2 - 3300;
    std::vector<std::pair<double, int>> dials;
    for (const FtxSlot& sl : slots) {
        const double d = sl.dialHz - dialHz;
        if (std::fabs(d) > half) continue;
        bool merged = false;
        for (auto& x : dials) if (std::fabs(x.first - d) < 100) { x.second |= 1 << sl.mode; merged = true; }
        if (!merged) dials.push_back({d, 1 << sl.mode});
    }
    for (const auto& x : dials) {
        auto c = std::make_unique<Chain>();
        c->init(r2, x.first);
        c->ftx = makeFtxDecoder();
        for (int mo = 0; mo < kFtxModes; mo++) c->ftx->setEnabled(mo, (x.second >> mo) & 1);
        c->ftx->setDialHz(dialHz + x.first);
        c->ftx->setSpotShiftHz(dialHz + x.first - reportDialHz);
        c->ftx->setForwardTo(&m.mainFtx);
        c->ftx->copyClockFrom(m.mainFtx);
        m.ftxChains.push_back(std::move(c));
    }
}

void HfdigWide::configureChannels(double chanRate, const std::vector<ChanSpec>& specs, double reportDialHz) {
    Impl& m = *p_;
    for (auto& c : m.ftxChains) if (c->ftx) c->ftx->setForwardTo(nullptr);
    m.ftxChains.clear(); m.rttyChains.clear(); m.chanOf.clear();
    for (const ChanSpec& sp : specs) {
        auto c = std::make_unique<Chain>();
        c->init(chanRate, sp.offsetHz);
        c->ftx = makeFtxDecoder();
        for (int mo = 0; mo < kFtxModes; mo++) c->ftx->setEnabled(mo, (sp.modeBits >> mo) & 1);
        c->ftx->setDialHz(sp.dialHz);
        c->ftx->setSpotShiftHz(sp.dialHz - reportDialHz);
        c->ftx->setForwardTo(&m.mainFtx);
        c->ftx->copyClockFrom(m.mainFtx);
        m.ftxChains.push_back(std::move(c));
        m.chanOf.push_back(sp.channel);
    }
}

void HfdigWide::feedChannel(int channel, const cf32* x, size_t n) {
    Impl& m = *p_;
    for (size_t i = 0; i < m.ftxChains.size(); i++) if (m.chanOf[i] == channel) m.ftxChains[i]->feed(x, n);
}

void HfdigWide::setOffsetHz(double hz) { p_->dec.setOffsetHz(hz); }

void HfdigWide::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    m.buf.clear();
    m.dec.process(x, n, m.buf);
    if (m.buf.empty()) return;
    for (auto& c : m.ftxChains) c->feed(m.buf.data(), m.buf.size());
    for (auto& c : m.rttyChains) c->feed(m.buf.data(), m.buf.size());
}

void HfdigWide::rttyTelemetry(std::vector<RttyWideSignal>& out) {
    out.clear();
    for (auto& c : p_->rttyChains) c->bank->snapshot(c->centre, out);
    // the channels overlap: the same pair seen twice is listed once (the one with more text)
    std::sort(out.begin(), out.end(), [](const RttyWideSignal& a, const RttyWideSignal& b) { return a.lowHz < b.lowHz; });
    std::vector<RttyWideSignal> r;
    for (auto& s : out) {
        bool dup = false;
        for (auto& o : r) if (std::fabs(o.lowHz - s.lowHz) < 25 && o.shiftHz == s.shiftHz) {
            if (s.chars > o.chars || (s.active && !o.active)) o = s;
            dup = true; break;
        }
        if (!dup) r.push_back(s);
    }
    out = std::move(r);
}

void HfdigWide::clearRtty() { for (auto& c : p_->rttyChains) c->bank->clearText(); }
int HfdigWide::ftxChannels() const { return (int)p_->ftxChains.size(); }
int HfdigWide::rttyChannels() const { return (int)p_->rttyChains.size(); }
double HfdigWide::spanHz() const { return p_->span; }

} // namespace dect2
