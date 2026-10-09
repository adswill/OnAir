// RTTY decoder of the HF digital receiver (see hfdig_rtty.h), and its test audio for the generator (hfdig_gen.h).
#include "dect2/hfdig_rtty.h"
#include "dect2/hfdig_gen.h"
#include "dect2/source.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = kHfdigAudioRate;
constexpr int kFft = 2048, kHop = 1024;
constexpr double kBinHz = kFs / kFft;
constexpr int kBinLo = 77, kBinHi = 844;               // 300 .. 3300 Hz
constexpr double kLockRatio = 251.0, kUnlockRatio = 100.0;   // pair score against the noise floor: 24 dB to lock, 20 dB to keep
constexpr int kUnlockFrames = 12, kRelinkFrames = 4;

// ITA2 (US TTY variant for the figures)
const char kLetters[32] = {0, 'E', '\n', 'A', ' ', 'S', 'I', 'U', '\r', 'D', 'R', 'J', 'N', 'F', 'C', 'K', 'T', 'Z', 'L', 'W', 'H', 'Y', 'P', 'Q', 'O', 'B', 'G', 0, 'M', 'X', 'V', 0};
const char kFigures[32] = {0, '3', '\n', '-', ' ', '\'', '8', '7', '\r', '$', '4', 0, ',', '!', ':', '(', '5', '"', ')', '2', '#', '6', '0', '1', '9', '?', '&', 0, '.', '/', ';', 0};
constexpr int kFigsCode = 27, kLtrsCode = 31;

