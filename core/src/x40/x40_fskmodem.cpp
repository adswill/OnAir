// Telephone-style FSK modems on a radio channel: Bell 103 (originate 1070 / 1270 Hz, answer 2025 / 2225 Hz, 300 baud), ITU-T V.21 (channel 1 980 / 1180 Hz, channel 2 1650 / 1850 Hz, 300 baud), ITU-T V.23 mode 2
// (1300 Hz mark / 2100 Hz space, 1200 baud) and Bell 202 (1200 Hz mark / 2200 Hz space, 1200 baud) as audio tones in a single-sideband channel (the Canadian time station CHU sends Bell 103 answer tones, 300 baud, on its upper
// sideband); asynchronous bytes: a start bit (space), eight data bits least significant first, one or more stop bits (mark), idle mark between bytes.
// The complex channel (8 ksps, +-3.4 kHz) is searched for bursts of tone energy (32 ms frames, 8 times the noise floor for 0.1 s, ending after 0.2 s under 3 times); a burst of 0.4 s or more is analysed:
// Welch spectrum (7.8 Hz bins): the band 600 - 2700 Hz must hold 9 dB over the noise (3050 - 3400 Hz), in one sideband (the polarity of the sideband is read from it), with 60 % of that power near the two tones of the standard tried.
// A standard is tried with a dial error from -40 to +40 Hz in steps of 10 Hz (the closest standards are 90 Hz apart): one matched correlator per tone over one bit time gives mark minus space; each start bit is found as a
// crossing of that difference, the eight data bits and the first stop bit are read at their centres (the next start bit resynchronises, so a clock error of a few percent costs nothing); a stop bit that is not mark counts as a
// framing error. The standard with the most valid bytes wins when it has at least four, at most 15 % framing errors and decisive bits (mean |d| of 0.45); the dial error is then refined to 2 Hz.
// A message row has the standard, the baud rate, the measured tones, the bytes in hex and as ASCII (non-printing as a dot), and the number of framing errors. It does not know the meaning of the bytes (the time code of CHU
// has its own format: it is shown as the bytes). Receive only.
#include "x40_dsp.h"
#include <deque>

