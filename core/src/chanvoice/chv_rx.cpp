// Channelised analog voice receiver (see chv_rx.h).
#include "dect2/chanvoice/chv_rx.h"
#include "dect2/chanvoice/chv_mdc.h"
#include "dect2/chanvoice/chv_subtone.h"
#include "dect2/chanvoice/chv_tel.h"
#include "dect2/audio/audioout.h"
#include "dect2/common/threadpool.h"
#include "dect2/common/timecompat.h"
#include "dect2/dsp/carrierfind.h"
#include "dect2/marinevhf/marinevhf_scan.h"
#include "dect2/tones/tones_proto.h"
#include "dect2/wb/wb_export.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>

namespace dect2 {

namespace {

using chv::Demod;
using chv::Service;
constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = kChvAudioRate;
constexpr int kBlock = 480;                       // 10 ms
constexpr int kLookBlocks = 3;                    // the audio runs 30 ms late so the first syllable is not cut
constexpr int kHangFm = 15;                       // blocks the squelch stays open after the signal went
constexpr int kHangEnv = 40;                      // AM and SSB: speech pauses are longer
constexpr double kDevHz = 5000.0;
constexpr double kDeemphTau = 750e-6;
constexpr double kDeemphGain = 4.8;
constexpr int kMaxKeys = 512;
constexpr int kDynKeys = 24;
constexpr double kMaxRecSec = 300.0;

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void design(bool high, double fc, double fs, double q) {
        const double w = 2 * kPi * fc / fs, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
        if (high) { b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; }
        else { b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; }
        a1 = -2 * c / a0; a2 = (1 - al) / a0;
        z1 = z2 = 0;
    }
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};
// a 4th order Butterworth low-pass on a complex signal: the channel filter and the sideband filter
struct CLp {
    Biquad ir[2], qr[2];
    void design(double fc, double fs) { ir[0].design(false, fc, fs, 0.5412); ir[1].design(false, fc, fs, 1.3066); qr[0] = ir[0]; qr[1] = ir[1]; }
    std::complex<double> run(std::complex<double> s) {
        double i = s.real(), q = s.imag();
        for (int k = 0; k < 2; k++) { i = ir[k].run(i); q = qr[k].run(q); }
        return {i, q};
    }
};

struct Act {
    std::atomic<int> open{0};
    std::atomic<float> level{-200.f};
    std::atomic<double> mhz{0};
    std::atomic<int> demod{0};
    std::atomic<int> ctcss{-1};
    std::atomic<int> dcs{-1};        // code * 2 + inverted
};

struct CallRec { int key; std::string label; double mhz; int64_t wall; double startSec, durSec, peakDb, meanDb; int demod; chv::SubTone tone; std::string file; };
struct MdcRec { int key; std::string label; double mhz; chv::MdcPacket p; };

// What the channel receivers share with the receiver that owns them. Outlives them.
struct Ctx {
    std::atomic<int> squelch{50};
    std::atomic<bool> deemph{true};
    std::atomic<bool> recOn{false};
    std::atomic<double> recMinSec{0.5};
    std::atomic<int> demodOverride{-1};
    std::atomic<bool> mdcOn{false};
    std::atomic<bool> toneOn{true};
    std::mutex dirMu;
    std::string recDir;
    std::atomic<int> playKey{-1};
    std::atomic<double> spanSec{0};
    Act act[kMaxKeys];
    std::function<void(int key, const float* x, size_t n)> audio;
    std::function<void(const CallRec&)> callSink;
    std::function<void(const MdcRec&)> mdcSink;
    std::function<void(int key, int demod, const float* x, size_t n)> tap;
    std::string dir() { std::lock_guard<std::mutex> lk(dirMu); return recDir; }
};

std::string callFileName(const std::string& dir, int64_t wall, const std::string& label, double mhz, const char* sid) {
    struct tm tmv {};
    localTime((time_t)wall, &tmv);
    std::string l;
    for (char c : label) l += (std::isalnum((unsigned char)c) ? c : '_');
    char b[220];
    snprintf(b, sizeof b, "%s_%04d%02d%02d-%02d%02d%02d_%s_%.4fMHz.wav", sid, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, l.c_str(), mhz);
    return (std::filesystem::path(dir) / b).string();
}

// ---- one voice carrier
class ChannelRx {
public:
    ChannelRx(Service svc, double chanBwHz, Demod plan) : svc_(svc), plan_(plan), log_(1.0) {
        log_.setClock([] { return unixNow(); });
        log_.setOnClose([this](const marinevhf::CallEntry& e) { finishCall(e); });
        hp300_.design(true, 300, kRate, 0.7071); lp3k_.design(false, 3000, kRate, 0.7071); hp55_.design(true, 5500, kRate, 0.7071);
        ehp_.design(true, 300, kRate, 0.7071); elp_.design(false, 3000, kRate, 0.7071);
        sp_.design(std::max(3000.0, chanBwHz), kRate);   // the channel filter
        ssbU_.design(1350.0, kRate); ssbL_.design(1350.0, kRate);
        aDe_ = 1.0 - std::exp(-1.0 / (kRate * kDeemphTau));
        envOnly_ = svc == Service::HfVoice;
        if (svc == Service::Mdc) mdc_ = std::make_unique<chv::MdcDecoder>(kRate), mdc_->setCallback([this](const chv::MdcPacket& p) { if (ctx_ && ctx_->mdcSink) ctx_->mdcSink(MdcRec{key_, label_, mhz_, p}); });
        wr_ = 0;
        for (int i = 0; i < kBlock; i++) rot_[(size_t)i] = 0;
    }
    ~ChannelRx() {
        log_.closeAll(tSec_);
        if (ctx_ && key_ >= 0 && key_ < kMaxKeys) { ctx_->act[key_].open = 0; ctx_->act[key_].level = -200.f; ctx_->act[key_].ctcss = -1; ctx_->act[key_].dcs = -1; }
    }
    void bind(Ctx* c, int key, const std::string& label, double mhz) {
        ctx_ = c; key_ = key; label_ = label; mhz_ = mhz;
        if (key_ >= 0 && key_ < kMaxKeys) { ctx_->act[key_].mhz = mhz; ctx_->act[key_].demod = (int)plan_; }
    }
    // the finder's view of this carrier (set by the owner before every feed)
    void setFinder(bool active, double levelDb) { finderActive_ = active; finderLevel_ = levelDb; }
    int demodNow() const { return (int)cur_; }

