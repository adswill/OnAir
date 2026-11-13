// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GLONASS L1OF navigation message and orbit (see gnss_glo.h). Bit positions: ICD edition 5.1 Tables 4.6 and 4.11; scale factors Tables 4.5 and 4.9;
// the check: section 4.7 (Table 4.13); the orbit: Appendix A.3.1.2.
#include "dect2/gnss/gnss_glo.h"
#include "dect2/common/diag.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {
// Table 4.13: the data bits in each of the checksums C1..C7 (bit positions in the string)
bool inSet(int c, int i) {
    switch (c) {
    case 1: {
        static const int a[] = {9, 10, 12, 13, 15, 17, 19, 20, 22, 24, 26, 28, 30, 32, 34, 35, 37, 39, 41, 43, 45, 47, 49, 51, 53, 55, 57, 59, 61, 63, 65, 66, 68, 70, 72, 74, 76, 78, 80, 82, 84};
        for (int v : a) if (v == i) return true;
        return false;
    }
    case 2: {
        static const int a[] = {9, 11, 12, 14, 15, 18, 19, 21, 22, 25, 26, 29, 30, 33, 34, 36, 37, 40, 41, 44, 45, 48, 49, 52, 53, 56, 57, 60, 61, 64, 65, 67, 68, 71, 72, 75, 76, 79, 80, 83, 84};
        for (int v : a) if (v == i) return true;
        return false;
    }
    case 3: {
        static const int r[][2] = {{10, 12}, {16, 19}, {23, 26}, {31, 34}, {38, 41}, {46, 49}, {54, 57}, {62, 65}, {69, 72}, {77, 80}, {85, 85}};
        for (auto& x : r) if (i >= x[0] && i <= x[1]) return true;
        return false;
    }
    case 4: {
        static const int r[][2] = {{13, 19}, {27, 34}, {42, 49}, {58, 65}, {73, 80}};
        for (auto& x : r) if (i >= x[0] && i <= x[1]) return true;
        return false;
    }
    case 5: {
        static const int r[][2] = {{20, 34}, {50, 65}, {81, 85}};
        for (auto& x : r) if (i >= x[0] && i <= x[1]) return true;
        return false;
    }
    case 6: return i >= 35 && i <= 65;
    case 7: return i >= 66 && i <= 85;
    }
    return false;
}
int dataParity(const uint8_t* s, int c) {
    int p = 0;
    for (int i = 9; i <= 85; i++) if (inSet(c, i)) p ^= s[i] & 1;
    return p;
}
} // namespace

void gloSetHamming(uint8_t* s) {
    for (int c = 1; c <= 7; c++) s[c] = (uint8_t)dataParity(s, c);
    int p = 0;
    for (int i = 1; i <= 7; i++) p ^= s[i];
    for (int i = 9; i <= 85; i++) p ^= s[i] & 1;
    s[8] = (uint8_t)p;
}

int gloCheckString(uint8_t* s) {
    int C[8] = {};
    for (int c = 1; c <= 7; c++) C[c] = (s[c] & 1) ^ dataParity(s, c);
    int cs = 0;
    for (int i = 1; i <= 8; i++) cs ^= s[i] & 1;
    for (int i = 9; i <= 85; i++) cs ^= s[i] & 1;
    int ones = 0, K = 0;
    for (int c = 1; c <= 7; c++) if (C[c]) { ones++; K = c; }
    if ((ones == 0 && cs == 0) || (ones == 1 && cs == 1)) return 0;            // rule a
    if (ones >= 2 && cs == 1) {                                                 // rule b
        int v = 0;
        for (int c = 7; c >= 1; c--) v = (v << 1) | C[c];
        const int icor = v + 8 - K;
        if (icor > 85) {
            DIAG_DEBUG_EVERY(209947, 5.0, "A GLONASS string's check pattern points at bit %d, beyond the 85 data bits, so the string is rejected.", icor);
            return -1;
        }
        s[icor] ^= 1;
        return 1;
    }
    DIAG_DEBUG_EVERY(589503, 5.0, "A GLONASS string failed its Hamming check with more bit errors than one correction can fix, so the string is rejected.");
    return -1;                                                                  // rule c
}

uint32_t gloField(const uint8_t* s, int hi, int len) {
    uint32_t v = 0;
    for (int k = 0; k < len; k++) v = (v << 1) | (s[hi - k] & 1u);
    return v;
}
double gloSignMag(uint32_t v, int len) {
    const uint32_t mag = v & ((1u << (len - 1)) - 1);
    return (v >> (len - 1)) & 1u ? -(double)mag : (double)mag;
}
void gloPut(uint8_t* s, int hi, int len, uint32_t v) {
    for (int k = 0; k < len; k++) s[hi - k] = (uint8_t)((v >> (len - 1 - k)) & 1u);
}

int GloNav::parts() const {
    int n = 0;
    int64_t f = frameOf[1] >= 0 ? frameOf[1] : frameOf[2];
    for (int m = 1; m <= 4; m++) n += frameOf[m] >= 0 && frameOf[m] == f;
    return n;
}

