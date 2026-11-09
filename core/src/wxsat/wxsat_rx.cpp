// See wxsat_rx.h. Link parameters from SatDump's pipelines (GPL-3.0, Copyright Aang23 and contributors); code written for OnAir.
#include "dect2/wxsat/wxsat_rx.h"
#include "dect2/wxsat/wxsat_proto.h"
#include "dect2/wxsat/filecast.h"
#include "dect2/dvbs/dvbs_rx.h"
#include "dect2/common/mathconst.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/rrc.h"
#include "dect2/elektro/elektro_ggak.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {
using math::kPi;
using cfp = std::complex<float>;
constexpr double kPubSec = 0.1;

double nowSec() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }

inline int8_t sat8(float v) { return (int8_t)std::clamp((int)std::lround(v), -127, 127); }

// Decimating FIR, complex data, doubled ring
struct FirDec {
    std::vector<float> taps, re, im;
    int pos = 0, n = 0, dec = 1, cnt = 0;
    void design(int ratio, double cutoffNorm /* cycles a sample */) {
        dec = std::max(1, ratio);
        n = std::max(3, 12 * dec + 1);
        taps.assign((size_t)n, 0);
        double sum = 0;
        for (int i = 0; i < n; i++) {
            const double t = i - (n - 1) / 2.0;
            const double s = t == 0 ? 2 * cutoffNorm : std::sin(2 * kPi * cutoffNorm * t) / (kPi * t);
            const double w = 0.54 - 0.46 * std::cos(2 * kPi * i / (n - 1));
            taps[(size_t)i] = (float)(s * w);
            sum += taps[(size_t)i];
        }
        for (auto& v : taps) v /= (float)sum;
        re.assign((size_t)n * 2, 0); im.assign((size_t)n * 2, 0);
        pos = 0; cnt = 0;
    }
    void clear() { std::fill(re.begin(), re.end(), 0.f); std::fill(im.begin(), im.end(), 0.f); pos = 0; cnt = 0; }
    bool push(cfp x, cfp& out) {
        pos = (pos == 0 ? n : pos) - 1;
        re[(size_t)pos] = re[(size_t)pos + n] = x.real();
        im[(size_t)pos] = im[(size_t)pos + n] = x.imag();
        if (++cnt < dec) return false;
        cnt = 0;
        float a = 0, b = 0;
        const float* r = &re[(size_t)pos], *i2 = &im[(size_t)pos];
        for (int k = 0; k < n; k++) { a += taps[(size_t)k] * r[k]; b += taps[(size_t)k] * i2[k]; }
        out = {a, b};
        return true;
    }
};

// Symmetric matched filter
struct Rrc {
    std::vector<float> h, re, im;
    int n = 0, pos = 0;
    void design(double sps, double alpha) {
        const int half = std::max(4, (int)std::ceil(4 * sps));
        n = 2 * half + 1;
        h.resize((size_t)n);
        double e = 0;
        for (int i = 0; i < n; i++) { h[(size_t)i] = (float)dsp::rrc((i - half) / sps, alpha); e += h[(size_t)i] * h[(size_t)i]; }
        for (auto& v : h) v /= (float)std::sqrt(e);
        re.assign((size_t)n * 2, 0); im.assign((size_t)n * 2, 0);
        pos = 0;
    }
    void clear() { std::fill(re.begin(), re.end(), 0.f); std::fill(im.begin(), im.end(), 0.f); pos = 0; }
    cfp push(cfp x, bool complexIn) {
        pos = (pos == 0 ? n : pos) - 1;
        re[(size_t)pos] = re[(size_t)pos + n] = x.real();
        float a = 0;
        const float* r = &re[(size_t)pos];
        for (int k = 0; k < n; k++) a += h[(size_t)k] * r[k];
        if (!complexIn) return {a, 0};
        im[(size_t)pos] = im[(size_t)pos + n] = x.imag();
        float b = 0;
        const float* i2 = &im[(size_t)pos];
        for (int k = 0; k < n; k++) b += h[(size_t)k] * i2[k];
        return {a, b};
    }
};

enum Mod { Pm, Bpsk, Qpsk };

void loopGains(double bl, double zeta, double& a, double& b) {
    const double w = bl / (zeta + 1.0 / (4 * zeta));
    const double d = 1 + 2 * zeta * w + w * w;
    a = 4 * zeta * w / d; b = 4 * w * w / d;
}

class Demod {
public:
    double inRate = 0, sym = 0, offsetHz = 0, decRate = 0;
    Mod mod = Bpsk;
    bool acquired = false, acqPending = false;
    double cfoCoarse = 0;
    float agc = 1, carrierLock = 0, snrDb = 0;
    std::vector<std::pair<float, float>> cons;
    size_t consPos = 0;
    double acqPeak = 0;

    void configure(double rate, double symbolRate, Mod m, double alpha) {
        inRate = rate; sym = symbolRate; mod = m; alpha_ = alpha;
        int ratio = (int)std::floor(rate / (4.0 * sym));
        if (ratio < 2) ratio = 1;
        ratio_ = ratio;
        decRate = rate / ratio;
        if (ratio > 1) fir_.design(ratio, 0.5 / ratio * 0.95);
        spsNom_ = decRate / sym;
        rrc_.design(spsNom_, alpha);
        // loop bandwidths per symbol
        loopGains(m == Pm ? 0.004 * (spsNom_ / 4.0) : m == Bpsk ? 0.008 : 0.005, 0.707, cA_, cB_);
        tBl_ = 0.006;
        resetAll();
    }
    void setOffset(double hz) { offsetHz = hz; updateStep(); }
    void resetAll() {
        fir_.clear(); rrc_.clear();
        rot_ = 1; for (auto& h : h_) h = 0;
        acquired = false; acq_.clear(); acq_.reserve(kAcq); cfoCoarse = 0; updateStep();
        phase_ = 0; freq_ = 0; t_ = spsNom_ / 2; strobePh_ = 0; lastStrobe_ = 0; mid_ = 0; sps_ = spsNom_;
        agc = 0.1f; carrierLock = 0; snrDb = 0; meanAbs_ = 0; varAbs_ = 0;
        cons.clear(); consPos = 0;
    }
    void reacquire() { acquired = false; acq_.clear(); }
    double currentCfo() const { return cfoCoarse + freq_ * decRate / (2 * kPi); }