    void feed(const cf32* x, size_t n) {
        const int ovr = ctx_ ? ctx_->demodOverride.load(std::memory_order_relaxed) : -1;
        for (size_t i = 0; i < n; i++) {
            const std::complex<double> s0 = sp_.run({x[i].real(), x[i].imag()});
            const std::complex<float> s((float)s0.real(), (float)s0.imag());
            const double mag2 = std::norm(s0), mag = std::sqrt(mag2);
            pw_ += mag2; sumAbs_ += mag;
            // FM
            const std::complex<double> d = s0 * std::conj(prev_);
            prev_ = s0;
            const double f = std::atan2(d.imag(), d.real()) * (kRate / (2 * kPi));
            const double v = f / kDevHz;
            centroid_ += f * mag2;
            const double lo = lp3k_.run(hp300_.run(v));
            const double hi = hp55_.run(v);
            plo_ += lo * lo; phi_ += hi * hi;
            de_ += aDe_ * (lo - de_);
            const double fm = (deemph_ ? de_ * kDeemphGain : lo) * 0.5;
            // AM
            envMean_ += (mag - envMean_) * 0.0005;   // 40 ms
            const double e = envMean_ > 1e-9 ? mag / envMean_ - 1.0 : 0.0;
            const double am = elp_.run(ehp_.run(e)) * 0.5;
            // SSB: the sideband around the channel frequency
            const double ph = 2 * kPi * 1650.0 / kRate * (double)rotN_;
            const std::complex<double> rc(std::cos(ph), std::sin(ph));
            rotN_ = (rotN_ + 1) % 480;   // 1650 Hz at 48 kHz repeats every 480 samples
            const std::complex<double> zu = ssbU_.run(s0 * std::conj(rc)) * rc;
            const std::complex<double> zl = ssbL_.run(s0 * rc) * std::conj(rc);
            const double ssb = side_ >= 0 ? zu.real() : zl.real();
            if (ctx_ && ctx_->toneOn.load(std::memory_order_relaxed) && svc_ != Service::HfVoice) { const float vv = (float)std::max(-1.0, std::min(1.0, v)); tone_.feed48k(&vv, 1); }
            sblk_[(size_t)nb_] = s;
            fmB_[(size_t)nb_] = (float)fm; amB_[(size_t)nb_] = (float)am; ssbB_[(size_t)nb_] = (float)ssb; rawB_[(size_t)nb_] = (float)lo;
            nb_++;
            if (nb_ == kBlock) { block(ovr); nb_ = 0; pw_ = plo_ = phi_ = sumAbs_ = 0; centroid_ = 0; }
        }
    }

private:
    struct Blk { std::array<float, kBlock> a, raw; bool gate; int demod; };

    // the share of the block's power (|f| < 2.5 kHz) in its strongest 100 Hz line: an AM carrier holds most of it, single sideband speech very little
    double carrierShare() {
        struct Tab { std::vector<float> c, s; Tab() : c(51 * kBlock), s(51 * kBlock) { for (int k = 0; k < 51; k++) for (int n = 0; n < kBlock; n++) { const double w = 2 * kPi * (k - 25) * 100.0 * n / kRate; c[(size_t)(k * kBlock + n)] = (float)std::cos(w); s[(size_t)(k * kBlock + n)] = (float)-std::sin(w); } } };
        static const Tab tab;
        double best = 0, tot = 0;
        for (int k = 0; k < 51; k++) {
            double re = 0, im = 0;
            const float* c = &tab.c[(size_t)k * kBlock]; const float* sn = &tab.s[(size_t)k * kBlock];
            for (int n = 0; n < kBlock; n++) { re += c[n] * sblk_[(size_t)n].real() - sn[n] * sblk_[(size_t)n].imag(); im += c[n] * sblk_[(size_t)n].imag() + sn[n] * sblk_[(size_t)n].real(); }
            const double p = re * re + im * im;
            best = std::max(best, p); tot += p;
        }
        return tot > 0 ? best / tot : 0.0;
    }

    Demod decide(int ovr) {
        if (ovr >= 0 && ovr != (int)Demod::Auto) return (Demod)ovr;
        if (plan_ != Demod::Auto) return plan_;
        return autoDemod_;
    }

    void block(int ovr) {
        deemph_ = ctx_ ? ctx_->deemph.load(std::memory_order_relaxed) : true;
        const double lvl = 10 * std::log10(pw_ / kBlock + 1e-20);
        const double lo = plo_ / kBlock, hi = phi_ / kBlock;
        if (!init_) { qlo_ = lo; qhi_ = hi; init_ = true; }
        else { qlo_ += 0.4 * (lo - qlo_); qhi_ += 0.4 * (hi - qhi_); }
        const double qSmooth = pw_ / kBlock < 1e-9 ? -30.0 : 10 * std::log10((qlo_ + 1e-12) / (qhi_ + 1e-12));   // (no signal at all: closed)
        // the smoothed measure needs 100 ms to follow a signal that comes up 40 dB out of the noise: the last two blocks on their own open it at once
        // when they are clearly above the threshold (3 dB more), so a burst that starts the transmission is not cut
        const double qInst = 10 * std::log10((lo + 1e-12) / (hi + 1e-12));
        const double qFast = std::min(qInst, qInstPrev_) - 3.0;
        qInstPrev_ = qInst;
        const double quiet = std::max(qSmooth, pw_ / kBlock < 1e-9 ? -30.0 : qFast);
        const int sq = ctx_ ? ctx_->squelch.load(std::memory_order_relaxed) : 50;
        const double thr = 1.0 + 0.08 * std::max(0, std::min(100, sq));   // noise alone reads about -0.5 dB after the channel filter, a clean signal 25 dB and up
        // the envelope statistic for the classification: coefficient of variation over about 100 ms
        const double m1 = sumAbs_ / kBlock, m2 = pw_ / kBlock;
        const double cv = m1 > 1e-9 ? std::sqrt(std::max(0.0, m2 - m1 * m1)) / m1 : 0.0;
        cvS_ += 0.1 * (cv - cvS_);
        if (open_ && plan_ == Demod::Auto) carrierS_ += 0.2 * (carrierShare() - carrierS_);
        const Demod want = decide(ovr);
        const bool envAllowed = (plan_ == Demod::Auto && (ovr < 0 || ovr == (int)Demod::Auto)) || want == Demod::Am || want == Demod::Usb || want == Demod::Lsb;
        const bool fmOpen = !envOnly_ && quiet > thr;
        bool rose = false;
        const bool envSig = envAllowed && finderActive_ && (envOnly_ || plan_ == Demod::Auto || want != Demod::Nfm);
        // an FM signal that has opened is held by the quieting measure alone (the finder's activity lags and would keep the noise open)
        const bool sig = (open_ && cur_ == Demod::Nfm && !envOnly_) ? fmOpen : (fmOpen || envSig);
        if (!open_) {
            if (sig) {
                if (++above_ >= (fmOpen ? 2 : 1)) {
                    open_ = true; rose = true; hang_ = fmOpen ? kHangFm : kHangEnv; votes_ = 0; centroidAcc_ = 0; centroidN_ = 0; cvAcc_ = 0; carrierS_ = 1.0;
                    if (plan_ == Demod::Auto) autoDemod_ = fmOpen ? Demod::Nfm : Demod::Am;
                    cur_ = decide(ovr);
                }
            } else above_ = 0;
        } else {
            if (sig) hang_ = (fmOpen ? kHangFm : kHangEnv);
            else if (--hang_ <= 0) { open_ = false; above_ = 0; }
        }
        // classification (Auto): FM when the discriminator is quiet and clean, else SSB when the envelope is wild, else AM
        if (plan_ == Demod::Auto && (ovr < 0 || ovr == (int)Demod::Auto) && open_) {
            Demod vote;
            if (quiet > 6.0 && cvS_ < 0.35) vote = Demod::Nfm;
            else if (carrierS_ < 0.45) vote = (centroidAcc_ >= 0 ? Demod::Usb : Demod::Lsb);
            else vote = Demod::Am;
            centroidN_++;
            centroidAcc_ += centroid_ / std::max(1e-12, pw_) > 0 ? 1 : -1;
            if (vote == autoDemod_ || (vote != Demod::Nfm && autoDemod_ != Demod::Nfm && (vote == Demod::Usb || vote == Demod::Lsb) && (autoDemod_ == Demod::Usb || autoDemod_ == Demod::Lsb))) {
                votes_ = 0;
                if (vote != autoDemod_) autoDemod_ = vote;
            } else if (++votes_ >= (rose || firstOpen_ ? 1 : 6)) { autoDemod_ = vote; votes_ = 0; }
            firstOpen_ = false;
            if (autoDemod_ == Demod::Usb || autoDemod_ == Demod::Lsb) side_ = autoDemod_ == Demod::Usb ? 1 : -1;
        }
        if (rose) firstOpen_ = true;
        cur_ = decide(ovr);
        if (cur_ == Demod::Usb) side_ = 1; else if (cur_ == Demod::Lsb) side_ = -1;
        blocks_++;
        tSec_ = (double)blocks_ * kBlock / kRate;
        if (rose && !callActive_) { callActive_ = true; callSpan_ = ctx_ ? ctx_->spanSec.load() : 0; callTone_ = chv::SubTone{}; }
        log_.update(tSec_, 0, open_, lvl);
        const chv::SubTone tn = tone_.tone();
        if (open_ && tn.any()) callTone_ = tn;
        if (key_ >= 0 && key_ < kMaxKeys && ctx_) {
            Act& a = ctx_->act[key_];
            a.open = open_ ? 1 : 0; a.level = (float)lvl; a.demod = (int)cur_;
            a.ctcss = open_ ? tn.ctcssIdx : -1; a.dcs = (open_ && tn.dcsCode >= 0) ? tn.dcsCode * 2 + (tn.dcsInverted ? 1 : 0) : -1;
        }
        // the audio of the block by demodulator
        Blk b; b.gate = open_; b.demod = (int)cur_;
        const float* src = fmB_.data();
        if (cur_ == Demod::Am) src = amB_.data(); else if (cur_ == Demod::Usb || cur_ == Demod::Lsb) src = ssbB_.data();
        const bool agc = cur_ != Demod::Nfm;
        double g = 1.0;
        if (agc) {   // AGC: the audio's block peak against a target, fast attack, slow release
            double pk = 0;
            for (int i = 0; i < kBlock; i++) pk = std::max(pk, (double)std::fabs(src[i]));
            if (pk * agcGain_ > 0.5) agcGain_ = 0.5 / std::max(pk, 1e-9); else agcGain_ = std::min(agcGain_ * 1.003, cur_ == Demod::Am ? 3.0 : 4000.0);
            g = agcGain_;
        }
        for (int i = 0; i < kBlock; i++) b.a[(size_t)i] = (float)std::max(-1.0, std::min(1.0, src[i] * g));
        b.raw = rawB_;
        if (rose) for (auto& q : q_) q.gate = true;
        q_.push_back(b);
        if (q_.size() > (size_t)kLookBlocks) { emit(q_.front()); q_.pop_front(); }
    }

