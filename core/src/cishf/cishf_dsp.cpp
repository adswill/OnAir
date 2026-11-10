// CIS HF decoders and detectors (cishf_dsp.h). Clean-room from the public descriptions listed in docs/CIS-HF-3.9.md; the CIS 36-50 frame layout
// (44-bit start word with 21 ones, the 70-bit session key sent twice, 3:4 characters, 0x77 end of message) follows the notes in the Rivet decoder
// (Ian Wraith, GPL-3.0-or-later, CIS3650.java), read as a description: no code taken.
#include "dect2/cishf/cishf_dsp.h"
#include "dect2/dsp/fftutil.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <numeric>

namespace dect2::cishf {

namespace {
constexpr double kPi = 3.14159265358979323846;

std::string fmt(const char* f, double a) { char b[64]; snprintf(b, sizeof b, f, a); return b; }
std::string fmt2(const char* f, double a, double c) { char b[96]; snprintf(b, sizeof b, f, a, c); return b; }

// ---------------------------------------------------------------- averaged spectrum of the 12 kHz stream
class Psd {
public:
    static constexpr int kN = 4096;
    Psd() : fft_(kN), win_(kN), buf_(kN), tmp_(kN), avg_(kN, 0.f) {
        for (int i = 0; i < kN; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kN));
    }
    void reset() { std::fill(avg_.begin(), avg_.end(), 0.f); fill_ = 0; frames_ = 0; }
    void setAlpha(float a) { alpha_ = a; }
    // returns true when a new frame was added (every kN / 2 samples)
    bool push(const cf32* x, size_t n) {
        bool any = false;
        for (size_t i = 0; i < n; i++) {
            buf_[fill_++] = x[i];
            if (fill_ == kN) {
                for (int k = 0; k < kN; k++) tmp_[k] = buf_[k] * win_[k];
                fft_.forward(tmp_.data());
                const float a = frames_ < 4 ? 1.f / (float)(frames_ + 1) : alpha_;
                for (int k = 0; k < kN; k++) avg_[k] = (1 - a) * avg_[k] + a * std::norm(tmp_[k]);
                frames_++;
                std::copy(buf_.begin() + kN / 2, buf_.end(), buf_.begin());
                fill_ = kN / 2;
                any = true;
            }
        }
        return any;
    }
    int frames() const { return frames_; }
    double binHz() const { return kRate / kN; }
    int bin(double hz) const { int b = (int)std::lround(hz / binHz()); return ((b % kN) + kN) % kN; }
    float at(double hz) const { return avg_[bin(hz)]; }
    double noise() const {
        std::vector<float> v(avg_);
        std::nth_element(v.begin(), v.begin() + kN / 2, v.end());
        return std::max(1e-20, (double)v[kN / 2]);
    }
    // strongest bin in [lo, hi] Hz, refined by a parabola; returns the frequency and its power
    double peak(double lo, double hi, float& pw, double exLo = 1e9, double exHi = -1e9) const {
        int b0 = (int)std::ceil(lo / binHz()), b1 = (int)std::floor(hi / binHz());
        int best = b0; float bv = -1;
        for (int b = b0; b <= b1; b++) {
            const double f = b * binHz();
            if (f >= exLo && f <= exHi) continue;
            const float v = avg_[((b % kN) + kN) % kN];
            if (v > bv) { bv = v; best = b; }
        }
        pw = bv;
        const float l = avg_[(((best - 1) % kN) + kN) % kN], r = avg_[(((best + 1) % kN) + kN) % kN];
        const float den = l - 2 * bv + r;
        const double d = std::fabs(den) > 1e-30f ? 0.5 * (l - r) / den : 0;
        return (best + std::clamp(d, -0.5, 0.5)) * binHz();
    }
    // power summed over [lo, hi] Hz
    double band(double lo, double hi) const {
        double s = 0;
        for (int b = (int)std::ceil(lo / binHz()); b <= (int)std::floor(hi / binHz()); b++) s += avg_[((b % kN) + kN) % kN];
        return s;
    }
    // the occupied band: the widest run (gaps up to gapHz) above thr x noise, smoothed over 9 bins, in [lo, hi]
    bool occupied(double lo, double hi, double thr, double gapHz, double& f0, double& f1, double minNarrowHz = 0) const {
        const double nz = noise();
        const int b0 = (int)std::ceil(lo / binHz()), b1 = (int)std::floor(hi / binHz());
        std::vector<double> sm(b1 - b0 + 1, 0);
        for (int b = b0; b <= b1; b++) { double s = 0; for (int j = -4; j <= 4; j++) s += avg_[(((b + j) % kN) + kN) % kN]; sm[b - b0] = s / 9; }
        int bestS = -1, bestE = -1; double bestW = 0;
        int s = -1, last = -1;
        const int gap = (int)(gapHz / binHz());
        for (int i = 0; i <= (int)sm.size(); i++) {
            const bool on = i < (int)sm.size() && sm[i] > thr * nz;
            if (on) { if (s < 0) s = i; last = i; }
            if ((!on && s >= 0 && i - last > gap) || (i == (int)sm.size() && s >= 0)) {
                const double w = (last - s) * binHz();
                if (w > bestW && w >= minNarrowHz) { bestW = w; bestS = s; bestE = last; }
                s = -1;
            }
        }
        if (bestS < 0) return false;
        f0 = (b0 + bestS) * binHz(); f1 = (b0 + bestE) * binHz();
        return true;
    }
private:
    Fft fft_;
    std::vector<float> win_;
    std::vector<cf32> buf_, tmp_;
    std::vector<float> avg_;
    int fill_ = 0, frames_ = 0;
    float alpha_ = 0.15f;
};

double snrDbOf(const Psd& p, double f0, double f1) {
    const double nz = p.noise();
    const double s = p.band(f0, f1), n = nz * std::max(1.0, (f1 - f0) / p.binHz());
    return 10 * std::log10(std::max(1e-6, (s - n) / n));
}

// ---------------------------------------------------------------- 2-FSK: tone pair search and one demodulator per symbol rate
struct FskDemod {
    double baud = 50, sps = 240;
    int L = 240;
    std::vector<cf32> rh, rl;
    int idx = 0;
    std::complex<double> sh{0, 0}, sl{0, 0};
    double ph = 0, prevD = 0;
    int transitions = 0;
    double eye = 0;
    uint64_t n = 0;
    void init(double b) {
        baud = b; sps = kRate / b; L = std::max(4, (int)std::lround(sps));
        rh.assign(L, cf32(0, 0)); rl.assign(L, cf32(0, 0));
        idx = 0; sh = sl = 0; ph = 0; prevD = 0; transitions = 0; eye = 0; n = 0;
    }
    // hi / lo: the input mixed down by the upper and the lower tone. Returns 1 / 0 for a bit (1 = the higher tone), -1 for none
    int push(cf32 hi, cf32 lo) {
        sh += std::complex<double>(hi) - std::complex<double>(rh[idx]);
        sl += std::complex<double>(lo) - std::complex<double>(rl[idx]);
        rh[idx] = hi; rl[idx] = lo;
        if (++idx == L) idx = 0;
        if ((++n & 0xFFFFF) == 0) { sh = sl = 0; for (int i = 0; i < L; i++) { sh += std::complex<double>(rh[i]); sl += std::complex<double>(rl[i]); } }
        const double eh = std::norm(sh), el = std::norm(sl);
        const double d = (eh - el) / (eh + el + 1e-30);
        if ((d > 0) != (prevD > 0)) {
            const double g = transitions < 30 ? 0.35 : 0.08;
            ph -= g * (ph - 0.5);
            transitions++;
        }
        prevD = d;
        ph += 1.0 / sps;
        if (ph >= 1) {
            ph -= 1;
            eye = 0.97 * eye + 0.03 * std::fabs(d);
            return d > 0 ? 1 : 0;
        }
        return -1;
    }
};

class FskFront {
public:
    explicit FskFront(std::vector<double> bauds) : bauds_(std::move(bauds)) { psd_.setAlpha(0.25f); reset(); }
    void reset() {
        psd_.reset(); locked_ = false; centre_ = shift_ = 0; candC_ = candS_ = 0; candN_ = 0; t_ = 0; level_ = 0;
        dem_.assign(bauds_.size(), FskDemod());
        for (size_t i = 0; i < bauds_.size(); i++) dem_[i].init(bauds_[i]);
    }
    void setForced(double baud, double shift) {
        forcedShift_ = shift;
        if (baud > 0) { bauds_ = {baud}; } else bauds_ = defBauds_;
        reset();
    }
    void setDefaultBauds() { defBauds_ = bauds_; }
    // onBit(demod index, bit)
    template <class F> void push(const cf32* x, size_t n, F&& onBit) {
        if (psd_.push(x, n) && psd_.frames() >= 3) search();
        if (!locked_) { t_ += (double)n; return; }
        const double wc = 2 * kPi * centre_ / kRate, wd = kPi * shift_ / kRate;
        for (size_t i = 0; i < n; i++) {
            const double a = wc * t_;
            const cf32 base = x[i] * cf32((float)std::cos(a), (float)-std::sin(a));
            const double b = wd * t_;
            const cf32 r((float)std::cos(b), (float)-std::sin(b));
            const cf32 hi = base * r, lo = base * std::conj(r);
            for (size_t k = 0; k < dem_.size(); k++) { const int bit = dem_[k].push(hi, lo); if (bit >= 0) onBit((int)k, bit); }
            t_ += 1;
            if (t_ > 1e12) t_ = 0;
        }
    }
    bool locked() const { return locked_; }
    double centre() const { return centre_; }
    double shift() const { return shift_; }
    double snrDb() const { return snr_; }
    const std::vector<FskDemod>& demods() const { return dem_; }
    int bestDemod() const {
        int b = 0;
        for (size_t i = 1; i < dem_.size(); i++) if (dem_[i].eye > dem_[b].eye + 0.02) b = (int)i;
        return b;
    }
private:
    void search() {
        const double nz = psd_.noise();
        float p1, p2;
        const double f1 = psd_.peak(150, 5500, p1);
        if (p1 < 12 * nz) { lostCount_++; if (lostCount_ > 6) locked_ = false; return; }
        double f2;
        if (forcedShift_ > 0) {
            const float a = psd_.at(f1 + forcedShift_), b = psd_.at(f1 - forcedShift_);
            float pw;
            f2 = a > b ? psd_.peak(f1 + forcedShift_ - 8, f1 + forcedShift_ + 8, pw) : psd_.peak(f1 - forcedShift_ - 8, f1 - forcedShift_ + 8, pw);
            p2 = pw;
        } else {
            f2 = psd_.peak(std::max(100.0, f1 - 1100), std::min(5800.0, f1 + 1100), p2, f1 - 55, f1 + 55);
        }
        if (p2 < 6 * nz || p2 < p1 * 0.08f) { lostCount_++; if (lostCount_ > 6) locked_ = false; return; }
        lostCount_ = 0;
        const double c = 0.5 * (f1 + f2), s = std::fabs(f1 - f2);
        snr_ = snrDbOf(psd_, c - s / 2 - 40, c + s / 2 + 40);
        if (!locked_) {
            if (candN_ > 0 && std::fabs(c - candC_) < 6 && std::fabs(s - candS_) < 8) candN_++; else { candC_ = c; candS_ = s; candN_ = 1; }
            if (candN_ >= 2) { centre_ = c; shift_ = s; locked_ = true; }
        } else if (std::fabs(c - centre_) > 12 || std::fabs(s - shift_) > 15) {
            if (candN_ > 0 && std::fabs(c - candC_) < 6 && std::fabs(s - candS_) < 8) candN_++; else { candC_ = c; candS_ = s; candN_ = 1; }
            if (candN_ >= 3) { centre_ = c; shift_ = s; candN_ = 0; }
        } else {
            centre_ += 0.1 * (c - centre_);   // slow drift
        }
    }
    Psd psd_;
    std::vector<double> bauds_, defBauds_;
    std::vector<FskDemod> dem_;
    bool locked_ = false;
    double centre_ = 0, shift_ = 0, candC_ = 0, candS_ = 0, t_ = 0, level_ = 0, snr_ = 0, forcedShift_ = 0;
    int candN_ = 0, lostCount_ = 0;
};

// ---------------------------------------------------------------- CIS-36-50 / CIS-50-50 (BEE)
class BeeFramer {
public:
    std::function<void(const std::string&)> onText, onEvent;
    double baud = 50;
    uint64_t good = 0, bad = 0, messages = 0;
    int state = 0;   // 0 hunt, 1 message, 2 after the end of a message
    int pol = 0;
    void reset() { win_.clear(); state = 0; nb_ = 0; cur_ = 0; }
    void bit(int b) {
        win_.push_back((uint8_t)b);
        if (win_.size() > 184) win_.pop_front();
        if (state == 0) { if (win_.size() == 184) hunt(); return; }
        cur_ = (cur_ << 1) | ((b ^ pol) & 1);
        if (++nb_ < 7) return;
        const int c = cur_ & 0x7F;
        nb_ = 0; cur_ = 0;
        if (state == 1) inMessage(c); else afterEom(c);
    }
private:
    void hunt() {
        for (int p = 0; p < 2; p++) {
            int w = 0;
            for (int i = 0; i < 44; i++) w += win_[i] ^ p;
            if (w < 20 || w > 22) continue;
            int diff = 0;
            for (int i = 0; i < 70; i++) diff += win_[44 + i] != win_[114 + i];
            if (diff > 2) continue;
            int valid = 0;
            int key[10];
            for (int k = 0; k < 10; k++) {
                int a = 0, b = 0;
                for (int j = 0; j < 7; j++) { a = (a << 1) | (win_[44 + 7 * k + j] ^ p); b = (b << 1) | (win_[114 + 7 * k + j] ^ p); }
                key[k] = beeValidChar(a) ? a : b;
                valid += beeValidChar(key[k]);
            }
            if (valid < 8) continue;
            pol = p;
            uint64_t s = 0;
            for (int i = 0; i < 44; i++) s = (s << 1) | (uint64_t)(win_[i] ^ p);
            std::string k;
            char t[32];
            for (int i = 0; i < 10; i++) { snprintf(t, sizeof t, beeValidChar(key[i]) ? "%02X " : "[%02X] ", key[i]); k += t; }
            if (!k.empty()) k.pop_back();
            char e[200];
            snprintf(e, sizeof e, "message start at %.0f Bd: start word %011llX, session key %s", baud, (unsigned long long)s, k.c_str());
            if (onEvent) onEvent(e);
            if (onText) onText(std::string("\n[start, key ") + k + "] ");
            startMessage();
            return;
        }
    }
    void startMessage() { state = 1; nb_ = 0; cur_ = 0; chars_ = 0; errs_ = 0; invRun_ = 0; eomRun_ = 0; messages++; }
    void endMessage(const char* why) {
        char e[160];
        snprintf(e, sizeof e, "%s: %d characters, %d failed the 3:4 check", why, chars_, errs_);
        if (onEvent) onEvent(e);
        if (onText) onText("<EOM>\n");
    }
    void inMessage(int c) {
        if (c == kBeeEom) {
            if (++eomRun_ >= 3) { endMessage("end of message"); state = 2; invRun_ = 0; }
            return;
        }
        // a lone 0x77 followed by something else was a damaged character
        for (; eomRun_ > 0; eomRun_--) { chars_++; errs_++; bad++; if (onText) onText("[77] "); }
        char t[12];
        chars_++;
        if (beeValidChar(c)) { good++; invRun_ = 0; snprintf(t, sizeof t, "%02X ", c); }
        else { bad++; errs_++; invRun_++; snprintf(t, sizeof t, "[%02X] ", c); }
        if (onText) onText(t);
        if (invRun_ >= 12 || chars_ > 20000) { endMessage("signal lost"); state = 0; }
    }
    void afterEom(int c) {
        if (c == kBeeEom) { invRun_ = 0; return; }
        if (beeValidChar(c)) {
            if (onEvent) onEvent("next message (sent without a start sequence)");
            if (onText) onText("\n[next] ");
            startMessage();
            inMessage(c);
            return;
        }
        if (++invRun_ >= 6) state = 0;
    }
    std::deque<uint8_t> win_;
    int nb_ = 0, cur_ = 0, chars_ = 0, errs_ = 0, invRun_ = 0, eomRun_ = 0;
};

class BeeDecoder : public Decoder {
public:
    BeeDecoder() : front_({36, 50, 75, 100, 150}) { front_.setDefaultBauds(); build(); }
    void reset() override { front_.reset(); build(); }
    void setForced(double baud, double shift) override { front_.setForced(baud, shift); build(); }
    void push(const cf32* x, size_t n) override {
        if (fr_.size() != front_.demods().size()) build();
        front_.push(x, n, [this](int k, int b) { fr_[k].bit(b); });
    }
    Status status() const override {
        Status s;
        s.snrDb = front_.snrDb();
        if (!front_.locked()) { s.mode = "searching for a 2-FSK signal"; return s; }
        s.state = 1;
        int active = -1;
        for (size_t i = 0; i < fr_.size(); i++) if (fr_[i].state != 0) active = (int)i;
        const int show = active >= 0 ? active : front_.bestDemod();
        const double baud = front_.demods()[show].baud;
        uint64_t msgs = 0;
        for (const auto& f : fr_) { s.good += f.good; s.bad += f.bad; msgs += f.messages; }
        if (active >= 0) { s.state = 2; s.mode = fmt2("%.0f Bd, %.0f Hz shift: reading a message", baud, front_.shift()); }
        else s.mode = fmt2("%.0f Bd, %.0f Hz shift: waiting for a start of message", baud, front_.shift());
        s.params = {{"speed", fmt("%.0f Bd", baud)}, {"shift", fmt("%.0f Hz", front_.shift())}, {"centre", fmt("%.0f Hz", front_.centre())},
                    {"variant", baud >= 99 ? "CIS-50-50 (100 Bd traffic)" : "CIS-36-50 / CIS-50-50"}, {"messages", fmt("%.0f", (double)msgs)},
                    {"polarity", active >= 0 ? (fr_[active].pol ? "1 = higher tone" : "1 = lower tone") : "-"}, {"SNR", fmt("%.1f dB", front_.snrDb())}};
        return s;
    }
private:
    void build() {
        fr_.assign(front_.demods().size(), BeeFramer());
        for (size_t i = 0; i < fr_.size(); i++) {
            fr_[i].baud = front_.demods()[i].baud;
            fr_[i].onText = [this](const std::string& s) { text(s); };
            fr_[i].onEvent = [this](const std::string& s) { event(s); };
        }
    }
    FskFront front_;
    std::vector<BeeFramer> fr_;
};

// ---------------------------------------------------------------- CIS-14 (AMOR)
class Cis14Decoder : public Decoder {
public:
    Cis14Decoder() : front_({96}) { front_.setDefaultBauds(); }
    void reset() override { front_.reset(); bits_.clear(); total_ = 0; phase_ = -1; frames_ = 0; sinceCheck_ = 0; }
    void setForced(double baud, double shift) override { front_.setForced(baud, shift); bits_.clear(); total_ = 0; phase_ = -1; }
    void push(const cf32* x, size_t n) override { front_.push(x, n, [this](int, int b) { bit(b); }); }
    Status status() const override {
        Status s;
        s.snrDb = front_.snrDb();
        if (!front_.locked()) { s.mode = "searching for a 2-FSK signal"; return s; }
        s.state = phase_ >= 0 ? 2 : 1;
        s.mode = phase_ >= 0 ? fmt2("%.0f Bd, %.0f Hz shift: 14-bit frames", front_.demods()[0].baud, front_.shift())
                             : fmt2("%.0f Bd, %.0f Hz shift: looking for the 14-bit frame", front_.demods()[0].baud, front_.shift());
        s.good = frames_;
        s.params = {{"speed", fmt("%.0f Bd", front_.demods()[0].baud)}, {"shift", fmt("%.0f Hz", front_.shift())}, {"centre", fmt("%.0f Hz", front_.centre())},
                    {"frame period", phase_ >= 0 ? "14 bits (found)" : "-"}, {"state bits", phase_ >= 0 ? stateStr_ : "-"}, {"SNR", fmt("%.1f dB", front_.snrDb())}};
        return s;
    }
private:
    void bit(int b) {
        bits_.push_back((uint8_t)b);
        if (bits_.size() > 14 * 64) bits_.pop_front();
        total_++;
        if (++sinceCheck_ >= 14 && bits_.size() >= 14 * 24) { sinceCheck_ = 0; align(); }
        if (phase_ >= 0 && (int64_t)((total_ - (uint64_t)phase_) % 14) == 0 && bits_.size() >= 14) frame();
    }
    void align() {
        const size_t nf = bits_.size() / 14;
        const size_t start = bits_.size() - nf * 14;   // keep the phase relative to the total count
        double m[14] = {};
        for (size_t f = 0; f < nf; f++) for (int p = 0; p < 14; p++) m[p] += bits_[start + f * 14 + p] ? 1 : -1;
        double a[14];
        for (int p = 0; p < 14; p++) a[p] = std::fabs(m[p]) / (double)nf;
        int best = -1; double bv = 0;
        for (int p = 0; p < 14; p++) { const double v = std::min(a[p], a[(p + 1) % 14]); if (v > bv) { bv = v; best = p; } }
        std::vector<double> rest;
        for (int p = 0; p < 14; p++) if (p != best && p != (best + 1) % 14) rest.push_back(a[p]);
        std::nth_element(rest.begin(), rest.begin() + 6, rest.end());
        if (bv >= 0.85 && rest[6] <= 0.5) {
            // the absolute bit count at which a frame starts, modulo 14
            const uint64_t first = total_ - (uint64_t)(bits_.size() - start);   // count of the bit at index start
            const int ph = (int)((first + (uint64_t)best) % 14);
            if (ph != phase_) { phase_ = ph; event("14-bit frame found (state bits constant over " + std::to_string(nf) + " frames)"); }
        } else if (bv < 0.6 && phase_ >= 0) { phase_ = -1; event("14-bit frame lost"); }
    }
    void frame() {
        const size_t o = bits_.size() - 14;
        int st = 0, a = 0, b = 0, p = 0;
        st = bits_[o] << 1 | bits_[o + 1];
        for (int i = 0; i < 5; i++) { a = (a << 1) | bits_[o + 2 + 2 * i]; b = (b << 1) | bits_[o + 3 + 2 * i]; }
        p = bits_[o + 12] << 1 | bits_[o + 13];
        char t[48];
        snprintf(t, sizeof t, "%d%d %02X %02X %d%d\n", st >> 1, st & 1, a, b, p >> 1, p & 1);
        stateStr_ = std::string() + char('0' + (st >> 1)) + char('0' + (st & 1));
        text(t);
        frames_++;
    }
    FskFront front_;
    std::deque<uint8_t> bits_;
    uint64_t total_ = 0, frames_ = 0;
    int phase_ = -1, sinceCheck_ = 0;
    std::string stateStr_;
};

// ---------------------------------------------------------------- PSK order from differential symbols
struct Moments {
    std::complex<double> s2{0, 0}, s4{0, 0}, s8{0, 0};
    double n = 0;
    void add(std::complex<double> d) {
        const double m = std::abs(d);
        if (m < 1e-30) return;
        const auto u = d / m, u2 = u * u, u4 = u2 * u2;
        s2 += u2; s4 += u4; s8 += u4 * u4; n += 1;
    }
    void decay(double k) { s2 *= k; s4 *= k; s8 *= k; n *= k; }
    double m2() const { return n > 0 ? std::abs(s2) / n : 0; }
    double m4() const { return n > 0 ? std::abs(s4) / n : 0; }
    double m8() const { return n > 0 ? std::abs(s8) / n : 0; }
    int order() const { if (m2() > 0.55) return 2; if (m4() > 0.5) return 4; if (m8() > 0.35) return 8; return 0; }
};
const char* pskName(int q) { return q == 2 ? "BPSK" : q == 4 ? "QPSK" : q == 8 ? "8-PSK" : "unknown"; }

// ---------------------------------------------------------------- CIS-12 (MS5, AT-3004D)
class Cis12Decoder : public Decoder {
public:
    Cis12Decoder() { psd_.setAlpha(0.2f); reset(); }
    void reset() override {
        psd_.reset(); buf_.clear(); base_ = 0; locked_ = false; pilot_ = false; cfo_ = 0; active_ = 0; next_ = -1; symbols_ = 0; unsure_ = 0;
        mom_ = Moments(); havePrev_ = false; snr_ = 0; lineCount_ = 0; dataOn_ = false; quiet_ = 0;
    }
    void push(const cf32* x, size_t n) override {
        buf_.insert(buf_.end(), x, x + n);
        if (psd_.push(x, n) && psd_.frames() >= 4) search();
        if (!locked_) { trim(kT * 4); return; }
        process();
    }
    Status status() const override {
        Status s;
        s.cfoHz = cfo_;
        s.snrDb = snr_;
        if (!pilot_) { s.mode = "searching for the 3300 Hz pilot"; return s; }
        s.state = locked_ && dataOn_ ? 2 : 1;
        const int q = mom_.order();
        if (!locked_) s.mode = fmt("pilot at %.1f Hz, the carriers are off (standby)", 3300 + cfo_);
        else if (!dataOn_) s.mode = fmt2("pilot at %.1f Hz, %.0f carriers: no symbols yet", 3300 + cfo_, active_);
        else s.mode = std::string("12 x 120 Bd D") + pskName(q) + fmt(", pilot offset %+.1f Hz", cfo_);
        s.good = symbols_; s.bad = unsure_;
        s.params = {{"pilot", fmt("%.1f Hz", 3300 + cfo_)}, {"carriers seen", fmt("%.0f of 12", active_)}, {"carrier 1 .. 12", fmt2("%.0f .. %.0f Hz", 700 + cfo_, 2900 + cfo_)},
                    {"symbol rate", "120 Bd"}, {"modulation", q ? std::string("D") + pskName(q) : "-"},
                    {"phase-step spread", fmt2("M2 %.2f  M4 %.2f", mom_.m2(), mom_.m4())}, {"SNR", fmt("%.1f dB", snr_)}};
        return s;
    }
private:
    static constexpr int kT = 100;   // 12 kHz / 120 Bd
    static constexpr int kW = 60, kWoff = 20;   // 5 ms: the carriers 200 Hz apart are orthogonal over it
    void trim(size_t keep) { if (buf_.size() > keep) { const size_t d = buf_.size() - keep; buf_.erase(buf_.begin(), buf_.begin() + (long)d); base_ += d; } }
    void search() {
        const double nz = psd_.noise();
        float pp;
        const double fp = psd_.peak(3100, 3500, pp);
        const bool narrow = psd_.at(fp + 25) < pp * 0.1f && psd_.at(fp - 25) < pp * 0.1f;
        if (pp < 30 * nz || !narrow) { if (++miss_ > 8) { pilot_ = false; locked_ = false; } return; }
        miss_ = 0;
        pilot_ = true;
        const double c = fp - 3300;
        if (!locked_ || std::fabs(c - cfo_) > 3) cfo_ = c; else cfo_ += 0.2 * (c - cfo_);
        int act = 0;
        for (int k = 0; k < 12; k++) if (psd_.band(700 + 200 * k + cfo_ - 50, 700 + 200 * k + cfo_ + 50) > 40 * nz * (100 / psd_.binHz())) act++;
        active_ = act;
        snr_ = snrDbOf(psd_, 600 + cfo_, 3000 + cfo_);
        const bool was = locked_;
        locked_ = act >= 6;
        if (locked_ && !was) { event(fmt2("12 carriers found, pilot at %.1f Hz (offset %+.1f Hz)", 3300 + cfo_, cfo_)); next_ = -1; }
        if (!locked_ && was) event("carriers gone");
    }
    std::complex<double> integ(int k, int64_t s) const {
        // the carrier k over [s + kWoff, s + kWoff + kW) of the absolute sample count
        const double f = 700 + 200 * k + cfo_;
        const double w = -2 * kPi * f / kRate;
        const int64_t a = s + kWoff;
        const double ph0 = -2 * kPi * std::fmod(f * (double)a / kRate, 1.0);
        std::complex<double> r(std::cos(ph0), std::sin(ph0));
        const std::complex<double> st(std::cos(w), std::sin(w));
        std::complex<double> acc(0, 0);
        const cf32* p = buf_.data() + (a - (int64_t)base_);
        for (int i = 0; i < kW; i++) { acc += std::complex<double>(p[i]) * r; r *= st; }
        return acc;
    }
    void process() {
        const int64_t end = (int64_t)(base_ + buf_.size());
        // timing every 24 symbols: the integration window that does not straddle a phase change has the most energy
        if (next_ < 0 || ++sinceTiming_ >= 24) {
            sinceTiming_ = 0;
            const int64_t s0 = next_ < 0 ? (int64_t)base_ + kT : next_ - kT / 2;
            if (s0 + 24 * kT + kT + kWoff + kW > end) { if (next_ < 0) return; }
            else {
                double best = -1; int bd = 0;
                for (int d = 0; d < kT; d += 2) {
                    double e = 0;
                    for (int m = 0; m < 24; m++) for (int k = 0; k < 12; k += 2) e += std::norm(integ(k, s0 + d + m * kT));
                    if (e > best) { best = e; bd = d; }
                }
                const int64_t cand = s0 + bd;
                if (next_ < 0) next_ = cand;
                else {
                    int64_t diff = (cand - next_) % kT;
                    if (diff > kT / 2) diff -= kT;
                    if (diff < -kT / 2) diff += kT;
                    next_ += diff / 2;   // half the step: noise does not throw the clock about
                }
            }
        }
        while (next_ + kWoff + kW <= end) {
            if (next_ - (int64_t)base_ < 0) { next_ += kT; continue; }
            std::complex<double> z[12];
            double e = 0;
            for (int k = 0; k < 12; k++) { z[k] = integ(k, next_); e += std::norm(z[k]); }
            const double nz = psd_.noise() / (Psd::kN * 0.375) * kW * 12;   // the noise of 12 integrations
            const bool on = e > 6 * nz;
            if (on && havePrev_) {
                int q = mom_.order();
                char line[20];
                int unsureHere = 0;
                for (int k = 0; k < 12; k++) {
                    const std::complex<double> d = z[k] * std::conj(prev_[k]);
                    mom_.add(d);
                    int v = 0;
                    if (q) {
                        const double ang = std::arg(d) / (2 * kPi / q);
                        v = ((int)std::lround(ang) % q + q) % q;
                        if (std::fabs(ang - std::round(ang)) > 0.25) unsureHere++;
                    }
                    line[k] = q ? (char)('0' + v) : '?';
                }
                mom_.decay(0.995);
                line[12] = ' ';
                line[13] = 0;
                symbols_++;
                if (unsureHere > 2) unsure_++;
                if (!dataOn_) { dataOn_ = true; event("symbols started"); }
                std::string t(line);
                if (++lineCount_ % 6 == 0) t.back() = '\n';
                text(t);
                quiet_ = 0;
            } else if (!on && dataOn_ && ++quiet_ > 60) { dataOn_ = false; event("symbols stopped"); text("\n"); }
            for (int k = 0; k < 12; k++) prev_[k] = z[k];
            havePrev_ = on;
            next_ += kT;
        }
        trim((size_t)std::max<int64_t>(kT * 30, end - next_ + kT * 30));
    }
    Psd psd_;
    std::vector<cf32> buf_;
    size_t base_ = 0;
    bool locked_ = false, pilot_ = false, havePrev_ = false, dataOn_ = false;
    double cfo_ = 0, active_ = 0, snr_ = 0;
    int64_t next_ = -1;
    int sinceTiming_ = 0, miss_ = 0, lineCount_ = 0, quiet_ = 0;
    uint64_t symbols_ = 0, unsure_ = 0;
    Moments mom_;
    std::complex<double> prev_[12];
};

// ---------------------------------------------------------------- CIS-36 (MFSK-36) identification
class Cis36Decoder : public Decoder {
public:
    Cis36Decoder() : fft_(kF), win_(kL) { for (int i = 0; i < kL; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kL)); reset(); }
    void reset() override { buf_.clear(); rec_.clear(); t_ = 0; st_ = Status(); sinceAna_ = 0; reported_ = false; }
    void push(const cf32* x, size_t n) override {
        buf_.insert(buf_.end(), x, x + n);
        while (buf_.size() >= (size_t)kL) {
            frame();
            buf_.erase(buf_.begin(), buf_.begin() + kH);
            if (++sinceAna_ >= 80) { sinceAna_ = 0; analyse(); }
        }
    }
    Status status() const override { return st_; }
private:
    static constexpr int kL = 300, kH = 150, kF = 1024;
    struct Rec { double t, f; bool ok; };
    void frame() {
        std::vector<cf32> v(kF, cf32(0, 0));
        for (int i = 0; i < kL; i++) v[i] = buf_[i] * win_[i];
        fft_.forward(v.data());
        std::vector<float> p(kF);
        for (int i = 0; i < kF; i++) p[i] = std::norm(v[i]);
        const int b0 = (int)(150.0 / kRate * kF), b1 = (int)(4500.0 / kRate * kF);
        int best = b0;
        for (int b = b0; b <= b1; b++) if (p[b] > p[best]) best = b;
        std::vector<float> q(p.begin(), p.end());
        std::nth_element(q.begin(), q.begin() + kF / 2, q.end());
        const bool ok = p[best] > 25 * q[kF / 2];
        const float l = p[best - 1], r = p[best + 1], den = l - 2 * p[best] + r;
        const double d = std::fabs(den) > 1e-30f ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0;
        rec_.push_back({t_, (best + d) * kRate / kF, ok});
        if (rec_.size() > 80 * 12) rec_.pop_front();
        t_ += kH / kRate;
    }
    void analyse() {
        Status s;
        // tone histogram
        std::vector<double> h(3000, 0);   // 2 Hz bins up to 6 kHz
        int nok = 0;
        for (const auto& r : rec_) if (r.ok) { const int b = (int)(r.f / 2); if (b >= 0 && b < 3000) h[b] += 1; nok++; }
        if (nok < 80) { s.mode = "searching for an MFSK signal"; st_ = s; reported_ = false; return; }
        std::vector<double> hs(3000, 0);
        for (int i = 2; i < 2998; i++) hs[i] = h[i - 2] * 0.25 + h[i - 1] * 0.75 + h[i] + h[i + 1] * 0.75 + h[i + 2] * 0.25;
        std::vector<std::pair<double, double>> tones;   // frequency, weight
        const double minW = std::max(3.0, nok * 0.004);
        for (int i = 3; i < 2997; i++) {
            if (hs[i] < minW || hs[i] < hs[i - 1] || hs[i] < hs[i + 1]) continue;
            bool top = true;
            for (int j = -7; j <= 7; j++) if (j && hs[i + j] > hs[i]) top = false;
            if (!top) continue;
            double sw = 0, sf = 0;
            for (int j = -4; j <= 4; j++) { sw += h[i + j]; sf += h[i + j] * (i + j + 0.5) * 2; }
            if (sw > 0 && (tones.empty() || sf / sw - tones.back().first > 14)) tones.push_back({sf / sw, sw});
        }
        const int nt = (int)tones.size();
        if (nt < 4) { s.state = 0; s.mode = "a few tones only: not an MFSK signal"; st_ = s; reported_ = false; return; }
        // tone spacing: the widest grid that the tones sit on
        double sp = 0;
        for (double c = 100; c >= 15; c -= 0.05) {
            std::complex<double> z(0, 0); double w = 0;
            for (auto& t : tones) { z += t.second * std::polar(1.0, 2 * kPi * t.first / c); w += t.second; }
            if (std::abs(z) / w > 0.85) { sp = c; break; }
        }
        // symbol rate from the tone changes
        int changes = 0, pairs = 0;
        for (size_t i = 1; i < rec_.size(); i++) if (rec_[i].ok && rec_[i - 1].ok) { pairs++; if (std::fabs(rec_[i].f - rec_[i - 1].f) > std::max(8.0, sp * 0.5)) changes++; }
        const double rateFr = kRate / kH;
        double baud = pairs ? changes / (double)pairs * rateFr / std::max(0.5, 1 - 1.0 / nt) : 0;
        double snapped = 0;
        for (double b : {10.0, 20.0, 40.0}) if (std::fabs(baud - b) < 0.3 * b) snapped = b;
        int gaps = 0;
        for (int i = 1; i < nt; i++) if (sp > 0 && std::fabs(tones[i].first - tones[i - 1].first - 2 * sp) < 0.25 * sp) gaps++;
        const double span = tones.back().first - tones.front().first;
        const bool cis36 = sp > 37 && sp < 43 && nt >= 20 && span > 1000 && span < 1500;
        s.state = 1;
        char m[200];
        if (cis36) snprintf(m, sizeof m, "CIS-36 (MFSK-36): %d tones at %.1f Hz spacing, about %.0f Bd", nt, sp, snapped ? snapped : baud);
        else snprintf(m, sizeof m, "MFSK, %d tones at %.1f Hz spacing, about %.0f Bd: does not match CIS-36", nt, sp, baud);
        s.mode = m;
        s.params = {{"tones seen", fmt("%.0f", nt)}, {"spacing", sp > 0 ? fmt("%.1f Hz", sp) : "-"}, {"lowest / highest", fmt2("%.0f / %.0f Hz", tones.front().first, tones.back().first)},
                    {"span", fmt("%.0f Hz", span)}, {"symbol rate", snapped ? fmt("%.0f Bd", snapped) : fmt("about %.1f Bd", baud)},
                    {"double gaps", fmt("%.0f (control tones not sent)", gaps)}, {"identified", cis36 ? "CIS-36" : "no"}};
        if (cis36 && !reported_) { event(m); reported_ = true; }
        if (!cis36) reported_ = false;
        st_ = s;
    }
    Fft fft_;
    std::vector<float> win_;
    std::vector<cf32> buf_;
    std::deque<Rec> rec_;
    double t_ = 0;
    int sinceAna_ = 0;
    bool reported_ = false;
    Status st_;
};

