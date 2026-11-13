// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// FSK441 generator and receiver. Tones 882 / 1323 / 1764 / 2205 Hz (multiples 2..5 of 441 Hz), 441 baud, three tones per character: the 48-entry
// character set of WSJT / MSHV as a base 4 number of three digits (the first digit is 0, 1 or 2, so the tone 2205 Hz is never the first of a triple, which
// lets the receiver find the triple boundary). The receiver finds bursts above the noise, the tuning error, the dit timing and the triple phase itself.
#include "dect2/hamsci/hamsci_dsp.h"
#include "hamsci_int.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dect2 {
namespace hamsci {
using namespace detail;

namespace {
constexpr double kBaud = 441.0;
// index = character code of the transmit table (ASCII), value = the 6 bit code (MSHV config_msg_all.h, lookup_FSK441_TXRX)
const int kTx[92] = {13, 15, 17, 46, 47, 45, 44, 12, 11, 14, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 16, 48, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 15, 47, 30, 14, 16, 42, 46, 35, 36, 37, 21, 0, 11, 41, 10, 13, 43, 1,
                     2, 3, 4, 5, 6, 7, 8, 9, 49, 56, 52, 55, 54, 12, 63, 17, 18, 19, 20, 44, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 45, 63};
const char kRx[48] = {' ', '1', '2', '3', '4', '5', '6', '7', '8', '9', '.', ',', '?', '/', '#', ' ', '$', 'A', 'B', 'C', 'D', ' ', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
                      'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', ' ', '0', 'E', 'Z', '*', '!'};

int txCode(char c) {
    int j = (unsigned char)std::toupper((unsigned char)c);
    if (j < 0 || j > 91) j = 32;
    return kTx[j];
}

struct Tones4 { float e[4]; };

// energies of the four tones in every dit of x (start t0 samples, dit length fs/441), tones at (k + 2) * 441 + df
void ditEnergies(const float* x, size_t n, double fs, double df, double t0, std::vector<Tones4>& out) {
    const double dit = fs / kBaud;
    out.clear();
    for (double s = t0; s + dit <= (double)n; s += dit) {
        const size_t a = (size_t)std::floor(s), b = (size_t)std::floor(s + dit);
        Tones4 t;
        for (int k = 0; k < 4; k++) {
            const double w = 2 * kPi * ((k + 2) * kBaud + df) / fs;
            const std::complex<double> r(std::cos(w), -std::sin(w));
            std::complex<double> z(0, 0), p(std::cos(w * (double)a), -std::sin(w * (double)a));
            for (size_t i = a; i < b; i++) { z += (double)x[i] * p; p *= r; }
            t.e[k] = (float)std::norm(z);
        }
        out.push_back(t);
    }
}

double metric(const std::vector<Tones4>& d) {
    double m = 0;
    for (const Tones4& t : d) m += std::max(std::max(t.e[0], t.e[1]), std::max(t.e[2], t.e[3]));
    return m;
}
}  // namespace

std::vector<float> fsk441Generate(const std::string& msg, double fs, int repeats, double offsetHz, double amp) {
    std::vector<int> tones;
    std::string m = msg + "  ";
    for (int r = 0; r < std::max(1, repeats); r++)
        for (char c : m) {
            const int v = txCode(c);
            tones.push_back(v / 16); tones.push_back((v / 4) % 4); tones.push_back(v % 4);
        }
    const double dit = fs / kBaud;
    const size_t n = (size_t)std::ceil((double)tones.size() * dit);
    std::vector<float> y(n);
    double ph = 0;
    for (size_t i = 0; i < n; i++) {
        size_t d = (size_t)((double)i / dit);
        if (d >= tones.size()) d = tones.size() - 1;
        ph += 2 * kPi * ((tones[d] + 2) * kBaud + offsetHz) / fs;
        y[i] = (float)(amp * std::sin(ph));
    }
    return y;
}

bool fsk441DecodeBlock(const float* x, size_t n, double fs, Fsk441Burst& out, double tolHz) {
    const double dit = fs / kBaud;
    if (n < (size_t)(12 * dit)) return false;
    // the part of the block that carries the energy: at most 1.2 s for the search
    const size_t searchN = std::min(n, (size_t)(1.2 * fs));
    size_t a0 = 0;
    if (n > searchN) {
        const size_t step = (size_t)(0.05 * fs);
        double best = -1;
        for (size_t s = 0; s + searchN <= n; s += step) {
            double e = 0;
            for (size_t i = s; i < s + searchN; i += 4) e += (double)x[i] * x[i];
            if (e > best) { best = e; a0 = s; }
        }
    }
    std::vector<Tones4> d;
    double bestM = -1, bestDf = 0, bestT = 0;
    for (double df = -tolHz; df <= tolHz + 1e-9; df += 20.0)
        for (int ph = 0; ph < 4; ph++) {
            ditEnergies(x + a0, searchN, fs, df, ph * dit / 4, d);
            const double m = metric(d);
            if (m > bestM) { bestM = m; bestDf = df; bestT = ph * dit / 4; }
        }
    const double df0 = bestDf, t00 = bestT;
    for (double df = df0 - 15; df <= df0 + 15.01; df += 5.0)
        for (int ph = -3; ph <= 4; ph++) {
            double t0 = t00 + ph * dit / 16;
            if (t0 < 0) t0 += dit;
            if (t0 >= dit) t0 -= dit;
            ditEnergies(x + a0, searchN, fs, df, t0, d);
            const double m = metric(d);
            if (m > bestM) { bestM = m; bestDf = df; bestT = t0; }
        }
    // the timing found is relative to a0: bring it to the start of the block
    double tStart = std::fmod(bestT - (double)a0, dit);
    if (tStart < 0) tStart += dit;
    ditEnergies(x, n, fs, bestDf, tStart, d);
    // leading and trailing dits that carry almost no energy (the quiet part around a short burst) are cut off
    {
        double peak = 0;
        std::vector<double> tot(d.size());
        for (size_t i = 0; i < d.size(); i++) { tot[i] = d[i].e[0] + d[i].e[1] + d[i].e[2] + d[i].e[3]; peak = std::max(peak, tot[i]); }
        std::vector<double> srt(tot);
        std::sort(srt.begin(), srt.end());
        const double med = srt[srt.size() / 2];
        if (peak > 20 * med) {   // a clear burst against the rest of the block
            size_t a = 0, b = d.size();
            while (a < b && tot[a] < 0.3 * peak) a++;
            while (b > a && tot[b - 1] < 0.3 * peak) b--;
            d.assign(d.begin() + (long)a, d.begin() + (long)b);
        }
    }
    const int nd = (int)d.size();
    if (nd < 12) return false;
    std::vector<int> tone((size_t)nd);
    double sumMax = 0, sumRest = 0;
    int n4[3] = {0, 0, 0};
    for (int i = 0; i < nd; i++) {
        int k = 0;
        float mx = d[(size_t)i].e[0], sum = mx;
        for (int j = 1; j < 4; j++) { sum += d[(size_t)i].e[j]; if (d[(size_t)i].e[j] > mx) { mx = d[(size_t)i].e[j]; k = j; } }
        tone[(size_t)i] = k;
        sumMax += mx; sumRest += (sum - mx) / 3.0;
        if (k == 3) n4[i % 3]++;
    }
    const int n4min = std::min(n4[0], std::min(n4[1], n4[2]));
    const double n4mean = (n4[0] + n4[1] + n4[2]) / 3.0;
    const double contrast = 10 * std::log10(std::max(1e-12, sumMax / std::max(1e-12, sumRest)));
    if (std::getenv("HS_DEBUG")) std::fprintf(stderr, "fsk441: nd %d n4 %d %d %d contrast %.1f df %.0f\n", nd, n4[0], n4[1], n4[2], contrast, bestDf);
    if (n4mean < 2.0 || n4min > 0.4 * n4mean || contrast < 4.5) return false;   // no phase without a fourth tone: not FSK441 (noise, a carrier, another mode)
    int js = 0;
    for (int j = 0; j < 3; j++) if (n4[j] == n4min) { js = j; break; }
    // the phase of the triples is the one whose triple STARTS at i with no tone 3: n4[i % 3] counts tone 3 at positions i == k mod 3
    std::string text;
    int bad = 0, chars = 0;
    for (int i = js; i + 2 < nd; i += 3) {
        const int nc = 16 * tone[(size_t)i] + 4 * tone[(size_t)i + 1] + tone[(size_t)i + 2];
        chars++;
        if (nc <= 47) text += kRx[nc]; else { text += ' '; bad++; }
    }
    if (chars < 4 || bad * 4 > chars) return false;
    // trim and collapse
    std::string t;
    for (char c : text) { if (c == ' ' && (t.empty() || t.back() == ' ')) continue; t += c; }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    if (t.size() < 2) return false;
    out.text = t;
    out.freqOffsetHz = bestDf;
    out.durSec = (double)n / fs;
    out.snrDb = 10 * std::log10(std::max(1e-12, sumMax / std::max(1e-12, sumRest)));
    return true;
}

// ------------------------------------------------------------------------------------------------------------------ streaming
namespace { constexpr size_t kFrame = 256; }

Fsk441Decoder::Fsk441Decoder(double fs) : fs_(fs) { reset(); }
void Fsk441Decoder::reset() { buf_.clear(); base_ = total_ = 0; noise_.clear(); run_ = Run(); level_ = -120; nBursts_ = 0; out_.clear(); }
std::vector<Fsk441Burst> Fsk441Decoder::take() { std::vector<Fsk441Burst> r; r.swap(out_); return r; }

void Fsk441Decoder::feed(const float* x, size_t n) {
    buf_.insert(buf_.end(), x, x + n);
    total_ += n;
    process(false);
}

void Fsk441Decoder::process(bool) {
    // frames of fs/47 samples; band power 700 .. 2500 Hz
    const size_t fr = std::max<size_t>(64, (size_t)(fs_ * 0.02133));
    std::vector<cd> a;
    Run* r = &run_;
    size_t nfft = nextPow2(fr);
    a.assign(nfft, cd(0, 0));
    const size_t lo = (size_t)std::floor(700.0 * (double)nfft / fs_), hi = (size_t)std::ceil(2500.0 * (double)nfft / fs_);
    auto padLen = (size_t)(0.03 * fs_);
    while (r->scan + fr <= buf_.size()) {
        for (size_t i = 0; i < nfft; i++) a[i] = i < fr ? cd((double)buf_[r->scan + i] * (0.5 - 0.5 * std::cos(2 * kPi * (double)i / (double)fr)), 0) : cd(0, 0);
        fft(a);
        double p = 0;
        for (size_t k = lo; k <= hi && k < nfft / 2; k++) p += std::norm(a[k]);
        p = std::max(p, 1e-20);
        // noise: median of the quiet frames
        double med = 0;
        if (noise_.size() >= 24) { std::vector<float> t(noise_); std::nth_element(t.begin(), t.begin() + (long)t.size() / 2, t.end()); med = t[t.size() / 2]; }
        const bool warm = noise_.size() < 24;
        const bool hot = !warm && p > med * std::pow(10.0, thr_ / 10.0);
        if (!hot) { noise_.push_back((float)p); if (noise_.size() > 600) noise_.erase(noise_.begin(), noise_.begin() + 100); }
        if (!warm) level_ = 10 * std::log10(std::max(1e-20, p / std::max(1e-20, med)));
        if (hot) {
            if (!r->on) { r->on = true; r->start = r->scan; }
            r->below = 0;
        } else if (r->on) {
            r->below++;
        }
        const size_t now = r->scan + fr;
        if (r->on && (r->below >= 4 || now - r->start > (size_t)(2.0 * fs_))) {
            const size_t endS = r->below >= 4 ? now - (size_t)r->below * fr : now;
            const size_t s0 = r->start > padLen ? r->start - padLen : 0, e0 = std::min(buf_.size(), endS + padLen);
            if (e0 > s0) {
                Fsk441Burst b;
                if (fsk441DecodeBlock(buf_.data() + s0, e0 - s0, fs_, b)) {
                    b.startSec = (double)(base_ + s0) / fs_;
                    out_.push_back(b);
                    nBursts_++;
                }
            }
            if (r->below >= 4) r->on = false; else r->start = now;
        }
        r->scan += fr;
    }
    // drop what is no longer needed
    size_t keep = r->on ? r->start : r->scan;
    keep = keep > padLen ? keep - padLen : 0;
    keep = std::min(keep, r->scan);
    if (keep > (size_t)(4 * fs_)) {
        buf_.erase(buf_.begin(), buf_.begin() + (long)keep);
        base_ += keep;
        r->scan -= keep;
        if (r->on) r->start -= keep;
    }
}

}  // namespace hamsci
}  // namespace dect2