    void emit(const Blk& b) {
        static const std::array<float, kBlock> zeros{};
        const float* p = b.gate ? b.a.data() : zeros.data();
        if (ctx_) {
            if (ctx_->tap) ctx_->tap(key_, b.demod, p, kBlock);
            if (key_ >= 0 && ctx_->audio && ctx_->playKey.load(std::memory_order_relaxed) == key_) ctx_->audio(key_, p, kBlock);
        }
        if (mdc_ && b.gate) {   // the flat audio: MDC data is not pre-emphasised
            mdc_->feed(b.raw.data(), kBlock);
        }
        if (callActive_) {
            if (b.gate && !recStarted_) recStarted_ = true;
            if (recStarted_ && rec_.size() < (size_t)(kMaxRecSec * kRate)) {
                rec_.insert(rec_.end(), p, p + kBlock);
                if (b.gate) recGateEnd_ = rec_.size();
            }
        }
    }

    void finishCall(const marinevhf::CallEntry& e) {
        CallRec c;
        c.key = key_; c.label = label_; c.mhz = mhz_; c.wall = e.wallTime; c.startSec = callSpan_; c.durSec = e.durationSec();
        c.peakDb = e.peakDb; c.meanDb = e.meanDb; c.demod = (int)cur_; c.tone = callTone_;
        while (!q_.empty()) { emit(q_.front()); q_.pop_front(); }
        if (ctx_ && ctx_->recOn.load() && recGateEnd_ > 0 && (double)recGateEnd_ / kRate >= ctx_->recMinSec.load()) {
            const std::string dir = ctx_->dir();
            if (!dir.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                std::string path = callFileName(dir, e.wallTime, label_, mhz_, chv::serviceId(svc_));
                for (int k = 2; std::filesystem::exists(path, ec) && k < 100; k++) {
                    path = callFileName(dir, e.wallTime, label_, mhz_, chv::serviceId(svc_));
                    path.insert(path.size() - 4, "_" + std::to_string(k));
                }
                rec_.resize(recGateEnd_);
                std::string err;
                if (wb::writeWavMono16(path, rec_, (int)kRate, err)) c.file = path;
            }
        }
        rec_.clear(); recGateEnd_ = 0; recStarted_ = false; callActive_ = false;
        if (ctx_ && ctx_->callSink) ctx_->callSink(c);
    }

    Service svc_;
    Demod plan_, cur_ = Demod::Nfm, autoDemod_ = Demod::Nfm;
    Ctx* ctx_ = nullptr;
    int key_ = -1;
    std::string label_;
    double mhz_ = 0;
    marinevhf::CallLog log_;
    Biquad hp300_, lp3k_, hp55_, ehp_, elp_;
    CLp sp_, ssbU_, ssbL_;
    double aDe_ = 0.1, de_ = 0, envMean_ = 0.01;
    bool deemph_ = true, envOnly_ = false;
    std::complex<double> prev_{1.0, 0.0};
    std::array<float, kBlock> fmB_{}, amB_{}, ssbB_{}, rawB_{};
    std::array<double, kBlock> rot_{};
    std::array<std::complex<float>, kBlock> sblk_{};
    double carrierS_ = 0, qInstPrev_ = -30;
    int nb_ = 0, wr_ = 0, rotN_ = 0, side_ = 1;
    double pw_ = 0, plo_ = 0, phi_ = 0, qlo_ = 0, qhi_ = 0, sumAbs_ = 0, centroid_ = 0, cvS_ = 0;
    int centroidAcc_ = 0, centroidN_ = 0, votes_ = 0; double cvAcc_ = 0;
    bool init_ = false, open_ = false, firstOpen_ = false, finderActive_ = false;
    double finderLevel_ = -200;
    int above_ = 0, hang_ = 0;
    double agcGain_ = 1.0;
    uint64_t blocks_ = 0;
    double tSec_ = 0, callSpan_ = 0;
    bool callActive_ = false, recStarted_ = false;
    std::deque<Blk> q_;
    std::vector<float> rec_;
    size_t recGateEnd_ = 0;
    chv::SubToneReader tone_;
    chv::SubTone callTone_;
    std::unique_ptr<chv::MdcDecoder> mdc_;
};

// ---- one wireless microphone candidate: wide FM at 192 kHz
class MicRx {
public:
    void setActive(bool a, double snrDb) { active_ = a; snr_ = snrDb; }
    void bind(int id, double mhz, std::function<void(const float*, size_t)> audio) { id_ = id; mhz_ = mhz; audio_ = std::move(audio); lp_.design(false, 15000, kChvMicRate, 0.7071); lp2_.design(false, 15000, kChvMicRate, 0.7071); }
    double devKhz() const { return devKhz_.load(); }
    double meanHz() const { return meanF_; }
    bool fmLike() const { return fmLike_.load(); }
    void feed(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const std::complex<double> s(x[i].real(), x[i].imag());
            const double mag = std::abs(s);
            const std::complex<double> d = s * std::conj(prev_);
            prev_ = s;
            const double f = std::atan2(d.imag(), d.real()) * (kChvMicRate / (2 * kPi));
            sumM_ += mag; sumM2_ += mag * mag; cnt_++;
                        meanF_ += (f - meanF_) * 2e-5;   // about 0.25 s
            const double a = lp2_.run(lp_.run(f)) / 75000.0;
            devAcc_ = std::max(devAcc_, std::fabs(a) * 75000.0);
            if (++dec_ == 4) { dec_ = 0; out_.push_back((float)std::max(-1.0, std::min(1.0, a * 3.0))); }
            if (cnt_ == 19200) {   // 100 ms
                const double m1 = sumM_ / cnt_, m2 = sumM2_ / cnt_;
                const double cv = m1 > 1e-9 ? std::sqrt(std::max(0.0, m2 - m1 * m1)) / m1 : 1.0;
                maxPw_ = std::max(m2, maxPw_ * 0.97);
                const bool sig = active_ && snr_ > 6.0 && ++win_ > 2 && m2 > 0.7 * maxPw_;   // a carrier is on (the finder lags at both ends: only full-strength windows count)
                if (sig) fmLike_ = cv < 0.35 && devAcc_ > 3000.0;   // a constant envelope that moves by kHz: analogue FM (an unmodulated carrier or an AM one does not)
                if (sig) devKhz_ = devKhz_.load() == 0 ? devAcc_ / 1000.0 : 0.7 * devKhz_.load() + 0.3 * devAcc_ / 1000.0;
                sumM_ = sumM2_ = 0; cnt_ = 0; devAcc_ = 0;
            }
        }
        if (audio_ && !out_.empty()) { audio_(out_.data(), out_.size()); }
        out_.clear();
    }
private:
    int id_ = 0; double mhz_ = 0;
    bool active_ = false; double snr_ = 0;
    std::function<void(const float*, size_t)> audio_;
    Biquad lp_, lp2_;
    std::complex<double> prev_{1.0, 0.0};
    double sumM_ = 0, sumM2_ = 0, devAcc_ = 0, meanF_ = 0, maxPw_ = 0;
    int win_ = 0;
    int cnt_ = 0, dec_ = 0;
    std::atomic<double> devKhz_{0};
    std::atomic<bool> fmLike_{false};
    std::vector<float> out_;
};