// ---------------------------------------------------------------- OFDM analyser (CIS OFDM HDR, CIS-128)
struct OfdmMeas {
    bool found = false;
    double tu = 0, ts = 0, baud = 0, spacing = 0, f0 = 0, f1 = 0, rho = 0;
    int carriers = 0, order = 0;
    double m2 = 0, m4 = 0, m8 = 0;
    bool centreGap = false;
};

class OfdmDecoder : public Decoder {
public:
    explicit OfdmDecoder(int kind) : kind_(kind), fftB_(kNB) { reset(); }
    void reset() override { buf_.clear(); st_ = Status(); last_ = OfdmMeas(); reported_ = false; psd_.reset(); }
    void push(const cf32* x, size_t n) override {
        psd_.push(x, n);
        buf_.insert(buf_.end(), x, x + n);
        if (buf_.size() >= (size_t)kBlock) {
            analyse();
            buf_.erase(buf_.begin(), buf_.begin() + kBlock / 2);
        }
    }
    Status status() const override { return st_; }
private:
    static constexpr int kBlock = 24000, kNB = 32768;
    void analyse() {
        Status s;
        OfdmMeas m;
        double f0 = 0, f1 = 0;
        // the occupied band, without a narrow pilot
        if (psd_.frames() < 6 || !psd_.occupied(100, 5900, 6, 70, f0, f1, 300)) { s.mode = "searching for an OFDM signal"; st_ = s; reported_ = false; return; }
        const double snr = snrDbOf(psd_, f0, f1);
        // useful symbol length from the guard's correlation: R(L) = |sum x[n] x*[n + L]|
        const int lmin = 150, lmax = 800;
        const int N = kBlock - lmax;
        double e = 0;
        for (int i = 0; i < N; i++) e += std::norm(buf_[i]);
        std::vector<double> R(lmax + 2, 0);
        std::vector<std::complex<double>> Rc(lmax + 2);
        for (int L = lmin - 1; L <= lmax + 1; L++) {
            std::complex<float> acc(0, 0);
            const cf32* a = buf_.data();
            const cf32* b = buf_.data() + L;
            for (int i = 0; i < N; i++) acc += a[i] * std::conj(b[i]);
            Rc[L] = std::complex<double>(acc) / e;
            R[L] = std::abs(Rc[L]);
        }
        std::vector<double> rv(R.begin() + lmin, R.begin() + lmax);
        std::nth_element(rv.begin(), rv.begin() + rv.size() / 2, rv.end());
        const double med = rv[rv.size() / 2];
        int bl = lmin;
        for (int L = lmin; L <= lmax; L++) if (R[L] > R[bl]) bl = L;
        const double pk = R[bl] - med;
        if (pk < 0.06) { s.state = 0; s.mode = fmt("signal %.0f Hz wide, no OFDM guard found", f1 - f0); s.params = {{"band", fmt2("%.0f .. %.0f Hz", f0, f1)}}; st_ = s; reported_ = false; return; }
        const double l = R[bl - 1], r = R[bl + 1], den = l - 2 * R[bl] + r;
        m.tu = bl + (std::fabs(den) > 1e-12 ? std::clamp(0.5 * (l - r) / den, -0.5, 0.5) : 0);
        m.rho = pk;
        // the symbol rate: the guard correlation, summed over Tu / 8, repeats once per symbol
        const int G = std::max(8, bl / 8);
        std::vector<double> a(N - G, 0);
        {
            std::complex<double> acc(0, 0);
            std::vector<std::complex<double>> c(N);
            for (int i = 0; i < N; i++) c[i] = std::complex<double>(buf_[i] * std::conj(buf_[i + bl]));
            for (int i = 0; i < G; i++) acc += c[i];
            for (int i = 0; i + G < N; i++) { a[i] = std::abs(acc); acc += c[i + G] - c[i]; }
        }
        const double mean = std::accumulate(a.begin(), a.end(), 0.0) / (double)a.size();
        std::vector<cf32> v(kNB, cf32(0, 0));
        for (size_t i = 0; i < a.size() && i < (size_t)kNB; i++) v[i] = cf32((float)(a[i] - mean), 0);
        fftB_.forward(v.data());
        const double bh = kRate / kNB;
        int b0 = (int)(8 / bh), b1 = (int)(60 / bh), bb = b0;
        for (int b = b0; b <= b1; b++) if (std::norm(v[b]) > std::norm(v[bb])) bb = b;
        // prefer the fundamental: a strong line at half the frequency wins
        if (bb / 2 >= b0) { int h = bb / 2 - 3; int hb = h; for (int b = h; b <= bb / 2 + 3; b++) if (std::norm(v[b]) > std::norm(v[hb])) hb = b; if (std::norm(v[hb]) > 0.3 * std::norm(v[bb])) bb = hb; }
        {
            const double L = std::norm(v[bb - 1]), C = std::norm(v[bb]), Rr = std::norm(v[bb + 1]), dd = L - 2 * C + Rr;
            m.baud = (bb + (std::fabs(dd) > 1e-30 ? std::clamp(0.5 * (L - Rr) / dd, -0.5, 0.5) : 0)) * bh;
        }
        m.ts = kRate / m.baud;
        // refine the symbol length by folding: the sharpest fold wins
        double bestTs = m.ts, bestPk = 0, bestPh = 0;
        for (double t = m.ts * 0.995; t <= m.ts * 1.005; t += m.ts * 0.00025) {
            const int nb = (int)std::lround(t);
            std::vector<double> fold(nb, 0);
            for (size_t i = 0; i < a.size(); i++) fold[(size_t)std::fmod((double)i, t) % nb] += a[i];
            int ib = 0; for (int i = 1; i < nb; i++) if (fold[i] > fold[ib]) ib = i;
            const double mf = std::accumulate(fold.begin(), fold.end(), 0.0) / nb;
            if (fold[ib] / mf > bestPk) { bestPk = fold[ib] / mf; bestTs = t; bestPh = ib; }
        }
        m.ts = bestTs; m.baud = kRate / m.ts;
        m.spacing = kRate / m.tu;
        m.f0 = f0; m.f1 = f1;
        m.carriers = (int)std::lround((f1 - f0) / m.spacing) + 1;
        // PSK order: a DFT of the useful part of each symbol at the carriers, the change from symbol to symbol, per carrier
        const double frac = std::fmod(-std::arg(Rc[bl]) / (2 * kPi) + 10, 1.0);
        const int tuI = (int)std::lround(m.tu);
        const double tg = m.ts - m.tu;
        const double st0 = bestPh + G / 2.0;   // the fold peaks with its window inside the guard: start the DFT in the middle of the guard
        std::vector<double> fk;
        for (int k = (int)std::ceil(f0 / m.spacing - frac) ; (k + frac) * m.spacing <= f1; k++) fk.push_back((k + frac) * m.spacing);
        std::vector<Moments> mm(fk.size());
        std::vector<std::complex<double>> prev(fk.size());
        bool hp = false;
        std::vector<double> carrierPow(fk.size(), 0);
        for (double t0 = st0; t0 + tuI < (double)kBlock; t0 += m.ts) {
            const int s0 = (int)std::lround(t0);
            if (s0 < 0) continue;
            for (size_t k = 0; k < fk.size(); k++) {
                const double w = -2 * kPi * fk[k] / kRate;
                std::complex<double> r(std::cos(w * s0), std::sin(w * s0)), stp(std::cos(w), std::sin(w)), acc(0, 0);
                for (int i = 0; i < tuI; i++) { acc += std::complex<double>(buf_[s0 + i]) * r; r *= stp; }
                if (hp) mm[k].add(acc * std::conj(prev[k]));
                prev[k] = acc;
                carrierPow[k] += std::norm(acc);
            }
            hp = true;
        }
        if (!fk.empty()) {
            for (auto& q : mm) { m.m2 += q.m2(); m.m4 += q.m4(); m.m8 += q.m8(); }
            m.m2 /= (double)fk.size(); m.m4 /= (double)fk.size(); m.m8 /= (double)fk.size();
            m.order = m.m2 > 0.55 ? 2 : m.m4 > 0.5 ? 4 : m.m8 > 0.35 ? 8 : 0;
            // a missing carrier in the middle of the band
            const size_t mid = fk.size() / 2;
            std::vector<double> cp(carrierPow);
            std::nth_element(cp.begin(), cp.begin() + cp.size() / 2, cp.end());
            const double medp = cp[cp.size() / 2];
            for (size_t k = mid >= 3 ? mid - 3 : 0; k < std::min(fk.size(), mid + 4); k++) if (carrierPow[k] < 0.2 * medp) m.centreGap = true;
            int on = 0; for (double p : carrierPow) on += p > 0.2 * medp; m.carriers = on;
        }
        m.found = true;
        last_ = m;
        // which mode
        std::string name;
        struct Ref { const char* name; double tuMs, baud; int order; };
        if (kind_ == Hdr) {
            static const Ref refs[] = {{"CIS-45 (40 Bd)", 16, 40, 2}, {"CIS-45 (33.3 Bd)", 16, 33.333, 2}, {"CIS-60", 22.5, 35.555, 8}, {"CIS-93", 32, 22.222, 4}};
            for (const auto& rf : refs) if (std::fabs(m.tu / kRate * 1000 - rf.tuMs) < rf.tuMs * 0.03 && std::fabs(m.baud - rf.baud) < rf.baud * 0.03) name = rf.name;
        } else {
            if (m.carriers >= 110 && m.carriers <= 140 && m.baud > 17 && m.baud < 25) name = "CIS-128";
            if (m.carriers >= 110 && m.carriers <= 140 && m.baud > 36 && m.baud < 48) name = "CIS-128 (6 kHz)";
        }
        s.state = 1;
        s.snrDb = snr;
        char line[240];
        if (!name.empty()) snprintf(line, sizeof line, "%s: %d carriers at %.2f Hz, %.2f Bd, %s", name.c_str(), m.carriers, m.spacing, m.baud, pskName(m.order));
        else snprintf(line, sizeof line, "OFDM, %d carriers at %.2f Hz, %.2f Bd: no %s match", m.carriers, m.spacing, m.baud, kind_ == Hdr ? "CIS-45 / 60 / 93" : "CIS-128");
        s.mode = line;
        s.params = {{"identified", name.empty() ? "no" : name}, {"band", fmt2("%.0f .. %.0f Hz", f0, f1)}, {"carriers", fmt("%.0f", m.carriers)}, {"spacing", fmt("%.2f Hz", m.spacing)},
                    {"symbol rate", fmt("%.3f Bd", m.baud)}, {"useful / guard", fmt2("%.2f / %.2f ms", m.tu / kRate * 1000, (m.ts - m.tu) / kRate * 1000)},
                    {"carrier modulation", pskName(m.order)}, {"phase-step spread", fmt2("M2 %.2f  M4 %.2f", m.m2, m.m4) + fmt("  M8 %.2f", m.m8)},
                    {"centre carrier off", m.centreGap ? "yes" : "no"}, {"pilot 3300 Hz", psd_.at(3300) > 20 * psd_.noise() ? "present" : "-"}, {"SNR", fmt("%.1f dB", snr)}};
        if (!name.empty() && !reported_) { event(line); reported_ = true; }
        if (name.empty()) reported_ = false;
        st_ = s;
    }
    int kind_;
    Fft fftB_;
    Psd psd_;
    std::vector<cf32> buf_;
    Status st_;
    OfdmMeas last_;
    bool reported_ = false;
};