namespace dect2::x40 {

namespace {
constexpr double kWorkTarget = 8000, kCut = 3400;
constexpr int kFrame = 256;

struct Std { const char* name; double mark, space, baud; };
const Std kStd[] = {
    {"Bell 103 originate, 300 baud", 1270, 1070, 300}, {"Bell 103 answer, 300 baud", 2225, 2025, 300},
    {"V.21 channel 1, 300 baud", 980, 1180, 300}, {"V.21 channel 2, 300 baud", 1650, 1850, 300},
    {"V.23 mode 2, 1200 baud", 1300, 2100, 1200}, {"Bell 202, 1200 baud", 1200, 2200, 1200},
};
constexpr int kNumStd = 6;

class ModemDec : public Decoder {
public:
    bool configure(double fs) override {
        const int d = std::max(1, (int)std::floor(fs / kWorkTarget));
        if (!dec_.configure(fs, d, std::min(kCut, 0.45 * fs / d))) return false;
        rate_ = dec_.outRate(); binHz_ = rate_ / kFrame;
        win_.resize(kFrame); for (int i = 0; i < kFrame; i++) win_[i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / kFrame));
        reset(); return true;
    }
    void reset() override { dec_.reset(); fr_.clear(); hist_.clear(); inBurst_ = false; burst_.clear(); onCnt_ = offCnt_ = 0; t_ = 0; nMsg_ = 0; skip_ = (size_t)(0.3 * rate_); have_ = false; res_ = Res(); prevFr_.clear(); }
    void feed(const cf32* x, size_t n) override {
        dec_.process(x, n, [&](cf32 a) { if (skip_) { skip_--; return; } fr_.push_back(a); if ((int)fr_.size() >= kFrame) { frame(); fr_.clear(); } });
    }
    void fill(X40Telemetry& t) const override {
        if (!have_) { t.headline = "waiting for data"; return; }
        if (!res_.ok) { t.headline = nMsg_ ? fmt("%d message(s) so far", nMsg_) : "no FSK modem signal yet"; return; }
        t.headline = fmt("%s: %d bytes", res_.std.c_str(), res_.nbytes);
        t.facts.push_back({"standard", res_.std});
        t.facts.push_back({"tones (mark / space)", fmt("%.1f / %.1f Hz, %s sideband, offset %+.0f Hz", res_.mark, res_.space, res_.negative ? "lower" : "upper", res_.offset)});
        t.facts.push_back({"last message", fmt("%d bytes, %d framing errors, %.1f s", res_.nbytes, res_.frameErr, res_.dur)});
        t.facts.push_back({"messages", fmt("%d", nMsg_)});
    }
private:
    struct Res { bool ok = false, negative = false; std::string std; double mark = 0, space = 0, offset = 0, dur = 0; int nbytes = 0, frameErr = 0; };
    void frame() {
        have_ = true; t_ += (double)kFrame / rate_;
        std::vector<cf32> a(kFrame); for (int i = 0; i < kFrame; i++) a[i] = fr_[i] * win_[i];
        fftx(a);
        double pb = 0; for (int k = 0; k < kFrame; k++) { const double f = std::fabs((k < kFrame / 2 ? k : k - kFrame) * binHz_); if (f >= 350 && f <= 2800) pb += std::norm(a[k]); }
        hist_.push_back((float)pb); if (hist_.size() > (size_t)(10 * rate_ / kFrame)) hist_.pop_front();
        std::vector<float> s(hist_.begin(), hist_.end()); std::nth_element(s.begin(), s.begin() + (long)(s.size() / 5), s.end()); const double floorP = std::max((double)s[s.size() / 5], 1e-12);
        const bool on = pb > 8.0 * floorP, off = pb < 3.0 * floorP;
        if (!inBurst_) {
            if (on) { if (++onCnt_ >= 3) { inBurst_ = true; burst_.clear(); burst_.insert(burst_.end(), prevFr_.begin(), prevFr_.end()); startT_ = t_ - 4.0 * kFrame / rate_; offCnt_ = 0; } } else onCnt_ = 0;
        }
        if (inBurst_) {
            burst_.insert(burst_.end(), fr_.begin(), fr_.end());
            if (off) { if (++offCnt_ >= 6) endBurst(); } else offCnt_ = 0;
            if (burst_.size() > (size_t)(25 * rate_)) endBurst();
        } else { prevFr_.insert(prevFr_.end(), fr_.begin(), fr_.end()); if (prevFr_.size() > (size_t)(3 * kFrame)) prevFr_.erase(prevFr_.begin(), prevFr_.end() - 3 * kFrame); if (off) out->energy(); }
    }
    void endBurst() {
        inBurst_ = false; onCnt_ = offCnt_ = 0;
        std::vector<cf32> b; b.swap(burst_);
        const double dur = (double)b.size() / rate_;
        if (dur < 0.4) return;
        analyse(b, dur);
    }
    // One trial: the channel demodulated as one standard with a tone shift of `off` Hz in one sideband. d = mark minus space over the last bit (normalised, +1 mark, -1 space).
    struct Trial { std::vector<uint8_t> bytes; int ferr = 0; double decisive = 0, cap = 0; };
    Trial trial(const std::vector<cf32>& b, const Std& st, double off, double sgn) const {
        Trial r; const size_t n = b.size();
        const int L = std::max(4, (int)std::lround(rate_ / st.baud));
        const double T = rate_ / st.baud;
        std::vector<float> d(n, 0.f), e(n, 0.f); cf32 sm(0, 0), ss(0, 0);
        std::vector<cf32> pm(n), ps(n);
        const double wm = -2 * kPi * sgn * (st.mark + off) / rate_, ws = -2 * kPi * sgn * (st.space + off) / rate_;
        cf32 rm(1, 0), rs(1, 0); const cf32 sm1((float)std::cos(wm), (float)std::sin(wm)), ss1((float)std::cos(ws), (float)std::sin(ws));
        for (size_t i = 0; i < n; i++) { pm[i] = b[i] * rm; ps[i] = b[i] * rs; rm *= sm1; rs *= ss1; if ((i & 1023) == 1023) { rm /= std::abs(rm); rs /= std::abs(rs); } }
        for (size_t i = 0; i < n; i++) { sm += pm[i]; ss += ps[i]; if (i >= (size_t)L) { sm -= pm[i - L]; ss -= ps[i - L]; } const float em = std::norm(sm), es = std::norm(ss); d[i] = (em - es) / (em + es + 1e-20f); e[i] = std::max(em, es); }
        std::vector<float> es2(e.begin() + std::min(n, (size_t)L * 2), e.end()); float thr = 0;
        if (!es2.empty()) { std::nth_element(es2.begin(), es2.begin() + (long)(es2.size() * 8 / 10), es2.end()); thr = 0.3f * es2[es2.size() * 8 / 10]; }   // a bit counts where the tone is on: 30 % of the 80th percentile of the energy
        size_t i = (size_t)L * 2;
        while (i < n && d[i] < 0.5f) i++;                                             // the first stretch of mark
        double dsum = 0; int dn = 0;
        while (i + 1 < n) {
            if (!(d[i - 1] >= 0 && d[i] < 0) || e[i] < thr) { i++; continue; }                     // a start bit: d goes from mark to space
            const double tc = (double)(i - 1) + d[i - 1] / (d[i - 1] - d[i]);        // the crossing, half a bit after the start edge
            bool ok = true, quiet = false; unsigned v = 0; double dd = 0, cp = 0;
            for (int k = 0; k <= 9 && ok; k++) {
                const size_t j = (size_t)std::llround(tc + ((double)k + 0.5) * T);    // d[j] covers the bit that ends at j: bit k ends at tc + (k + 0.5) T
                if (j >= n || e[j] < thr) { ok = false; quiet = true; break; }          // no tone there: not a byte
                if (k == 0) continue;
                const bool mark = d[j] > 0; dd += std::fabs(d[j]); cp += e[j];
                if (k <= 8) { if (mark) v |= 1u << (k - 1); } else if (!mark) { r.ferr++; ok = false; }
            }
            if (ok) { r.bytes.push_back((uint8_t)v); dsum += dd / 9; dn++; r.cap += cp; i = (size_t)std::llround(tc + 9.0 * T); } else i = (size_t)std::llround(tc + 1.0 * T);
            (void)quiet;
        }
        r.decisive = dn ? dsum / dn : 0;
        return r;
    }
    bool analyse(const std::vector<cf32>& b, double dur) {
        constexpr int N = 1024; const double bh = rate_ / N;
        std::vector<double> psd(N, 0.0); int segs = 0;
        std::vector<cf32> a(N);
        for (size_t o = 0; o + N <= b.size(); o += N / 2) { for (int i = 0; i < N; i++) a[i] = b[o + i] * (float)(0.5 - 0.5 * std::cos(2 * kPi * i / N)); fftx(a); for (int k = 0; k < N; k++) psd[k] += std::norm(a[k]); segs++; }
        if (segs < 2) return false;
        // power in the band of the standards (600 - 2700 Hz) per sideband, and the noise: the median bin power at 3050 - 3400 Hz
        double pw[2] = {0, 0}; std::vector<double> nz;
        for (int k = 0; k < N; k++) { const double f = (k < N / 2 ? k : k - N) * bh, af = std::fabs(f); if (af >= 600 && af <= 2700) pw[f > 0 ? 0 : 1] += psd[k]; if (af >= 3050 && af <= 3400) nz.push_back(psd[k]); }
        if (nz.size() < 8) return false;
        std::nth_element(nz.begin(), nz.begin() + (long)(nz.size() / 2), nz.end()); const double noise = std::max(nz[nz.size() / 2], 1e-30);
        const double inBand = pw[0] + pw[1]; if (inBand < 8.0 * noise * (2100.0 / bh)) return false;      // the band must hold 9 dB over the noise
        const int pol = pw[0] >= pw[1] ? 0 : 1; if (pw[pol ? 0 : 1] > 0.25 * pw[pol]) return false;         // one sideband carries the signal
        const double sgn = pol == 0 ? 1.0 : -1.0;
        int pick = -1; double bestScore = 0, bestOff = 0; Trial bestT;
        for (int k = 0; k < kNumStd; k++) {
            // the share of the power that lies near the two tones (a 300 baud signal is spread by about its baud rate)
            for (double off = -40; off <= 40; off += 10) {
                const Std& st = kStd[k];
                const double halfW = 0.5 * st.baud + 40;
                double nm = 0, nsp = 0; for (int q = 0; q < N; q++) { const double f = sgn * (q < N / 2 ? q : q - N) * bh; if (f > 0) { if (std::fabs(f - (st.mark + off)) < halfW) nm += psd[q]; if (std::fabs(f - (st.space + off)) < halfW) nsp += psd[q]; } }
                if (nm < 0.06 * pw[pol] || nsp < 0.06 * pw[pol] || nm + nsp < 0.6 * pw[pol]) continue;
                Trial t = trial(b, st, off, sgn);
                if (t.bytes.size() >= 4 && t.ferr <= (int)(0.15 * (double)t.bytes.size()) && t.decisive >= 0.45 && t.cap > bestScore) { bestScore = t.cap; pick = k; bestOff = off; bestT = t; }
            }
        }
        if (pick < 0) return false;
        // the dial error to 2 Hz around the best shift, by the same score
        { const Std& st = kStd[pick]; double bs = bestScore; const double c0 = bestOff; for (double off = c0 - 8; off <= c0 + 8; off += 2) { Trial t = trial(b, st, off, sgn); if (t.bytes.size() >= bestT.bytes.size() && t.cap > bs) { bs = t.cap; bestOff = off; bestT = t; } } }
        const Std& st = kStd[pick]; const std::vector<uint8_t>& bytes = bestT.bytes; const double off = bestOff;
        Res r; r.ok = true; r.negative = pol == 1; r.std = st.name; r.mark = st.mark + off; r.space = st.space + off; r.offset = off; r.dur = dur; r.nbytes = (int)bytes.size(); r.frameErr = std::max(bestT.ferr, 0);
        res_ = r; nMsg_++;
        std::string hex, asc; for (size_t q = 0; q < bytes.size() && q < 48; q++) { hex += fmt("%02X ", bytes[q]); asc += (bytes[q] >= 32 && bytes[q] < 127) ? (char)bytes[q] : '.'; }
        if (bytes.size() > 48) { hex += "..."; asc += "..."; }
        out->good(0, off);
        out->row("Message", st.name, fmt("%d bytes: %s| %s | tones %.0f / %.0f Hz (%s sideband, offset %+.0f Hz), %d framing errors, %.1f s", (int)bytes.size(), hex.c_str(), asc.c_str(), r.mark, r.space, r.negative ? "lower" : "upper", off, r.frameErr, dur), toDb((float)(inBand / noise / (2100.0 / bh))));
        return true;
    }
    Decimator dec_; double rate_ = 1, binHz_ = 1, t_ = 0, startT_ = 0; int nMsg_ = 0, onCnt_ = 0, offCnt_ = 0; size_t skip_ = 0; bool have_ = false, inBurst_ = false;
    std::vector<float> win_; std::vector<cf32> fr_, burst_, prevFr_; std::deque<float> hist_; Res res_;
};
}

std::unique_ptr<Decoder> makeFskModem() { return std::make_unique<ModemDec>(); }

} // namespace dect2::x40
