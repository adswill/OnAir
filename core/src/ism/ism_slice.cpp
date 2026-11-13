// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors: the pulse slicers (PWM, PPM, PCM, Manchester-zero) that turn a train of pulses into a bit buffer. Written from rtl_433 src/pulse_slicer.c.
#include "ism_internal.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace dect2 {

namespace ism_detail {

thread_local float tlPcmBitUs = 0;   // the bit width slicePcm measured for the rows it hands out

void slicePcm(const IsmTrain& t, const Timing& d, const Emit& emit) {   // NRZ only (short == long), as every protocol here uses
    const auto& P = t.p;
    const float sl = d.s, tol = d.tol > 0 ? d.tol : d.l / 4;
    float T = sl;
    int minCount = 12, pre = 0;
    for (size_t n = 0; n < P.size(); n++) {   // the exact bit width from a preamble of single bits
        float w = 0; int c = 0;
        while (n < P.size() && (int)(P[n].pulseUs / T + 0.5f) == 1 && (int)(P[n].gapUs / T + 0.5f) == 1) { w += P[n].pulseUs + P[n].gapUs; c += 2; n++; }
        if (c >= minCount) { T = w / (float)c; minCount = c; pre = c; }
    }
    if (!pre) {                               // or measured over all single and double bits
        float w = 0; int c = 0;
        for (const auto& q : P) {
            if (std::fabs(q.pulseUs - sl) <= tol) { w += q.pulseUs; c += 1; }
            if (std::fabs(q.pulseUs - 2 * sl) <= tol) { w += q.pulseUs; c += 2; }
            if (std::fabs(q.gapUs - sl) <= tol) { w += q.gapUs; c += 1; }
            if (std::fabs(q.gapUs - 2 * sl) <= tol) { w += q.gapUs; c += 2; }
        }
        if (c > 20) T = w / (float)c;
    }
    tlPcmBitUs = T;
    const float gapLimit = d.gap > 0 ? d.gap : d.reset;
    const int maxZeros = (int)(gapLimit / T);
    Bits bits;
    for (size_t n = 0; n < P.size(); n++) {
        const int highs = (int)(P[n].pulseUs / T + 0.5f);
        int lows = std::min((int)(P[n].gapUs / T + 0.5f), maxZeros);
        for (int i = 0; i < highs; i++) bits.addBit(1);
        for (int i = 0; i < lows; i++) bits.addBit(0);
        if (P[n].gapUs > gapLimit && P[n].gapUs <= d.reset) bits.addRow();
        if ((n == P.size() - 1 || P[n].gapUs > d.reset) && bits.hasData()) { emit(bits); bits.clear(); }
    }
}

void slicePpm(const IsmTrain& t, const Timing& d, const Emit& emit) {
    float zl, zu, ol, ou, syl = 0, syu = 0;
    if (d.tol > 0) {
        zl = d.s - d.tol; zu = d.s + d.tol; ol = d.l - d.tol; ou = d.l + d.tol;
        if (d.sync > 0) { syl = d.sync - d.tol; syu = d.sync + d.tol; }
    } else { zl = 0; zu = (d.s + d.l) / 2 + 1; ol = zu - 1; ou = d.gap > 0 ? d.gap : d.reset; }
    Bits bits;
    const auto& P = t.p;
    for (size_t n = 0; n < P.size(); n++) {
        const float g = P[n].gapUs;
        if (g > zl && g < zu) bits.addBit(0);
        else if (g > ol && g < ou) bits.addBit(1);
        else if (g > syl && g < syu) bits.addSync();
        else if (g < d.reset) bits.addRow();
        if ((n == P.size() - 1 || g >= d.reset) && bits.hasData()) { emit(bits); bits.clear(); }
    }
}

void slicePwm(const IsmTrain& t, const Timing& d, const Emit& emit) {
    float ol, ou, zl, zu, syl = 0, syu = 0;
    const float s = d.s, l = d.l, sy = d.sync;
    if (d.tol > 0) {
        ol = s - d.tol; ou = s + d.tol; zl = l - d.tol; zu = l + d.tol;
        if (sy > 0) { syl = sy - d.tol; syu = sy + d.tol; }
    } else if (sy <= 0) { ol = 0; ou = (s + l) / 2 + 1; zl = ou - 1; zu = 1e9f; }
    else if (sy < s) { syl = 0; syu = (sy + s) / 2 + 1; ol = syu - 1; ou = (s + l) / 2 + 1; zl = ou - 1; zu = 1e9f; }
    else if (sy < l) { ol = 0; ou = (s + sy) / 2 + 1; syl = ou - 1; syu = (sy + l) / 2 + 1; zl = syu - 1; zu = 1e9f; }
    else { ol = 0; ou = (s + l) / 2 + 1; zl = ou - 1; zu = (l + sy) / 2 + 1; syl = zu - 1; syu = 1e9f; }
    Bits bits;
    const auto& P = t.p;
    for (size_t n = 0; n < P.size(); n++) {
        const float p = P[n].pulseUs;
        if (p > ol && p < ou) bits.addBit(1);
        else if (p > zl && p < zu) bits.addBit(0);
        else if (p > syl && p < syu) bits.addSync();
        else if (p <= ol) {}
        else bits.addRow();
        if (n == P.size() - 1 || P[n].gapUs > d.reset) { if (bits.hasData()) emit(bits); bits.clear(); }
        else if (d.gap > 0 && P[n].gapUs > d.gap && bits.n.back() > 0) bits.addRow();
    }
}

void sliceMcZero(const IsmTrain& t, const Timing& d, const Emit& emit) {
    const float s = d.s;
    float since = 0;
    Bits bits;
    bits.addBit(0);
    const auto& P = t.p;
    for (size_t n = 0; n < P.size(); n++) {
        if (P[n].pulseUs + since > s * 1.5f) { bits.addBit(1); since = 0; }
        else since += P[n].pulseUs;
        if (n == P.size() - 1 || P[n].gapUs > d.reset) {
            emit(bits); bits.clear(); bits.addBit(0); since = 0;
        } else if (P[n].gapUs + since > s * 1.5f) { bits.addBit(0); since = 0; }
        else since += P[n].gapUs;
    }
}


} // namespace ism_detail

} // namespace dect2