struct Slot {
    int id = 0, key = -1;
    std::string label;
    double mhz = 0;
    bool fixed = false;       // the channelizer sits on the plan frequency (CB, HF: SSB needs an exact reference), else it follows the carrier
    Channelizer ch;
    std::unique_ptr<ChannelRx> rx;
    std::unique_ptr<MicRx> mic;
    std::vector<cf32> tmp;
    double hz = 0;            // the finder's position of the carrier
    double levelDb = -200, snrDb = 0, widthHz = 0, ageSec = 0;
    bool active = false;
    int micBand = -1;
};

} // namespace

struct ChanVoiceReceiver::Impl {
    struct Stat { int calls = 0; double totalSec = 0; double lastHeardSpan = -1; int64_t lastHeardWall = 0; double peakDb = -200; chv::SubTone lastTone; };

    Ctx ctx;
    std::function<void(const std::string&)> log;
    std::mutex mu;                       // the settings, the scan, the finder and the slots (feed holds it)
    std::mutex callMu;
    double rate = 0, offHz = 0, centerHz = 0, dialHz = 0;
    Service svc = Service::Cb;
    int regionOf[chv::kServiceCount] = {};
    CarrierFinder finder;
    bool cfgDone = false;
    double gridOrigin = -1e18;
    int carrierLimit = 10;
    std::atomic<bool> silent{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false};
    uint64_t samples = 0;
    double lastDecide = -1;
    std::map<int, std::unique_ptr<Slot>> slots;
    std::map<int, int> dynKey;           // finder carrier id -> key >= plan size
    std::vector<cf32> hist; size_t histW = 0, histN = 0;   // the last 0.25 s of the span
    // what plays
    int listenKey = -1;
    bool scanOn = false, singleTuner = false;
    std::vector<int> scanKeys;
    double dwellSec = 0.4, hangSec = 2.0;
    marinevhf::ChannelScan scan;
    int autoKey = -1; double lastAutoOpen = 0;
    int micListen = 0, micBand = -1;
    // results
    std::deque<ChvCall> calls;
    std::deque<ChvMdcEvent> mdcList;
    std::map<int, Stat> stats;
    uint64_t callSerial = 0, mdcSerial = 0;
    int recFiles = 0;
    std::mutex audioMu;
    std::unique_ptr<AudioOut> audioOut;
    int lastAudioKey = -2;
    ChvTelemetry tel;
    double lastBuild = -1e9;
    double tuneRequestMhz = 0;

    Impl() {
        ctx.audio = [this](int key, const float* x, size_t n) { audioWrite(key, x, n); };
        ctx.callSink = [this](const CallRec& c) { callDone(c); };
        ctx.mdcSink = [this](const MdcRec& m) { mdcDone(m); };
        scan.setConfig(scanCfg());
    }
    int region() const { return regionOf[(int)svc]; }
    const std::vector<chv::Channel>& plan() const { return chv::channels(svc, region()); }
    int planSize() const { return (int)std::min<size_t>(plan().size(), kMaxKeys - kDynKeys); }
    bool isMic() const { return svc == Service::Mic; }
    double centerAbs() const { return centerHz > 0 ? centerHz : dialHz > 0 ? dialHz - offHz : 0.0; }
    bool fixedSlots() const { return svc == Service::Cb || svc == Service::HfVoice; }
    double matchTolHz() const {
        switch (svc) { case Service::Cb: return 6000; case Service::Frs: return 6250; case Service::Pmr: return 6250; case Service::HfVoice: return 3600; case Service::Ham: return region() == 1 ? 7500 : 6250; default: return 6250; }
    }
    double channelBw() const {
        switch (svc) { case Service::Cb: return region() == 2 ? 4500 : 4500; case Service::Frs: return 5500; case Service::Pmr: return 5500; case Service::Ham: return region() == 1 ? 7000 : 6000; case Service::HfVoice: return 4000; default: return 6000; }
    }
    Demod planDemod(const chv::Channel* c) const {
        switch (svc) { case Service::Mdc: return Demod::Nfm; case Service::HfVoice: return Demod::Usb; default: return c ? c->demod : Demod::Nfm; }
    }

    marinevhf::ScanConfig scanCfg() const {
        marinevhf::ScanConfig c;
        c.dwellSec = dwellSec; c.hangSec = hangSec; c.singleTuner = singleTuner; c.priorityKey = -1;
        return c;
    }

    void regrid() {   // under mu
        if (rate <= 0) return;
        const double c = centerAbs();
        const double step = isMic() ? 100000.0 : chv::rasterHz(svc, region());
        double origin;
        double ref = 0;   // a channel of the plan: the raster passes through it (the CB channels sit 5 kHz off the 10 kHz multiples)
        if (!isMic() && svc != Service::Mdc && !plan().empty()) ref = plan()[0].mhz * 1e6;
        if (c > 0) origin = std::fmod(ref - c, step); else origin = std::fmod(offHz, step);
        if (origin < 0) origin += step;
        if (cfgDone && std::fabs(origin - gridOrigin) < 1.0) { priors(); return; }
        CarrierCfg cfg;
        cfg.stepHz = step;
        cfg.gridOriginHz = origin;
        cfg.maxCarriers = carrierLimit;
        cfg.onDb = onDbFor(); cfg.offDb = cfg.onDb - 4;
        cfg.holdSec = isMic() ? 8 : 5; cfg.debounceSec = 0.12; cfg.avgSec = 0.05;
        if (isMic()) { cfg.avgSec = 0.1; cfg.debounceSec = 0.3; cfg.minSnrDb = 6; cfg.mergeSplit = true; cfg.skirtDb = 20; }
        finder.configure(rate, cfg);
        gridOrigin = origin; cfgDone = true;
        priors();
    }
    double onDbFor() const { return 5.0 + 0.08 * ctx.squelch.load(); }
    void priors() {
        std::vector<double> rel;
        const double c = centerAbs();
        if (c > 0 && !isMic() && svc != Service::Mdc && svc != Service::HfVoice) for (const auto& ch : plan()) rel.push_back(ch.mhz * 1e6 - c);
        finder.setPriors(rel, false);
    }

