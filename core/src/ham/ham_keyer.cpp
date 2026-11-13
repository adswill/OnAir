// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham CW keyer: straight, iambic A / B, Ultimatic and bug keying, keyed audio, text to edges and edges back to text (ham_keyer.h).
#include "dect2/ham/ham_keyer.h"
#include "dect2/cw/cw_morse.h"
#include <algorithm>
#include <cmath>

namespace dect2::ham {

const char* keyerModeName(KeyerMode m) {
    switch (m) {
    case KeyerMode::Straight: return "Straight key";
    case KeyerMode::IambicA: return "Iambic A";
    case KeyerMode::IambicB: return "Iambic B";
    case KeyerMode::Ultimatic: return "Ultimatic";
    case KeyerMode::Bug: return "Bug";
    }
    return "?";
}

double CwKeyer::dotMs() const { return 1200.0 / std::max(5.0, std::min(60.0, c_.wpm)); }

void CwKeyer::reset() {
    dit_ = dah_ = sk_ = ditMem_ = dahMem_ = squeezeSeen_ = false;
    cur_ = last_ = lastPressed_ = None;
    inEl_ = false;
    if (key_) edge(now_, false);
    edges_.clear();
}

void CwKeyer::edge(double ms, bool down) {
    if (down == key_) return;
    key_ = down;
    edges_.push_back({ms, down});
}

std::vector<KeyEdge> CwKeyer::takeEdges() { std::vector<KeyEdge> e; e.swap(edges_); return e; }

void CwKeyer::startElement(double ms, El e) {
    const double dot = dotMs();
    const double w = std::max(25.0, std::min(75.0, c_.weight));
    const double d = dot * (w - 50) / 50;
    const double ratio = std::max(2.5, std::min(4.5, c_.dashRatio));
    cur_ = e;
    inEl_ = true;
    elEnd_ = ms + (e == Dit ? dot : ratio * dot) + d;
    spEnd_ = elEnd_ + dot - d;
    if (e == Dit) ditMem_ = false; else dahMem_ = false;
    // a squeeze at the start: the other paddle is already down (iambic B and Ultimatic remember it)
    squeezeSeen_ = dit_ && dah_;
    if (c_.mode == KeyerMode::IambicB && squeezeSeen_) { if (e == Dit) dahMem_ = true; else ditMem_ = true; }
    edge(ms, true);
}

CwKeyer::El CwKeyer::nextElement() {
    // a squeeze from idle starts with the paddle pressed first (the one not pressed last)
    const El prev = cur_ != None ? cur_ : (lastPressed_ == Dit ? Dah : Dit);
    El n = None;
    switch (c_.mode) {
    case KeyerMode::IambicA:   // only the paddles held now: both alternate, one repeats
        if (dit_ && dah_) n = prev == Dit ? Dah : Dit;
        else if (dit_) n = Dit;
        else if (dah_) n = Dah;
        break;
    case KeyerMode::IambicB:   // held paddles first, then the memories (an opposite press during the element, the squeeze bonus)
        if (dit_ && dah_) n = prev == Dit ? Dah : Dit;
        else if (prev == Dit && dahMem_) n = Dah;
        else if (prev == Dah && ditMem_) n = Dit;
        else if (dit_) n = Dit;
        else if (dah_) n = Dah;
        else if (ditMem_) n = Dit;
        else if (dahMem_) n = Dah;
        break;
    case KeyerMode::Ultimatic:  // both held: the last pressed repeats
        if (dit_ && dah_) n = lastPressed_ == Dah ? Dah : Dit;
        else if (dit_) n = Dit;
        else if (dah_) n = Dah;
        else if (prev == Dit && dahMem_) n = Dah;
        else if (prev == Dah && ditMem_) n = Dit;
        break;
    case KeyerMode::Bug:
        n = dit_ ? Dit : None;
        break;
    default: break;
    }
    if (n == None) { ditMem_ = dahMem_ = false; }
    return n;
}

void CwKeyer::run(double ms) {
    if (c_.mode == KeyerMode::Straight) { now_ = std::max(now_, ms); return; }
    while (now_ < ms || (!inEl_ && now_ <= ms)) {
        if (!inEl_) {
            const El n = nextElement();
            if (n == None) { cur_ = None; now_ = std::max(now_, ms); break; }
            startElement(now_, n);
            continue;
        }
        if (key_ && ms >= elEnd_) edge(elEnd_, false);
        if (ms >= spEnd_) { inEl_ = false; last_ = cur_; now_ = spEnd_; continue; }
        now_ = ms;
        break;
    }
}

void CwKeyer::paddles(double ms, bool ditIn, bool dahIn) {
    run(ms);
    bool d = ditIn, a = dahIn;
    if (c_.swap) std::swap(d, a);
    if (d && !dit_) lastPressed_ = Dit;
    if (a && !dah_) lastPressed_ = Dah;
    if (c_.mode == KeyerMode::Bug) {   // the dah paddle is a straight key
        dit_ = d;
        if (a != dah_) { dah_ = a; if (!inEl_) edge(ms, a); }
        if (!inEl_ && !dah_) { now_ = ms; run(ms); }   // dits start when the dah contact is open
        return;
    }
    if (inEl_ && c_.mode != KeyerMode::IambicA) {   // memories: the opposite paddle pressed during the element or its gap
        if (cur_ == Dit && a && !dah_) dahMem_ = true;
        if (cur_ == Dah && d && !dit_) ditMem_ = true;
    }
    dit_ = d;
    dah_ = a;
    if (dit_ && dah_) squeezeSeen_ = true;
    if (!inEl_) { now_ = ms; run(ms); }
}

void CwKeyer::straight(double ms, bool down) {
    run(ms);
    sk_ = down;
    if (c_.mode == KeyerMode::Straight) edge(ms, down);
}

// ---- audio, text, decode ----
std::vector<float> keyerAudio(const std::vector<KeyEdge>& e, double hz, double rate, double riseMs) {
    if (e.empty()) return {};
    const double t0 = e.front().ms - 10, t1 = e.back().ms + 50;
    const size_t n = (size_t)std::max(0.0, (t1 - t0) / 1000.0 * rate);
    std::vector<float> a(n, 0.f);
    const double rise = std::max(1.0, riseMs) / 1000.0 * rate;
    // the envelope: a raised-cosine ramp starting at each down edge and each up edge
    size_t k = 0;
    bool down = false;
    double edgeAt = -1e18, g0 = 0;   // where the current ramp started, the gain there
    double g = 0;
    for (size_t i = 0; i < n; i++) {
        const double t = t0 + (double)i * 1000.0 / rate;
        while (k < e.size() && e[k].ms <= t) { down = e[k].down; edgeAt = (double)i; g0 = g; k++; }
        const double x = std::min(1.0, ((double)i - edgeAt) / rise);
        const double r = 0.5 - 0.5 * std::cos(3.14159265358979323846 * x);
        g = down ? g0 + (1 - g0) * r : g0 * (1 - r);
        a[i] = (float)(g * std::sin(2 * 3.14159265358979323846 * hz * (double)i / rate));
    }
    return a;
}

std::vector<KeyEdge> keyerText(const std::string& text, const KeyerCfg& c, double t) {
    const double dot = 1200.0 / std::max(5.0, std::min(60.0, c.wpm));
    const double w = std::max(25.0, std::min(75.0, c.weight));
    const double d = dot * (w - 50) / 50;
    const double ratio = std::max(2.5, std::min(4.5, c.dashRatio));
    std::vector<KeyEdge> e;
    bool any = false;
    for (const std::string& tok : cw::tokenize(text)) {
        if (tok == " ") { if (any) t += 4 * dot; continue; }
        const std::string code = cw::encodeSymbol(tok);
        if (code.empty()) continue;
        for (size_t i = 0; i < code.size(); i++) {
            e.push_back({t, true});
            t += (code[i] == '-' ? ratio * dot : dot) + d;
            e.push_back({t, false});
            t += (i + 1 < code.size() ? dot : 3 * dot) - d;
        }
        any = true;
    }
    return e;
}

std::string keyerDecode(const std::vector<KeyEdge>& e, double* wpm) {
    cw::Timing tm;
    for (size_t i = 1; i < e.size(); i++) {
        const double ms = e[i].ms - e[i - 1].ms;
        if (e[i - 1].down && !e[i].down) tm.mark(ms);
        else if (!e[i - 1].down && e[i].down) tm.space(ms);
    }
    tm.idle(5000);
    if (wpm) *wpm = tm.wpm();
    return tm.take();
}

}  // namespace dect2::ham
