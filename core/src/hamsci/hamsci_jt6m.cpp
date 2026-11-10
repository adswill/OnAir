// JT6M (WSJT, K1JT): 44-tone FSK for 6 m meteor and ionospheric scatter. One symbol = 512 samples at 11025 Hz (46.44 ms, 21.533 baud),
// tone n = (n + 51) x 11025 / 512 Hz: the sync tone (n = -1) at 1076.66 Hz, the 43 data tones 1098.2 .. 2002.6 Hz. Every third symbol is
// the sync tone, the two after it carry one character each (14.4 characters a second). The character set, in tone order, is
// "0123456789.,  /#?$" then A .. Z (n = 0 .. 9 digits, 10 '.', 11 ',', 12 space, 13 '/', 14 '#', 15 '?', 16 '$', 17 .. 42 A .. Z).
// The message gets one trailing space and is padded to an even length, then repeats for the whole transmit period.
// Facts read from WSJT 5 (gen6m.f, gentone.f) as carried in MSHV (LZ2HV, GPL-3.0, genmesage_ms.cpp); this receiver and generator are new code.
//
// Receiver: a grid of tone energies at quarter-symbol steps and quarter-tone frequency steps; the symbol phase, tuning error and frame
// phase are the ones whose sync symbols stand out most (a sum of clipped log ratios, so a short ping counts as much as a weak long signal);
// each frame whose sync tone is clear gives its two characters, and runs of clear frames are the pings.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_int.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dect2 {
namespace hamsci {
using namespace detail;

namespace {
constexpr int kData = 43;
constexpr int kTones = 44;                     // grid tone 0 = sync, 1 .. 43 = data 0 .. 42
constexpr double kDf = 11025.0 / 512.0;        // 21.533 Hz
constexpr double kSyncHz = 50.0 * kDf;         // 1076.66 Hz
const char kSet[] = "0123456789., /#?$ABCDEFGHIJKLMNOPQRSTUVWXYZ";   // 43
}  // namespace

int jt6mCharIndex(char c) {
    c = (char)std::toupper((unsigned char)c);
    for (int i = 0; i < kData; i++) if (kSet[i] == c) return i;
    return -1;
}
char jt6mChar(int i) { return i >= 0 && i < kData ? kSet[i] : '?'; }
double jt6mToneHz(int n) { return (double)(n + 51) * kDf; }

std::string jt6mMessage(const std::string& msg) {
    std::string m;
    for (char c : msg) m += jt6mCharIndex(c) >= 0 ? (char)std::toupper((unsigned char)c) : ' ';
    while (!m.empty() && m.back() == ' ') m.pop_back();
    if (m.size() > 58) m.resize(58);
    m += ' ';
    if (m.size() % 2) m += ' ';
    return m;
}

std::vector<float> jt6mGenerate(const std::string& msg, double fs, double seconds, double offsetHz, double amp, double startSec, double driftHzPerSec) {
    const std::string m = jt6mMessage(msg);
    const double nsps = fs * 512.0 / 11025.0;
    const size_t n = (size_t)(seconds * fs);
    std::vector<float> y(n, 0.f);
    const size_t first = (size_t)std::max(0.0, startSec * fs);
    if (first >= n) return y;
    const long nsym = (long)std::floor((double)(n - first) / nsps);
    size_t ci = 0;
    for (long s = 0; s < nsym; s++) {
        int tone;
        if (s % 3 == 0) tone = -1;
        else { tone = jt6mCharIndex(m[ci % m.size()]); ci++; }
        const size_t a = first + (size_t)std::llround((double)s * nsps), b = std::min(n, first + (size_t)std::llround((double)(s + 1) * nsps));
        const double t = (double)(a - first) / fs;
        const double f = jt6mToneHz(tone) + offsetHz + driftHzPerSec * t;
        const double w = 2 * kPi * f / fs;
        // WSJT starts every tone at phase 0 (gentone.f)
        for (size_t i = a; i < b; i++) y[i] = (float)(amp * std::sin(w * (double)(i - a)));
    }
    return y;
}

// Runs of frames with a clear sync (a gap of up to 2 frames is bridged); a ping needs 3 good frames, or 2 with a strong sync.
std::vector<Jt6mPing> jt6mGroupPings(const std::vector<Jt6mFrame>& fr, double frameSec) {
    std::vector<Jt6mPing> pings;
    size_t i = 0;
    while (i < fr.size()) {
        if (!fr[i].good) { i++; continue; }
        size_t j = i, last = i;
        int good = 0;
        double rsum = 0;
        while (j < fr.size() && j <= last + 3 && fr[j].startSec - fr[last].startSec < 3.5 * frameSec) {
            if (fr[j].good) { last = j; good++; rsum += fr[j].sync; }
            j++;
        }
        if (good >= 2 && (good >= 3 || rsum / good >= 10.0)) {
            Jt6mPing p;
            p.startSec = fr[i].startSec;
            p.durSec = fr[last].startSec - fr[i].startSec + frameSec;
            p.frames = good;
            p.snrDb = 10 * std::log10(std::max(1e-6, rsum / good - 1.0));
            for (size_t k = i; k <= last; k++)
                for (int q = 0; q < 2; q++) {
                    const bool clear = fr[k].good || fr[k].conf[q] >= 2.0;
                    p.text += clear && fr[k].c[q] >= 0 ? kSet[fr[k].c[q]] : '_';
                }
            pings.push_back(p);
        }
        i = last + 1;
    }
    return pings;
}

bool jt6mDecodeBlock(const float* x, size_t n, double fs, Jt6mBlock& out, double tolHz) {
    out = Jt6mBlock();
    const double nsps = fs * 512.0 / 11025.0;
    const double hop = nsps / 4.0;
    const size_t wlen = (size_t)std::llround(nsps);
    if (n < (size_t)(9 * nsps) + wlen) return false;
    const size_t N = nextPow2(wlen) * 4;
    const double binHz = fs / (double)N;
    const double fineHz = kDf / 4.0;
    const int sMax = (int)std::ceil(2 * tolHz / fineHz);
    const int G = sMax + 4 * (kTones - 1) + 1;
    const double gridLo = kSyncHz - tolHz;
    const size_t nh = (size_t)((double)(n - wlen) / hop);
    std::vector<float> E(nh * (size_t)G);
    std::vector<cd> a(N);
    std::vector<double> win(wlen);
    for (size_t i = 0; i < wlen; i++) win[i] = 0.5 - 0.5 * std::cos(2 * kPi * ((double)i + 0.5) / (double)wlen);
    std::vector<int> bin((size_t)G);
    for (int g = 0; g < G; g++) bin[(size_t)g] = (int)std::lround((gridLo + g * fineHz) / binHz);
    for (size_t h = 0; h < nh; h++) {
        const size_t st = (size_t)std::floor((double)h * hop);
        for (size_t i = 0; i < N; i++) a[i] = i < wlen ? cd((double)x[st + i] * win[i], 0) : cd(0, 0);
        fft(a);
        for (int g = 0; g < G; g++) {
            const int b = bin[(size_t)g];
            E[h * (size_t)G + (size_t)g] = (b > 0 && (size_t)b < N / 2) ? (float)std::norm(a[(size_t)b]) : 0.f;
        }
    }
    // the noise level of every hop for every shift: the median would be best, the mean of the 44 tones without the largest 3 is close and cheap
    auto symMean = [&](size_t h, int sh) {
        const float* r = &E[h * (size_t)G + (size_t)sh];
        float t1 = 0, t2 = 0, t3 = 0;
        double s = 0;
        for (int k = 0; k < kTones; k++) {
            const float v = r[4 * k];
            s += v;
            if (v > t1) { t3 = t2; t2 = t1; t1 = v; } else if (v > t2) { t3 = t2; t2 = v; } else if (v > t3) t3 = v;
        }
        return (s - t1 - t2 - t3) / (double)(kTones - 3) + 1e-30;
    };
    double gFloor = 0;
    {   // a floor under the noise level: quiet stretches of a noise-free signal must not give huge ratios
        std::vector<float> t;
        for (size_t h = 0; h < nh; h += 7) t.push_back((float)symMean(h, sMax / 2));
        std::nth_element(t.begin(), t.begin() + (long)t.size() / 2, t.end());
        gFloor = 0.02 * t[t.size() / 2] + 1e-30;
    }
    std::vector<double> mean((size_t)nh * (size_t)(sMax + 1));
    for (size_t h = 0; h < nh; h++)
        for (int sh = 0; sh <= sMax; sh++) mean[h * (size_t)(sMax + 1) + (size_t)sh] = symMean(h, sh) + gFloor;
    auto ratio = [&](size_t h, int sh, int tone) { return (double)E[h * (size_t)G + (size_t)(sh + 4 * tone)] / mean[h * (size_t)(sMax + 1) + (size_t)sh]; };

    // search: symbol phase (quarter symbols), shift, frame phase (which of three symbols is the sync)
    double bestScore = 0;
    int bestPh = 0, bestS = 0, bestFo = 0;
    for (int ph = 0; ph < 4; ph++) {
        const long nsym = ((long)nh - ph) / 4;
        if (nsym < 9) continue;
        for (int sh = 0; sh <= sMax; sh++)
            for (int fo = 0; fo < 3; fo++) {
                double sc = 0;
                for (long s = fo; s < nsym; s += 3) {
                    const size_t h = (size_t)(4 * s + ph);
                    const double r = ratio(h, sh, 0);
                    // a clear sync tone counts; noise (ratio ~1) and a tone that is there all the time (a carrier: it is in the data symbols too) do not
                    double d = std::log(std::max(1e-9, r)) - std::log(3.0);
                    if (d <= 0) continue;
                    if (s + 1 < nsym) {
                        const double rd = ratio((size_t)(4 * (s + 1) + ph), sh, 0);
                        if (rd > 0.5 * r) continue;
                    }
                    // clipped so a short ping counts as much as a long weak signal; the small unclipped part breaks the tie between the true shift and
                    // the next tone, whose window leakage (-6 dB) still clips on a strong signal
                    sc += std::min(d, 4.0) + 0.02 * d;
                }
                if (sc > bestScore) { bestScore = sc; bestPh = ph; bestS = sh; bestFo = fo; }
            }
    }
    if (std::getenv("HS_DEBUG")) std::fprintf(stderr, "jt6m: score %.2f ph %d sh %d fo %d\n", bestScore, bestPh, bestS, bestFo);
    if (bestScore < 4.0) return false;
    const long nsym = ((long)nh - bestPh) / 4;
    // frames: sync, then the two characters
    std::vector<Jt6mFrame> fr;
    for (long s = bestFo; s + 2 < nsym; s += 3) {
        Jt6mFrame f;
        f.startSec = ((double)bestPh * hop + (double)s * nsps) / fs;
        f.sync = ratio((size_t)(4 * s + bestPh), bestS, 0);
        for (int j = 0; j < 2; j++) {
            const size_t h = (size_t)(4 * (s + 1 + j) + bestPh);
            int bk = -1;
            double mx = 0, second = 0;
            for (int k = 0; k < kTones; k++) {
                const double v = ratio(h, bestS, k);
                if (v > mx) { second = mx; mx = v; bk = k; } else if (v > second) second = v;
            }
            f.c[j] = bk - 1;   // -1 = the sync tone in a data slot
            f.conf[j] = mx / std::max(1e-9, second);
        }
        const double rd = ratio((size_t)(4 * (s + 1) + bestPh), bestS, 0);
        f.good = f.sync >= 5.0 && rd < 0.5 * f.sync && f.c[0] >= 0 && f.c[1] >= 0;
        fr.push_back(f);
    }
    const double frameSec = 3.0 * nsps / fs;
    std::vector<Jt6mPing> pings = jt6mGroupPings(fr, frameSec);
    if (pings.empty()) return false;
    // averaged message: the period (even number of characters 2 .. 60) over all the frames with a clear sync
    {
        size_t ngood = 0;
        for (const auto& f : fr) ngood += f.good;
        int bestP = 0;
        double bestAgree = 0;
        std::string bestText;
        if (ngood >= 6) {
            for (int P = 2; P <= 60; P += 2) {
                std::vector<double> fold((size_t)P * kData, 0.0);
                std::vector<int> cnt((size_t)P, 0);
                for (size_t fi = 0; fi < fr.size(); fi++) {
                    if (!fr[fi].good) continue;
                    const long s = bestFo + 3 * (long)fi;
                    for (int q = 0; q < 2; q++) {
                        const int pos = (int)((2 * (long)fi + q) % P);
                        const size_t h = (size_t)(4 * (s + 1 + q) + bestPh);
                        for (int k = 1; k < kTones; k++) fold[(size_t)pos * kData + (size_t)(k - 1)] += std::max(0.0, ratio(h, bestS, k) - 1.0);   // the noise level taken out
                        cnt[(size_t)pos]++;
                    }
                }
                double agree = 0;
                int filled = 0;
                std::string t;
                for (int pos = 0; pos < P; pos++) {
                    if (cnt[(size_t)pos] < 2) { t += '_'; continue; }
                    const double* v = &fold[(size_t)pos * kData];
                    int bk = 0;
                    double mx = 0, sum = 0;
                    for (int k = 0; k < kData; k++) { sum += v[k]; if (v[k] > mx) { mx = v[k]; bk = k; } }
                    // how much of the folded energy the winning tone holds: one for a true period, ~1 / repeats for a wrong one
                    agree += mx / std::max(1e-9, sum);
                    filled++;
                    t += kSet[bk];
                }
                if (filled < P / 2) continue;
                const double score = agree / P;
                if (score > bestAgree * 1.02) { bestAgree = score; bestP = P; bestText = t; }
            }
        }
        if (bestP > 0 && bestAgree > 0.35) { out.period = bestP; out.averaged = bestText; }
    }
    out.pings = pings;
    out.freqOffsetHz = gridLo + bestS * fineHz - kSyncHz;
    out.score = bestScore;
    double ss = 0;
    int ng = 0;
    for (const auto& f : fr) if (f.good) { ss += f.sync; ng++; }
    out.frames = fr;
    out.goodFrames = ng;
    out.snrDb = ng ? 10 * std::log10(std::max(1e-6, ss / ng - 1.0)) : 0;
    return true;
}

// ------------------------------------------------------------------------------------------------------------------ streaming
// Windows of 6 s every 3 s; a window hands over the frames that start in its middle 3 s (the first window from 0, a flush to the end), so every
// frame is used once. The frames are grouped into pings as they come; a ping closes when no clear frame has come for 3 frames.
Jt6mDecoder::Jt6mDecoder(double fs) : fs_(fs) { reset(); }
void Jt6mDecoder::reset() { buf_.clear(); base_ = total_ = 0; frameTo_ = -1; level_ = -120; out_.clear(); pend_.clear(); last_ = Jt6mBlock(); nPings_ = 0; }
std::vector<Jt6mPing> Jt6mDecoder::take() { std::vector<Jt6mPing> r; r.swap(out_); return r; }

void Jt6mDecoder::closePings(bool all) {
    const double frameSec = 3.0 * 512.0 / 11025.0;
    // the pending frames end with a clear one more than 3 frames ago (or a flush): group them
    size_t lastGood = pend_.size();
    for (size_t k = pend_.size(); k-- > 0;) if (pend_[k].good) { lastGood = k; break; }
    if (lastGood == pend_.size()) { if (pend_.size() > 8) pend_.erase(pend_.begin(), pend_.end() - 4); return; }
    // a long continuous signal is shown every 10 s
    if (!all && pend_.back().startSec - pend_[lastGood].startSec < 3.5 * frameSec && pend_.back().startSec - pend_.front().startSec < 10.0) return;
    for (auto& p : jt6mGroupPings(pend_, frameSec)) { p.freqOffsetHz = offset_; out_.push_back(p); nPings_++; }
    pend_.clear();
}

void Jt6mDecoder::analyse(bool flush) {
    const size_t win = (size_t)(6 * fs_), step = (size_t)(3 * fs_);
    while (buf_.size() >= win || (flush && !buf_.empty())) {
        const size_t n = std::min(win, buf_.size());
        const bool final = flush && buf_.size() <= win;
        const double t0 = (double)base_ / fs_;
        const double lo = base_ == 0 ? 0.0 : 1.5, hi = final ? 1e9 : 4.5;
        Jt6mBlock b;
        const double frameSec = 3.0 * 512.0 / 11025.0;
        if (n > (size_t)(0.7 * fs_) && jt6mDecodeBlock(buf_.data(), n, fs_, b)) {
            offset_ = b.freqOffsetHz;
            for (auto f : b.frames) {
                if (f.startSec < lo || f.startSec >= hi) continue;
                f.startSec += t0;
                if (f.startSec < frameTo_) continue;
                frameTo_ = f.startSec + 0.5 * frameSec;
                pend_.push_back(f);
                closePings(false);
            }
            if (!b.averaged.empty()) last_ = b;
        } else {
            closePings(true);
        }
        if (final) { base_ += buf_.size(); buf_.clear(); break; }
        buf_.erase(buf_.begin(), buf_.begin() + (long)step);
        base_ += step;
    }
    if (flush) closePings(true);
}

void Jt6mDecoder::feed(const float* x, size_t n) {
    buf_.insert(buf_.end(), x, x + n);
    total_ += n;
    double e = 0;
    for (size_t i = 0; i < n; i++) e += (double)x[i] * x[i];
    level_ = 10 * std::log10(std::max(1e-12, e / (double)std::max<size_t>(1, n)));
    analyse(false);
}
void Jt6mDecoder::flush() { analyse(true); }

}  // namespace hamsci
}  // namespace dect2
