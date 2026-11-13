// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Shared by the fist benchmark and its test: a hand-sent text through the 1 kHz channel chain (the receiver's 65-tap +-50 Hz low-pass, the
// magnitude, then cw::Chain), and the character error rate against the sent text.
#pragma once
#include "dect2/cw/cw_chain.h"
#include "dect2/cw/cw_fist.h"
#include <algorithm>
#include <cmath>
#include <complex>
#include <random>
#include <string>
#include <vector>

namespace fh {
namespace cw = dect2::cw;

inline std::vector<float> lowpass1k() {   // as cw_rx.cpp
    const int kFir = 65;
    std::vector<float> h((size_t)kFir);
    const double fc = 50.0 / 1000.0, kPi = 3.14159265358979323846;
    double s = 0;
    for (int i = 0; i < kFir; i++) {
        const double m = i - (kFir - 1) / 2.0;
        const double sinc = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
        const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (kFir - 1)) + 0.08 * std::cos(4 * kPi * i / (kFir - 1));
        h[(size_t)i] = (float)(sinc * w);
        s += sinc * w;
    }
    for (float& v : h) v = (float)(v / s);
    return h;
}

// semi-global edit distance: the whole reference must be matched, the decoded text may have extra at both ends (the start-up)
inline int editDistance(const std::string& ref, const std::string& dec) {
    const size_t m = ref.size(), n = dec.size();
    std::vector<int> prev(n + 1, 0), cur(n + 1, 0);
    for (size_t i = 1; i <= m; i++) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= n; j++) {
            const int sub = prev[j - 1] + (ref[i - 1] == dec[j - 1] ? 0 : 1);
            cur[j] = std::min({sub, prev[j] + 1, cur[j - 1] + 1});
        }
        std::swap(prev, cur);
    }
    int best = prev[0];
    for (size_t j = 1; j <= n; j++) best = std::min(best, prev[j]);
    return best;
}

inline std::string norm(const std::string& s) {
    std::string o;
    for (char c : s) { if (c == ' ' && (o.empty() || o.back() == ' ')) continue; o += c; }
    while (!o.empty() && o.back() == ' ') o.pop_back();
    return o;
}

struct Result {
    int errs = 0, len = 0;
    double wpm = 0, ratio = 0;
    std::string ref, dec, conf;
    cw::FistStats st;
    double cer() const { return len ? (double)errs / len : 0; }
};

struct Opt {
    cw::FistRender r;
    bool adaptive = true;
    bool freeText = false;
    int chars = 260;
    double wpm = 0;               // 0: the profile's
    double wpm2 = 0;              // > 0: the second half of the text is sent at this speed (an abrupt change)
    double manualWpm = 0;
    std::string text;             // else made from the seed
    bool qrm = false;
};

// noise only (no key): the characters decoded in `sec` seconds
inline int noiseChars(bool adaptive, double sec, uint32_t seed, double* markSnr = nullptr) {
    cw::FistRender r; r.seed = seed; r.leadSec = sec; r.tailSec = 0;
    const std::vector<std::complex<float>> x = cw::fistRender({}, r);
    const std::vector<float> lp = lowpass1k();
    std::vector<std::complex<float>> fir(2 * lp.size());
    size_t pos = 0;
    cw::Chain ch; ch.setAdaptive(adaptive);
    std::string text, conf, alt;
    for (size_t i = 0; i < x.size(); i++) {
        fir[pos] = x[i]; fir[pos + lp.size()] = x[i];
        if (++pos >= lp.size()) pos = 0;
        std::complex<float> y(0, 0);
        const std::complex<float>* h = &fir[pos];
        for (size_t j = 0; j < lp.size(); j++) y += h[j] * lp[j];
        ch.sample(std::abs(y));
        ch.take(text, conf, alt);
    }
    int n = 0;
    for (char c : text) if (c != ' ') n++;
    return n;
}

inline Result run(const cw::FistProfile& p, const Opt& o, uint32_t seed) {
    std::mt19937 g(seed * 7919u + 17u);
    const std::string preamble = "VVV VVV CQ CQ DE ";
    const std::string body = o.text.empty() ? cw::fistText(g, o.chars, o.freeText) : o.text;
    std::vector<double> dur;
    if (o.wpm2 > 0) {
        size_t cut = body.find(' ', body.size() / 2);
        if (cut == std::string::npos) cut = body.size() / 2;
        dur = cw::fistKeying(preamble + body.substr(0, cut), p, g, o.wpm);
        const std::vector<double> d2 = cw::fistKeying(body.substr(cut), p, g, o.wpm2);
        dur.insert(dur.end(), d2.begin(), d2.end());
    } else dur = cw::fistKeying(preamble + body, p, g, o.wpm);
    cw::FistRender r = o.r;
    r.seed = seed;
    std::vector<double> qdur;
    if (o.qrm) { std::mt19937 g2(seed + 99); qdur = cw::fistKeying(cw::fistText(g2, 300), *cw::fistProfile("straight"), g2, 0); }
    const std::vector<std::complex<float>> x = cw::fistRender(dur, r, qdur);
    const std::vector<float> lp = lowpass1k();
    std::vector<std::complex<float>> fir(2 * lp.size());
    size_t pos = 0;
    cw::Chain ch;
    ch.setAdaptive(o.adaptive);
    ch.setManualWpm(o.manualWpm);
    std::string text, conf, alt;
    for (size_t i = 0; i < x.size(); i++) {
        fir[pos] = x[i]; fir[pos + lp.size()] = x[i];
        if (++pos >= lp.size()) pos = 0;
        std::complex<float> y(0, 0);
        const std::complex<float>* h = &fir[pos];
        for (size_t j = 0; j < lp.size(); j++) y += h[j] * lp[j];
        ch.sample(std::abs(y));
        ch.take(text, conf, alt);
    }
    Result res;
    res.ref = norm(body);
    res.dec = norm(text);
    res.conf = conf;
    res.errs = editDistance(res.ref, res.dec);
    res.len = (int)res.ref.size();
    res.wpm = ch.wpm();
    cw::FistStats st; ch.stats(st);
    res.ratio = st.dashRatio;
    res.st = st;
    return res;
}

} // namespace fh