// In-place radix 2 FFT.
void fft(std::vector<std::complex<float>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / (double)len;
        const std::complex<float> wl((float)std::cos(ang), (float)std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (size_t k = 0; k < len / 2; k++) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v; a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

} // namespace

struct HfdigRtty::Impl {
    // settings, from any thread
    std::atomic<int> baudIdx{0}, shiftIdx{0};
    std::atomic<bool> reverse{false}, unshift{false}, clearReq{false};
    std::atomic<uint32_t> cfgGen{1};
    uint32_t cfgSeen = 0;

    // receiver thread
    HfdigRttyTelemetry tel;
    double baud = 45.45, shift = 170;
    bool rev = false, uns = false;
    uint64_t pos = 0, lastCharAt = 0;
    // spectrum
    std::vector<float> win, ring;
    int ringPos = 0, sinceFrame = 0;
    std::vector<float> pw, scratch;
    std::vector<std::complex<float>> fftBuf;
    int frames = 0;
    // lock
    bool locked = false;
    double loHz = 0, hiHz = 0;
    int weakFrames = 0, relinkFrames = 0;
    double relinkLo = 0;
    double ratio = 0;                         // pair score of the locked tones against the floor
    // demodulator
    int len = 176;
    double bitSamples = 176.0;
    std::vector<double> bMI, bMQ, bSI, bSQ;
    double sMI = 0, sMQ = 0, sSI = 0, sSQ = 0;
    int bIdx = 0;
    std::complex<double> phM{1, 0}, phS{1, 0}, stM{1, 0}, stS{1, 0};
    int renorm = 0;
    double maxM = 0, maxS = 0;
    bool mark = true;
    // character framing
    int fsm = 0;                              // 0 waiting for a start edge, 1 sampling bits
    double cnt = 0;
    int bit = 0, code = 0;
    double soft = 0;
    bool figs = false;
    float eye = 0.5f;

    Impl() {
        win.resize(kFft);
        for (int i = 0; i < kFft; i++) win[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFft));
        ring.assign(kFft, 0.f);
        pw.assign(kFft / 2, 0.f);
        fftBuf.resize(kFft);
        applyConfig();
    }

    void resetAll() {
        tel = HfdigRttyTelemetry();
        pos = 0; lastCharAt = 0;
        std::fill(ring.begin(), ring.end(), 0.f);
        std::fill(pw.begin(), pw.end(), 0.f);
        ringPos = 0; sinceFrame = 0; frames = 0;
        figs = false; eye = 0.5f;
        unlock();
    }

    void applyConfig() {
        baud = kRttyBaudTable[std::clamp(baudIdx.load(), 0, kRttyBauds - 1)];
        shift = kRttyShiftTable[std::clamp(shiftIdx.load(), 0, kRttyShifts - 1)];
        rev = reverse.load(); uns = unshift.load();
        bitSamples = kFs / baud;
        len = std::max(2, (int)std::lround(bitSamples));
        bMI.assign((size_t)len, 0.0); bMQ = bMI; bSI = bMI; bSQ = bMI;
        unlock();
    }

    void unlock() {
        locked = false; loHz = hiHz = 0; weakFrames = relinkFrames = 0; ratio = 0;
        clearDemod();
    }

    void clearDemod() {
        std::fill(bMI.begin(), bMI.end(), 0.0); std::fill(bMQ.begin(), bMQ.end(), 0.0);
        std::fill(bSI.begin(), bSI.end(), 0.0); std::fill(bSQ.begin(), bSQ.end(), 0.0);
        sMI = sMQ = sSI = sSQ = 0; bIdx = 0;
        maxM = maxS = 0; mark = true; fsm = 0; code = 0; bit = 0;
    }

    void setTones() {   // the NCOs follow the tones of the pair
        const double mk = rev ? hiHz : loHz, sp = rev ? loHz : hiHz;
        stM = std::polar(1.0, -2 * kPi * mk / kFs);
        stS = std::polar(1.0, -2 * kPi * sp / kFs);
        tel.markHz = mk; tel.spaceHz = sp;
    }

    // ------------------------------------------------------------------ spectrum and pair search
    double binPeak(int k) const { return std::max({pw[(size_t)k - 1], pw[(size_t)k], pw[(size_t)k + 1]}); }

    double floorLevel() {
        scratch.assign(pw.begin() + kBinLo, pw.begin() + kBinHi + 1);
        std::nth_element(scratch.begin(), scratch.begin() + (long)scratch.size() / 2, scratch.end());
        return std::max(1e-12, (double)scratch[scratch.size() / 2]);
    }

    // centre of the tone near bin k (power above the floor, five bins)
    double centroid(int k, double fl) const {
        int kk = k;
        for (int d = -1; d <= 1; d++) if (pw[(size_t)(k + d)] > pw[(size_t)kk]) kk = k + d;
        double sw = 0, sk = 0;
        for (int d = -2; d <= 2; d++) {
            const double w = std::max(0.0, (double)pw[(size_t)(kk + d)] - fl);
            sw += w; sk += w * (kk + d);
        }
        return (sw > 0 ? sk / sw : (double)kk) * kBinHz;
    }

    void frame() {
        for (int i = 0; i < kFft; i++) fftBuf[(size_t)i] = std::complex<float>(ring[(size_t)((ringPos + i) % kFft)] * win[(size_t)i], 0.f);
        fft(fftBuf);
        for (int k = 0; k < kFft / 2; k++) {
            const float p = std::norm(fftBuf[(size_t)k]);
            pw[(size_t)k] = frames == 0 ? p : 0.65f * pw[(size_t)k] + 0.35f * p;
        }
        frames++;
        if (frames < 3) return;
        const double fl = floorLevel();
        const int ds = (int)std::lround(shift / kBinHz);
        // best pair: the lower tone at bin k, the upper one ds bins above
        double best = 0, bestA = 0, bestB = 0;
        int bestK = -1;
        for (int k = kBinLo; k + ds <= kBinHi; k++) {
            const double a = binPeak(k), b = binPeak(k + ds);
            double sc = std::sqrt(a) + std::sqrt(b);
            sc *= sc;
            if ((rev ? b : a) >= (rev ? a : b)) sc *= 1.2;   // an idle line sends mark alone: prefer the pair that has the mark where it is strongest
            if (sc > best) { best = sc; bestK = k; bestA = a; bestB = b; }
        }
        if (bestK < 0) return;
        const double top = std::max(bestA, bestB);
        const bool hasA = bestA > 0.15 * top, hasB = bestB > 0.15 * top;
        const double fa = centroid(bestK, fl), fb = centroid(bestK + ds, fl);
        double lo;
        if (hasA && hasB) lo = 0.5 * (fa + (fb - shift));
        else if (hasA) lo = fa;
        else lo = fb - shift;
        const double candRatio = best / fl;

        if (!locked) {
            if (candRatio >= kLockRatio) {
                locked = true; loHz = lo; hiHz = lo + shift;
                weakFrames = relinkFrames = 0;
                clearDemod(); setTones();
            }
            return;
        }
        // locked: how strong is the current pair?
        const int kl = (int)std::lround(loHz / kBinHz), kh = (int)std::lround(hiHz / kBinHz);
        if (kl < kBinLo - 1 || kh > kBinHi + 1) { unlock(); return; }
        double cur = std::sqrt(binPeak(kl)) + std::sqrt(binPeak(kh));
        cur = cur * cur / fl;
        ratio = cur;
        if (cur < kUnlockRatio) {
            if (++weakFrames >= kUnlockFrames) { unlock(); return; }
        } else {
            weakFrames = 0;
        }
        if (std::fabs(lo - loHz) <= 12.0) {
            relinkFrames = 0;
            if (hasA && hasB && cur >= kUnlockRatio) { loHz += 0.1 * (lo - loHz); hiHz = loHz + shift; setTones(); }
        } else if (candRatio > 2.0 * std::max(cur, 1.0) && candRatio >= kLockRatio) {
            if (relinkFrames > 0 && std::fabs(lo - relinkLo) > 12.0) relinkFrames = 0;
            relinkLo = lo;
            if (++relinkFrames >= kRelinkFrames) {
                loHz = lo; hiHz = lo + shift;
                weakFrames = relinkFrames = 0;
                clearDemod(); setTones();
            }
        } else {
            relinkFrames = 0;
        }
    }

    // ------------------------------------------------------------------ characters
    void emit(int c) {
        if (c == kFigsCode) { figs = true; return; }
        if (c == kLtrsCode) { figs = false; return; }
        const char ch = figs ? kFigures[c] : kLetters[c];
        if (c == 4 && uns) figs = false;
        if (ch == 0 || ch == '\r') return;
        tel.text.push_back(ch);
        tel.chars++;
        if (tel.text.size() > kRttyTextMax + 256) tel.text.erase(0, tel.text.size() - kRttyTextMax);
    }

    void sampleBit(double x) {
        const bool m = x > 0;
        soft += std::min(1.0, std::fabs(x));
        if (bit == 0) {                                   // start: must be space
            if (m) { fsm = 0; return; }
        } else if (bit <= 5) {
            if (m) code |= 1 << (bit - 1);
        } else {                                          // stop: must be mark
            fsm = 0;
            const double q = soft / 7.0;
            if (!m || q < 0.3) { tel.framingErrors++; return; }
            eye = 0.9f * eye + 0.1f * (float)std::min(1.0, q);
            lastCharAt = pos;
            emit(code);
            return;
        }
        bit++;
        cnt += bitSamples;
    }

    void demod(float v) {
        // mix to zero at both tones, integrate over one bit
        phM *= stM; phS *= stS;
        if (++renorm >= 512) { renorm = 0; phM /= std::abs(phM); phS /= std::abs(phS); }
        const double mi = v * phM.real(), mq = v * phM.imag(), si = v * phS.real(), sq = v * phS.imag();
        const size_t i = (size_t)bIdx;
        sMI += mi - bMI[i]; bMI[i] = mi; sMQ += mq - bMQ[i]; bMQ[i] = mq;
        sSI += si - bSI[i]; bSI[i] = si; sSQ += sq - bSQ[i]; bSQ[i] = sq;
        if (++bIdx >= len) bIdx = 0;
        const double em = std::sqrt(sMI * sMI + sMQ * sMQ) / len, es = std::sqrt(sSI * sSI + sSQ * sSQ) / len;
        // ATC: each tone against its own slowly decaying peak (the other tone's peak, scaled, as the floor: an idle line is mark alone)
        const double dec = 1.0 - 1.0 / (0.6 * kFs);
        maxM = std::max(em, maxM * dec + em * (1 - dec));
        maxS = std::max(es, maxS * dec + es * (1 - dec));
        const double refM = std::max({maxM, 0.3 * maxS, 1e-9}), refS = std::max({maxS, 0.3 * maxM, 1e-9});
        const double x = em / refM - es / refS;
        const bool m = x > 0.15 ? true : (x < -0.15 ? false : mark);
        if (fsm == 0) {
            if (mark && !m) { fsm = 1; bit = 0; code = 0; soft = 0; cnt = 0.5 * bitSamples; }
        } else {
            cnt -= 1.0;
            if (cnt <= 0) sampleBit(x);
        }
        mark = m;
    }

    void run(const float* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            ring[(size_t)ringPos] = x[i];
            if (++ringPos >= kFft) ringPos = 0;
            if (++sinceFrame >= kHop) { sinceFrame = 0; frame(); }
            pos++;
            if (locked) demod(x[i]);
        }
    }

    void fill() {
        tel.baud = baud; tel.shiftHz = shift; tel.reverse = rev; tel.unshiftOnSpace = uns;
        tel.audioSamples = pos;
        tel.state = !locked ? 0 : (pos - lastCharAt < 3 * (uint64_t)kFs && lastCharAt > 0 ? 2 : 1);
        if (!locked) { tel.markHz = tel.spaceHz = 0; tel.quality = 0; }
        else {
            const double snr = 10 * std::log10(std::max(1.0, ratio));
            const float specQ = (float)std::clamp((snr - 20.0) / 25.0, 0.0, 1.0);
            tel.quality = specQ * (0.3f + 0.7f * eye);
        }
    }
};

