// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// QRSS / DFCW grabber. The audio is mixed to the chosen centre (found automatically from the strongest steady narrow tone, or set by hand) and brought to
// 100 Hz complex. A spectrogram with a window of about half a dot (frequency resolution 2 / dot) is cut from it; the strongest bin inside the window is
// followed as the key-down level (and, for DFCW, as the frequency that tells a dot from a dash). The element length comes from the data: every length
// from 1 to 120 s is tried and the one under which the key-down and key-up times fall on whole multiples (QRSS: 1 and 3, 1 / 3 / 7; DFCW: whole
// elements) with the smallest error wins. Then the runs are turned into Morse and read.
#include "dect2/hamsci/hamsci_dsp.h"
#include "dect2/cw/cw_morse.h"
#include "hamsci_int.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>

namespace dect2 {
namespace hamsci {
using namespace detail;

namespace {
constexpr double kDecRate = 100.0;
const double kDots[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 15, 20, 30, 45, 60, 90, 120};

struct Track {
    double hopSec = 0;
    std::vector<float> level;       // peak power over the noise, dB
    std::vector<float> freq;        // peak frequency, Hz from the centre
    std::vector<std::vector<float>> rows;
    double binHz = 0;
};

Track makeTrack(const std::vector<std::complex<float>>& y, double rate, double dotSec, double spanHz, bool wantRows) {
    Track t;
    const size_t W = std::max<size_t>(32, (size_t)std::llround(dotSec * 0.5 * rate));
    const size_t N = nextPow2(W * 2);
    const size_t hop = std::max<size_t>(1, W / 4);
    t.hopSec = (double)hop / rate;
    t.binHz = rate / (double)N;
    const int half = std::max(2, (int)std::floor(spanHz / 2 / t.binHz));
    std::vector<cd> a(N);
    std::vector<double> win(W);
    for (size_t i = 0; i < W; i++) win[i] = 0.5 - 0.5 * std::cos(2 * kPi * ((double)i + 0.5) / (double)W);
    std::vector<std::vector<float>> P;
    for (size_t s = 0; s + W <= y.size(); s += hop) {
        for (size_t i = 0; i < N; i++) a[i] = i < W ? cd((double)y[s + i].real(), (double)y[s + i].imag()) * win[i] : cd(0, 0);
        fft(a);
        std::vector<float> row((size_t)(2 * half + 1));
        for (int k = -half; k <= half; k++) row[(size_t)(k + half)] = (float)std::norm(a[(size_t)((k + (int)N) % (int)N)]);
        P.push_back(std::move(row));
    }
    if (P.empty()) return t;
    std::vector<float> all;
    for (auto& r : P) for (size_t k = 0; k < r.size(); k += 2) all.push_back(r[k]);
    std::nth_element(all.begin(), all.begin() + (long)all.size() / 2, all.end());
    const double noise = std::max(1e-20, (double)all[all.size() / 2] / 0.69);
    for (auto& r : P) {
        size_t bk = 0;
        for (size_t k = 1; k < r.size(); k++) if (r[k] > r[bk]) bk = k;
        double f = (double)((int)bk - half);
        if (bk > 0 && bk + 1 < r.size()) {   // parabolic peak
            const double l = r[bk - 1], c = r[bk], rr = r[bk + 1];
            const double den = l - 2 * c + rr;
            if (den < 0) f += 0.5 * (l - rr) / den;
        }
        t.freq.push_back((float)(f * t.binHz));
        t.level.push_back((float)(10 * std::log10(std::max(1e-20, (double)r[bk]) / noise)));
        if (wantRows) {
            std::vector<float> row(r.size());
            for (size_t k = 0; k < r.size(); k++) row[k] = (float)(10 * std::log10(std::max(1e-20, (double)r[k]) / noise));
            t.rows.push_back(std::move(row));
        }
    }
    return t;
}

struct Run { bool on; double sec; double freq; };

// key state by a threshold halfway between the quiet floor and the keyed level of the track. DFCW: a change of the keyed frequency also ends a run
// (the cluster of the frequency is stored as -1 / +1 in Run::freq).
bool runsOf(const Track& t, std::vector<Run>& runs, double* snrOut, bool dfcw = false) {
    runs.clear();
    if (t.level.size() < 20) return false;
    std::vector<float> s(t.level);
    std::sort(s.begin(), s.end());
    const double lo = s[s.size() * 20 / 100], hi = s[s.size() * 95 / 100];
    if (hi - lo < 6.0) return false;
    if (snrOut) *snrOut = hi - lo;
    const double thr = lo + 0.5 * (hi - lo);
    const size_t nF = t.level.size();
    std::vector<int> st(nF, 0);
    if (!dfcw) {
        for (size_t i = 0; i < nF; i++) st[i] = t.level[i] > thr ? 1 : 0;
    } else {
        std::vector<double> f;
        for (size_t i = 0; i < nF; i++) if (t.level[i] > thr) f.push_back(t.freq[i]);
        if (f.size() < 8) return false;
        double a = *std::min_element(f.begin(), f.end()), b = *std::max_element(f.begin(), f.end());
        for (int it = 0; it < 12; it++) {
            double sa = 0, sb = 0; int na = 0, nb = 0;
            for (double v : f) { if (std::fabs(v - a) <= std::fabs(v - b)) { sa += v; na++; } else { sb += v; nb++; } }
            if (na) a = sa / na;
            if (nb) b = sb / nb;
        }
        const double mid = 0.5 * (a + b);
        for (size_t i = 0; i < nF; i++) st[i] = t.level[i] > thr ? (t.freq[i] > mid ? 2 : 1) : 0;
        std::vector<int> sm(st);   // 3-frame median: the frames where the window straddles two elements
        for (size_t i = 1; i + 1 < nF; i++) { int v[3] = {st[i - 1], st[i], st[i + 1]}; std::sort(v, v + 3); sm[i] = v[1]; }
        st = sm;
    }
    int cur = st[0];
    size_t start = 0;
    auto push = [&](size_t endIdx) {
        const double dur = (double)(endIdx - start) * t.hopSec;
        double fr = 0;
        if (cur != 0) {
            if (dfcw) fr = cur == 2 ? 1.0 : -1.0;
            else { for (size_t i = start; i < endIdx; i++) fr += t.freq[i]; fr /= std::max<size_t>(1, endIdx - start); }
        }
        runs.push_back({cur != 0, dur, fr});
    };
    for (size_t i = 1; i < nF; i++) if (st[i] != cur) { push(i); cur = st[i]; start = i; }
    push(nF);
    // the first and last runs are cut by the ends of the record
    if (!runs.empty()) runs.erase(runs.begin());
    if (!runs.empty()) runs.pop_back();
    return runs.size() >= 4;
}

// least squares refinement of the element length: every run length is (about) a whole multiple k of it
double refineDot(const std::vector<Run>& runs, double dot, bool dfcw, double* biasOut = nullptr) {
    // sec = k * T + sg * 0.13 * T (sg = +1 key down, -1 key up): the spectrogram window (half a dot) lengthens the marks and shortens the gaps by about
    // 0.13 dot each; the factor was measured on the generator's own signals (tests/hamsci/test_hamsci_qrss.cpp), a free fit cannot tell 6 s from 4 s
    constexpr double kBias = 0.13;
    for (int it = 0; it < 6; it++) {
        double num = 0, den = 0;
        for (const Run& r : runs) {
            const double sg = r.on ? 1.0 : -1.0;
            const double u = (r.sec - sg * kBias * dot) / dot;
            double k;
            if (dfcw) k = std::max(1.0, std::round(u));
            else if (r.on) k = u < 2.0 ? 1.0 : 3.0;
            else k = u < 2.0 ? 1.0 : u < 5.0 ? 3.0 : 7.0;
            const double w = k + sg * kBias;
            num += r.sec * w; den += w * w;
        }
        if (den > 0) dot = num / den;
    }
    if (biasOut) *biasOut = kBias * dot;
    return dot;
}

std::vector<Run> corrected(const std::vector<Run>& runs, double b) {
    std::vector<Run> c(runs);
    for (Run& r : c) r.sec = std::max(0.0, r.sec - (r.on ? b : -b));
    return c;
}

// error of the run lengths against whole multiples of the dot
double fitError(const std::vector<Run>& runs, double dot, bool dfcw) {
    double e = 0;
    int n = 0;
    if (dfcw && !runs.empty()) {   // every length is a whole number of a small enough element: the shortest runs must be one element long
        std::vector<double> d;
        for (const Run& r : runs) d.push_back(r.sec);
        std::sort(d.begin(), d.end());
        const double p = d[d.size() / 6];
        e += std::min(1.0, std::fabs(p / dot - 1.0)) * (double)runs.size() * 0.5;
        n += (int)(runs.size() * 0.5);
    }
    for (const Run& r : runs) {
        const double u = r.sec / dot;
        double best = 1e9;
        if (dfcw) {
            const double k = std::max(1.0, std::round(u));
            best = std::fabs(u - k) / k;
            if (u < 0.6) best = 1.0;
        } else if (r.on) {
            best = std::min(std::fabs(u - 1.0) / 1.0, std::fabs(u - 3.0) / 3.0);
        } else {
            best = std::min(std::fabs(u - 1.0), std::min(std::fabs(u - 3.0) / 3.0, std::fabs(u - 7.0) / 7.0));
        }
        e += std::min(best, 1.0);
        n++;
    }
    return n ? e / n : 1.0;
}

std::string morseText(const std::vector<Run>& runs, double dot, bool dfcw, bool rev, int* bad) {
    std::string out, code;
    int badN = 0;
    auto flushChar = [&] {
        if (code.empty()) return;
        const char* c = cw::decodeSymbol(code);
        if (c) out += c; else { out += '?'; badN++; }
        code.clear();
    };
    if (!dfcw) {
        for (const Run& r : runs) {
            const double u = r.sec / dot;
            if (r.on) code += u < 2.0 ? '.' : '-';
            else if (u >= 5.0) { flushChar(); if (!out.empty() && out.back() != ' ') out += ' '; }
            else if (u >= 2.0) flushChar();
        }
        flushChar();
    } else {
        // two clusters of frequency among the key-down runs
        std::vector<double> f;
        for (const Run& r : runs) if (r.on) f.push_back(r.freq);
        if (f.size() < 4) return "";
        double lo = *std::min_element(f.begin(), f.end()), hi = *std::max_element(f.begin(), f.end());
        for (int it = 0; it < 10; it++) {
            double sl = 0, sh = 0; int nl = 0, nh = 0;
            for (double v : f) { if (std::fabs(v - lo) <= std::fabs(v - hi)) { sl += v; nl++; } else { sh += v; nh++; } }
            if (nl) lo = sl / nl;
            if (nh) hi = sh / nh;
        }
        const double mid = 0.5 * (lo + hi);
        for (const Run& r : runs) {
            const int k = std::max(1, (int)std::lround(r.sec / dot));
            if (r.on) {
                const bool upper = r.freq > mid;
                const char sym = (upper != rev) ? '-' : '.';
                for (int i = 0; i < k; i++) code += sym;
            } else if (k >= 3) { flushChar(); if (!out.empty() && out.back() != ' ') out += ' '; }
            else flushChar();
        }
        flushChar();
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (bad) *bad = badN;
    return out;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------- offline helpers
bool qrssReadTrack(const std::vector<float>& level, const std::vector<float>& freq, double frameSec, bool dfcw, bool dfcwRev, double dotFixed, double& dotSec, std::string& text) {
    Track t;
    t.level = level; t.freq = freq; t.hopSec = frameSec;
    std::vector<Run> runs;
    if (!runsOf(t, runs, nullptr, dfcw)) return false;
    double bestDot = dotFixed, bestE = 1e9, bestB = 0;
    if (dotFixed <= 0) {
        for (double d : kDots) {
            if (d < 2.0 * frameSec) continue;
            double b = 0;
            const double t = refineDot(runs, d, dfcw, &b);
            const double e = fitError(corrected(runs, b), t, dfcw) + 0.0005 * t;
            if (e < bestE) { bestE = e; bestDot = t; bestB = b; }
        }
    }
    dotSec = bestDot;
    text = morseText(corrected(runs, bestB), bestDot, dfcw, dfcwRev, nullptr);
    return !text.empty();
}

std::vector<float> qrssGenerate(const std::string& text, double dotSec, double fs, double toneHz, bool dfcw, double shiftHz, double amp, double leadSec) {
    // key list: (on, units, freqOffset)
    struct K { bool on; double u; double df; };
    std::vector<K> keys;
    auto tokens = cw::tokenize(text);
    for (const std::string& tok : tokens) {
        if (tok == " ") { keys.push_back({false, dfcw ? 3.0 : 7.0, 0}); continue; }
        const std::string code = cw::encodeSymbol(tok);
        for (size_t i = 0; i < code.size(); i++) {
            const bool dash = code[i] == '-';
            if (dfcw) keys.push_back({true, 1.0, dash ? shiftHz : 0.0});
            else { keys.push_back({true, dash ? 3.0 : 1.0, 0}); if (i + 1 < code.size()) keys.push_back({false, 1.0, 0}); }
        }
        keys.push_back({false, dfcw ? 1.0 : 3.0, 0});
    }
    std::vector<float> y((size_t)(leadSec * fs), 0.f);
    double ph = 0;
    for (const K& k : keys) {
        const size_t n = (size_t)std::llround(k.u * dotSec * fs);
        const double w = 2 * kPi * (toneHz + k.df) / fs;
        for (size_t i = 0; i < n; i++) {
            if (k.on) { ph += w; const double u = (double)i / (double)n; const double ramp = std::min(1.0, std::min(u, 1 - u) * 20);   // 5 % ramps against clicks
                        y.push_back((float)(amp * ramp * std::sin(ph))); }
            else y.push_back(0.f);
        }
    }
    return y;
}

// ---------------------------------------------------------------------------------------------------------------------------------- grabber
QrssGrabber::QrssGrabber(double fs, bool dfcw) : fs_(fs), dfcw_(dfcw) { reset(); }

void QrssGrabber::reset() {
    in_.clear(); dec_.clear();
    lastSearchSec_ = -1e9; dotKnown_ = 0;
    phase_ = 0; decCnt_ = 0; acc_ = {0, 0};
    sampleN_ = 0; decN_ = 0; rowsDone_ = 0;
    frames_.clear();
    emitted_.clear(); textTaken_ = 0;
    st_ = QrssTelemetryCore();
    st_.centreHz = centre_;
}

std::string QrssGrabber::takeText() {
    if (st_.text.size() <= textTaken_) { if (st_.text.size() < textTaken_) textTaken_ = st_.text.size(); return ""; }
    std::string r = st_.text.substr(textTaken_);
    textTaken_ = st_.text.size();
    return r;
}

void QrssGrabber::feed(const float* x, size_t n) {
    const int D = std::max(1, (int)std::lround(fs_ / kDecRate));
    const bool haveCentre = !auto_ || st_.locked;
    for (size_t i = 0; i < n; i++) {
        sampleN_++;
        if (!haveCentre) { in_.push_back(x[i]); continue; }
        phase_ += 2 * kPi * centre_ / fs_;
        if (phase_ > 2 * kPi) phase_ -= 2 * kPi;
        if ((sampleN_ & 63) == 1) osc_ = cd(std::cos(phase_), -std::sin(phase_)), step_ = cd(std::cos(2 * kPi * centre_ / fs_), -std::sin(2 * kPi * centre_ / fs_));
        else osc_ *= step_;
        acc_ += (double)x[i] * osc_;
        if (++decCnt_ >= D) { dec_.push_back(std::complex<float>((float)(acc_.real() / D), (float)(acc_.imag() / D))); acc_ = {0, 0}; decCnt_ = 0; decN_++; }
    }
    if (!haveCentre) {
        const size_t maxN = (size_t)(60 * fs_);
        if (in_.size() > maxN) in_.erase(in_.begin(), in_.begin() + (long)(in_.size() - maxN));
        if (in_.size() >= (size_t)(30 * fs_) && sampleN_ % (size_t)(5 * fs_) < n) {
            // strongest steady narrow tone in 300 .. 3500 Hz
            const size_t N = nextPow2((size_t)(fs_ * 6.0));
            std::vector<double> P(N / 2, 0.0);
            std::vector<cd> a(N);
            int fr = 0;
            for (size_t s = 0; s + N <= in_.size(); s += N / 2, fr++) {
                for (size_t i = 0; i < N; i++) a[i] = cd((double)in_[s + i] * (0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)N)), 0);
                fft(a);
                for (size_t k = 0; k < N / 2; k++) P[k] += std::norm(a[k]);
            }
            const double binHz = fs_ / (double)N;
            const size_t kLo = (size_t)(300 / binHz), kHi = (size_t)(std::min(3500.0, fs_ / 2 - 100) / binHz);
            std::vector<double> t(P.begin() + (long)kLo, P.begin() + (long)kHi);
            std::nth_element(t.begin(), t.begin() + (long)t.size() / 2, t.end());
            const double med = t[t.size() / 2];
            size_t bk = kLo;
            for (size_t k = kLo; k < kHi; k++) if (P[k] > P[bk]) bk = k;
            if (fr >= 2 && P[bk] > 30.0 * med) {   // 15 dB over the median in the average of the last 30 .. 60 s
                centre_ = (double)bk * binHz;
                st_.locked = true;
                // replay the stored audio through the decimator
                std::vector<float> old(in_);
                in_.clear();
                sampleN_ -= old.size();
                feed(old.data(), old.size());
            }
        }
        st_.centreHz = centre_;
        return;
    }
    st_.centreHz = centre_;
    const size_t keepMax = (size_t)(3 * 3600 * kDecRate);
    if (dec_.size() > keepMax) dec_.erase(dec_.begin(), dec_.begin() + (long)(dec_.size() - keepMax));
    // analyse every 2 s of new data
    static const double kEvery = 2.0;
    if ((double)(decN_ - rowsDone_) / kDecRate >= kEvery) { rowsDone_ = decN_; analyse(); }
}

void QrssGrabber::analyse() {
    const double rate = fs_ / std::max(1, (int)std::lround(fs_ / kDecRate));
    const double dur = (double)dec_.size() / rate;
    if (dur < 20) return;
    // first pass: element length
    double dot = dotFixed_;
    std::vector<Run> runs;
    double snr = 0;
    if (dot <= 0 && dotKnown_ > 0 && dur - lastSearchSec_ < std::max(30.0, 0.25 * lastSearchSec_)) dot = dotKnown_;   // the search over all element lengths is repeated when the record has grown by a quarter
    if (dot > 0) {
        Track t = makeTrack(dec_, rate, dot, spanHz_, false);
        runsOf(t, runs, &snr, dfcw_);
    } else {
        lastSearchSec_ = dur;
        double bestE = 1e9, bestD = 0;
        for (double d : kDots) {
            if (dur < 12 * d) continue;
            Track t = makeTrack(dec_, rate, d, spanHz_, false);
            std::vector<Run> r;
            double s = 0;
            if (!runsOf(t, r, &s, dfcw_) || r.size() < 5) continue;
            double b = 0;
            const double tt = refineDot(r, d, dfcw_, &b);   // the candidate list is coarse: 5 s and 6 s both fit a short message until the smoothing bias is taken out
            const double e = fitError(corrected(r, b), tt, dfcw_) + 0.0005 * tt;
            if (std::getenv("HS_DEBUG")) { std::fprintf(stderr, "cand %.1f -> T %.2f err %.3f runs:", d, tt, e); for (auto& q : r) std::fprintf(stderr, " %c%.1f", q.on ? '#' : '_', q.sec); std::fprintf(stderr, "\n"); }
            if (e < bestE) { bestE = e; bestD = tt; snr = s; runs = r; }
        }
        if (bestD > 0 && bestE < 0.2) dot = bestD;
        dotKnown_ = dot;
    }
    st_.dotSec = dot;
    st_.snrDb = snr;
    // rows for the picture: window by the element length found (or 3 s)
    {
        const double wd = dot > 0 ? dot : 3.0;
        const size_t keep = std::min<size_t>(dec_.size(), (size_t)((double)maxRows * wd * 0.125 * rate + wd * rate));
        std::vector<std::complex<float>> tail(dec_.end() - (long)keep, dec_.end());
        Track t = makeTrack(tail, rate, wd, spanHz_, true);
        st_.rows = t.rows;
        if (st_.rows.size() > maxRows) st_.rows.erase(st_.rows.begin(), st_.rows.begin() + (long)(st_.rows.size() - maxRows));
        st_.rowSec = t.hopSec; st_.binHz = t.binHz; st_.spanHz = spanHz_;
    }
    if (dot <= 0 || runs.empty()) return;
    Track t = makeTrack(dec_, rate, dot, spanHz_, false);
    runsOf(t, runs, nullptr, dfcw_);
    if (std::getenv("HS_DEBUG")) { std::fprintf(stderr, "qrss dot %.2f runs:", dot); for (auto& r : runs) std::fprintf(stderr, " %c%.1f", r.on ? '#' : '_', r.sec); std::fprintf(stderr, "\n"); }
    {
        double b = 0;
        const double tt = refineDot(runs, dot, dfcw_, &b);
        if (dotFixed_ <= 0) { dot = tt; dotKnown_ = tt; st_.dotSec = tt; }
        runs = corrected(runs, b);
    }
    int bad = 0;
    const std::string txt = morseText(runs, dot, dfcw_, dfcwRev_, &bad);
    st_.text = txt;
}

}  // namespace hamsci
}  // namespace dect2