    template<class S> void feed(const cf32* x, size_t n, S&& sink) {
        for (size_t i = 0; i < n; i++) {
            rot_ *= step_;
            if (((++renorm_) & 4095) == 0) rot_ /= std::abs(rot_);
            cfp v = cfp(x[i].real(), x[i].imag()) * rot_;
            cfp y;
            if (ratio_ == 1) y = v; else if (!fir_.push(v, y)) continue;
            sample(y, sink);
        }
    }

private:
    static constexpr int kAcq = 1 << 15;
    std::vector<cfp> acq_;
    FirDec fir_;
    Rrc rrc_;
    int ratio_ = 1;
    double spsNom_ = 2, sps_ = 2, alpha_ = 0.5, cA_ = 0.01, cB_ = 1e-4, tBl_ = 0.005;
    double phase_ = 0, freq_ = 0, t_ = 1;
    std::complex<double> rotD_{1, 0};
    cfp rot_{1, 0}, step_{1, 0};
    uint32_t renorm_ = 0;
    cfp h_[4];
    int strobePh_ = 0;
    cfp lastStrobe_{0, 0}, mid_{0, 0};
    float meanAbs_ = 0, varAbs_ = 0;
    void updateStep() {
        const double w = -2 * kPi * (offsetHz + cfoCoarse) / inRate;
        step_ = cfp((float)std::cos(w), (float)std::sin(w));
    }

    void acquire() {
        const int log2n = 15, N = 1 << log2n;
        std::vector<float> re((size_t)N), im((size_t)N), pow((size_t)N);
        const int mult = mod == Pm ? 1 : mod == Bpsk ? 2 : 4;
        for (int i = 0; i < N; i++) {
            cfp v = acq_[(size_t)i];
            cfp w = mod == Pm ? v : mod == Bpsk ? v * v : (v * v) * (v * v);
            w /= std::max(1e-7f, std::abs(w));   // limits strong interference
            const float win = 0.5f - 0.5f * (float)std::cos(2 * kPi * i / N);
            re[(size_t)i] = w.real() * win; im[(size_t)i] = w.imag() * win;
        }
        fftSplit(re.data(), im.data(), log2n, false);
        for (int i = 0; i < N; i++) pow[(size_t)i] = re[(size_t)i] * re[(size_t)i] + im[(size_t)i] * im[(size_t)i];
        const double maxHz = std::min(0.45 * decRate / mult * 1.0, mod == Pm ? 0.45 * sym : 0.45 * decRate / mult);
        const int maxBin = (int)(maxHz * mult / decRate * N);
        int best = 0; float bv = 0;
        double total = 0;
        for (int k = -maxBin; k <= maxBin; k++) { const float v = pow[(size_t)((k + N) % N)]; total += v; if (v > bv) { bv = v; best = k; } }
        acqPeak = bv / std::max(1e-9, total / (2 * maxBin + 1));
        double delta = 0;
        if (acqPeak > 6) {
            const float l = pow[(size_t)((best - 1 + N) % N)], c = bv, r = pow[(size_t)((best + 1 + N) % N)];
            const double d = 2 * c - l - r;
            delta = d > 0 ? 0.5 * (r - l) / d : 0;
            cfoCoarse += (best + delta) * decRate / N / mult;
        }
        updateStep();
        phase_ = 0; freq_ = 0;
        acquired = true;
    }

    template<class S> void sample(cfp y, S&& sink) {
        if (!acquired) {
            acq_.push_back(y);
            if ((int)acq_.size() >= kAcq) { acquire(); acq_.clear(); }
            return;
        }
        // carrier
        const cfp nco((float)std::cos(phase_), (float)-std::sin(phase_));
        cfp v = y * nco;
        if (mod == Pm) {
            const float e = std::atan2(v.imag(), v.real());
            freq_ += cB_ * e * 0.5f;
            phase_ += freq_ + cA_ * e * 0.5f;
            carrierLock += 0.0005f * (std::cos(e) * std::cos(e) - carrierLock);
            v = rrc_.push(cfp(v.imag(), 0), false);
        } else {
            phase_ += freq_;
            v = rrc_.push(v, true);
        }
        if (phase_ > 2 * kPi) phase_ -= 2 * kPi; else if (phase_ < -2 * kPi) phase_ += 2 * kPi;
        h_[0] = h_[1]; h_[1] = h_[2]; h_[2] = h_[3]; h_[3] = v;
        t_ -= 1.0;
        while (t_ <= 0) {
            const float d = (float)-t_;
            const float mu = 1.f - d;
            const cfp a = interp(mu);
            event(a, sink);
        }
    }

    cfp interp(float mu) const {
        const float c0 = -mu * (mu - 1) * (mu - 2) / 6, c1 = (mu + 1) * (mu - 1) * (mu - 2) / 2, c2 = -(mu + 1) * mu * (mu - 2) / 2, c3 = (mu + 1) * mu * (mu - 1) / 6;
        return h_[0] * c0 + h_[1] * c1 + h_[2] * c2 + h_[3] * c3;
    }

    template<class S> void event(cfp a, S&& sink) {
        if (strobePh_ == 0) { mid_ = a; strobePh_ = 1; t_ += sps_ * 0.5; return; }
        strobePh_ = 0;
        const float mag2 = std::max(1e-9f, std::abs(a) * std::abs(a));
        agc += 0.002f * (std::sqrt(mag2 / (mod == Qpsk ? 2 : 1)) - agc);
        // Gardner
        float e;
        if (mod == Qpsk) e = (lastStrobe_.real() - a.real()) * mid_.real() + (lastStrobe_.imag() - a.imag()) * mid_.imag();
        else e = (lastStrobe_.real() - a.real()) * mid_.real();
        e /= std::max(1e-6f, agc * agc);
        lastStrobe_ = a;
        e = std::clamp(e, -1.f, 1.f);
        double wA, wB;
        loopGains(tBl_, 0.9, wA, wB);
        sps_ = std::clamp(sps_ + wB * e * spsNom_, spsNom_ * 0.992, spsNom_ * 1.008);
        t_ += sps_ * 0.5 + wA * e * spsNom_ * 0.5;
        // carrier (BPSK, QPSK)
        const float I = a.real() / agc, Q = a.imag() / agc;
        if (mod != Pm) {
            const float ec = mod == Bpsk ? std::clamp((I > 0 ? 1.f : -1.f) * Q, -1.f, 1.f)
                                         : std::clamp((I > 0 ? 1.f : -1.f) * Q - (Q > 0 ? 1.f : -1.f) * I, -1.f, 1.f);
            freq_ += cB_ * ec / spsNom_;
            phase_ += cA_ * ec;
            carrierLock += 0.002f * ((1.f - std::fabs(ec)) - carrierLock);
        }
        const float ab = (mod == Qpsk) ? 0.5f * (std::fabs(I) + std::fabs(Q)) : std::fabs(I);
        meanAbs_ += 0.003f * (ab - meanAbs_);
        varAbs_ += 0.003f * ((ab - meanAbs_) * (ab - meanAbs_) - varAbs_);
        snrDb = 10.f * (float)std::log10(std::max(1e-3f, meanAbs_ * meanAbs_ / std::max(1e-6f, 2 * varAbs_)));
        if (cons.size() < 256) cons.emplace_back(I, mod == Qpsk ? Q : 0.f);
        else { cons[consPos] = {I, mod == Qpsk ? Q : 0.f}; consPos = (consPos + 1) % 256; }
        sink(I, Q);
    }
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------------------
struct WxsatReceiver::Impl {
    const int P;
    const WxProfileInfo info;
    mutable std::mutex mu;
    std::function<void(const std::string&)> logCb;
    double rate = 0, offset = 0, setRate = 0;
    std::atomic<bool> resetReq{false}, clearReq{false};
    WxsatTelemetry tel, pub;