    int planKeyFor(double absHz, bool* inPlan) {
        *inPlan = false;
        if (svc == Service::Mdc || isMic() || centerAbs() <= 0) return -1;
        const auto& v = plan();
        int best = -1; double bd = matchTolHz();
        for (int k = 0; k < planSize(); k++) {
            double d = absHz - v[(size_t)k].mhz * 1e6;
            if (svc == Service::HfVoice) { if (d < -600 || d > bd) continue; d = std::fabs(d - 1500); }   // USB: the energy sits above the carrier frequency
            else d = std::fabs(d);
            if (d <= bd) { bd = d; best = k; }
        }
        if (best >= 0) *inPlan = true;
        return best;
    }

    void rebuildScan(double now) {   // under mu
        scan.stop();
        std::vector<int> keys;
        if (scanOn) {
            if (scanKeys.empty()) for (int k = 0; k < planSize(); k++) keys.push_back(k);
            else for (int k : scanKeys) if (k >= 0 && k < planSize()) keys.push_back(k);
        }
        scan.setChannels(keys);
        scan.setConfig(scanCfg());
        scan.start(now);
    }

    void decide(double now) {   // under mu
        const int n = planSize();
        auto open = [&](int k) { return k >= 0 && k < n && ctx.act[k].open.load(std::memory_order_relaxed) != 0; };
        tuneRequestMhz = 0;
        int r = scan.update(now, open);
        if (scanOn && singleTuner && r >= 0 && r < n) tuneRequestMhz = plan()[(size_t)r].mhz;
        int play = -1;
        if (isMic()) { ctx.playKey = -1; return; }
        if (scanOn) play = (singleTuner ? (open(r) ? r : -1) : r);
        else if (listenKey >= 0) play = r >= 0 ? r : listenKey;
        else if (r >= 0) play = r;
        else {   // automatic: the strongest open channel, sticky (carriers off the plan count too)
            int best = -1; float bl = -1e9f;
            for (int k = 0; k < kMaxKeys; k++) if (ctx.act[k].open.load(std::memory_order_relaxed) && ctx.act[k].level.load() > bl) { best = k; bl = ctx.act[k].level.load(); }
            auto openAny = [&](int k) { return k >= 0 && k < kMaxKeys && ctx.act[k].open.load(std::memory_order_relaxed) != 0; };
            if (autoKey >= 0 && openAny(autoKey)) {
                lastAutoOpen = now;
                if (best >= 0 && bl > ctx.act[autoKey].level.load() + 6.f) autoKey = best;
            } else if (autoKey < 0 || now - lastAutoOpen >= 1.0) autoKey = best;
            if (autoKey >= 0 && openAny(autoKey)) lastAutoOpen = now;
            play = autoKey;
        }
        ctx.playKey = play;
    }

