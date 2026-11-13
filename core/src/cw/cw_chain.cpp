// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// CW signal chain (see cw_chain.h).
#include "dect2/cw/cw_chain.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace dect2 {
namespace cw {

ChainTune& chainTune() {
    static ChainTune t;
    return t;
}

struct Chain::Impl {
    bool adaptive = true;
    // adaptive detector
    bool init = false, raw = false;
    float sm = 0, hi = 0, lo = 0;
    // debounce
    bool key = false, started = false;
    int run = 0, cnt = 0;
    FistDecoder dec;
    // legacy (0.2.7): its own detector, Keyer and Timing
    bool lInit = false, lRaw = false;
    float lHi = 0, lLo = 0;
    Timing tm;
    Keyer keyer;
    bool lKey = false;
    uint64_t unknown = 0;

    float dotMs() const { return dec.locked() ? (float)dec.dotMs() : 0.f; }

    bool detect(float e) {
        const ChainTune& tu = chainTune();
        if (!init) { init = true; sm = e; lo = e; hi = e; }
        if (hi > 0) e = std::min(e, 2.5f * hi + 4.f * lo);   // an impulse is not a mark
        const float dot = dotMs();
        // a longer window when the signal is weak (the gaps are resolved less well, but the noise matters more)
        const float ratio = lo > 0 ? hi / lo : 4.f;
        const float weak = std::max(0.f, std::min(1.f, (tu.weakRatio - ratio) / (tu.weakRatio - 2.2f)));
        const float frac = tu.smoothFrac + tu.smoothWeak * weak;
        const float len = dot > 0 ? std::max(tu.smoothMin, std::min(tu.smoothMax, frac * dot)) : 4.f;
        sm += (e - sm) / len;
        const float span = hi - lo;
        const bool open = hi > tu.openRatio * lo;
        if (!raw && open && sm > lo + tu.thrOn * span) raw = true;
        else if (raw && sm < lo + tu.thrOff * span) raw = false;
        if (raw) hi += (sm > hi ? 0.10f : 0.02f) * (sm - hi);
        else if (sm > hi) hi += tu.attackIdle * (sm - hi);
        else hi += (lo - hi) / std::max(tu.hiDecayMs, tu.hiDecayDots * dot);
        if (!raw) lo += (sm < lo ? 0.02f : 0.004f) * (sm - lo);
        if (hi < lo) hi = lo;
        return raw;
    }

    void debounce(bool r) {
        const ChainTune& tu = chainTune();
        const float dot = dotMs();
        const int g = dot > 0 ? std::max(3, std::min(15, (int)std::lround(tu.debounceFrac * dot))) : 5;
        cnt = r != key ? cnt + 1 : 0;
        run++;
        if (cnt >= g) {
            const int d = run - cnt;
            if (key) dec.mark(d); else if (started) dec.space(d);
            started = true;
            key = r;
            run = cnt;
            cnt = 0;
        }
        if (!key && started && run % 10 == 0) dec.idle(run);
    }

    // the 0.2.7 detector, unchanged
    bool legacyDetect(float e) {
        if (!lInit) { lInit = true; lLo = e; lHi = e; }
        const float span = lHi - lLo;
        const bool open = lHi > 2.8f * lLo;
        const bool k = lRaw ? (e > lLo + 0.35f * span) : (open && e > lLo + 0.5f * span);
        if (e > lHi) lHi += 0.05f * (e - lHi);
        if (lRaw) lHi += 0.01f * (e - lHi);
        else lLo += (e < lLo ? 0.05f : 0.004f) * (e - lLo);
        lHi -= 0.0005f * (lHi - lLo);
        lRaw = k;
        return k;
    }
};

Chain::Chain() : p_(std::make_unique<Impl>()) {}
Chain::~Chain() = default;

void Chain::reset() {
    const bool a = p_->adaptive;
    p_ = std::make_unique<Impl>();
    p_->adaptive = a;
}
void Chain::setAdaptive(bool on) { if (p_->adaptive != on) { reset(); p_->adaptive = on; } }
bool Chain::adaptive() const { return p_->adaptive; }
void Chain::setManualWpm(double wpm) { p_->dec.setManualWpm(wpm); }

void Chain::sample(float e) {
    Impl& m = *p_;
    if (m.adaptive) {
        if (std::getenv("T_LEGDET")) m.debounce(m.legacyDetect(e)); else m.debounce(m.detect(e));
    } else {
        const bool k = m.legacyDetect(e);
        m.keyer.sample(k, m.tm);
        m.lKey = m.keyer.key();
    }
}

bool Chain::key() const { return p_->adaptive ? p_->key : p_->lKey; }
bool Chain::locked() const { return p_->adaptive ? p_->dec.locked() : p_->tm.locked(); }
double Chain::wpm() const { return p_->adaptive ? p_->dec.wpm() : (p_->tm.locked() ? p_->tm.wpm() : 0.0); }
bool Chain::farnsworth() const {
    if (!p_->adaptive) return p_->tm.farnsworth();
    FistStats s; p_->dec.stats(s);
    return s.locked && s.charGapDots > 4.2f;
}
float Chain::markLevel() const { return p_->adaptive ? p_->hi : p_->lHi; }
float Chain::noiseLevel() const { return p_->adaptive ? p_->lo : p_->lLo; }
uint64_t Chain::unknownChars() const { return p_->unknown; }

void Chain::take(std::string& text, std::string& conf, std::string& alt) {
    Impl& m = *p_;
    if (!m.adaptive) {
        const std::string s = m.tm.take();
        for (char c : s) { if (c == '*') m.unknown++; }
        text += s; conf.append(s.size(), '9'); alt.append(s.size(), ' ');
        return;
    }
    DecTok t[32];
    for (;;) {
        const size_t k = m.dec.pop(t, 32);
        if (!k) break;
        for (size_t i = 0; i < k; i++) {
            const size_t len = std::strlen(t[i].text);
            text += t[i].text;
            conf.append(len, (char)('0' + std::min(9, t[i].conf / 10)));
            std::string a(len, ' ');
            if (t[i].alt[0]) a[0] = t[i].alt[1] ? '^' : t[i].alt[0];
            alt += a;
            if (t[i].text[0] == '*') m.unknown++;
        }
    }
}

void Chain::stats(FistStats& s) const {
    if (p_->adaptive) { p_->dec.stats(s); return; }
    s = FistStats();
    s.locked = p_->tm.locked();
    s.wpm = (float)p_->tm.wpm(); s.dotMs = (float)p_->tm.dotMs();
    s.dashRatio = (float)p_->tm.dashRatio(); s.charGapDots = (float)p_->tm.charGapDots();
}
int Chain::liveAlternatives(char (*out)[48], int maxN) const { return p_->adaptive ? p_->dec.liveAlternatives(out, maxN) : 0; }

} // namespace cw
} // namespace dect2