    Demod dem;
    // APT
    FirDec aptFir;
    cfp aptPrev{1, 0};
    double aptAudioRate = 0;
    wxsat::AptDecoder apt;
    std::vector<float> aptBuf;
    double aptCfo = 0;
    GgakReceiver ggak;
    uint64_t ggakSeq = 0;

    // chips (Manchester)
    bool manchester = false;
    float prevChip = 0;
    int chipN = 0;
    int alignment = -1;
    wxsat::Deframer def[2];   // the two chip pairings until one locks
    uint64_t maskedHits = 0;
    int defUse = -1;
    // NRZ-M
    int nrzPrev = 0;
    // QPSK Viterbi
    std::vector<int8_t> pairs;
    int pairsSince = 0, xt = 0;
    bool searchLock = false;
    hrpt::Viterbi vit;
    wxsat::Deframer defCcsds, msu, visDef[3], irDef;
    wxsat::AwsDemux aws[64];
    int msgLast = -1;
    uint64_t cyclesDone = 0;
    // file broadcasts: the DVB-S/S2 receiver of the app and the file layer behind it
    std::unique_ptr<DvbsReceiver> dvb;
    std::unique_ptr<wxsat::FilecastLayer> files;
    std::atomic<double> fcSymbols{0};
    std::atomic<bool> fcSymbolsSet{false};
    std::atomic<bool> fileBlocking{false};    // a recording: wait for the decoder rather than lose frames
    uint64_t dvbSeq = 0;
    bool isFilecast() const { return P == kWxHimawariCast || P == kWxGeoNetCast; }
    // the DVB-S2 receiver's own limit, at the rate it is told to expect
    bool fileRateOk() const { const double hz = fcSymbols.load(); return dvbsMaxSymbolRate(rate, 0.35) * 1.02 >= (hz > 0 ? hz : info.symbolRate); }
    wxsat::MwrDecoder mwr;
    wxsat::HirsDecoder hirs;
    double lastGood = 0, lastAcq = 0, tStart = 0;
    int64_t nIn = 0, nextPub = 0;

    // pictures
    std::shared_ptr<WxImage> im[2];
    std::map<int, std::shared_ptr<WxLine>> edit[2];
    std::vector<int> dirty[2];
    bool imgDirty[2] = {false, false};
    uint64_t imgId = 0;
    bool haveMs = false;
    uint32_t lastMs = 0;
    int lastIdx = -1;
    int lineCount = 0, visEdit = -1;
    float mmn[32], mmx[32];
    bool mwrInit = false;

    explicit Impl(int p) : P(std::clamp(p, 0, (int)kWxNumProfiles - 1)), info(wxProfile(P)) {
        tel.profile = pub.profile = P;
        if (isFilecast()) files = std::make_unique<wxsat::FilecastLayer>(P);
        apt.onLine = [this](int idx, const uint8_t* px) { setLine(0, idx, px, 2080); };
    }

    void say(const std::string& s) { if (logCb) logCb(std::string("wxsat ") + info.id + ": " + s); }

    void initImages() {
        const auto mk = [&](int w, int c, const char* n) { auto i = std::make_shared<WxImage>(); i->name = n; i->width = w; i->channels = c; i->id = ++imgId; return i; };
        im[0].reset(); im[1].reset();
        switch (P) {
        case kWxApt: im[0] = mk(2080, 1, "APT"); break;
        case kWxNoaaHrpt: im[0] = mk(kHrptWidth, kHrptChannels, "AVHRR"); im[1] = mk(wxsat::kHirsWidth, wxsat::kHirsChannels, "HIRS"); break;
        case kWxNoaaGac: im[0] = mk(wxsat::kGacWidth, kHrptChannels, "AVHRR GAC"); im[1] = mk(wxsat::kHirsWidth, wxsat::kHirsChannels, "HIRS"); break;
        case kWxNoaaDsb: im[0] = mk(wxsat::kHirsWidth, wxsat::kHirsChannels, "HIRS"); break;
        case kWxMeteorHrpt: im[0] = mk(wxsat::kMsuMrWidth, wxsat::kMsuMrChannels, "MSU-MR"); break;
        case kWxElektroRdas: case kWxArktikaRdas:
            im[0] = mk(wxsat::kMsuGsIrWidth, wxsat::kMsuGsIrChannels, "MSU-GS IR");
            im[1] = mk(wxsat::kMsuGsVisWidth, 3, "MSU-GS visible (reduced)");
            break;
        case kWxAws: im[0] = mk(wxsat::kMwrWidth, wxsat::kMwrChannels, "MWR"); break;
        case kWxMsgRaw: im[0] = mk(wxsat::kSeviriNrmOutW, wxsat::kSeviriNrmChannels, "SEVIRI"); im[1] = mk(wxsat::kSeviriHrvOutW, 1, "SEVIRI HRV"); break;
        default: break;
        }
        for (auto& e : edit) e.clear();
        for (auto& d : dirty) d.clear();
        haveMs = false; lastIdx = -1; lineCount = 0; mwrInit = false;
    }