HfdigRtty::HfdigRtty() : p_(std::make_unique<Impl>()) {}
HfdigRtty::~HfdigRtty() = default;

void HfdigRtty::reset() { p_->resetAll(); p_->fill(); }

void HfdigRtty::feedAudio(const float* x, size_t n) {
    Impl& m = *p_;
    const uint32_t g = m.cfgGen.load();
    if (g != m.cfgSeen) { m.cfgSeen = g; m.applyConfig(); }
    if (m.clearReq.exchange(false)) { m.tel.text.clear(); m.tel.chars = 0; m.tel.framingErrors = 0; }
    m.run(x, n);
    m.fill();
}

void HfdigRtty::telemetry(HfdigRttyTelemetry& out) const { out = p_->tel; }

void HfdigRtty::setBaudIndex(int i) { p_->baudIdx = std::clamp(i, 0, kRttyBauds - 1); p_->cfgGen++; }
void HfdigRtty::setShiftIndex(int i) { p_->shiftIdx = std::clamp(i, 0, kRttyShifts - 1); p_->cfgGen++; }
void HfdigRtty::setReverse(bool r) { p_->reverse = r; p_->cfgGen++; }
void HfdigRtty::setUnshiftOnSpace(bool u) { p_->unshift = u; p_->cfgGen++; }
void HfdigRtty::clearText() { p_->clearReq = true; }
int HfdigRtty::baudIndex() const { return p_->baudIdx.load(); }
int HfdigRtty::shiftIndex() const { return p_->shiftIdx.load(); }
bool HfdigRtty::reverse() const { return p_->reverse.load(); }
bool HfdigRtty::unshiftOnSpace() const { return p_->unshift.load(); }

