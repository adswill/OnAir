// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW test signals made by human fists (see cw_fist.h).
#include "dect2/cw/cw_fist.h"
#include "dect2/common/mathconst.h"
#include "dect2/common/stable_random.h"
#include "dect2/cw/cw_morse.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace dect2 {
namespace cw {

const std::vector<FistProfile>& fistProfiles() {
    static const std::vector<FistProfile> v = [] {
        std::vector<FistProfile> p;
        FistProfile f;
        f.name = "machine"; f.wpm = 20; p.push_back(f);
        f = FistProfile(); f.name = "steady";                  // a careful straight key: 8 % spread, a little heavy
        f.wpm = 18; f.dotJit = 0.08; f.dashJit = 0.08; f.dashRatio = 3.1; f.intraJit = 0.08; f.charGapSd = 0.3; f.wordGapSd = 0.8; f.speedWalk = 0.01; p.push_back(f);
        f = FistProfile(); f.name = "straight";                // a straight key: 15 - 20 % spread, drifting speed
        f.wpm = 16; f.dotJit = 0.17; f.dashJit = 0.15; f.dashRatio = 2.9; f.intraJit = 0.17; f.charGapSd = 0.6; f.wordGapSd = 1.5; f.speedWalk = 0.03; f.speedSwing = 0.08; p.push_back(f);
        f = FistProfile(); f.name = "sloppy";                  // 25 % spread, short dashes, lazy gaps, wandering speed
        f.wpm = 13; f.dotJit = 0.25; f.dashJit = 0.22; f.dashRatio = 2.6; f.dashRatioSd = 0.3; f.intraJit = 0.25; f.charGap = 3.6; f.charGapSd = 0.9;
        f.wordGap = 9; f.wordGapSd = 2.5; f.speedWalk = 0.05; f.speedSwing = 0.12; p.push_back(f);
        f = FistProfile(); f.name = "bug";                     // a semi-automatic key: steady dots, dashes by hand
        f.wpm = 24; f.dotJit = 0.04; f.dashJit = 0.0; f.dashRatio = 3.4; f.dashRatioSd = 0.55; f.intraJit = 0.06; f.charGapSd = 0.5; f.wordGapSd = 1.2; f.speedWalk = 0.008; p.push_back(f);
        f = FistProfile(); f.name = "bug-lazy";
        f.wpm = 28; f.dotJit = 0.05; f.dashRatio = 3.9; f.dashRatioSd = 0.8; f.intraJit = 0.1; f.charGap = 3.8; f.charGapSd = 1.0; f.wordGap = 10; f.wordGapSd = 2.5; f.speedWalk = 0.012; p.push_back(f);
        f = FistProfile(); f.name = "heavy";                   // heavy weighting: marks too long, gaps too short
        f.wpm = 20; f.weight = 0.35; f.dotJit = 0.1; f.dashJit = 0.1; f.dashRatio = 3.0; f.intraJit = 0.12; f.charGapSd = 0.4; f.wordGapSd = 1; f.speedWalk = 0.015; p.push_back(f);
        f = FistProfile(); f.name = "light";
        f.wpm = 22; f.weight = -0.25; f.dotJit = 0.1; f.dashJit = 0.1; f.dashRatio = 3.0; f.intraJit = 0.12; f.charGapSd = 0.4; f.wordGapSd = 1; f.speedWalk = 0.015; p.push_back(f);
        f = FistProfile(); f.name = "paddles";                 // iambic paddles by ear: steady, swing after a dash
        f.wpm = 30; f.dotJit = 0.05; f.dashJit = 0.05; f.dashRatio = 3.0; f.swing = 0.18; f.intraJit = 0.05; f.charGapSd = 0.5; f.wordGapSd = 1.5; f.speedWalk = 0.01; p.push_back(f);
        f = FistProfile(); f.name = "slow-hand";
        f.wpm = 9; f.dotJit = 0.2; f.dashJit = 0.2; f.dashRatio = 3.0; f.intraJit = 0.2; f.charGap = 3.4; f.charGapSd = 0.8; f.wordGap = 8; f.wordGapSd = 2; f.speedWalk = 0.04; p.push_back(f);
        f = FistProfile(); f.name = "fast-hand";
        f.wpm = 40; f.dotJit = 0.13; f.dashJit = 0.12; f.dashRatio = 2.9; f.intraJit = 0.14; f.charGapSd = 0.5; f.wordGapSd = 1.5; f.speedWalk = 0.03; p.push_back(f);
        return p;
    }();
    return v;
}

const FistProfile* fistProfile(const std::string& name) {
    for (const FistProfile& p : fistProfiles()) if (name == p.name) return &p;
    return nullptr;
}

namespace {
using N = dect2::normal_distribution<double>;

int pick(std::mt19937& g, int n) { return (int)(g() % (uint32_t)n); }
template <size_t K> const char* one(std::mt19937& g, const char* const (&a)[K]) { return a[g() % K]; }

std::string callsign(std::mt19937& g) {
    // a few prefixes are not in the decoder's language model on purpose
    static const char* const pre[] = {"LY", "DL", "G", "M", "OK", "SP", "OH", "SM", "ON", "PA", "F", "I", "EA", "HB9", "OE", "YL", "ES", "UR", "RA", "UA", "JA", "VK", "ZL", "W", "K", "N", "VE", "LU", "PY", "9A", "S5", "YU", "HA", "OM", "TF", "SV", "LZ", "YO", "CT", "EI", "GM", "GW", "5B4", "A61", "TA", "4X", "ZS", "VU", "BV", "HL"};
    std::string s = pre[g() % (sizeof pre / sizeof *pre)];
    if (s.size() == 1 && (g() & 1)) s += (char)('A' + pick(g, 26)) ;      // W1AW / WA1ABC style
    if (s.size() >= 1 && std::isdigit((unsigned char)s.back())) {}        // 5B4: digit already ends the prefix
    else s += (char)('0' + pick(g, 10));
    const int n = 1 + pick(g, 3) + (pick(g, 4) == 0 ? 1 : 0);
    for (int i = 0; i < n && i < 4; i++) s += (char)('A' + pick(g, 26));
    if (pick(g, 12) == 0) s += pick(g, 2) ? "/P" : "/M";
    return s;
}

std::string rst(std::mt19937& g) {
    static const char* const r[] = {"599", "599", "5NN", "579", "559", "589", "449", "339", "569", "459"};
    return one(g, r);
}

std::string name(std::mt19937& g) {
    static const char* const n[] = {"JOHN", "TOM", "PETE", "ANDRIUS", "KRIS", "MIKE", "HANS", "JURGEN", "BOB", "ED", "STEVE", "PAUL", "ALEX", "ZIGMAS", "TONY", "FRED", "KARL", "OLE", "JAN", "VLAD", "GIEDRIUS", "NIGEL", "DAVE", "MARK", "LUCA", "ROBERTO"};
    return one(g, n);
}

std::string qth(std::mt19937& g) {
    static const char* const n[] = {"VILNIUS", "BERLIN", "LONDON", "KAUNAS", "PARIS", "HELSINKI", "ROMA", "WARSAW", "OSLO", "TALLINN", "RIGA", "WIEN", "PRAHA", "MADRID", "DUBLIN", "BREMEN", "LEEDS", "GDANSK"};
    return one(g, n);
}

std::string word(std::mt19937& g) {
    static const char* const w[] = {"UR", "RST", "QTH", "NAME", "OP", "HR", "WX", "TEMP", "RIG", "ANT", "PWR", "WATTS", "AGE", "HW", "CPY", "FB", "TNX", "TKS", "PSE", "AGN", "ES", "VY", "GL", "GE", "GM", "GA", "OM", "YL", "XYL", "SIG", "QSB", "QRM", "QRN", "QRP", "QRO", "QSL", "QSY", "QRZ", "QRL", "ABT", "CUL", "DX", "FER", "INFO", "NR", "RCVR", "FINE", "BUSINESS", "SUNNY", "COLD", "WARM", "RAIN", "HI", "SRI", "DIPOLE", "VERTICAL", "YAGI", "LOOP", "BEST", "WID", "TU", "73", "88", "DE", "BK", "CALL"};
    return one(g, w);
}
} // namespace

std::string fistText(std::mt19937& g, int chars, bool freeText) {
    std::string out;
    if (freeText) {
        static const char* const al = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        while ((int)out.size() < chars) {
            const int n = 2 + pick(g, 5);
            for (int i = 0; i < n; i++) out += al[pick(g, 36)];
            out += ' ';
        }
        while (!out.empty() && out.back() == ' ') out.pop_back();
        return out;
    }
    while ((int)out.size() < chars) {
        std::string s;
        const std::string me = callsign(g), him = callsign(g);
        switch (pick(g, 9)) {
        case 0: s = "CQ CQ CQ DE " + me + " " + me + " " + me + " <AR>"; break;
        case 1: s = "CQ TEST " + me + " " + me + " TEST"; break;
        case 2: s = him + " DE " + me + " UR " + rst(g) + " " + rst(g) + " <BT> NAME " + name(g) + " " + name(g) + " <BT> QTH " + qth(g) + " <BT> HW? <KN>"; break;
        case 3: s = me + " DE " + him + " TNX FER CALL <BT> UR RST " + rst(g) + " <BT> OP " + name(g) + " QTH " + qth(g) + " <AR>"; break;
        case 4: s = "R R " + him + " DE " + me + " FB " + name(g) + " TNX FER QSO ES 73 <SK>"; break;
        case 5: s = him + " 5NN " + std::to_string(1 + pick(g, 120)) + " TU"; break;
        case 6: s = "QRZ? DE " + me + " K"; break;
        case 7: s = word(g) + " " + word(g) + " " + word(g) + " " + name(g) + " " + word(g) + " " + word(g) + " <BT> " + word(g) + " " + std::to_string(5 + pick(g, 90)) + " " + word(g); break;
        default: s = me + " DE " + him + " GM " + name(g) + " ES TNX FER RPRT <BT> RIG " + std::string(pick(g, 2) ? "IC7300" : "FT991") + " PWR " + std::to_string(5 * (1 + pick(g, 20))) + " W ANT " + word(g) + " <BT> WX " + word(g) + " TEMP " + std::to_string(pick(g, 30)) + " C <KN>"; break;
        }
        if (!out.empty()) out += ' ';
        out += s;
    }
    return out;
}

std::vector<double> fistKeying(const std::string& text, const FistProfile& p, std::mt19937& g, double wpmOverride) {
    N nd(0, 1);
    const double wpm0 = wpmOverride > 0 ? wpmOverride : p.wpm;
    double walk = 0;
    double phase = 6.283185307 * (g() % 1000) / 1000.0;
    double t = 0;
    std::vector<double> d;
    auto unit = [&] {   // the dot at this moment, seconds
        const double w = wpm0 * std::exp(walk) * (1 + p.speedSwing * std::sin(phase + 6.283185307 * t / 20.0));
        return 1.2 / std::max(3.0, w);
    };
    auto add = [&](double s) { d.push_back(s); t += s; };
    for (const std::string& tok : tokenize(text)) {
        if (tok == " ") {
            if (d.empty()) continue;
            const double u = unit();
            double wg = p.wordGap + p.wordGapSd * nd(g) - p.weight * 0;
            wg = std::max(wg, p.charGap + 1.5);
            d.back() = std::max(0.0, wg) * u;   // replaces the character gap
            continue;
        }
        const std::string code = encodeSymbol(tok);
        if (code.empty()) continue;
        walk = 0.9 * walk + p.speedWalk * nd(g);
        const double u = unit();
        bool prevDash = false;
        for (size_t i = 0; i < code.size(); i++) {
            const bool dash = code[i] == '-';
            double m;
            if (dash) {
                m = (p.dashRatio + p.dashRatioSd * nd(g) + p.weight) * (1 + p.dashJit * nd(g));
                m = std::max(1.3, m);
            } else {
                m = (1 + p.weight) * (1 + p.dotJit * nd(g));
                if (prevDash) m *= 1 + p.swing;
                m = std::max(0.35, m);
            }
            add(m * u);
            prevDash = dash;
            if (i + 1 < code.size()) add(std::max(0.3, (p.intraGap - p.weight) * (1 + p.intraJit * nd(g))) * u);
        }
        add(std::max(1.2, p.charGap - p.weight + p.charGapSd * nd(g)) * u);
    }
    if (!d.empty()) d.back() = std::max(d.back(), 7 * 1.2 / std::max(3.0, wpm0));
    return d;
}

std::vector<std::complex<float>> fistRender(const std::vector<double>& dur, const FistRender& r, const std::vector<double>& qrmDur) {
    using dect2::math::kPi;
    double total = r.leadSec + r.tailSec;
    for (double v : dur) total += v;
    const size_t n = (size_t)(total * r.rate);
    std::mt19937 g(r.seed * 2654435761u + 12345u);
    N nd(0, 1);
    std::vector<std::complex<float>> out(n);
    // noise: sigma per component so that the carrier is snrDb over the noise in 500 Hz
    const double sigma = 1.0;
    const double amp = std::sqrt(std::pow(10.0, r.snrDb / 10) * 2 * sigma * sigma * 500.0 / r.rate);
    for (auto& v : out) v = std::complex<float>((float)(sigma * nd(g)), (float)(sigma * nd(g)));
    // key function: 0..1 with raised-cosine edges, from the durations
    auto keyTrack = [&](const std::vector<double>& dd, double lead) {
        std::vector<float> k(n, 0.f);
        std::vector<float> on(n, 0.f);
        double t = lead;
        for (size_t i = 0; i < dd.size(); i++) {
            if (i % 2 == 0) {
                const size_t a = (size_t)(t * r.rate), b = std::min(n, (size_t)((t + dd[i]) * r.rate));
                for (size_t s = a; s < b && s < n; s++) on[s] = 1.f;
            }
            t += dd[i];
        }
        // edges: a moving average of the on/off wave of length edgeMs shaped by a raised cosine (two passes of a box ~ a cosine)
        const int L = std::max(1, (int)std::lround(r.edgeMs * 1e-3 * r.rate));
        std::vector<float> w((size_t)L);
        double s = 0;
        for (int i = 0; i < L; i++) { w[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / L)); s += w[(size_t)i]; }
        for (float& x : w) x = (float)(x / s);
        for (size_t i = 0; i < n; i++) {
            double a = 0;
            for (int j = 0; j < L; j++) { const long idx = (long)i + j - L / 2; if (idx >= 0 && idx < (long)n) a += on[(size_t)idx] * w[(size_t)j]; }
            k[i] = (float)a;
        }
        return k;
    };
    const std::vector<float> key = keyTrack(dur, r.leadSec);
    std::vector<float> qk;
    if (!qrmDur.empty() && r.qrmDb > -100) qk = keyTrack(qrmDur, r.leadSec + 0.37);
    // fading: a sine, plus a random part (low-passed noise, a few seconds)
    double rf1 = 0, rf2 = 0;
    double ph = 0, qph = 0;
    double nextCrash = r.crashPerMin > 0 ? -std::log(1 - (g() % 100000) / 100000.0) * 60 / r.crashPerMin : 1e18;
    int crashLeft = 0; double crashAmp = 0;
    std::complex<float> clickState(0, 0);
    float prevKey = 0;
    const double qamp = amp * std::pow(10.0, r.qrmDb / 20);
    const double clickAmp = amp * std::pow(10.0, r.clickDb / 20);
    for (size_t i = 0; i < n; i++) {
        const double t = (double)i / r.rate;
        double fade = 1;
        if (r.qsbDepthDb > 0 && r.qsbPeriodSec > 0) {
            double x = 0.5 - 0.5 * std::cos(2 * kPi * t / r.qsbPeriodSec);
            if (r.qsbRandom > 0) {
                rf1 += 0.0006 * (nd(g) - 0 * rf1) - 0.0006 * rf1 * 0.5; rf2 += 0.002 * (rf1 * 6 - rf2);
                const double rnd = 1 / (1 + std::exp(-rf2 * 4));
                x = (1 - r.qsbRandom) * x + r.qsbRandom * rnd;
            }
            fade = std::pow(10.0, -r.qsbDepthDb / 20 * x);
        }
        ph = std::remainder(ph + 2 * kPi * (r.offsetHz + r.driftHzPerMin * t / 60) / r.rate, 2 * kPi);
        const float kv = key[i];
        std::complex<float> s(0, 0);
        if (kv > 0.f) s = std::polar((float)(amp * fade * kv), (float)ph);
        if (clickAmp > 0 && std::fabs(kv - prevKey) > 0.02f) clickState += std::polar((float)(clickAmp * 3 * std::fabs(kv - prevKey) * 10), (float)ph);
        prevKey = kv;
        clickState *= 0.6f;
        out[i] += s + clickState;
        if (!qk.empty() && qk[i] > 0.f) {
            qph = std::remainder(qph + 2 * kPi * (r.offsetHz + r.qrmOffsetHz) / r.rate, 2 * kPi);
            out[i] += std::polar((float)(qamp * qk[i]), (float)qph);
        }
        if (t >= nextCrash) {
            crashLeft = std::max(1, (int)(0.002 * r.rate)); crashAmp = 30;
            nextCrash = t - std::log(1 - (g() % 100000) / 100000.0) * 60 / r.crashPerMin;
        }
        if (crashLeft > 0) { out[i] += std::complex<float>((float)(crashAmp * sigma * nd(g)), (float)(crashAmp * sigma * nd(g))); crashLeft--; }
    }
    return out;
}

} // namespace cw
} // namespace dect2