    void setup() {
        double alpha = 0.5;
        Mod mod = Bpsk;
        manchester = false;
        switch (P) {
        case kWxNoaaHrpt: case kWxMeteorHrpt: mod = Pm; alpha = 1.0; manchester = true; break;
        case kWxNoaaDsb: mod = Pm; alpha = 1.0; manchester = true; break;
        case kWxNoaaGac: mod = Bpsk; alpha = 0.5; break;
        case kWxElektroRdas: case kWxArktikaRdas: mod = Bpsk; alpha = 0.5; break;
        case kWxAws: mod = Qpsk; alpha = 0.45; break;
        case kWxMsgRaw: mod = Qpsk; alpha = 0.35; break;
        default: break;
        }
        if (P == kWxApt) {
            const int ratio = std::max(1, (int)std::floor(rate / 40000.0));
            aptAudioRate = rate / ratio;
            aptFir.design(ratio, 18000.0 / rate);
            apt.configure(aptAudioRate);
            aptPrev = {1, 0};
            return;
        }
        if (P == kWxArktikaGgak) { ggak.configure(rate); ggak.setSignalOffset(offset); return; }
        if (isFilecast()) { if (dvb) configureDvb(); return; }
        if (rate >= info.minRate - 1) { dem.configure(rate, info.symbolRate, mod, alpha); dem.setOffset(offset); }
        const auto frameCb = [this](const uint8_t* f) { frame(f); };
        for (int k = 0; k < 2; k++) def[k].onFrame = frameCb;
        defCcsds.onFrame = frameCb;
        switch (P) {
        case kWxNoaaHrpt: for (int k = 0; k < 2; k++) def[k].configure(hrpt::noaaSync60(), 60, hrpt::kNoaaWords * 10, 8, 14); break;
        case kWxNoaaDsb: for (int k = 0; k < 2; k++) def[k].configure(wxsat::kDsbSync, 16, wxsat::kDsbBytes * 8, 0, 4); break;
        case kWxMeteorHrpt: for (int k = 0; k < 2; k++) def[k].configure(meteor::kAsm, 32, wxsat::kCadu * 8, 3, 10); break;
        case kWxNoaaGac: def[0].configure(wxsat::kGacSync, 32, wxsat::kGacBits, 3, 10); break;
        case kWxElektroRdas: case kWxArktikaRdas: def[0].configure(meteor::kAsm, 32, wxsat::kCadu * 8, 2, 8); break;
        case kWxAws: case kWxMsgRaw: defCcsds.configure(meteor::kAsm, 32, wxsat::kAwsCadu * 8, 3, 10); break;
        default: break;
        }
        msu.configure(wxsat::kMsuSync, 64, wxsat::kMsuMrFrame * 8, 10, 14);
        msu.onFrame = [this](const uint8_t* f) { msuMr(f); };
        irDef.configure(wxsat::kMsuSync, 64, wxsat::kMsuGsIrFrame * 8, 10, 14);
        irDef.onFrame = [this](const uint8_t* f) { ir(f); };
        for (int v = 0; v < 3; v++) { visDef[v].configure(wxsat::kMsuSync, 64, wxsat::kMsuGsVisFrame * 8, 10, 14); visDef[v].onFrame = [this, v](const uint8_t* f) { vis(v, f); }; }
        for (auto& a : aws) { a.reset(); a.onPacket = [this](const wxsat::AwsPacket& p) { awsPacket(p); }; }
    }

    void resetState() {
        if (P == kWxApt) { apt.reset(); aptFir.clear(); aptPrev = {1, 0}; aptBuf.clear(); }
        else if (P == kWxArktikaGgak) { ggak.reset(); }
        else if (isFilecast()) { if (dvb) dvb->reset(); files->reset(); }
        else if (rate >= info.minRate - 1) dem.resetAll();
        chipN = 0; alignment = -1; defUse = -1; prevChip = 0; nrzPrev = 0;
        for (auto& d : def) d.reset();
        defCcsds.reset(); msu.reset(); irDef.reset(); for (auto& d : visDef) d.reset();
        pairs.clear(); pairsSince = 0; searchLock = false; vit.reset();
        lastGood = lastAcq = nowSec();
        tel.state = 0; tel.frameLock = false;
    }

    // ---- bits ----
    void bit(int b) {
        if (P == kWxElektroRdas || P == kWxArktikaRdas) { const int d = b ^ nrzPrev; nrzPrev = b; b = d; }
        if (defUse >= 0) def[defUse].push(b); else def[0].push(b);
    }

    void chip(float c) {
        // two possible pairings of the chips into bits, tried side by side until one finds the sync
        chipN++;
        const int pair = chipN & 1;
        if (defUse < 0) {
            def[pair].push(prevChip - c > 0 ? 1 : 0);
            if (def[pair].locked()) { defUse = pair; alignment = pair; say("frame sync (Manchester pairing " + std::to_string(pair) + ")"); }
        } else if (pair == defUse) {
            def[defUse].push(prevChip - c > 0 ? 1 : 0);
            if (!def[defUse].locked()) { defUse = -1; alignment = -1; tel.frameLock = false; }
        }
        prevChip = c;
    }

    void symbol(float I, float Q) {
        switch (P) {
        case kWxNoaaHrpt: case kWxNoaaDsb: case kWxMeteorHrpt: chip(I); break;
        case kWxNoaaGac: case kWxElektroRdas: case kWxArktikaRdas: bit(I > 0 ? 1 : 0); break;
        case kWxAws: case kWxMsgRaw: qpskPair(sat8(I * 40), sat8(Q * 40)); break;
        default: break;
        }
    }