// ---------------------------------------------------------------- CIS-3000 identification
class Cis3000Decoder : public Decoder {
public:
    Cis3000Decoder() : fft_(kN) { reset(); }
    void reset() override { buf_.clear(); st_ = Status(); psd_.reset(); for (auto& a : acc_) a.assign(kN, 0.0); blocks_ = 0; reported_ = false; }
    void push(const cf32* x, size_t n) override {
        psd_.push(x, n);
        buf_.insert(buf_.end(), x, x + n);
        while (buf_.size() >= (size_t)kBlock) { block(); buf_.erase(buf_.begin(), buf_.begin() + kBlock); }
    }
    Status status() const override { return st_; }
private:
    static constexpr int kBlock = 12000, kN = 16384;
    void block() {
        Status s;
        double f0, f1;
        if (psd_.frames() < 4 || !psd_.occupied(100, 5900, 6, 60, f0, f1, 800)) { s.mode = "searching for a single-carrier PSK signal"; st_ = s; blocks_ = 0; reported_ = false; for (auto& a : acc_) std::fill(a.begin(), a.end(), 0.0); return; }
        const double fc = 0.5 * (f0 + f1);
        if (blocks_ > 0 && std::fabs(fc - fcPrev_) > 40) { blocks_ = 0; for (auto& a : acc_) std::fill(a.begin(), a.end(), 0.0); }
        fcPrev_ = fc;
        std::vector<cf32> y(kBlock);
        const double w = -2 * kPi * fc / kRate;
        for (int i = 0; i < kBlock; i++) y[i] = buf_[i] * cf32((float)std::cos(w * i), (float)std::sin(w * i));
        // 0: |y|^2 (symbol rate line), 1..3: y^2, y^4, y^8 of the unit-amplitude samples (carrier lines)
        for (int q = 0; q < 4; q++) {
            std::vector<cf32> v(kN, cf32(0, 0));
            double mean = 0;
            if (q == 0) { for (int i = 0; i < kBlock; i++) mean += std::norm(y[i]); mean /= kBlock; }
            for (int i = 0; i < kBlock; i++) {
                if (q == 0) v[i] = cf32((float)(std::norm(y[i]) - mean), 0);
                else { const float m = std::abs(y[i]); cf32 u = m > 0 ? y[i] / m : cf32(0, 0); cf32 r = u * u; if (q >= 2) r *= r; if (q >= 3) r *= r; v[i] = r; }
            }
            fft_.forward(v.data());
            for (int i = 0; i < kN; i++) acc_[q][i] += std::norm(v[i]);
        }
        blocks_++;
        const double bh = kRate / kN;
        auto line = [&](int q, double lo, double hi, double& f) {
            int b0 = (int)std::floor(lo / bh), b1 = (int)std::ceil(hi / bh), bb = b0;
            std::vector<double> vals;
            for (int b = b0; b <= b1; b++) { const double v = acc_[q][((b % kN) + kN) % kN]; vals.push_back(v); if (v > acc_[q][((bb % kN) + kN) % kN]) bb = b; }
            std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
            f = bb * bh;
            return 10 * std::log10(acc_[q][((bb % kN) + kN) % kN] / std::max(1e-30, vals[vals.size() / 2]));
        };
        double fb, f2, f4, f8;
        const double rb = line(0, 500, 5000, fb);
        const double r2 = line(1, -5900, 5900, f2), r4 = line(2, -5900, 5900, f4), r8 = line(3, -5900, 5900, f8);
        const double thr = 16;
        const int order = r2 > thr ? 2 : r4 > thr ? 4 : r8 > thr ? 8 : 0;
        const double fine = order == 2 ? f2 / 2 : order == 4 ? f4 / 4 : order == 8 ? f8 / 8 : 0;
        const bool rateOk = rb > thr;
        s.state = 1;
        s.snrDb = snrDbOf(psd_, f0, f1);
        const bool is3000 = rateOk && fb > 2900 && fb < 3100 && order == 8;
        char m[200];
        if (is3000) snprintf(m, sizeof m, "CIS-3000: 8-PSK, %.0f Bd, centre %.0f Hz", fb, fc + fine);
        else snprintf(m, sizeof m, "PSK signal %.0f Hz wide: %s, %s: no CIS-3000 match", f1 - f0, rateOk ? fmt("%.0f Bd", fb).c_str() : "no symbol rate line", pskName(order));
        s.mode = m;
        s.params = {{"identified", is3000 ? "CIS-3000" : "no"}, {"band", fmt2("%.0f .. %.0f Hz", f0, f1)}, {"centre", fmt("%.1f Hz", fc + fine)}, {"symbol rate", rateOk ? fmt("%.1f Bd", fb) : "-"},
                    {"modulation", pskName(order)}, {"lines y^2 / y^4 / y^8", fmt2("%.0f / %.0f dB", r2, r4) + fmt(" / %.0f dB", r8)}, {"rate line", fmt("%.0f dB", rb)}, {"SNR", fmt("%.1f dB", s.snrDb)}};
        if (is3000 && !reported_) { event(m); reported_ = true; }
        if (!is3000) reported_ = false;
        if (blocks_ >= 8) { for (auto& a : acc_) for (auto& v : a) v *= 0.5; blocks_ = 4; }
        st_ = s;
    }
    Fft fft_;
    Psd psd_;
    std::vector<cf32> buf_;
    std::vector<double> acc_[4];
    int blocks_ = 0;
    double fcPrev_ = 0;
    bool reported_ = false;
    Status st_;
};

}  // namespace

std::unique_ptr<Decoder> makeDecoder(int kind) {
    switch (kind) {
    case Bee: return std::make_unique<BeeDecoder>();
    case Cis14: return std::make_unique<Cis14Decoder>();
    case Cis12: return std::make_unique<Cis12Decoder>();
    case Cis36: return std::make_unique<Cis36Decoder>();
    case Hdr: return std::make_unique<OfdmDecoder>(Hdr);
    case C128: return std::make_unique<OfdmDecoder>(C128);
    case C3000: return std::make_unique<Cis3000Decoder>();
    default: return nullptr;
    }
}

}  // namespace dect2::cishf