bool gloHandleString(GloNav& nav, const uint8_t* s, int64_t frameId, GloEphemeris* eph) {
    const int m = (int)gloField(s, 84, 4);
    GloEphemeris& e = nav.pend;
    const double km = 1000.0;
    switch (m) {
    case 1:
        e.P1 = (int)gloField(s, 78, 2);
        e.tk = gloField(s, 76, 5) * 3600.0 + gloField(s, 71, 6) * 60.0 + gloField(s, 65, 1) * 30.0;
        e.vel[0] = gloSignMag(gloField(s, 64, 24), 24) * std::ldexp(1.0, -20) * km;
        e.acc[0] = gloSignMag(gloField(s, 40, 5), 5) * std::ldexp(1.0, -30) * km;
        e.pos[0] = gloSignMag(gloField(s, 35, 27), 27) * std::ldexp(1.0, -11) * km;
        break;
    case 2:
        e.Bn = (int)gloField(s, 80, 3);
        e.P2 = (int)gloField(s, 77, 1);
        e.tbIndex = (int)gloField(s, 76, 7);
        e.tb = e.tbIndex * 900.0;
        e.vel[1] = gloSignMag(gloField(s, 64, 24), 24) * std::ldexp(1.0, -20) * km;
        e.acc[1] = gloSignMag(gloField(s, 40, 5), 5) * std::ldexp(1.0, -30) * km;
        e.pos[1] = gloSignMag(gloField(s, 35, 27), 27) * std::ldexp(1.0, -11) * km;
        break;
    case 3:
        e.P3 = (int)gloField(s, 80, 1);
        e.gammaN = gloSignMag(gloField(s, 79, 11), 11) * std::ldexp(1.0, -40);
        e.P = (int)gloField(s, 67, 2);
        e.ln = (int)gloField(s, 65, 1);
        e.vel[2] = gloSignMag(gloField(s, 64, 24), 24) * std::ldexp(1.0, -20) * km;
        e.acc[2] = gloSignMag(gloField(s, 40, 5), 5) * std::ldexp(1.0, -30) * km;
        e.pos[2] = gloSignMag(gloField(s, 35, 27), 27) * std::ldexp(1.0, -11) * km;
        break;
    case 4:
        e.tauN = gloSignMag(gloField(s, 80, 22), 22) * std::ldexp(1.0, -30);
        e.dTauN = gloSignMag(gloField(s, 58, 5), 5) * std::ldexp(1.0, -30);
        e.En = (int)gloField(s, 53, 5);
        e.P4 = (int)gloField(s, 34, 1);
        e.FT = (int)gloField(s, 33, 4);
        e.NT = (int)gloField(s, 26, 11);
        e.n = (int)gloField(s, 15, 5);
        e.M = (int)gloField(s, 10, 2);
        break;
    case 5:
        e.NA = (int)gloField(s, 80, 11);
        e.tauC = gloSignMag(gloField(s, 69, 32), 32) * std::ldexp(1.0, -31);
        e.N4 = (int)gloField(s, 36, 5);
        e.tauGps = gloSignMag(gloField(s, 31, 22), 22) * std::ldexp(1.0, -30);
        e.has5 = true;
        return false;
    default:
        return false;
    }
    nav.frameOf[m] = frameId;
    for (int i = 1; i <= 4; i++) if (nav.frameOf[i] != frameId) return false;
    *eph = e;
    return true;
}

double gloWrapDay(double dt) {
    while (dt > 43200.0) dt -= 86400.0;
    while (dt < -43200.0) dt += 86400.0;
    return dt;
}

namespace {
void deriv(const double* y, const double* acc, double* d) {
    const double x = y[0], yy = y[1], z = y[2];
    const double r2 = x * x + yy * yy + z * z, r = std::sqrt(r2);
    const double mr3 = kGloMu / (r2 * r);
    const double j = 1.5 * kGloJ2 * kGloMu * kGloAe * kGloAe / (r2 * r2 * r);
    const double zr = 5.0 * z * z / r2;
    const double w2 = kGloOmega * kGloOmega;
    d[0] = y[3]; d[1] = y[4]; d[2] = y[5];
    d[3] = -mr3 * x - j * x * (1.0 - zr) + w2 * x + 2.0 * kGloOmega * y[4] + acc[0];
    d[4] = -mr3 * yy - j * yy * (1.0 - zr) + w2 * yy - 2.0 * kGloOmega * y[3] + acc[1];
    d[5] = -mr3 * z - j * z * (3.0 - zr) + acc[2];
}
} // namespace

void gloEphemerisState(const GloEphemeris& e, double t, double pos[3], double vel[3], double* clockS, double step) {
    double y[6] = {e.pos[0], e.pos[1], e.pos[2], e.vel[0], e.vel[1], e.vel[2]};
    const double dt = gloWrapDay(t - e.tb);
    const int n = std::max(1, (int)std::ceil(std::fabs(dt) / step));
    const double h = dt / n;
    for (int i = 0; i < n; i++) {
        double k1[6], k2[6], k3[6], k4[6], tmp[6];
        deriv(y, e.acc, k1);
        for (int q = 0; q < 6; q++) tmp[q] = y[q] + 0.5 * h * k1[q];
        deriv(tmp, e.acc, k2);
        for (int q = 0; q < 6; q++) tmp[q] = y[q] + 0.5 * h * k2[q];
        deriv(tmp, e.acc, k3);
        for (int q = 0; q < 6; q++) tmp[q] = y[q] + h * k3[q];
        deriv(tmp, e.acc, k4);
        for (int q = 0; q < 6; q++) y[q] += h / 6.0 * (k1[q] + 2 * k2[q] + 2 * k3[q] + k4[q]);
    }
    for (int q = 0; q < 3; q++) { pos[q] = y[q]; if (vel) vel[q] = y[3 + q]; }
    if (clockS) *clockS = -e.tauN + e.gammaN * dt;
}

} // namespace dect2