    // QPSK: the encoded sync word settles the phase ambiguity
    static void xform(int t, int a, int b, int& oa, int& ob) {
        if (t & 4) b = -b;
        switch (t & 3) { case 0: oa = a; ob = b; break; case 1: oa = -b; ob = a; break; case 2: oa = -a; ob = -b; break; default: oa = b; ob = -a; break; }
    }
    void qpskPair(int8_t a, int8_t b) {
        if (searchLock) { int oa, ob; xform(xt, a, b, oa, ob); vitPush(oa, ob); return; }
        pairs.push_back(a); pairs.push_back(b);
        pairsSince++;
        const int frameSyms = wxsat::kAwsCadu * 8, need = 2 * frameSyms + 64;
        if ((int)pairs.size() / 2 > 3 * need) pairs.erase(pairs.begin(), pairs.begin() + (std::ptrdiff_t)(pairs.size() - (size_t)need * 2));
        if (pairsSince >= frameSyms / 2 && (int)pairs.size() / 2 >= frameSyms + 64) { pairsSince = 0; search(frameSyms); }
    }
    void search(int frameSyms) {
        static const uint64_t pat = meteor::encodedAsm();
        const uint64_t mask = (1ull << 52) - 1;
        const int np = (int)(pairs.size() / 2);
        for (int t = 0; t < 8; t++) {
            uint64_t w = 0;
            std::vector<int> hits;
            for (int i = 0; i < np; i++) {
                int oa, ob; xform(t, pairs[(size_t)i * 2], pairs[(size_t)i * 2 + 1], oa, ob);
                w = w << 2 | (uint64_t)(oa > 0) << 1 | (uint64_t)(ob > 0);
                if (i >= 31 && __builtin_popcountll((w ^ pat) & mask) <= 7) hits.push_back(i);
            }
            int at = -1;
            for (size_t x = 0; x < hits.size() && at < 0; x++)
                for (size_t y = x + 1; y < hits.size(); y++) if (hits[y] - hits[x] == frameSyms) { at = hits[y]; break; }
            if (at < 0) continue;
            searchLock = true; xt = t;
            vit.reset(); defCcsds.reset();
            const int from = std::max(0, at - 31 - frameSyms - 40);
            for (int i = from; i < np; i++) { int oa, ob; xform(xt, pairs[(size_t)i * 2], pairs[(size_t)i * 2 + 1], oa, ob); vitPush(oa, ob); }
            pairs.clear();
            say("sync word found, ambiguity " + std::to_string(t));
            return;
        }
    }
    void vitPush(int a, int b) {
        vit.push(a, b);
        auto& o = vit.out();
        for (uint8_t bb : o) defCcsds.push(bb);
        o.clear();
    }

    // ---- frames ----
    void good() { tel.blocksOk++; tel.asmHits++; lastGood = nowSec(); }

    void frame(const uint8_t* f) {
        switch (P) {
        case kWxNoaaHrpt: noaaHrpt(f); break;
        case kWxNoaaGac: gac(f); break;
        case kWxNoaaDsb: dsb(f); break;
        case kWxMeteorHrpt: meteorCadu(f); break;
        case kWxElektroRdas: case kWxArktikaRdas: rdas(f); break;
        case kWxAws: case kWxMsgRaw: awsCadu(f); break;
        default: break;
        }
    }

    void noaaHrpt(const uint8_t* f) {
        uint16_t w[hrpt::kNoaaWords];
        wxsat::unpack10(f, hrpt::kNoaaWords, w);
        hrpt::Line L;
        if (!hrpt::decodeNoaaFrame(w, L)) { tel.blocksBad++; return; }
        good();
        tel.scid = wxsat::noaaScid(w);
        tel.satName = wxsat::noaaSatName(tel.scid);
        int idx = 0;
        if (haveMs) { double d = (double)L.ms - lastMs; if (d < -43200000.0) d += 86400000.0; const long n = std::lround(d / hrpt::kLineMs); idx = (n <= 0 || n > 20000) ? lastIdx + 1 : lastIdx + (int)n; }
        haveMs = true; lastMs = L.ms; lastIdx = idx;
        setLine16(0, idx, L.px.data(), 2);
        wxsat::noaaTipBlocks(w, false, [this](const uint8_t* t) { tip(t); });
    }

    void gac(const uint8_t* f) {
        uint8_t b[wxsat::kGacBytes];
        std::memcpy(b, f, sizeof b);
        wxsat::gacScramble(b);
        uint16_t w[wxsat::kGacWords];
        wxsat::unpack10(b, wxsat::kGacWords, w);
        good();
        tel.scid = wxsat::noaaScid(w);
        tel.satName = wxsat::noaaSatName(tel.scid);
        std::vector<uint16_t> px;
        wxsat::gacDecodeLine(w, px);
        setLine16(0, lineCount++, px.data(), 2);
        wxsat::noaaTipBlocks(w, true, [this](const uint8_t* t) { tip(t); });
    }

    void tip(const uint8_t* t) {
        tel.tipFrames++;
        std::vector<uint16_t> ln; int idx = 0;
        if (hirs.push(t, ln, idx)) setLine16(im[1] ? 1 : 0, idx, ln.data(), 5);
    }

    void dsb(const uint8_t* f) {
        good();
        tel.scid = f[2] & 15;
        tel.satName = wxsat::noaaSatName(tel.scid);
        tip(f);
    }

    void meteorCadu(const uint8_t* f) {
        good();
        uint8_t pl[wxsat::kMeteorPayload];
        wxsat::meteorPayload(f, pl);
        for (int i = 0; i < wxsat::kMeteorPayload * 8; i++) msu.push(pl[i >> 3] >> (7 - (i & 7)) & 1);
    }

    void msuMr(const uint8_t* f) {
        tel.packets++;
        std::vector<uint16_t> px;
        wxsat::msuMrDecodeFrame(f, px);
        setLine16(0, lineCount++, px.data(), 2);
    }

    void rdas(const uint8_t* f) {
        uint8_t c[wxsat::kCadu];
        std::memcpy(c, f, wxsat::kCadu);
        meteor::pnXor(c + 4, wxsat::kCadu - 4);
        good();
        const int vc = wxsat::rdasVcid(c);
        tel.vcid = vc; tel.vcidFrames[vc & 63]++;
        tel.scid = c[4] << 2 | c[5] >> 6;
        const uint8_t* p = c + 24;
        const int nbits = wxsat::kRdasPayload * 8;
        const auto feed = [&](wxsat::Deframer& d) { for (int i = 0; i < nbits; i++) d.push(p[i >> 3] >> (7 - (i & 7)) & 1); };
        if (vc == 2) feed(visDef[0]); else if (vc == 5) feed(visDef[1]); else if (vc == 3) feed(visDef[2]); else if (vc == 4) feed(irDef);
    }

    void ir(const uint8_t* f) {
        tel.packets++;
        wxsat::MsuGsIr o;
        wxsat::msuGsIrDecode(f, o);
        if (!o.valid) return;
        WxLine& ln = editLine(0, o.counter);
        const int seg = o.segment / 2;
        for (int c = 0; c < wxsat::kMsuGsIrChannels; c++)
            for (int x = 0; x < 183; x++) ln.px[(size_t)c * wxsat::kMsuGsIrWidth + (size_t)seg * 183 + (size_t)x] = (uint8_t)(o.seg[(size_t)c * 183 + (size_t)x] >> 2);
        touch(0, o.counter);
    }

