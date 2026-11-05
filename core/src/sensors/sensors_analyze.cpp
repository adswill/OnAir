// Unknown signal analyser, see sensors_analyze.h. The width histogram idea and the guess names follow rtl_433's pulse analyser
// (src/pulse_analyzer.c, GPL-2.0-or-later, (C) Benjamin Larsson, Christian W. Zuckschwerdt); the code is new.
#include "sensors_analyze.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "bitbuffer.h"
#include "pulse_slicer.h"
#include "r_device.h"
}

namespace dect2 { namespace sensors {

namespace {

struct Cluster { double mean = 0, lo = 0, hi = 0; int count = 0; };

// widths that are close together (within a quarter of the cluster mean plus a small slack) form a cluster
std::vector<Cluster> cluster(std::vector<double> v) {
    std::vector<Cluster> out;
    std::sort(v.begin(), v.end());
    for (double x : v) {
        if (!out.empty() && x <= out.back().mean * 1.28 + 12.0) {
            Cluster& c = out.back();
            c.mean = (c.mean * c.count + x) / (c.count + 1);
            c.hi = x; c.count++;
        } else {
            Cluster c; c.mean = c.lo = c.hi = x; c.count = 1;
            out.push_back(c);
        }
    }
    return out;
}

// a few stray widths (noise, a damaged edge) are not a symbol
std::vector<Cluster> significant(const std::vector<Cluster>& c, size_t total) {
    std::vector<Cluster> o;
    const int minCount = std::max(2, (int)(total / 30));
    for (const auto& x : c) if (x.count >= minCount) o.push_back(x);
    return o;
}

struct Capture {
    std::vector<std::string> rows;
    int longest = 0, atLongest = 0;
};

int captureFn(r_device* d, bitbuffer_t* b) {
    Capture* c = (Capture*)d->decode_ctx;
    for (int r = 0; r < b->num_rows; r++) {
        const int n = b->bits_per_row[r];
        if (n < 8) continue;
        std::string s;
        const int cap = std::min(n, 400);
        s.reserve(cap);
        for (int i = 0; i < cap; i++) s += ((b->bb[r][i / 8] >> (7 - (i % 8))) & 1) ? '1' : '0';
        c->rows.push_back(s);
        if (n > c->longest) { c->longest = n; c->atLongest = 1; }
        else if (n == c->longest) c->atLongest++;
    }
    return 0;
}

struct Cand {
    std::string mod;          // flex name, e.g. OOK_PWM
    std::string label;        // for people
    unsigned modulation = 0;
    double s = 0, l = 0, g = 0, r = 0, t = 0;
    Capture cap;
    double score = 0;
};

void tryCand(const pulse_data_t* p, Cand& c) {
    r_device d;
    std::memset(&d, 0, sizeof d);
    d.name = "analyse";
    d.modulation = c.modulation;
    d.short_width = (float)c.s; d.long_width = (float)c.l;
    d.gap_limit = (float)c.g; d.reset_limit = (float)c.r;
    d.tolerance = (float)c.t;
    d.decode_fn = captureFn;
    d.decode_ctx = &c.cap;
    switch (c.modulation) {
    case OOK_PULSE_PCM: case FSK_PULSE_PCM: pulse_slicer_pcm(p, &d); break;
    case OOK_PULSE_PPM: pulse_slicer_ppm(p, &d); break;
    case OOK_PULSE_PWM: case FSK_PULSE_PWM: pulse_slicer_pwm(p, &d); break;
    case OOK_PULSE_MANCHESTER_ZEROBIT: case FSK_PULSE_MANCHESTER_ZEROBIT: pulse_slicer_manchester_zerobit(p, &d); break;
    case OOK_PULSE_DMC: pulse_slicer_dmc(p, &d); break;
    default: break;
    }
    c.score = c.cap.longest >= 12 ? c.cap.longest + 0.3 * c.cap.atLongest : 0;
    if (c.cap.longest > 700) c.score = 0;   // one endless row: the slicer did not find a clock
}

std::string fmtUs(double v) { char b[32]; std::snprintf(b, sizeof b, "%.0f", v); return b; }

} // namespace

void analyse(const pulse_data_t* p, bool fsk, SensorUnknown& out) {
    const unsigned n = p->num_pulses;
    const double us = 1e6 / (double)p->sample_rate;
    std::vector<double> P, G;
    for (unsigned i = 0; i < n; i++) {
        P.push_back(p->pulse[i] * us);
        if (i + 1 < n) G.push_back(p->gap[i] * us);   // the last gap is only the end of the package
    }
    out.pulses = (int)n;
    double total = 0;
    for (unsigned i = 0; i < n; i++) total += (p->pulse[i] + p->gap[i]) * us;
    out.durationMs = total / 1000.0;
    for (unsigned i = 0; i < n && i < 80; i++) { out.pulseUs.push_back((float)(p->pulse[i] * us)); out.gapUs.push_back((float)(p->gap[i] * us)); }
    {
        std::string w;
        for (unsigned i = 0; i < n && i < 60; i++) {
            char b[48];
            std::snprintf(b, sizeof b, "%s%u/%u", i ? "  " : "", (unsigned)std::lround(p->pulse[i] * us), (unsigned)std::lround(p->gap[i] * us));
            w += b;
        }
        if (n > 60) w += "  ...";
        out.widths = w;
    }
    const auto pc = significant(cluster(P), P.size());
    const auto gc = significant(cluster(G), G.size());
    char gs[256];
    std::snprintf(gs, sizeof gs, "%s: %zu pulse width%s, %zu gap width%s", fsk ? "FSK" : "OOK", pc.size(), pc.size() == 1 ? "" : "s", gc.size(), gc.size() == 1 ? "" : "s");
    out.guess = gs;
    if (pc.empty() || gc.empty()) { out.guess += " (too irregular to analyse)"; return; }

    // the shortest clearly repeated width is the clock candidate
    double T = 1e9;
    for (const auto& c : pc) T = std::min(T, c.mean);
    for (const auto& c : gc) T = std::min(T, c.mean);
    T = std::max(T, 20.0);
    double maxIn = 0;   // the longest width inside a row
    for (const auto& c : pc) maxIn = std::max(maxIn, c.mean);
    for (const auto& c : gc) maxIn = std::max(maxIn, c.mean);

    std::vector<Cand> cands;
    const unsigned oPWM = fsk ? FSK_PULSE_PWM : OOK_PULSE_PWM;
    const unsigned oPCM = fsk ? FSK_PULSE_PCM : OOK_PULSE_PCM;
    const unsigned oMC = fsk ? FSK_PULSE_MANCHESTER_ZEROBIT : OOK_PULSE_MANCHESTER_ZEROBIT;
    const std::string pre = fsk ? "FSK_" : "OOK_";
    // gap limit between the symbols' gaps and the row separators
    auto gapLimit = [&](size_t symbols) {
        if (gc.size() > symbols) return (gc[symbols - 1].hi + gc[symbols].lo) / 2.0;
        return gc[symbols - 1].hi * 1.5 + 10;
    };
    if (pc.size() >= 2) {
        Cand c; c.mod = pre + "PWM"; c.label = "Pulse width modulation"; c.modulation = oPWM;
        c.s = pc[0].mean; c.l = pc[1].mean;
        c.g = gapLimit(1); c.r = std::max(c.g * 2, c.g + 1000);
        c.t = std::max(8.0, (pc[1].mean - pc[0].mean) * 0.35);
        // a PWM with a gap that also carries information is a PPM: the gap limit must stay above the widest gap of the row
        if (gc.size() >= 2 && gc.back().mean < 4 * pc.back().mean) { c.g = gc.back().hi * 1.5 + 10; c.r = c.g * 2; }
        cands.push_back(c);
    }
    if (!fsk && gc.size() >= 2) {
        Cand c; c.mod = "OOK_PPM"; c.label = "Pulse position modulation"; c.modulation = OOK_PULSE_PPM;
        c.s = gc[0].mean; c.l = gc[1].mean;
        c.g = gapLimit(2); c.r = std::max(c.g * 2, c.g + 1000);
        c.t = std::max(8.0, (gc[1].mean - gc[0].mean) * 0.35);
        cands.push_back(c);
    }
    {
        Cand c; c.mod = pre + "PCM"; c.label = "Pulse code (NRZ)"; c.modulation = oPCM;
        c.s = c.l = T; c.g = 0; c.r = maxIn * 2.5; c.t = T * 0.3;
        cands.push_back(c);
    }
    {
        Cand c; c.mod = pre + "MC_ZEROBIT"; c.label = "Manchester"; c.modulation = oMC;
        c.s = T; c.l = 0; c.g = 0; c.r = maxIn * 2.5; c.t = T * 0.3;
        cands.push_back(c);
    }
    if (!fsk) {
        Cand c; c.mod = "OOK_DMC"; c.label = "Differential Manchester"; c.modulation = OOK_PULSE_DMC;
        c.s = T; c.l = 0; c.g = 0; c.r = maxIn * 2.5; c.t = T * 0.3;
        cands.push_back(c);
    }
    Cand* best = nullptr;
    for (auto& c : cands) {
        tryCand(p, c);
        if (c.score > 0 && (!best || c.score > best->score)) best = &c;
    }
    if (!best) {
        out.guess += "; no consistent bit pattern found";
        return;
    }
    out.modName = best->label;
    out.shortUs = best->s; out.longUs = best->l; out.gapLimitUs = best->g; out.resetUs = best->r;
    char buf[320];
    if (best->modulation == oPWM || best->modulation == OOK_PULSE_PPM)
        std::snprintf(buf, sizeof buf, "%s, short %s us, long %s us", best->label.c_str(), fmtUs(best->s).c_str(), fmtUs(best->l).c_str());
    else
        std::snprintf(buf, sizeof buf, "%s, clock %s us", best->label.c_str(), fmtUs(best->s).c_str());
    out.guess += std::string("; probably ") + buf;
    // rows: the distinct ones first
    std::vector<std::string> rows;
    for (const auto& r : best->cap.rows) {
        if (std::find(rows.begin(), rows.end(), r) == rows.end()) rows.push_back(r);
        if (rows.size() >= 6) break;
    }
    for (const auto& r : rows) {
        out.bits += r + "\n";
        std::string h;
        for (size_t i = 0; i < r.size(); i += 4) {
            int v = 0;
            for (size_t k = 0; k < 4; k++) v = v * 2 + (i + k < r.size() && r[i + k] == '1' ? 1 : 0);
            h += "0123456789abcdef"[v];
        }
        out.hex += h + "\n";
    }
    if (!out.bits.empty()) { out.bits.pop_back(); out.hex.pop_back(); }
    char spec[240];
    std::snprintf(spec, sizeof spec, "n=unknown,m=%s,s=%s,l=%s,r=%s,g=%s,t=%s,bits>=%d", best->mod.c_str(), fmtUs(best->s).c_str(),
                  fmtUs(best->l).c_str(), fmtUs(best->r).c_str(), fmtUs(best->g).c_str(), fmtUs(best->t).c_str(), std::max(8, best->cap.longest - 2));
    out.flexSpec = spec;
}

} } // namespace dect2::sensors