    void audioWrite(int key, const float* x, size_t n) {
        if (silent.load()) return;
        std::lock_guard<std::mutex> lk(audioMu);
        if (!audioOut) { audioOut = std::make_unique<AudioOut>(); audioOut->start(48000); audioOut->setStartThreshold(48000 / 4); }
        audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        if (key != lastAudioKey) { audioOut->flush(); lastAudioKey = key; }
        if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
        std::vector<float> st(n * 2);
        for (size_t i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = x[i];
        audioOut->write(st.data(), (int)n);
    }

    std::string keyLabel(int key) const {
        if (key >= 0 && key < planSize()) return plan()[(size_t)key].label;
        return std::string();
    }

    void callDone(const CallRec& c0) {
        ChvCall c;
        std::string line;
        {
            std::lock_guard<std::mutex> lk(callMu);
            c.serial = ++callSerial; c.key = c0.key; c.label = c0.label; c.mhz = c0.mhz; c.wallTime = c0.wall; c.startSec = c0.startSec; c.durationSec = c0.durSec;
            c.peakDb = c0.peakDb; c.meanDb = c0.meanDb; c.demod = c0.demod; c.tone = c0.tone; c.file = c0.file;
            calls.push_front(c);
            if (calls.size() > kChvCallMax) calls.pop_back();
            Stat& s = stats[c.key];
            s.calls++; s.totalSec += c.durationSec; s.lastHeardSpan = c.startSec + c.durationSec; s.lastHeardWall = c.wallTime; s.peakDb = std::max(s.peakDb, c.peakDb);
            if (c.tone.any()) s.lastTone = c.tone;
            if (!c.file.empty()) recFiles++;
            char b[220];
            const std::string tl = chv::toneLabel(c.tone.ctcssHz, c.tone.dcsCode, c.tone.dcsInverted);
            snprintf(b, sizeof b, "%s %s (%.4f MHz, %s%s%s): %.1f s, peak %.0f dB%s", chv::serviceName(svc), c.label.empty() ? "carrier" : c.label.c_str(), c.mhz, chv::demodName((Demod)c.demod),
                     tl.empty() ? "" : ", tone ", tl.c_str(), c.durationSec, c.peakDb, c.file.empty() ? "" : ", recorded");
            line = b;
        }
        if (log) log(line);
    }
    void mdcDone(const MdcRec& m) {
        std::string line;
        {
            std::lock_guard<std::mutex> lk(callMu);
            ChvMdcEvent e;
            e.serial = ++mdcSerial; e.wallTime = unixNow(); e.key = m.key; e.label = m.label; e.mhz = m.mhz; e.op = m.p.op; e.arg = m.p.arg; e.unitId = m.p.unitId; e.dbl = m.p.dbl;
            for (int i = 0; i < 4; i++) e.extra[i] = m.p.extra[i];
            e.text = chv::mdcOpText(m.p);
            mdcList.push_front(e);
            if (mdcList.size() > kChvMdcMax) mdcList.pop_back();
            char b[200];
            snprintf(b, sizeof b, "MDC-1200 %.4f MHz: unit %04X, %s", m.mhz, m.p.unitId, e.text.c_str());
            line = b;
        }
        if (log) log(line);
    }

    // ---- the carriers
    void clearSlots() { slots.clear(); dynKey.clear(); }
    void retire(int id) { auto it = slots.find(id); if (it != slots.end()) { slots.erase(it); } dynKey.erase(id); }

    int freeDynKey() {
        const int base = planSize();
        for (int i = 0; i < kDynKeys; i++) {
            bool used = false;
            for (auto& d : dynKey) if (d.second == base + i) used = true;
            if (!used) return base + i;
        }
        return -1;
    }

    Slot* addSlot(const Carrier& c) {
        const double ctr = centerAbs();
        const double abs = ctr > 0 ? ctr + c.hz : 0;
        auto s = std::make_unique<Slot>();
        s->id = c.id; s->hz = c.hz;
        bool inPlan = false;
        int key = isMic() ? -1 : planKeyFor(abs, &inPlan);
        if (key >= 0) {
            // a second carrier on a channel that already has one is off the plan
            for (auto& o : slots) if (o.second->key == key) { key = -1; inPlan = false; break; }
        }
        if (!inPlan) key = isMic() ? -1 : freeDynKey();
        if (!isMic() && key < 0) return nullptr;
        s->key = key;
        if (inPlan) { const auto& ch = plan()[(size_t)key]; s->label = ch.label; s->mhz = ch.mhz; }
        else if (!isMic()) { dynKey[c.id] = key; char b[24]; snprintf(b, sizeof b, "%.4f", abs / 1e6); s->label = abs > 0 ? b : "?"; s->mhz = abs / 1e6; }
        else { s->mhz = abs / 1e6; }
        s->fixed = inPlan && fixedSlots();
        double shift = c.hz;
        if (s->fixed && ctr > 0) shift = plan()[(size_t)key].mhz * 1e6 - ctr;
        if (isMic()) {
            if (!s->ch.configure(rate, kChvMicRate, shift, 160000.0)) return nullptr;
            s->mic = std::make_unique<MicRx>();
            const int id = c.id;
            s->mic->bind(id, s->mhz, [this, id](const float* x, size_t n) { if (micListen == id && !silent.load()) { audioWrite(10000 + id, x, n); } });
        } else {
            if (!s->ch.configure(rate, kRate, shift, 8000.0)) return nullptr;
            Demod pd = planDemod(inPlan ? &plan()[(size_t)key] : nullptr);
            s->rx = std::make_unique<ChannelRx>(svc, channelBw(), pd);
            s->rx->bind(&ctx, key, s->label, s->mhz);
        }
        // the start of the transmission that made the finder notice the carrier
        if (histN > 0) {
            std::vector<cf32> tmp;
            const size_t cap = hist.size();
            const size_t start = histN < cap ? 0 : histW;
            std::vector<cf32> lin; lin.reserve(histN);
            for (size_t i = 0; i < histN; i++) lin.push_back(hist[(start + i) % cap]);
            s->ch.process(lin.data(), lin.size(), tmp);
            if (s->rx) s->rx->feed(tmp.data(), tmp.size()); else if (s->mic) s->mic->feed(tmp.data(), tmp.size());
        }
        Slot* raw = s.get();
        slots[c.id] = std::move(s);
        return raw;
    }

    void feedSpan(const cf32* x, size_t n) {   // under mu
        finder.setKeep(0);
        // squelch slider -> the finder's thresholds
        { static thread_local int lastSq = -1; (void)lastSq; }
        finder.setThresholds(onDbFor(), onDbFor() - 4, isMic() ? 6 : 0, isMic() ? 0.3 : 0.12, isMic() ? 8 : 5);
        finder.feed(x, n);
        for (int id : finder.takeRemoved()) retire(id);
        for (const Carrier& c : finder.carriers()) {
            Slot* s = nullptr;
            auto it = slots.find(c.id);
            if (it != slots.end()) s = it->second.get();
            else {
                // HF voice: the speech of one sideband fills more than one finder cell; the cells above a channel that already has a carrier belong to it
                bool part = false;
                if (svc == Service::HfVoice && centerAbs() > 0) {
                    const double abs = centerAbs() + c.hz;
                    for (auto& o : slots) { const double oa = centerAbs() + o.second->hz; if (o.second->key >= 0 && o.second->key < planSize() && abs > oa - 800 && abs < oa + 3600) part = true; }
                }
                if (part) continue;
                s = addSlot(c);
            }
            if (!s) continue;
            s->hz = c.hz; s->levelDb = c.levelDb; s->snrDb = c.snrDb; s->widthHz = c.widthHz; s->active = c.active; s->ageSec = c.ageSec;
            if (!s->fixed && std::fabs(c.hz - s->ch.shiftHz()) > 0.06 * finder.cfg().stepHz) s->ch.setShift(c.hz);
            if (s->rx) s->rx->setFinder(c.active, c.levelDb);
            if (s->mic) s->mic->setActive(c.active, c.snrDb);
        }
        std::vector<Slot*> v;
        for (auto& p : slots) v.push_back(p.second.get());
        auto work = [&](size_t i) {
            Slot& s = *v[i];
            s.tmp.clear();
            s.ch.process(x, n, s.tmp);
            if (s.rx) s.rx->feed(s.tmp.data(), s.tmp.size()); else if (s.mic) s.mic->feed(s.tmp.data(), s.tmp.size());
        };
        if (v.size() > 1) ThreadPool::shared().parallelFor(v.size(), work); else if (v.size() == 1) work(0);
        // history for the pre-roll (after the carriers were fed: a carrier found in the next block replays only what came before it)
        if (!hist.empty()) {
            const size_t cap = hist.size();
            if (n >= cap) { std::copy(x + (n - cap), x + n, hist.begin()); histW = 0; histN = cap; }
            else {
                const size_t first = std::min(n, cap - histW);
                std::copy(x, x + first, hist.begin() + (long)histW);
                if (first < n) std::copy(x + first, x + n, hist.begin());
                histW = (histW + n) % cap; histN = std::min(cap, histN + n);
            }
        }
    }
};

ChanVoiceReceiver::ChanVoiceReceiver() : p_(new Impl()) {}
ChanVoiceReceiver::~ChanVoiceReceiver() { std::lock_guard<std::mutex> lk(p_->mu); p_->clearSlots(); }

void ChanVoiceReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->cfgDone = false;
    p_->samples = 0; p_->lastDecide = -1;
    p_->ctx.spanSec = 0;
    p_->clearSlots();
    p_->hist.assign((size_t)std::max(1000.0, inputRateHz * 0.25), cf32{});
    p_->histW = p_->histN = 0;
    if (inputRateHz >= chanVoiceTuning(p_->svc).minSampleRate - 1) p_->regrid();
    p_->rebuildScan(0);
}
void ChanVoiceReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offHz = hz; p_->regrid(); }
void ChanVoiceReceiver::setCenterHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    const double old = p_->centerHz;
    p_->centerHz = hz;
    if (old > 0 && hz > 0 && std::fabs(hz - old) > 0.45 * p_->rate) { p_->clearSlots(); p_->finder.reset(); p_->histN = 0; }
    else if (old > 0 && hz > 0 && hz != old && p_->cfgDone) {   // a small retune: the carriers move with it
        p_->finder.shift(hz - old);
        for (int id : p_->finder.takeRemoved()) p_->retire(id);
        p_->histN = 0;
    }
    p_->regrid();
}
void ChanVoiceReceiver::setDialHz(double hz) { if (hz <= 0) return; std::lock_guard<std::mutex> lk(p_->mu); p_->dialHz = hz; p_->regrid(); }
bool ChanVoiceReceiver::ready() const { return p_->rate >= chanVoiceTuning(p_->svc).minSampleRate - 1 && p_->cfgDone; }
void ChanVoiceReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->clearSlots(); p_->finder.reset(); p_->histN = 0; p_->autoKey = -1; p_->ctx.playKey = -1;
}
void ChanVoiceReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

void ChanVoiceReceiver::feed(const cf32* x, size_t n) {
    if (!n) return;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!(p_->rate >= chanVoiceTuning(p_->svc).minSampleRate - 1) || !p_->cfgDone) return;
    p_->ctx.spanSec = (double)p_->samples / p_->rate;
    p_->feedSpan(x, n);
    p_->samples += n;
    const double now = (double)p_->samples / p_->rate;
    if (now - p_->lastDecide >= 0.05) { p_->lastDecide = now; p_->decide(now); }
}

void ChanVoiceReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->callMu);
    p_->calls.clear(); p_->mdcList.clear(); p_->stats.clear(); p_->recFiles = 0; p_->callSerial = 0; p_->mdcSerial = 0;
}

void ChanVoiceReceiver::setService(chv::Service s) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (s == p_->svc && p_->cfgDone) return;
    p_->svc = s;
    p_->clearSlots(); p_->finder.reset(); p_->histN = 0;
    p_->ctx.mdcOn = s == chv::Service::Mdc;
    p_->listenKey = -1; p_->autoKey = -1; p_->ctx.playKey = -1; p_->micListen = 0;
    p_->singleTuner = s == chv::Service::HfVoice;
    p_->cfgDone = false;
    { std::lock_guard<std::mutex> ck(p_->callMu); p_->stats.clear(); }
    if (p_->rate > 0 && p_->rate >= chanVoiceTuning(s).minSampleRate - 1) p_->regrid();
    p_->rebuildScan((double)p_->samples / std::max(1.0, p_->rate));
}
chv::Service ChanVoiceReceiver::service() const { return p_->svc; }
void ChanVoiceReceiver::setRegion(int r) {
    std::lock_guard<std::mutex> lk(p_->mu);
    const int n = (int)chv::regions(p_->svc).size();
    r = std::max(0, std::min(n - 1, r));
    if (r == p_->regionOf[(int)p_->svc]) return;
    p_->regionOf[(int)p_->svc] = r;
    p_->clearSlots(); p_->finder.reset(); p_->histN = 0;
    p_->listenKey = -1; p_->autoKey = -1;
    p_->cfgDone = false;
    { std::lock_guard<std::mutex> ck(p_->callMu); p_->stats.clear(); }
    if (p_->rate > 0 && p_->rate >= chanVoiceTuning(p_->svc).minSampleRate - 1) p_->regrid();
    p_->rebuildScan((double)p_->samples / std::max(1.0, p_->rate));
}
int ChanVoiceReceiver::region() const { return p_->region(); }
void ChanVoiceReceiver::setDemodOverride(int d) { p_->ctx.demodOverride = d; }
void ChanVoiceReceiver::listen(int key) { std::lock_guard<std::mutex> lk(p_->mu); p_->listenKey = key; p_->autoKey = -1; p_->lastDecide = -1; }
void ChanVoiceReceiver::setScan(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->scanOn = on; p_->rebuildScan((double)p_->samples / std::max(1.0, p_->rate)); }
void ChanVoiceReceiver::setScanKeys(const std::vector<int>& k) { std::lock_guard<std::mutex> lk(p_->mu); p_->scanKeys = k; p_->rebuildScan((double)p_->samples / std::max(1.0, p_->rate)); }
void ChanVoiceReceiver::setScanTiming(double dwell, double hang) { std::lock_guard<std::mutex> lk(p_->mu); p_->dwellSec = std::max(0.05, dwell); p_->hangSec = std::max(0.0, hang); p_->scan.setConfig(p_->scanCfg()); }
void ChanVoiceReceiver::setSingleTuner(bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->singleTuner = on; p_->rebuildScan((double)p_->samples / std::max(1.0, p_->rate)); }
void ChanVoiceReceiver::setRecording(bool on, const std::string& dir, double minSec) {
    { std::lock_guard<std::mutex> lk(p_->ctx.dirMu); p_->ctx.recDir = dir; }
    p_->ctx.recMinSec = minSec;
    p_->ctx.recOn = on && !dir.empty();
}
void ChanVoiceReceiver::setSquelch(int percent) { p_->ctx.squelch = std::max(0, std::min(100, percent)); }
void ChanVoiceReceiver::setDeemphasis(bool on) { p_->ctx.deemph = on; }
void ChanVoiceReceiver::setCarrierLimit(int n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    n = std::max(1, std::min(24, n));
    if (n == p_->carrierLimit) return;
    p_->carrierLimit = n; p_->cfgDone = false; p_->regrid();
}
void ChanVoiceReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void ChanVoiceReceiver::setMuted(bool m) { p_->muted = m; }
void ChanVoiceReceiver::setSilent(bool s) { p_->silent = s; }
void ChanVoiceReceiver::setChannelTap(std::function<void(int, int, const float*, size_t)> cb) { p_->ctx.tap = std::move(cb); }
int ChanVoiceReceiver::carrierCount() const { std::lock_guard<std::mutex> lk(p_->mu); return (int)p_->slots.size(); }
int ChanVoiceReceiver::playingKey() const { return p_->ctx.playKey.load(); }
void ChanVoiceReceiver::setMicListen(int id) { p_->micListen = id; }
void ChanVoiceReceiver::setMicBand(int index) { p_->micBand = index; }