    void vis(int v, const uint8_t* f) {
        tel.packets++;
        std::vector<uint16_t> a, b;
        const int counter = wxsat::msuGsVisDecode(f, a, b);
        if (counter < 0 || counter % wxsat::kMsuGsVisDecim != 0) return;
        const int y = counter / wxsat::kMsuGsVisDecim;
        WxLine& ln = editLine(1, y);
        for (int x = 0; x < wxsat::kMsuGsVisWidth; x++) ln.px[(size_t)v * wxsat::kMsuGsVisWidth + (size_t)x] = (uint8_t)(a[(size_t)x * wxsat::kMsuGsVisDecim] >> 2);
        touch(1, y);
    }

    void awsCadu(const uint8_t* f) {
        uint8_t v[wxsat::kAwsVcdu];
        const int e = wxsat::awsDecodeCadu(f + 4, v);
        if (e < 0) { tel.blocksBad++; return; }
        good(); tel.rsCorrected += (uint64_t)e;
        tel.scid = v[0] << 2 | v[1] >> 6;
        tel.vcid = v[1] & 63;
        tel.vcidFrames[tel.vcid]++;
        aws[tel.vcid].push(v);
    }

    // SEVIRI lands by the time in the packet; north is at the top, so the first scan line of a disc (the south) is the last row. Standard channels 2:1, HRV 3:1.
    void msgPacket(const wxsat::AwsPacket& p) {
        tel.packets++;
        if (tel.vcid != 0 || p.apid != wxsat::kSeviriApid) return;
        const wxsat::SeviriPacket s = wxsat::seviriDecode(tel.scid, p);
        if (!s.valid) return;
        tel.satName = wxsat::seviriSatName(tel.scid);
        if (s.chunk == 0) {
            // a new disc: the scan position has jumped back
            if (msgLast >= 0 && s.line + 1000 < msgLast) discDone();
            msgLast = s.line;
        }
        if (s.chunk < 11) {
            for (int k = 0; k < 3; k++) {
                const int row = s.line + k;
                if (row & 1 || row >= wxsat::kSeviriNrmLines) continue;
                const int y = wxsat::kSeviriNrmOutH - 1 - row / 2;
                WxLine& ln = editLine(0, y);
                const int c = wxsat::seviriChannelSwapped(s.chunk) ? 2 - k : k;
                uint8_t* out = &ln.px[(size_t)s.chunk * wxsat::kSeviriNrmOutW];
                for (int x = 0; x < wxsat::kSeviriNrmOutW; x++) out[wxsat::kSeviriNrmOutW - 1 - x] = (uint8_t)(s.px[(size_t)c * wxsat::kSeviriNrmWidth + (size_t)x * 2] >> 2);
                touch(0, y);
                if (s.chunk == 0) { tel.lines++; }
            }
        } else {
            const int n = s.chunk < 15 ? 2 : 1;
            for (int k = 0; k < n; k++) {
                const int row = s.line + k;
                if (row % 3 || row >= wxsat::kSeviriHrvLines) continue;
                const int y = wxsat::kSeviriHrvOutH - 1 - row / 3;
                WxLine& ln = editLine(1, y);
                for (int x = 0; x < wxsat::kSeviriHrvOutW; x++) ln.px[(size_t)(wxsat::kSeviriHrvOutW - 1 - x)] = (uint8_t)(s.px[(size_t)k * wxsat::kSeviriHrvWidth + (size_t)x * 3] >> 2);
                touch(1, y);
            }
        }
    }

    void discDone() {
        flushEdits();
        tel.imageDone = im[0] ? std::make_shared<WxImage>(*im[0]) : nullptr;
        tel.image2Done = im[1] ? std::make_shared<WxImage>(*im[1]) : nullptr;
        tel.cyclesDone = ++cyclesDone;
        initImages();
        tel.lines = 0;
        say("a full disc is finished");
    }

    void configureDvb() {
        dvb->configure(rate);
        dvb->setStandardHint(2);
        const double hz = fcSymbols.load();
        dvb->setSymbolRate(hz > 0 ? hz : info.symbolRate);
    }

    void startDvb() {
        if (dvb) { if (fcSymbolsSet.exchange(false)) configureDvb(); return; }
        dvb = std::make_unique<DvbsReceiver>();
        dvb->setBlocking(fileBlocking.load());
        dvb->setLogCallback(logCb);
        wxsat::FilecastLayer* fl = files.get();
        dvb->setPacketCallback([fl](const uint8_t* pk, size_t n, double secs) { fl->pushTs(pk, n, secs); });
        configureDvb();
    }

    void awsPacket(const wxsat::AwsPacket& p) {
        if (P == kWxMsgRaw) { msgPacket(p); return; }
        tel.packets++;
        std::vector<uint16_t> px;
        if (tel.vcid == 3 && mwr.push(p, px)) {
            // rolling contrast per channel (16-bit counts)
            if (!mwrInit) { for (int c = 0; c < wxsat::kMwrChannels; c++) { mmn[c] = 65535; mmx[c] = 0; } mwrInit = true; }
            std::vector<uint8_t> out(px.size());
            for (int c = 0; c < wxsat::kMwrChannels; c++) {
                float lo = 65535, hi = 0;
                for (int x = 0; x < wxsat::kMwrWidth; x++) { const float v = px[(size_t)c * wxsat::kMwrWidth + (size_t)x]; lo = std::min(lo, v); hi = std::max(hi, v); }
                mmn[c] = std::min(mmn[c] * 0.98f + lo * 0.02f, lo); mmx[c] = std::max(mmx[c] * 0.98f + hi * 0.02f, hi);
                const float r = std::max(1.f, mmx[c] - mmn[c]);
                for (int x = 0; x < wxsat::kMwrWidth; x++) out[(size_t)c * wxsat::kMwrWidth + (size_t)x] = (uint8_t)std::clamp((int)std::lround((px[(size_t)c * wxsat::kMwrWidth + (size_t)x] - mmn[c]) / r * 255.f), 0, 255);
            }
            setLine(0, lineCount++, out.data(), (int)out.size());
        }
    }