std::unique_ptr<HfdigRtty> makeRttyDecoder() { return std::make_unique<HfdigRtty>(); }

// ---------------------------------------------------------------------- test audio

namespace {

class RttyTestAudio : public HfdigTestAudio {
public:
    explicit RttyTestAudio(const SynthConfig& c) {
        const int bi = std::clamp(c.modeOpt[1], 0, kRttyBauds - 1), si = std::clamp(c.modeOpt[2], 0, kRttyShifts - 1);
        step_ = kRttyBaudTable[bi] / kFs;
        const double lo = c.modeVal[0] > 0 ? c.modeVal[0] : 2125.0;
        const double hi = lo + kRttyShiftTable[si];
        markW_ = 2 * kPi * (c.modeOpt[3] ? hi : lo) / kFs;
        spaceW_ = 2 * kPi * (c.modeOpt[3] ? lo : hi) / kFs;
        // the loop: idle mark, LTRS, the text with the shifts it needs, idle mark
        segs_.push_back({true, 20.0});
        bool figs = false;
        auto put = [&](int code) {
            segs_.push_back({false, 1.0});
            for (int b = 0; b < 5; b++) segs_.push_back({((code >> b) & 1) != 0, 1.0});
            segs_.push_back({true, 1.5});
        };
        put(kLtrsCode);
        for (const char* p = kRttyTestText; *p; p++) {
            const char ch = *p;
            if (ch == '\n') { put(8); put(2); continue; }
            int code = -1;
            bool needFigs = false;
            for (int k = 0; k < 32 && code < 0; k++) if (kLetters[k] == ch && ch != 0) code = k;
            if (code < 0) { for (int k = 0; k < 32 && code < 0; k++) if (kFigures[k] == ch && ch != 0) code = k; needFigs = true; }
            if (code < 0) continue;
            if (code != 4 && needFigs != figs) { put(needFigs ? kFigsCode : kLtrsCode); figs = needFigs; }
            put(code);
        }
        segs_.push_back({true, 20.0});
        rem_ = segs_[0].len;
    }
    void generate(float* out, size_t n) override {
        for (size_t i = 0; i < n; i++) {
            rem_ -= step_;
            while (rem_ <= 0) { idx_ = (idx_ + 1) % segs_.size(); rem_ += segs_[idx_].len; }
            phase_ += segs_[idx_].mark ? markW_ : spaceW_;
            if (phase_ > 2 * kPi) phase_ -= 2 * kPi;
            out[i] = 0.5f * (float)std::sin(phase_);
        }
    }
private:
    struct Seg { bool mark; double len; };
    std::vector<Seg> segs_;
    size_t idx_ = 0;
    double rem_ = 0, step_ = 0, markW_ = 0, spaceW_ = 0, phase_ = 0;
};

} // namespace

std::unique_ptr<HfdigTestAudio> makeRttyTestAudio(const SynthConfig& cfg) { return std::make_unique<RttyTestAudio>(cfg); }

} // namespace dect2
