// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Time signal frames (see time_proto.h).
#include "dect2/time/time_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "dect2/common/mathconst.h"

namespace dect2 {
namespace timesig {

namespace {

struct Field { int pos; int w; };

void put(int* b, const Field* f, int n, int v) {
    for (int i = 0; i < n; i++) if (v >= f[i].w) { b[f[i].pos] = 1; v -= f[i].w; }
}
// weights in descending order, as BCD: each decimal digit must stay <= 9
bool get(const int* b, const Field* f, int n, int& v) {
    v = 0;
    int units = 0, tens = 0;
    for (int i = 0; i < n; i++) if (b[f[i].pos]) { v += f[i].w; if (f[i].w < 10) units += f[i].w; else if (f[i].w < 100) tens += f[i].w / 10; }
    return units <= 9 && tens <= 9;
}
int parity(const int* b, int from, int to) { int p = 0; for (int i = from; i <= to; i++) p ^= b[i]; return p; }

int curYear() {
    const time_t t = time(nullptr);
    int y, m, d;
    civilFromDays((int64_t)t / 86400, y, m, d);
    return y;
}

// the full year closest to the hint for a two-digit year
int fullYear(int yy, int hint) {
    if (hint <= 0) hint = curYear();
    int y = hint - hint % 100 + yy;
    if (y > hint + 50) y -= 100; else if (y < hint - 50) y += 100;
    return y;
}

int daysInMonth(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return d[m - 1];
}
bool isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

struct Civil { int y, mo, d, h, mi, doy, wd; };
Civil civil(int64_t t) {
    Civil c;
    const int64_t days = (t >= 0 ? t : t - 86399) / 86400;
    const int64_t sod = t - days * 86400;
    civilFromDays(days, c.y, c.mo, c.d);
    c.h = (int)(sod / 3600); c.mi = (int)(sod / 60 % 60);
    c.doy = (int)(days - daysFromCivil(c.y, 1, 1)) + 1;
    c.wd = weekdayOf(days);
    return c;
}

// the 60 symbols from bits (and MSF B bits)
std::vector<uint8_t> syms(const int* a, const int* bb) {
    std::vector<uint8_t> s(60);
    for (int i = 0; i < 60; i++) s[i] = (uint8_t)(a[i] | (bb ? bb[i] << 1 : 0));
    return s;
}

const Field kWMin[] = {{1, 40}, {2, 20}, {3, 10}, {5, 8}, {6, 4}, {7, 2}, {8, 1}};
const Field kWHour[] = {{12, 20}, {13, 10}, {15, 8}, {16, 4}, {17, 2}, {18, 1}};
const Field kWDoy[] = {{22, 200}, {23, 100}, {25, 80}, {26, 40}, {27, 20}, {28, 10}, {30, 8}, {31, 4}, {32, 2}, {33, 1}};
const int kMarks[] = {0, 9, 19, 29, 39, 49, 59};

bool doyOk(int doy, int hundreds) {   // BCD hundreds digit of a day of year
    return doy >= 1 && doy <= 366 && hundreds <= 3;
}


// ---- BPC / HLA / BPM coded helpers
int bitsOf(const int* b, int from, int n) {   // LSB first
    int v = 0;
    for (int i = 0; i < n; i++) if (b[from + i]) v |= 1 << i;
    return v;
}
void putBits(int* b, int from, int n, int v) { for (int i = 0; i < n; i++) b[from + i] = v >> i & 1; }
int oddPc(const int* b, int from, int n) {   // the odd parity bit: the ones with it are odd
    int p = 0;
    for (int i = 0; i < n; i++) p ^= b[from + i];
    return p ^ 1;
}
// BPC: the quaternary digits of a frame as two bits each, MSB first
int bpcV(const uint8_t* s, int i) { return s[i] & 3; }
int bpcOnes(const uint8_t* s, int a, int b) { int n = 0; for (int i = a; i <= b; i++) n += (bpcV(s, i) & 1) + (bpcV(s, i) >> 1); return n; }

} // namespace

const char* stationName(int s) {
    static const char* n[] = {"DCF77", "MSF", "WWVB", "JJY 40 kHz", "JJY 60 kHz", "WWV / WWVH", "CHU", "BPM", "BPC", "HLA", "BPM coded", "RWM"};
    return s >= 0 && s < kNumStations ? n[s] : "?";
}
double stationKhz(int s) {
    static const double f[] = {77.5, 60, 60, 40, 60, 10000, 7850, 5000, 68.5, 5000, 5000, 9996};
    return s >= 0 && s < kNumStations ? f[s] : 0;
}
bool stationIsHf(int s) { return s == Wwv || s == Chu || s == Bpm || s == Hla || s == BpmC || s == Rwm; }
bool stationKeysCarrier(int s) { return !stationIsHf(s) || s == Rwm; }
const char* stationTimeZone(int s) {
    switch (s) { case Dcf77: return "CET / CEST"; case Msf: return "UK (GMT / BST)"; case Jjy40: case Jjy60: return "JST"; case Bpm: return "UTC / UT1"; case Bpc: return "CST (UTC+8)"; case Hla: return "KST (UTC+9)"; default: return "UTC"; }
}
int stationForKhz(double khz, int pref60) {
    auto nearBy = [&](double f, double tol) { return std::fabs(khz - f) <= tol; };
    if (nearBy(77.5, 1.5)) return Dcf77;
    if (nearBy(40, 1.5)) return Jjy40;
    if (nearBy(68.5, 1.5)) return Bpc;
    if (nearBy(60, 1.5)) return pref60 == Msf || pref60 == Wwvb || pref60 == Jjy60 ? pref60 : Msf;
    for (double f : {4996.0, 9996.0, 14996.0}) if (nearBy(f, 2)) return Rwm;   // 4 kHz below WWV's: before WWV's wider window
    for (double f : {2500.0, 5000.0, 10000.0, 15000.0, 20000.0}) if (nearBy(f, 8)) return Wwv;
    // CHU removed from the product (#150); 3.33 / 7.85 / 14.67 MHz are not auto-identified.
    return -1;
}

int64_t daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void civilFromDays(int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = (int)(doy - (153 * mp + 2) / 5 + 1);
    m = (int)(mp < 10 ? mp + 3 : mp - 9);
    y = (int)(yoe + era * 400) + (m <= 2);
}
int weekdayOf(int64_t days) { const int64_t w = (days + 3) % 7; return (int)(w < 0 ? w + 7 : w) + 1; }   // 1970-01-01 was a Thursday

static int64_t lastSunday(int y, int m) {
    const int64_t last = daysFromCivil(y, m, daysInMonth(y, m));
    return last - (weekdayOf(last) % 7);
}
bool euSummerTime(int64_t utc) {
    const Civil c = civil(utc);
    const int64_t on = lastSunday(c.y, 3) * 86400 + 3600, off = lastSunday(c.y, 10) * 86400 + 3600;
    return utc >= on && utc < off;
}
bool usDstOnDate(int y, int m, int d) {
    const int64_t mar1 = daysFromCivil(y, 3, 1), nov1 = daysFromCivil(y, 11, 1);
    const int64_t start = mar1 + (7 - weekdayOf(mar1)) % 7 + 7;   // the second Sunday of March
    const int64_t end = nov1 + (7 - weekdayOf(nov1)) % 7;         // the first Sunday of November
    const int64_t t = daysFromCivil(y, m, d);
    return t >= start && t < end;
}

// RWM: the doubled seconds of a value (+n: from first, -n: from neg), the CCIR / ITU-R TF.460 code
static void rwmDouble(std::vector<uint8_t>& s, int v, int pos, int neg, int maxN) {
    const int n = std::min(maxN, std::abs(v));
    for (int k = 0; k < n; k++) s[(size_t)((v > 0 ? pos : neg) + k)] = S1;
}
// RWM: a value from its two runs of doubled seconds; false when both runs are set or a run has a gap
static bool rwmValue(const uint8_t* s, int pos, int neg, int maxN, int& v) {
    auto run = [&](int a, int& n) {
        n = 0;
        while (n < maxN && s[a + n] == S1) n++;
        for (int k = n; k < maxN; k++) if (s[a + k] == S1) return false;
        return true;
    };
    int p = 0, q = 0;
    if (!run(pos, p) || !run(neg, q) || (p && q)) return false;
    v = p ? p : -q;
    return true;
}

std::vector<uint8_t> encodeMinute(int st, int64_t M, const EncodeOpts& o) {
    if (st == Rwm) {
        std::vector<uint8_t> s(60, S0);
        s[0] = SMark;
        rwmDouble(s, o.dut1, 1, 9, 8);
        rwmDouble(s, o.dUt1, 21, 31, 4);
        return s;
    }
    int a[60] = {}, bb[60] = {};
    if (st == Dcf77) {
        const int64_t T = M + 60;
        const bool cest = euSummerTime(T);
        const Civil c = civil(T + (cest ? 7200 : 3600));
        a[15] = o.callBit;
        a[16] = euSummerTime(T) != euSummerTime(T - T % 3600 + 3600);
        a[17] = cest; a[18] = !cest; a[19] = o.leapAnnounce; a[20] = 1;
        const Field mi[] = {{27, 40}, {26, 20}, {25, 10}, {24, 8}, {23, 4}, {22, 2}, {21, 1}};
        const Field hr[] = {{34, 20}, {33, 10}, {32, 8}, {31, 4}, {30, 2}, {29, 1}};
        const Field dy[] = {{41, 20}, {40, 10}, {39, 8}, {38, 4}, {37, 2}, {36, 1}};
        const Field wd[] = {{44, 4}, {43, 2}, {42, 1}};
        const Field mo[] = {{49, 10}, {48, 8}, {47, 4}, {46, 2}, {45, 1}};
        const Field yr[] = {{57, 80}, {56, 40}, {55, 20}, {54, 10}, {53, 8}, {52, 4}, {51, 2}, {50, 1}};
        put(a, mi, 7, c.mi); put(a, hr, 6, c.h); put(a, dy, 6, c.d); put(a, wd, 3, c.wd); put(a, mo, 5, c.mo); put(a, yr, 8, c.y % 100);
        a[28] = parity(a, 21, 27); a[35] = parity(a, 29, 34); a[58] = parity(a, 36, 57);
        std::vector<uint8_t> s = syms(a, nullptr);
        s[59] = SNone;
        return s;
    }
    if (st == Msf) {
        const int64_t T = M + 60;
        const bool bst = euSummerTime(T);
        const Civil c = civil(T + (bst ? 3600 : 0));
        const Field yr[] = {{17, 80}, {18, 40}, {19, 20}, {20, 10}, {21, 8}, {22, 4}, {23, 2}, {24, 1}};
        const Field mo[] = {{25, 10}, {26, 8}, {27, 4}, {28, 2}, {29, 1}};
        const Field dy[] = {{30, 20}, {31, 10}, {32, 8}, {33, 4}, {34, 2}, {35, 1}};
        const Field wd[] = {{36, 4}, {37, 2}, {38, 1}};
        const Field hr[] = {{39, 20}, {40, 10}, {41, 8}, {42, 4}, {43, 2}, {44, 1}};
        const Field mi[] = {{45, 40}, {46, 20}, {47, 10}, {48, 8}, {49, 4}, {50, 2}, {51, 1}};
        put(a, yr, 8, c.y % 100); put(a, mo, 5, c.mo); put(a, dy, 6, c.d); put(a, wd, 3, c.wd % 7); put(a, hr, 6, c.h); put(a, mi, 7, c.mi);
        for (int i = 53; i <= 58; i++) a[i] = 1;
        const int d = o.dut1 > 8 ? 8 : o.dut1 < -8 ? -8 : o.dut1;
        for (int i = 0; i < (d > 0 ? d : -d); i++) bb[(d > 0 ? 1 : 9) + i] = 1;
        bb[53] = euSummerTime(T) != euSummerTime(T + 3600);
        bb[54] = !parity(a, 17, 24); bb[55] = !parity(a, 25, 35); bb[56] = !parity(a, 36, 38); bb[57] = !parity(a, 39, 51);
        bb[58] = bst;
        std::vector<uint8_t> s = syms(a, bb);
        s[0] = SMark;
        return s;
    }
    const Civil c = civil(M + (st == Jjy40 || st == Jjy60 ? 9 * 3600 : 0));
    if (st == Wwvb || st == Jjy40 || st == Jjy60 || st == Bpm) {
        put(a, kWMin, 7, c.mi); put(a, kWHour, 6, c.h); put(a, kWDoy, 10, c.doy);
        if (st == Wwvb || st == Bpm) {
            const int d = o.dut1;
            if (d >= 0) { a[36] = 1; a[38] = 1; } else a[37] = 1;
            const Field du[] = {{40, 8}, {41, 4}, {42, 2}, {43, 1}};
            put(a, du, 4, d < 0 ? -d : d);
            const Field yr[] = {{45, 80}, {46, 40}, {47, 20}, {48, 10}, {50, 8}, {51, 4}, {52, 2}, {53, 1}};
            put(a, yr, 8, c.y % 100);
            a[55] = isLeap(c.y); a[56] = o.leapAnnounce;
            int py, pm, pd;
            civilFromDays(daysFromCivil(c.y, c.mo, c.d) - 1, py, pm, pd);
            if (st == Bpm) { a[57] = o.ut1; a[58] = 0; }
            else { a[57] = usDstOnDate(c.y, c.mo, c.d); a[58] = usDstOnDate(py, pm, pd); }
        } else {
            a[36] = parity(a, 12, 18); a[37] = parity(a, 1, 8);
            const Field yr[] = {{41, 80}, {42, 40}, {43, 20}, {44, 10}, {45, 8}, {46, 4}, {47, 2}, {48, 1}};
            put(a, yr, 8, c.y % 100);
            const Field wd[] = {{50, 4}, {51, 2}, {52, 1}};
            put(a, wd, 3, c.wd % 7);
            a[53] = o.leapAnnounce; a[54] = o.leapAnnounce && o.leapSign > 0;
        }
        std::vector<uint8_t> s = syms(a, nullptr);
        for (int m : kMarks) s[m] = SMark;
        return s;
    }
    if (st == Wwv) {
        int py, pm, pd;
        civilFromDays(daysFromCivil(c.y, c.mo, c.d) - 1, py, pm, pd);
        a[2] = usDstOnDate(py, pm, pd); a[3] = o.leapAnnounce;
        const Field yu[] = {{7, 8}, {6, 4}, {5, 2}, {4, 1}};
        const Field yt[] = {{54, 80}, {53, 40}, {52, 20}, {51, 10}};
        const Field mi[] = {{17, 40}, {16, 20}, {15, 10}, {13, 8}, {12, 4}, {11, 2}, {10, 1}};
        const Field hr[] = {{26, 20}, {25, 10}, {23, 8}, {22, 4}, {21, 2}, {20, 1}};
        const Field dy[] = {{41, 200}, {40, 100}, {38, 80}, {37, 40}, {36, 20}, {35, 10}, {33, 8}, {32, 4}, {31, 2}, {30, 1}};
        put(a, yu, 4, c.y % 10); put(a, yt, 4, c.y % 100 / 10 * 10); put(a, mi, 7, c.mi); put(a, hr, 6, c.h); put(a, dy, 10, c.doy);
        a[50] = o.dut1 >= 0;
        const Field du[] = {{58, 4}, {57, 2}, {56, 1}};
        put(a, du, 3, std::min(7, o.dut1 < 0 ? -o.dut1 : o.dut1));
        a[55] = usDstOnDate(c.y, c.mo, c.d);
        std::vector<uint8_t> s = syms(a, nullptr);
        for (int m : kMarks) s[m] = SMark;
        s[0] = SNone;
        return s;
    }
    if (st == Bpc) {
        std::vector<uint8_t> s;
        for (int i = 0; i < 3; i++) { const std::vector<uint8_t> f = encodeBpcFrame(M + 20 * i); s.insert(s.end(), f.begin(), f.end()); }
        return s;
    }
    if (st == Hla) {
        const Civil c = civil(M + 9 * 3600);
        const Field mi[] = {{17, 40}, {16, 20}, {15, 10}, {13, 8}, {12, 4}, {11, 2}, {10, 1}};
        const Field hr[] = {{26, 20}, {25, 10}, {23, 8}, {22, 4}, {21, 2}, {20, 1}};
        const Field dy[] = {{41, 200}, {40, 100}, {38, 80}, {37, 40}, {36, 20}, {35, 10}, {33, 8}, {32, 4}, {31, 2}, {30, 1}};
        put(a, mi, 7, c.mi); put(a, hr, 6, c.h); put(a, dy, 10, c.doy);
        std::vector<uint8_t> s = syms(a, nullptr);
        for (int m = 9; m < 60; m += 10) s[(size_t)m] = SMark;
        s[0] = SNone;
        return s;
    }
    return std::vector<uint8_t>(60, SUnknown);
}

std::vector<uint8_t> encodeBpcFrame(int64_t t) {
    const int64_t M = t - (((t % 60) + 60) % 60);
    const int idx = (int)((t - M) / 20);
    const Civil c = civil(t + 8 * 3600);
    std::vector<uint8_t> s(20, 0);
    const int h12 = c.h % 12, pm = c.h >= 12, yy = c.y % 100;
    s[0] = SNone;
    s[1] = (uint8_t)idx;
    s[3] = (uint8_t)(h12 >> 2); s[4] = (uint8_t)(h12 & 3);
    s[5] = (uint8_t)(c.mi >> 4); s[6] = (uint8_t)(c.mi >> 2 & 3); s[7] = (uint8_t)(c.mi & 3);
    s[8] = (uint8_t)(c.wd >> 2); s[9] = (uint8_t)(c.wd & 3);
    s[10] = (uint8_t)(pm << 1 | (bpcOnes(s.data(), 1, 9) & 1));
    s[11] = (uint8_t)(c.d >> 4); s[12] = (uint8_t)(c.d >> 2 & 3); s[13] = (uint8_t)(c.d & 3);
    s[14] = (uint8_t)(c.mo >> 2); s[15] = (uint8_t)(c.mo & 3);
    s[16] = (uint8_t)(yy >> 4 & 3); s[17] = (uint8_t)(yy >> 2 & 3); s[18] = (uint8_t)(yy & 3);
    s[19] = (uint8_t)((yy >> 6 & 1) << 1 | (bpcOnes(s.data(), 11, 18) & 1));
    return s;
}

std::vector<uint8_t> encodeBpmcSecond(int64_t u, double dut1Ms) {
    const Civil c = civil(u);
    const int sec = (int)(((u % 60) + 60) % 60);
    int b[52] = {};
    b[0] = 1;
    putBits(b, 2, 6, sec); b[8] = oddPc(b, 2, 6);
    putBits(b, 9, 6, c.mi); b[15] = oddPc(b, 9, 6);
    putBits(b, 16, 5, c.h); b[21] = oddPc(b, 16, 5);
    putBits(b, 22, 5, c.d); b[27] = oddPc(b, 22, 5);
    putBits(b, 28, 4, c.mo); b[32] = oddPc(b, 28, 4);
    putBits(b, 33, 6, c.y % 100); b[39] = oddPc(b, 33, 6);
    const int mag = std::min(1023, (int)std::floor(std::fabs(dut1Ms)));
    putBits(b, 40, 10, mag); b[50] = dut1Ms >= 0; b[51] = oddPc(b, 40, 11);
    return std::vector<uint8_t>(b, b + 52);
}
std::vector<uint8_t> encodeBpmcMinute(int dut1Tenth, int leapCount, bool ls) {
    int b[46] = {};
    for (int i = 0; i < 30; i++) b[i] = 1;
    putBits(b, 32, 4, dut1Tenth); b[36] = oddPc(b, 32, 4);
    putBits(b, 37, 6, leapCount); b[43] = oddPc(b, 37, 6);
    b[44] = ls; b[45] = oddPc(b, 44, 1);
    return std::vector<uint8_t>(b, b + 46);
}

bool decodeBpmcSecond(const uint8_t* e, Fields& f, int yearHint) {
    f = Fields();
    int b[52];
    for (int i = 0; i < 52; i++) b[i] = e[i] ? 1 : 0;
    if (!b[0] || b[1]) return false;
    if (oddPc(b, 2, 6) != b[8] || oddPc(b, 9, 6) != b[15] || oddPc(b, 16, 5) != b[21] || oddPc(b, 22, 5) != b[27] || oddPc(b, 28, 4) != b[32] ||
        oddPc(b, 33, 6) != b[39] || oddPc(b, 40, 11) != b[51]) return false;
    const int sec = bitsOf(b, 2, 6), mi = bitsOf(b, 9, 6), h = bitsOf(b, 16, 5), d = bitsOf(b, 22, 5), mo = bitsOf(b, 28, 4), yy = bitsOf(b, 33, 6);
    const int mag = bitsOf(b, 40, 10);
    if (sec > 59 || mi > 59 || h > 23 || mo < 1 || mo > 12) return false;
    f.year = fullYear(yy, yearHint);
    if (d < 1 || d > daysInMonth(f.year, mo)) return false;
    f.month = mo; f.day = d; f.hour = h; f.minute = mi; f.second = sec;
    f.doy = (int)(daysFromCivil(f.year, mo, d) - daysFromCivil(f.year, 1, 1)) + 1;
    f.weekday = weekdayOf(daysFromCivil(f.year, mo, d));
    f.dut1Ms = b[50] ? mag : -mag;
    f.dut1Neg = !b[50];
    f.hasDut1Ms = true;
    f.utcMinute = daysFromCivil(f.year, mo, d) * 86400 + h * 3600 + mi * 60;
    return true;
}
bool decodeBpmcMinute(const uint8_t* e, int& dut1Tenth, int& leapCount, bool& ls) {
    int b[46];
    for (int i = 0; i < 46; i++) b[i] = e[i] ? 1 : 0;
    for (int i = 0; i < 30; i++) if (!b[i]) return false;
    if (b[30] || b[31]) return false;
    if (oddPc(b, 32, 4) != b[36] || oddPc(b, 37, 6) != b[43] || oddPc(b, 44, 1) != b[45]) return false;
    dut1Tenth = bitsOf(b, 32, 4); leapCount = bitsOf(b, 37, 6); ls = b[44] != 0;
    return dut1Tenth <= 9;
}

static bool decodeBpc(const uint8_t* sym, Fields& f, int yearHint) {
    f = Fields();
        if (sym[0] != SNone) return false;
        for (int i = 1; i < 20; i++) if (sym[i] > 3) return false;
        const int idx = sym[1], ones19 = bpcOnes(sym, 1, 9), ones1118 = bpcOnes(sym, 11, 18);
        if (idx > 2) return false;
        if ((ones19 & 1) != (sym[10] & 1) || (ones1118 & 1) != (sym[19] & 1)) return false;
        const int h12 = sym[3] * 4 + sym[4], mi = sym[5] * 16 + sym[6] * 4 + sym[7], wd = sym[8] * 4 + sym[9];
        const int d = sym[11] * 16 + sym[12] * 4 + sym[13], mo = sym[14] * 4 + sym[15];
        const int yy = (sym[19] >> 1) * 64 + sym[16] * 16 + sym[17] * 4 + sym[18];
        if (h12 > 11 || mi > 59 || wd < 1 || wd > 7 || mo < 1 || mo > 12 || yy > 99) return false;
        f.year = fullYear(yy, yearHint);
        if (d < 1 || d > daysInMonth(f.year, mo)) return false;
        f.month = mo; f.day = d; f.minute = mi; f.weekday = wd;
        f.hour = h12 + ((sym[10] >> 1) ? 12 : 0);
        if (weekdayOf(daysFromCivil(f.year, mo, d)) != wd) return false;
        f.doy = (int)(daysFromCivil(f.year, mo, d) - daysFromCivil(f.year, 1, 1)) + 1;
        f.utcOffsetMin = 8 * 60;
        f.frameIndex = idx; f.second = idx * 20;
        f.utcMinute = daysFromCivil(f.year, mo, d) * 86400 + f.hour * 3600 + mi * 60 - f.utcOffsetMin * 60 + idx * 20;
        return true;
}

bool decodeMinute(int st, const uint8_t* sym, Fields& f, int yearHint) {
    if (st == Bpc) return decodeBpc(sym, f, yearHint);
    f = Fields();
    if (st == Rwm) {   // the minute pulse, then single or doubled pulses; the 10 pulse minutes carry no DUT1
        if (sym[0] != SMark) return false;
        bool ten = false;
        for (int i = 1; i < 60; i++) {
            if (sym[i] == SMark || sym[i] == SUnknown || sym[i] == SNone || sym[i] == SAB) return false;
            ten |= sym[i] == SB;
        }
        if (ten) return true;
        for (int i = 17; i < 60; i++) if (sym[i] == S1 && !((i >= 21 && i <= 24) || (i >= 31 && i <= 34))) return false;
        if (!rwmValue(sym, 1, 9, 8, f.dut1) || !rwmValue(sym, 21, 31, 4, f.dUt1)) return false;
        return true;
    }
    int a[60] = {}, bb[60] = {};
    for (int i = 0; i < 60; i++) {
        if (sym[i] == SUnknown) return false;
        a[i] = sym[i] < SMark ? (sym[i] & 1) : 0;
        bb[i] = sym[i] < SMark ? (sym[i] >> 1 & 1) : 0;
        if (st != Msf && (sym[i] == SB || sym[i] == SAB)) return false;
    }
    auto isMark = [&](int i) { return sym[i] == SMark; };
    if (st == Dcf77) {
        if (sym[59] != SNone) return false;
        for (int i = 0; i < 59; i++) if (sym[i] >= SMark) return false;
        if (a[0] != 0 || a[20] != 1 || a[17] == a[18]) return false;
        if (parity(a, 21, 28) || parity(a, 29, 35) || parity(a, 36, 58)) return false;
        const Field mi[] = {{27, 40}, {26, 20}, {25, 10}, {24, 8}, {23, 4}, {22, 2}, {21, 1}};
        const Field hr[] = {{34, 20}, {33, 10}, {32, 8}, {31, 4}, {30, 2}, {29, 1}};
        const Field dy[] = {{41, 20}, {40, 10}, {39, 8}, {38, 4}, {37, 2}, {36, 1}};
        const Field wd[] = {{44, 4}, {43, 2}, {42, 1}};
        const Field mo[] = {{49, 10}, {48, 8}, {47, 4}, {46, 2}, {45, 1}};
        const Field yr[] = {{57, 80}, {56, 40}, {55, 20}, {54, 10}, {53, 8}, {52, 4}, {51, 2}, {50, 1}};
        int yy;
        if (!get(a, mi, 7, f.minute) || !get(a, hr, 6, f.hour) || !get(a, dy, 6, f.day) || !get(a, wd, 3, f.weekday) || !get(a, mo, 5, f.month) || !get(a, yr, 8, yy)) return false;
        f.year = fullYear(yy, yearHint);
        if (f.minute > 59 || f.hour > 23 || f.month < 1 || f.month > 12 || f.day < 1 || f.day > daysInMonth(f.year, f.month) || f.weekday < 1) return false;
        if (weekdayOf(daysFromCivil(f.year, f.month, f.day)) != f.weekday) return false;
        f.dst = a[17]; f.utcOffsetMin = a[17] ? 120 : 60;
        f.dstAnnounce = a[16]; f.leapAnnounce = a[19]; f.callBit = a[15];
        f.doy = (int)(daysFromCivil(f.year, f.month, f.day) - daysFromCivil(f.year, 1, 1)) + 1;
        f.utcMinute = daysFromCivil(f.year, f.month, f.day) * 86400 + f.hour * 3600 + f.minute * 60 - f.utcOffsetMin * 60 - 60;
        return true;
    }
    if (st == Msf) {
        if (!isMark(0)) return false;
        for (int i = 1; i < 60; i++) if (sym[i] >= SMark) return false;
        static const int kM[] = {0, 1, 1, 1, 1, 1, 1, 0};
        for (int i = 0; i < 8; i++) if (a[52 + i] != kM[i]) return false;
        for (int i = 1; i <= 16; i++) if (a[i]) return false;
        for (int i = 17; i <= 52; i++) if (bb[i]) return false;
        if (bb[59]) return false;
        if (!(parity(a, 17, 24) ^ bb[54]) || !(parity(a, 25, 35) ^ bb[55]) || !(parity(a, 36, 38) ^ bb[56]) || !(parity(a, 39, 51) ^ bb[57])) return false;
        const Field yr[] = {{17, 80}, {18, 40}, {19, 20}, {20, 10}, {21, 8}, {22, 4}, {23, 2}, {24, 1}};
        const Field mo[] = {{25, 10}, {26, 8}, {27, 4}, {28, 2}, {29, 1}};
        const Field dy[] = {{30, 20}, {31, 10}, {32, 8}, {33, 4}, {34, 2}, {35, 1}};
        const Field wd[] = {{36, 4}, {37, 2}, {38, 1}};
        const Field hr[] = {{39, 20}, {40, 10}, {41, 8}, {42, 4}, {43, 2}, {44, 1}};
        const Field mi[] = {{45, 40}, {46, 20}, {47, 10}, {48, 8}, {49, 4}, {50, 2}, {51, 1}};
        int yy, w;
        if (!get(a, yr, 8, yy) || !get(a, mo, 5, f.month) || !get(a, dy, 6, f.day) || !get(a, wd, 3, w) || !get(a, hr, 6, f.hour) || !get(a, mi, 7, f.minute)) return false;
        f.year = fullYear(yy, yearHint);
        if (f.minute > 59 || f.hour > 23 || f.month < 1 || f.month > 12 || f.day < 1 || f.day > daysInMonth(f.year, f.month) || w > 6) return false;
        f.weekday = w == 0 ? 7 : w;
        if (weekdayOf(daysFromCivil(f.year, f.month, f.day)) != f.weekday) return false;
        int pos = 0, neg = 0;
        for (int i = 1; i <= 8; i++) pos += bb[i];
        for (int i = 9; i <= 16; i++) neg += bb[i];
        if (pos && neg) return false;
        f.dut1 = pos - neg;
        f.dst = bb[58]; f.utcOffsetMin = bb[58] ? 60 : 0; f.dstAnnounce = bb[53];
        f.doy = (int)(daysFromCivil(f.year, f.month, f.day) - daysFromCivil(f.year, 1, 1)) + 1;
        f.utcMinute = daysFromCivil(f.year, f.month, f.day) * 86400 + f.hour * 3600 + f.minute * 60 - f.utcOffsetMin * 60 - 60;
        return true;
    }
    if (st == Wwvb || st == Jjy40 || st == Jjy60 || st == Wwv || st == Bpm) {
        for (int i = 0; i < 60; i++) {
            bool m = false;
            for (int k : kMarks) if (k == i) m = true;
            if (st == Wwv && i == 0) { if (sym[0] != SNone) return false; continue; }
            if (m != isMark(i) || sym[i] == SNone) return false;
        }
        int doy = 0;
        if (st == Wwv) {
            for (int i : {1, 8, 14, 18, 24, 27, 28, 34, 42, 43, 44, 45, 46, 47, 48}) if (a[i]) return false;
            const Field yu[] = {{7, 8}, {6, 4}, {5, 2}, {4, 1}};
            const Field yt[] = {{54, 80}, {53, 40}, {52, 20}, {51, 10}};
            const Field mi[] = {{17, 40}, {16, 20}, {15, 10}, {13, 8}, {12, 4}, {11, 2}, {10, 1}};
            const Field hr[] = {{26, 20}, {25, 10}, {23, 8}, {22, 4}, {21, 2}, {20, 1}};
            const Field dy[] = {{41, 200}, {40, 100}, {38, 80}, {37, 40}, {36, 20}, {35, 10}, {33, 8}, {32, 4}, {31, 2}, {30, 1}};
            int u, t;
            if (!get(a, yu, 4, u) || !get(a, yt, 4, t) || !get(a, mi, 7, f.minute) || !get(a, hr, 6, f.hour) || !get(a, dy, 10, doy)) return false;
            if (u > 9) return false;
            f.year = fullYear(t + u, yearHint);
            f.dstCode = a[55] << 1 | a[2];
            f.leapAnnounce = a[3];
            const int du = a[56] + 2 * a[57] + 4 * a[58];
            f.dut1 = a[50] ? du : -du;
        } else {
            const bool jjy = st != Wwvb && st != Bpm;
            for (int i : {4, 10, 11, 14, 20, 21, 24, 34, 35}) if (a[i]) return false;
            if (!get(a, kWMin, 7, f.minute) || !get(a, kWHour, 6, f.hour) || !get(a, kWDoy, 10, doy)) return false;
            if (jjy) {
                for (int i : {38, 40, 55, 56, 57, 58}) if (a[i]) return false;
                if (a[36] != parity(a, 12, 18) || a[37] != parity(a, 1, 8)) return false;
                if (f.minute == 15 || f.minute == 45) {   // the call sign replaces the year and weekday
                    f.year = yearHint > 0 ? yearHint : curYear();
                } else {
                    const Field yr[] = {{41, 80}, {42, 40}, {43, 20}, {44, 10}, {45, 8}, {46, 4}, {47, 2}, {48, 1}};
                    const Field wd[] = {{50, 4}, {51, 2}, {52, 1}};
                    int yy, w;
                    if (!get(a, yr, 8, yy) || !get(a, wd, 3, w) || w > 6) return false;
                    f.year = fullYear(yy, yearHint);
                    f.weekday = w == 0 ? 7 : w;
                }
                f.leapAnnounce = a[53]; f.leapSign = a[53] ? (a[54] ? 1 : -1) : 0;
                f.utcOffsetMin = 9 * 60;
            } else {
                if (a[44] || a[54]) return false;
                const bool plus = a[36] && !a[37] && a[38], minus = !a[36] && a[37] && !a[38];
                if (!plus && !minus) return false;
                const Field du[] = {{40, 8}, {41, 4}, {42, 2}, {43, 1}};
                const Field yr[] = {{45, 80}, {46, 40}, {47, 20}, {48, 10}, {50, 8}, {51, 4}, {52, 2}, {53, 1}};
                int d, yy;
                if (!get(a, du, 4, d) || !get(a, yr, 8, yy)) return false;
                f.dut1 = plus ? d : -d;
                f.year = fullYear(yy, yearHint);
                f.leapYear = a[55];
                if (f.leapYear != isLeap(f.year)) return false;
                f.leapAnnounce = a[56];
                if (st == Bpm) f.ut1 = a[57] != 0; else f.dstCode = a[57] << 1 | a[58];
            }
        }
        if (f.minute > 59 || f.hour > 23 || !doyOk(doy, doy / 100) || doy > (isLeap(f.year) ? 366 : 365)) return false;
        f.doy = doy;
        const int64_t days = daysFromCivil(f.year, 1, 1) + doy - 1;
        civilFromDays(days, f.year, f.month, f.day);
        if (f.weekday > 0 && weekdayOf(days) != f.weekday) return false;
        if (f.weekday < 0) f.weekday = weekdayOf(days);
        if (f.dstCode >= 0) f.dst = f.dstCode == 3 || f.dstCode == 2 ? (f.dstCode == 3 ? 1 : 0) : (f.dstCode == 1 ? 1 : 0);
        f.utcMinute = days * 86400 + f.hour * 3600 + f.minute * 60 - f.utcOffsetMin * 60;
        return true;
    }
    if (st == Hla) {
        if (sym[0] != SNone) return false;
        for (int i = 1; i < 60; i++) {
            const bool m = i % 10 == 9;
            if (m != isMark(i) || sym[i] == SNone) return false;
        }
        for (int i = 1; i <= 8; i++) if (a[i]) return false;
        for (int i : {14, 18, 24, 27, 28, 34, 42, 43, 44, 45, 46, 47, 48, 50, 51, 52, 53, 54, 55, 56, 57, 58}) if (a[i]) return false;
        const Field mi[] = {{17, 40}, {16, 20}, {15, 10}, {13, 8}, {12, 4}, {11, 2}, {10, 1}};
        const Field hr[] = {{26, 20}, {25, 10}, {23, 8}, {22, 4}, {21, 2}, {20, 1}};
        const Field dy[] = {{41, 200}, {40, 100}, {38, 80}, {37, 40}, {36, 20}, {35, 10}, {33, 8}, {32, 4}, {31, 2}, {30, 1}};
        int doy;
        if (!get(a, mi, 7, f.minute) || !get(a, hr, 6, f.hour) || !get(a, dy, 10, doy)) return false;
        f.year = yearHint > 0 ? yearHint : curYear();
        if (f.minute > 59 || f.hour > 23 || !doyOk(doy, doy / 100) || doy > (isLeap(f.year) ? 366 : 365)) return false;
        f.doy = doy;
        const int64_t days = daysFromCivil(f.year, 1, 1) + doy - 1;
        civilFromDays(days, f.year, f.month, f.day);
        f.weekday = weekdayOf(days);
        f.utcOffsetMin = 9 * 60;
        f.utcMinute = days * 86400 + f.hour * 3600 + f.minute * 60 - f.utcOffsetMin * 60;
        return true;
    }
    return false;
}

std::string fieldsText(const Fields& f, int st) {
    char b[112];
    const int64_t u = f.utcMinute + ((st == Dcf77 || st == Msf) ? 60 : 0) + (st == BpmC && f.second > 0 ? f.second : 0);   // the minute the frame names
    const Civil c = civil(u);
    const bool secs = st == Bpc || st == BpmC;
    int n = secs ? snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d UTC", c.y, c.mo, c.d, c.h, c.mi, (int)(((u % 60) + 60) % 60))
                 : snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d UTC", c.y, c.mo, c.d, c.h, c.mi);
    if (f.utcOffsetMin) {
        const char* z = st == Dcf77 ? (f.dst ? "CEST" : "CET") : st == Msf ? (f.dst ? "BST" : "GMT") : st == Bpc ? "CST" : st == Hla ? "KST" : "JST";
        if (secs) snprintf(b + n, sizeof b - (size_t)n, " (%02d:%02d:%02d %s)", f.hour, f.minute, f.second >= 0 ? f.second : 0, z);
        else snprintf(b + n, sizeof b - (size_t)n, " (%02d:%02d %s)", f.hour, f.minute, z);
    } else if (st == Msf) {
        snprintf(b + n, sizeof b - (size_t)n, " (GMT)");
    } else if (st == BpmC && f.hasDut1Ms) {
        snprintf(b + n, sizeof b - (size_t)n, ", DUT1 %+.1f ms", f.dut1Ms);
    }
    if (st == Rwm) {   // no time of day on air: the minute is the computer's
        if (f.dut1 != 99) snprintf(b + n, sizeof b - (size_t)n, ", DUT1 %+.1f s, dUT1 %+.2f s", f.dut1 * 0.1, f.dUt1 != 99 ? f.dUt1 * 0.02 : 0.0);
        else snprintf(b + n, sizeof b - (size_t)n, ", 10 pulse minute (no DUT1)");
    }
    return b;
}

std::vector<uint8_t> chuFrameA(int doy, int hour, int minute, int second) {
    const int d[10] = {6, doy / 100, doy / 10 % 10, doy % 10, hour / 10, hour % 10, minute / 10, minute % 10, second / 10, second % 10};
    std::vector<uint8_t> b(10);
    for (int i = 0; i < 5; i++) b[i] = b[i + 5] = (uint8_t)(d[2 * i] | d[2 * i + 1] << 4);
    return b;
}
std::vector<uint8_t> chuFrameB(int year) {
    const int d[10] = {0, 0, year / 1000 % 10, year / 100 % 10, year / 10 % 10, year % 10, 3, 7, 0, 0};
    std::vector<uint8_t> b(10);
    for (int i = 0; i < 5; i++) { b[i] = (uint8_t)(d[2 * i] | d[2 * i + 1] << 4); b[i + 5] = (uint8_t)~b[i]; }
    return b;
}
bool chuParseA(const uint8_t* b, int& doy, int& hour, int& minute, int& second) {
    for (int i = 0; i < 5; i++) if (b[i] != b[i + 5]) return false;
    int d[10];
    for (int i = 0; i < 5; i++) { d[2 * i] = b[i] & 15; d[2 * i + 1] = b[i] >> 4; }
    for (int x : d) if (x > 9) return false;
    if (d[0] != 6) return false;
    doy = d[1] * 100 + d[2] * 10 + d[3]; hour = d[4] * 10 + d[5]; minute = d[6] * 10 + d[7]; second = d[8] * 10 + d[9];
    return doy >= 1 && doy <= 366 && hour <= 23 && minute <= 59 && second <= 60;
}
bool chuIsB(const uint8_t* b) {
    for (int i = 0; i < 5; i++) if ((uint8_t)~b[i] != b[i + 5]) return false;
    return true;
}

void stationSite(int s, double& lat, double& lon, bool hawaii) {
    switch (s) {
    case Dcf77: lat = 50.0156; lon = 9.0108; break;      // Mainflingen
    case Msf: lat = 54.9113; lon = -3.2786; break;       // Anthorn
    case Wwvb: lat = 40.6781; lon = -105.0472; break;    // Fort Collins
    case Jjy40: lat = 37.3725; lon = 140.8489; break;    // Ohtakadoya-yama
    case Jjy60: lat = 33.4653; lon = 130.1756; break;    // Hagane-yama
    case Wwv: if (hawaii) { lat = 21.9878; lon = -159.7650; } else { lat = 40.6798; lon = -105.0418; } break;   // Kauai / Fort Collins
    case Chu: lat = 45.2950; lon = -75.7536; break;      // Ottawa
    case Bpm: case BpmC: lat = 34.949; lon = 109.543; break;   // Pucheng
    case Bpc: lat = 34.457; lon = 115.837; break;        // Shangqiu
    case Hla: lat = 36.387; lon = 127.368; break;        // Daejeon
    case Rwm: lat = 56.733; lon = 37.633; break;         // Taldom, Moscow region
    default: lat = lon = 0; break;
    }
}
double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double r = dect2::math::kPi / 180;
    const double p1 = lat1 * r, p2 = lat2 * r, dp = (lat2 - lat1) * r, dl = (lon2 - lon1) * r;
    const double h = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(h)));
}
double propagationMs(int s, double km) {
    constexpr double c = 299.792458;   // km per ms
    if (!stationIsHf(s)) return km / c;
    const int n = std::max(1, (int)std::ceil(km / 4000));
    const double hop = km / n;
    return n * 2 * std::sqrt(hop * hop / 4 + 300.0 * 300.0) / c;
}

} // namespace timesig
} // namespace dect2