    // ---- pictures ----
    void growLines(int w, int idx) {
        if ((int)im[w]->line.size() <= idx) im[w]->line.resize((size_t)idx + 1);
        im[w]->lines = std::max(im[w]->lines, idx + 1);
    }
    void setLine16(int w, int idx, const uint16_t* px, int shift) {
        if (!im[w] || idx < 0 || idx > 50000) return;
        const size_t n = (size_t)im[w]->width * (size_t)im[w]->channels;
        auto ln = std::make_shared<WxLine>();
        ln->idx = idx; ln->px.resize(n);
        for (size_t i = 0; i < n; i++) ln->px[i] = (uint8_t)std::min(255, px[i] >> shift);
        growLines(w, idx);
        im[w]->line[(size_t)idx] = ln;
        imgDirty[w] = true;
        tel.lines = (uint64_t)im[w]->lines;
    }
    void setLine(int w, int idx, const uint8_t* px, int n) {
        if (!im[w] || idx < 0 || idx > 50000) return;
        auto ln = std::make_shared<WxLine>();
        ln->idx = idx; ln->px.assign(px, px + std::min(n, im[w]->width * im[w]->channels));
        growLines(w, idx);
        im[w]->line[(size_t)idx] = ln;
        imgDirty[w] = true;
        tel.lines = (uint64_t)im[w]->lines;
    }
    WxLine& editLine(int w, int y) {
        auto it = edit[w].find(y);
        if (it != edit[w].end()) return *it->second;
        auto l = std::make_shared<WxLine>();
        l->idx = y;
        l->px.assign((size_t)im[w]->width * (size_t)im[w]->channels, 0);
        growLines(w, y);
        if ((size_t)y < im[w]->line.size() && im[w]->line[(size_t)y]) *l = *im[w]->line[(size_t)y];
        edit[w][y] = l;
        return *l;
    }
    void touch(int w, int y) { dirty[w].push_back(y); imgDirty[w] = true; }
    void flushEdits() {
        for (int w = 0; w < 2; w++) {
            if (dirty[w].empty()) continue;
            std::sort(dirty[w].begin(), dirty[w].end());
            dirty[w].erase(std::unique(dirty[w].begin(), dirty[w].end()), dirty[w].end());
            for (int y : dirty[w]) {
                auto it = edit[w].find(y);
                if (it == edit[w].end()) continue;
                im[w]->line[(size_t)y] = std::make_shared<const WxLine>(*it->second);
            }
            tel.lines = (uint64_t)im[w]->lines;
            dirty[w].clear();
            if (edit[w].size() > 800) edit[w].clear();
        }
    }

    void report() {
        flushEdits();
        tel.inputRate = rate;
        tel.symbolRate = info.symbolRate;
        tel.timeSec = nIn / std::max(1.0, rate);
        if (P == kWxArktikaGgak) {
            GgakTelemetry g;
            if (ggak.telemetry(g, ggakSeq)) { ggakSeq = g.seq; tel.ggak = std::move(g); }
            tel.state = tel.ggak.state >= 2 ? 3 : tel.ggak.state;
            tel.blocksOk = tel.ggak.blocksOk; tel.blocksBad = tel.ggak.blocksBad; tel.cfoHz = tel.ggak.cfoHz; tel.snrDb = tel.ggak.snrDb; tel.frameLock = tel.ggak.frameLock;
        } else if (isFilecast()) {
            if (fileRateOk() && dvb) {
                DvbsTelemetry d;
                if (dvb->telemetry(d, dvbSeq)) {
                    dvbSeq = d.seq;
                    tel.cfoHz = d.cfoHz; tel.snrDb = d.snrDb;
                    tel.blocksOk = d.blocksOk; tel.blocksBad = d.blocksBad;
                    tel.frameLock = d.state >= 2;
                    tel.carrierLock = d.state >= 1 ? 1.f : 0.f;
                    tel.symbolRate = d.symbolRate;
                    tel.state = d.dataValid ? 3 : d.state >= 2 ? 2 : d.state;
                    tel.hint = d.signalNote;
                }
            } else if (!fileRateOk()) {
                char b[160];
                snprintf(b, sizeof b, "this link needs %.1f Msps or more (a recording works)", info.minRate / 1e6);
                tel.hint = b;
            }
            if (files) tel.files = files->telemetry();
        } else if (P == kWxApt) {
            tel.state = apt.locked() ? 3 : apt.carrierLevel() > 0.02f ? 1 : 0;
            tel.frameLock = apt.locked();
            tel.blocksOk = apt.linesOut();
            tel.cfoHz = aptCfo; tel.symbolRate = 4160;
        } else if (rate >= info.minRate - 1) {
            tel.cfoHz = dem.currentCfo();
            tel.snrDb = dem.snrDb;
            tel.carrierLock = dem.carrierLock;
            tel.constellation = dem.cons;
            bool fl = false;
            for (auto& d : def) fl = fl || d.locked();
            fl = fl || defCcsds.locked();
            tel.frameLock = fl;
            const double since = nowSec() - lastGood;
            tel.state = tel.blocksOk > 0 && since < 3 ? 3 : fl ? 2 : (dem.acquired && dem.carrierLock > 0.3f ? 1 : 0);
            if (!fl && nowSec() - lastAcq > 2.5 && since > 2.5) { dem.reacquire(); searchLock = false; pairs.clear(); lastAcq = nowSec(); }
        } else tel.hint = "the sample rate is below what this link needs";
        tel.dataValid = tel.state >= 3;
        tel.seq++;
        for (int w = 0; w < 2; w++) {
            if (!imgDirty[w] || !im[w]) continue;
            auto copy = std::make_shared<WxImage>(*im[w]);
            (w == 0 ? tel.image : tel.image2) = copy;
            imgDirty[w] = false;
            tel.imageSeq++;
        }
        std::lock_guard<std::mutex> lk(mu);
        pub = tel;
    }

    void feed(const cf32* x, size_t n) {
        if (rate <= 0 || n == 0) return;
        if (resetReq.exchange(false)) resetState();
        if (clearReq.exchange(false)) { initImages(); tel.image.reset(); tel.image2.reset(); tel.lines = 0; msgLast = -1; }
        if (P == kWxArktikaGgak) ggak.feed(x, n);
        else if (isFilecast()) {
            if (fileRateOk()) { startDvb(); dvb->feed(x, n); }
        } else if (P == kWxApt) {
            for (size_t i = 0; i < n; i++) {
                cfp y;
                if (!aptFir.push(cfp(x[i].real(), x[i].imag()) * aptRot, y)) { aptRot *= aptStep; continue; }
                aptRot *= aptStep;
                const float d = std::arg(y * std::conj(aptPrev));
                aptPrev = y;
                aptMean += 0.001f * (d - aptMean);
                aptCfo = aptMean * aptAudioRate / (2 * kPi);
                aptBuf.push_back(d / (float)(2 * kPi * 17000.0 / aptAudioRate));
                if (aptBuf.size() >= 4096) { apt.pushAudio(aptBuf.data(), aptBuf.size()); aptBuf.clear(); }
            }
            const float m = std::abs(aptRot); if (m > 0) aptRot /= m;
        } else if (rate >= info.minRate - 1) {
            dem.feed(x, n, [this](float I, float Q) { symbol(I, Q); });
        }
        nIn += (int64_t)n;
        if (nIn >= nextPub) { nextPub = nIn + (int64_t)(kPubSec * rate); report(); }
    }