bool ChanVoiceReceiver::telemetry(ChvTelemetry& out, uint64_t lastSeq) {
    Impl& I = *p_;
    const double wall = systemNowSec();
    if (wall - I.lastBuild >= 0.1) {
        I.lastBuild = wall;
        ChvTelemetry t;
        struct SlotView { int key; std::string label; double mhz; bool active, open; double level; int demod; chv::SubTone tone; int id; double width, snr, dev, age; bool fm; bool mic; };
        std::vector<SlotView> sv;
        Service svc; int reg; double span, ctr;
        std::vector<int> scanned; bool scanOn, singleTuner;
        std::vector<chv::Channel> planCopy;
        int scanState = 0; double tuneReq = 0; int micL;
        {
            std::lock_guard<std::mutex> lk(I.mu);
            svc = I.svc; reg = I.region(); span = I.ctx.spanSec.load(); ctr = I.centerAbs();
            t.inputRate = I.rate;
            scanOn = I.scanOn; singleTuner = I.singleTuner; scanState = (int)I.scan.state(); tuneReq = I.tuneRequestMhz; micL = I.micListen;
            if (scanOn) { if (I.scanKeys.empty()) for (int k = 0; k < I.planSize(); k++) scanned.push_back(k); else scanned = I.scanKeys; }
            for (auto& p : I.slots) {
                Slot& s = *p.second;
                SlotView v; v.key = s.key; v.label = s.label; v.mhz = s.mhz; v.active = s.active; v.id = s.id; v.width = s.widthHz; v.snr = s.snrDb; v.age = s.ageSec; v.level = s.levelDb;
                v.open = s.rx ? I.ctx.act[std::max(0, s.key)].open.load() != 0 && s.key >= 0 : false;
                v.demod = s.rx ? s.rx->demodNow() : (int)Demod::Wfm;
                if (s.key >= 0 && s.key < kMaxKeys) { const int ci = I.ctx.act[s.key].ctcss.load(), dc = I.ctx.act[s.key].dcs.load(); if (ci >= 0) { v.tone.ctcssIdx = ci; v.tone.ctcssHz = tones::ctcssHz(ci); } if (dc >= 0) { v.tone.dcsCode = dc / 2; v.tone.dcsInverted = dc & 1; } }
                v.dev = s.mic ? s.mic->devKhz() : 0; if (s.mic) { v.mhz = (I.centerAbs() + s.ch.shiftHz() + s.mic->meanHz()) / 1e6; } v.fm = s.mic ? s.mic->fmLike() : false; v.mic = (bool)s.mic;
                sv.push_back(std::move(v));
            }
        }
        const auto& chs = chv::channels(svc, reg);
        t.service = (int)svc; t.region = reg; t.timeSec = span; t.centerHz = ctr;
        t.carriers = (int)sv.size();
        t.scanOn = scanOn; t.scanState = scanState; t.singleTuner = singleTuner; t.tuneRequestMhz = tuneReq;
        t.spanCovers = I.rate >= 100e3;
        const int playKey = I.ctx.playKey.load();
        t.playingKey = playKey;
        int planN;
        { std::lock_guard<std::mutex> lk(I.mu); planN = I.planSize(); }
        if (playKey >= 0 && playKey < planN) { t.playingLabel = chs[(size_t)playKey].label; t.playingMhz = I.ctx.act[playKey].mhz.load(); }
        else if (playKey >= planN && playKey < kMaxKeys) { for (auto& v : sv) if (v.key == playKey) { t.playingLabel = v.label; t.playingMhz = v.mhz; } }
        const time_t now = (time_t)unixNow(); struct tm g{}; gmTime(now, &g);
        std::vector<bool> inScan((size_t)planN, false);
        for (int k : scanned) if (k >= 0 && k < planN) inScan[(size_t)k] = true;
        {
            std::lock_guard<std::mutex> lk(I.callMu);
            t.callsTotal = I.callSerial; t.mdcTotal = I.mdcSerial; t.recordedFiles = I.recFiles;
            if (svc != Service::Mic && svc != Service::Mdc) {
                for (int k = 0; k < planN; k++) {
                    ChvChannelRow r; const chv::Channel& c = chs[(size_t)k];
                    r.key = k; r.number = c.number; r.label = c.label; r.name = c.name; r.mhz = c.mhz; r.txMhz = c.txMhz; r.demod = (int)c.demod;
                    r.inScan = inScan[(size_t)k]; r.playing = k == playKey;
                    r.onAir = chv::onAirAtMinute(c, g.tm_min);
                    r.open = I.ctx.act[k].open.load() != 0;
                    r.active = r.open;
                    for (auto& v : sv) if (v.key == k) { r.active = v.active || r.open; r.levelDb = v.level; r.demod = v.demod; }
                    if (r.open) {
                        r.levelDb = I.ctx.act[k].level.load(); r.demod = I.ctx.act[k].demod.load();
                        const int ci = I.ctx.act[k].ctcss.load(), dc = I.ctx.act[k].dcs.load();
                        if (ci >= 0) { r.tone.ctcssIdx = ci; r.tone.ctcssHz = tones::ctcssHz(ci); }
                        if (dc >= 0) { r.tone.dcsCode = dc / 2; r.tone.dcsInverted = dc & 1; }
                    }
                    auto it = I.stats.find(k);
                    if (it != I.stats.end()) {
                        r.calls = it->second.calls; r.totalSec = it->second.totalSec; r.lastHeardWall = it->second.lastHeardWall; r.peakDb = it->second.peakDb; r.lastTone = it->second.lastTone;
                        r.lastHeardAgoSec = std::max(0.0, span - it->second.lastHeardSpan);
                    }
                    if (r.open) r.lastHeardAgoSec = 0;
                    t.rows.push_back(std::move(r));
                }
            }
            if (svc != Service::Mic) {
                for (auto& v : sv) if (v.key >= planN || svc == Service::Mdc) {   // carriers off the plan
                    ChvChannelRow r; r.key = v.key; r.label = v.label; r.mhz = v.mhz; r.active = v.active; r.open = v.key >= 0 && I.ctx.act[v.key].open.load() != 0; r.levelDb = v.level; r.demod = v.demod; r.tone = v.tone;
                    r.playing = v.key == playKey;
                    auto it = I.stats.find(v.key);
                    if (it != I.stats.end()) { r.calls = it->second.calls; r.totalSec = it->second.totalSec; r.lastHeardWall = it->second.lastHeardWall; r.peakDb = it->second.peakDb; r.lastTone = it->second.lastTone; }
                    t.rows.push_back(std::move(r));
                }
            } else {
                const auto& bands = chv::micBands(reg);
                for (auto& v : sv) {
                    ChvMicRow m; m.id = v.id; m.mhz = v.mhz; m.widthKhz = v.width / 1000.0; m.levelDb = v.level; m.snrDb = v.snr; m.devKhz = v.dev; m.fmLike = v.fm; m.active = v.active; m.listening = v.id == micL; m.ageSec = v.age;
                    for (const auto& b : bands) if (v.mhz >= b.loMhz && v.mhz <= b.hiMhz) m.band = b.name;
                    t.mics.push_back(std::move(m));
                }
                std::sort(t.mics.begin(), t.mics.end(), [](const ChvMicRow& a, const ChvMicRow& b) { return a.mhz < b.mhz; });
            }
            for (const ChvCall& c : I.calls) t.calls.push_back(c);
            for (const ChvMdcEvent& e : I.mdcList) t.mdc.push_back(e);
        }
        for (const auto& r : t.rows) if (r.open) { t.state = 2; break; }
        if (t.state == 0) t.state = (I.rate >= chanVoiceTuning(svc).minSampleRate - 1 && I.cfgDone) ? 1 : 0;
        t.dataValid = t.callsTotal > 0 || !t.mics.empty();
        t.blocksOk = t.callsTotal;
        t.seq = I.tel.seq + 1;
        I.tel = std::move(t);
    }
    if (I.tel.seq <= lastSeq) return false;
    out = I.tel;
    return true;
}

ModeTuning chanVoiceTuning(chv::Service s) {
    ModeTuning t;
    t.stdMode = chv::familyOf(s) + 2;
    t.id = chv::serviceId(s);
    t.name = chv::serviceName(s);
    t.sampleRate = 2000000;
    t.minSampleRate = 250000;
    t.bandwidthMhz = 1.0;
    t.tuneOffsetHz = 0;
    t.carrierAtCentre = true;
    switch (s) {
    case chv::Service::Cb:      t.minMhz = 26.0; t.maxMhz = 28.0; t.defMhz = 27.185; t.bandwidthMhz = 0.5; t.sampleRate = 1000000; t.minSampleRate = 100000; t.tuneOffsetHz = 25000; break;
    case chv::Service::Frs:     t.minMhz = 462.0; t.maxMhz = 468.0; t.defMhz = 462.6375; t.sampleRate = 2000000; t.bandwidthMhz = 1.0; t.tuneOffsetHz = 12500; break;   // 1 - 22: 462.55 - 462.725 MHz in one capture; the 467 MHz channels are 5 MHz higher (tune there to hear them, or use 8 Msps)
    case chv::Service::Pmr:     t.minMhz = 446.0; t.maxMhz = 446.2; t.defMhz = 446.1; t.sampleRate = 1000000; t.bandwidthMhz = 0.5; t.minSampleRate = 250000; t.tuneOffsetHz = 25000; break;
    case chv::Service::Ham:     t.minMhz = 144.0; t.maxMhz = 450.0; t.defMhz = 145.7; t.sampleRate = 2000000; t.bandwidthMhz = 1.0; t.tuneOffsetHz = 25000; break;
    case chv::Service::HfVoice: t.minMhz = 2.8; t.maxMhz = 23.4; t.defMhz = 11.175; t.sampleRate = 250000; t.bandwidthMhz = 0.1; t.minSampleRate = 48000; t.tuneOffsetHz = 12000; break;
    case chv::Service::Mdc:     t.minMhz = 30.0; t.maxMhz = 960.0; t.defMhz = 155.0; t.sampleRate = 2000000; t.bandwidthMhz = 1.0; t.tuneOffsetHz = 25000; break;
    default:                    t.minMhz = 470.0; t.maxMhz = 960.0; t.defMhz = 590.0; t.sampleRate = 10000000; t.bandwidthMhz = 8.0; t.minSampleRate = 1000000; t.tuneOffsetHz = 0; break;
    }
    return t;
}

} // namespace dect2