    cfp aptRot{1, 0}, aptStep{1, 0};
    float aptMean = 0;
    void setAptStep() { const double w = -2 * kPi * offset / std::max(1.0, rate); aptStep = cfp((float)std::cos(w), (float)std::sin(w)); }
};

WxsatReceiver::WxsatReceiver(int profile) : p_(std::make_unique<Impl>(profile)) {}
WxsatReceiver::~WxsatReceiver() = default;

void WxsatReceiver::configure(double r) {
    Impl& m = *p_;
    m.rate = r; m.setRate = r; m.nIn = 0; m.nextPub = 0;
    m.setAptStep();
    if (!m.im[0]) m.initImages();
    m.setup();
    m.resetState();
}
void WxsatReceiver::setSignalOffset(double hz) {
    Impl& m = *p_;
    m.offset = hz;
    if (m.P == kWxApt) m.setAptStep();
    else if (m.P == kWxArktikaGgak) m.ggak.setSignalOffset(hz);
    else if (m.isFilecast()) {}   // the DVB-S2 receiver works on the carrier nearest the centre, wherever it is
    else if (m.rate >= m.info.minRate - 1) m.dem.setOffset(hz);
}
bool WxsatReceiver::ready() const { return p_->isFilecast() ? p_->fileRateOk() : p_->rate >= p_->info.minRate - 1; }
void WxsatReceiver::reset() { p_->resetReq = true; }
void WxsatReceiver::clearImage() {
    { std::lock_guard<std::mutex> lk(p_->mu); p_->pub.image.reset(); p_->pub.image2.reset(); p_->pub.imageSeq++; p_->pub.seq++; }
    p_->clearReq = true;
}
void WxsatReceiver::feed(const cf32* x, size_t n) { p_->feed(x, n); }
bool WxsatReceiver::telemetry(WxsatTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}
void WxsatReceiver::clearResults() { clearImage(); }
void WxsatReceiver::setSaveDir(const std::string& dir) { if (p_->files) p_->files->setSaveDir(dir); }
void WxsatReceiver::setFilecastSymbolRate(double hz) { p_->fcSymbols = hz; p_->fcSymbolsSet = true; }
void WxsatReceiver::setFilecastPid(int pid) { if (p_->files) p_->files->setPid(pid); }
wxsat::FilecastLayer* WxsatReceiver::fileLayer() { return p_->files.get(); }
void WxsatReceiver::setBlocking(bool b) { p_->fileBlocking = b; }
void WxsatReceiver::flush() { if (p_->dvb) { p_->dvb->flush(); } if (p_->files) p_->files->flush(); p_->report(); }
int WxsatReceiver::profile() const { return p_->P; }
void WxsatReceiver::setLogCallback(std::function<void(const std::string&)> cb) { p_->logCb = std::move(cb); }

static void putU32(std::string& s, uint32_t v) { for (int i = 0; i < 4; i++) s.push_back((char)(v >> (8 * i))); }
static uint32_t getU32(const std::string& s, size_t& p) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= (uint32_t)(uint8_t)s[p++] << (8 * i); return v; }

std::string WxsatReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    std::string s = "WXS1";
    for (const auto& im : {p_->pub.image, p_->pub.image2}) {
        if (!im) { putU32(s, 0); continue; }
        putU32(s, 1); putU32(s, (uint32_t)im->width); putU32(s, (uint32_t)im->channels); putU32(s, (uint32_t)im->lines);
        putU32(s, (uint32_t)im->name.size()); s += im->name;
        for (const auto& l : im->line) {
            if (!l) { s.push_back(0); continue; }
            s.push_back(1);
            s.append(reinterpret_cast<const char*>(l->px.data()), l->px.size());
        }
    }
    return s;
}

bool WxsatReceiver::loadResults(const std::string& s) {
    if (s.size() < 8 || s.compare(0, 4, "WXS1") != 0) return false;
    size_t p = 4;
    std::shared_ptr<WxImage> out[2];
    for (auto& o : out) {
        if (p + 4 > s.size()) return false;
        if (getU32(s, p) == 0) continue;
        if (p + 16 > s.size()) return false;
        auto im = std::make_shared<WxImage>();
        im->width = (int)getU32(s, p); im->channels = (int)getU32(s, p); im->lines = (int)getU32(s, p);
        const uint32_t nl = getU32(s, p);
        if (im->width <= 0 || im->channels <= 0 || im->lines < 0 || im->lines > 50000 || nl > 200 || p + nl > s.size()) return false;
        im->name.assign(s, p, nl); p += nl;
        const size_t perLine = (size_t)im->width * (size_t)im->channels;
        im->line.resize((size_t)im->lines);
        for (int y = 0; y < im->lines; y++) {
            if (p >= s.size()) return false;
            if (s[p++] == 0) continue;
            if (p + perLine > s.size()) return false;
            auto l = std::make_shared<WxLine>();
            l->idx = y;
            l->px.assign(s.begin() + (std::ptrdiff_t)p, s.begin() + (std::ptrdiff_t)(p + perLine));
            p += perLine;
            im->line[(size_t)y] = l;
        }
        o = im;
    }
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.image = out[0]; p_->pub.image2 = out[1]; p_->pub.imageSeq++; p_->pub.seq++;
    return true;
}

ModeTuning wxsatTuning(int p) {
    const WxProfileInfo& i = wxProfile(p);
    ModeTuning t;
    t.stdMode = i.family + 2; t.id = i.id; t.name = i.name;
    t.minMhz = 1; t.maxMhz = 12000; t.defMhz = i.defMhz;
    t.sampleRate = i.sampleRate; t.basebandHz = 0;
    t.bandwidthMhz = p == kWxHimawariCast ? 3.2 : p == kWxGeoNetCast ? 36 : p == kWxApt ? 0.05 : p == kWxNoaaDsb || p == kWxArktikaGgak ? 0.05 : p == kWxElektroRdas || p == kWxArktikaRdas ? 45 : i.symbolRate / 1e6 * 1.4;
    t.minSampleRate = i.minRate;
    t.tuneOffsetHz = (p == kWxApt || p == kWxNoaaDsb) ? 100000 : 0;
    t.carrierAtCentre = (p == kWxNoaaHrpt || p == kWxNoaaDsb || p == kWxMeteorHrpt);
    return t;
}

} // namespace dect2
