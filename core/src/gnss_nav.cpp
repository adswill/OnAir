// GPS navigation message and the models of IS-GPS-200 (see gnss_nav.h).
#include "dect2/gnss_nav.h"
#include <cmath>

namespace dect2 {

// ------------------------------------------------------------------ parity (IS-GPS-200 Table 20-XIV)
// the source bits (1 based) that go into each parity bit
static const uint8_t kPar25[] = {1, 2, 3, 5, 6, 10, 11, 12, 13, 14, 17, 18, 20, 23};
static const uint8_t kPar26[] = {2, 3, 4, 6, 7, 11, 12, 13, 14, 15, 18, 19, 21, 24};
static const uint8_t kPar27[] = {1, 3, 4, 5, 7, 8, 12, 13, 14, 15, 16, 19, 20, 22};
static const uint8_t kPar28[] = {2, 4, 5, 6, 8, 9, 13, 14, 15, 16, 17, 20, 21, 23};
static const uint8_t kPar29[] = {1, 3, 5, 6, 7, 9, 10, 14, 15, 16, 17, 18, 21, 22, 24};
static const uint8_t kPar30[] = {3, 5, 6, 8, 9, 10, 11, 13, 15, 19, 22, 23, 24};

template <size_t N> static uint32_t maskOf(const uint8_t (&l)[N]) {
    uint32_t m = 0;
    for (size_t i = 0; i < N; i++) m |= 1u << (24 - l[i]);
    return m;
}
static int parity32(uint32_t v) { return __builtin_parity(v); }

uint32_t lnavParity(uint32_t d24, int d29, int d30) {
    static const uint32_t m25 = maskOf(kPar25), m26 = maskOf(kPar26), m27 = maskOf(kPar27), m28 = maskOf(kPar28), m29 = maskOf(kPar29), m30 = maskOf(kPar30);
    const uint32_t p25 = (uint32_t)(d29 ^ parity32(d24 & m25));
    const uint32_t p26 = (uint32_t)(d30 ^ parity32(d24 & m26));
    const uint32_t p27 = (uint32_t)(d29 ^ parity32(d24 & m27));
    const uint32_t p28 = (uint32_t)(d30 ^ parity32(d24 & m28));
    const uint32_t p29 = (uint32_t)(d30 ^ parity32(d24 & m29));
    const uint32_t p30 = (uint32_t)(d29 ^ parity32(d24 & m30));
    return p25 << 5 | p26 << 4 | p27 << 3 | p28 << 2 | p29 << 1 | p30;
}

bool lnavCheckWord(const uint8_t* r, int prevD29, int prevD30, uint32_t* d24) {
    uint32_t d = 0;
    for (int i = 0; i < 24; i++) d = d << 1 | (uint32_t)((r[i] ^ prevD30) & 1);
    uint32_t p = 0;
    for (int i = 24; i < 30; i++) p = p << 1 | (uint32_t)(r[i] & 1);
    if (lnavParity(d, prevD29 & 1, prevD30 & 1) != p) return false;
    *d24 = d;
    return true;
}

bool lnavDecodeSubframe(const uint8_t* bits, int prevD29, int prevD30, LnavSubframe& out) {
    int p29 = prevD29, p30 = prevD30;
    for (int w = 0; w < 10; w++) {
        if (!lnavCheckWord(bits + 30 * w, p29, p30, &out.w[w])) return false;
        p29 = bits[30 * w + 28]; p30 = bits[30 * w + 29];
    }
    return true;
}

bool lnavDecodeSubframeAuto(const uint8_t* bits, LnavSubframe& out) {
    int p29 = 0, p30 = 0;       // true polarity of the previous word's last bits
    for (int w = 0; w < 10; w++) {
        uint8_t x[30];
        bool ok = false;
        for (int inv = 0; inv < 2 && !ok; inv++) {
            for (int i = 0; i < 30; i++) x[i] = (uint8_t)(bits[30 * w + i] ^ inv);
            ok = lnavCheckWord(x, p29, p30, &out.w[w]);
        }
        if (!ok) return false;
        p29 = x[28]; p30 = x[29];
    }
    return true;
}

uint32_t lnavField(const LnavSubframe& s, int first, int len) {
    uint32_t v = 0;
    for (int k = 0; k < len; k++) {
        const int p = first + k - 1;             // 0 based position in the 300 bits
        const int word = p / 30, off = p % 30;
        v = v << 1 | ((s.w[word] >> (23 - off)) & 1u);
    }
    return v;
}
static int32_t signExtend(uint32_t v, int len) {
    if (len < 32 && (v >> (len - 1) & 1u)) v |= ~0u << len;
    return (int32_t)v;
}
int32_t lnavFieldSigned(const LnavSubframe& s, int first, int len) { return signExtend(lnavField(s, first, len), len); }
uint32_t lnavField2(const LnavSubframe& s, int b1, int l1, int b2, int l2) {
    const uint64_t v = (uint64_t)lnavField(s, b1, l1) << l2 | lnavField(s, b2, l2);
    return (uint32_t)v;
}
int32_t lnavField2Signed(const LnavSubframe& s, int b1, int l1, int b2, int l2) { return signExtend(lnavField2(s, b1, l1, b2, l2), l1 + l2); }

unsigned lnavSubframeId(const LnavSubframe& s) { return lnavField(s, 50, 3); }
unsigned lnavTowCount(const LnavSubframe& s) { return lnavField(s, 31, 17); }

// ------------------------------------------------------------------ decoding (IS-GPS-200 Tables 20-I, 20-II, 20-III and Figure 20-1)
static double pw2(int e) { return std::ldexp(1.0, e); }

bool lnavParseSf1(const LnavSubframe& s, GpsEphemeris& e) {
    if (lnavSubframeId(s) != 1) return false;
    e.wn = (int)lnavField(s, 61, 10);
    e.uraIndex = (int)lnavField(s, 73, 4);
    e.health = (int)lnavField(s, 77, 6);
    e.iodc = (int)lnavField2(s, 83, 2, 211, 8);
    e.tgd = lnavFieldSigned(s, 197, 8) * pw2(-31);
    e.toc = lnavField(s, 219, 16) * 16.0;
    e.af2 = lnavFieldSigned(s, 241, 8) * pw2(-55);
    e.af1 = lnavFieldSigned(s, 249, 16) * pw2(-43);
    e.af0 = lnavFieldSigned(s, 271, 22) * pw2(-31);
    e.has1 = true;
    return true;
}

bool lnavParseSf2(const LnavSubframe& s, GpsEphemeris& e) {
    if (lnavSubframeId(s) != 2) return false;
    e.iode2 = (int)lnavField(s, 61, 8);
    e.crs = lnavFieldSigned(s, 69, 16) * pw2(-5);
    e.dn = lnavFieldSigned(s, 91, 16) * pw2(-43) * kPi;
    e.m0 = lnavField2Signed(s, 107, 8, 121, 24) * pw2(-31) * kPi;
    e.cuc = lnavFieldSigned(s, 151, 16) * pw2(-29);
    e.e = lnavField2(s, 167, 8, 181, 24) * pw2(-33);
    e.cus = lnavFieldSigned(s, 211, 16) * pw2(-29);
    e.sqrtA = lnavField2(s, 227, 8, 241, 24) * pw2(-19);
    e.toe = lnavField(s, 271, 16) * 16.0;
    e.fitFlag = (int)lnavField(s, 287, 1);
    e.has2 = true;
    return true;
}

bool lnavParseSf3(const LnavSubframe& s, GpsEphemeris& e) {
    if (lnavSubframeId(s) != 3) return false;
    e.cic = lnavFieldSigned(s, 61, 16) * pw2(-29);
    e.omega0 = lnavField2Signed(s, 77, 8, 91, 24) * pw2(-31) * kPi;
    e.cis = lnavFieldSigned(s, 121, 16) * pw2(-29);
    e.i0 = lnavField2Signed(s, 137, 8, 151, 24) * pw2(-31) * kPi;
    e.crc = lnavFieldSigned(s, 181, 16) * pw2(-5);
    e.omega = lnavField2Signed(s, 197, 8, 211, 24) * pw2(-31) * kPi;
    e.omegaDot = lnavFieldSigned(s, 241, 24) * pw2(-43) * kPi;
    e.iode3 = (int)lnavField(s, 271, 8);
    e.idot = lnavFieldSigned(s, 279, 14) * pw2(-43) * kPi;
    e.has3 = true;
    return true;
}

// Subframes 4 and 5: Table 20-V (almanac), Table 20-VI, Figures 20-1 sheets 4 and 5 (page 18 of subframe 4 carries the ionosphere and UTC data)
int lnavParseAlmanacPage(const LnavSubframe& s, int subframe, int* svIdOut, GpsAlmanac& a, GpsIono& iono, GpsUtc& utc, double* toaOut, int* wnaOut) {
    const unsigned id = lnavSubframeId(s);
    if ((int)id != subframe) return 0;
    const int svId = (int)lnavField(s, 63, 6);
    if (svIdOut) *svIdOut = svId;
    if (svId >= 1 && svId <= 32) {
        a.prn = svId;
        a.e = lnavField(s, 69, 16) * pw2(-21);
        a.toa = lnavField(s, 91, 8) * 4096.0;
        a.i0 = (0.3 + lnavFieldSigned(s, 99, 16) * pw2(-19)) * kPi;
        a.omegaDot = lnavFieldSigned(s, 121, 16) * pw2(-38) * kPi;
        a.health = (int)lnavField(s, 137, 8);
        a.sqrtA = lnavField(s, 151, 24) * pw2(-11);
        a.omega0 = lnavFieldSigned(s, 181, 24) * pw2(-23) * kPi;
        a.omega = lnavFieldSigned(s, 211, 24) * pw2(-23) * kPi;
        a.m0 = lnavFieldSigned(s, 241, 24) * pw2(-23) * kPi;
        a.af0 = lnavField2Signed(s, 271, 8, 290, 3) * pw2(-20);
        a.af1 = lnavFieldSigned(s, 279, 11) * pw2(-38);
        a.valid = a.sqrtA > 1000.0;     // an empty almanac (a satellite that is not in service) is all zero
        return 1;
    }
    if (subframe == 4 && svId == 56) {
        iono.alpha[0] = lnavFieldSigned(s, 69, 8) * pw2(-30);
        iono.alpha[1] = lnavFieldSigned(s, 77, 8) * pw2(-27);
        iono.alpha[2] = lnavFieldSigned(s, 91, 8) * pw2(-24);
        iono.alpha[3] = lnavFieldSigned(s, 99, 8) * pw2(-24);
        iono.beta[0] = lnavFieldSigned(s, 107, 8) * pw2(11);
        iono.beta[1] = lnavFieldSigned(s, 121, 8) * pw2(14);
        iono.beta[2] = lnavFieldSigned(s, 129, 8) * pw2(16);
        iono.beta[3] = lnavFieldSigned(s, 137, 8) * pw2(16);
        iono.valid = true;
        utc.a1 = lnavFieldSigned(s, 151, 24) * pw2(-50);
        utc.a0 = lnavField2Signed(s, 181, 24, 211, 8) * pw2(-30);
        utc.tot = (int)lnavField(s, 219, 8) * 4096;
        utc.wnt = (int)lnavField(s, 227, 8);
        utc.dtls = (int)lnavFieldSigned(s, 241, 8);
        utc.wnlsf = (int)lnavField(s, 249, 8);
        utc.dn = (int)lnavField(s, 257, 8);
        utc.dtlsf = (int)lnavFieldSigned(s, 271, 8);
        utc.valid = true;
        return 2;
    }
    if (subframe == 5 && svId == 51) {
        if (toaOut) *toaOut = lnavField(s, 69, 8) * 4096.0;
        if (wnaOut) *wnaOut = (int)lnavField(s, 77, 8);
        return 3;
    }
    return 0;
}

// ------------------------------------------------------------------ orbits (Table 20-IV)
double gpsWrap(double dt) {
    if (dt > 302400.0) dt -= 604800.0;
    else if (dt < -302400.0) dt += 604800.0;
    return dt;
}

int gpsResolveWeek(int wn10, int ref) {
    int w = wn10 + 1024 * ((ref - wn10 + 512) / 1024);
    return w;
}

static double solveKepler(double M, double e) {
    M = std::fmod(M, 2 * kPi);
    double E = M;
    for (int i = 0; i < 30; i++) {
        const double d = (E - e * std::sin(E) - M) / (1.0 - e * std::cos(E));
        E -= d;
        if (std::fabs(d) < 1e-13) break;
    }
    return E;
}

void gpsEphemerisState(const GpsEphemeris& e, double t, double pos[3], double* clockS, double* eccAnom) {
    const double A = e.sqrtA * e.sqrtA;
    const double n0 = std::sqrt(kGpsMu / (A * A * A));
    const double tk = gpsWrap(t - e.toe);
    const double n = n0 + e.dn;
    const double Mk = e.m0 + n * tk;
    const double Ek = solveKepler(Mk, e.e);
    const double sE = std::sin(Ek), cE = std::cos(Ek);
    const double nu = std::atan2(std::sqrt(1.0 - e.e * e.e) * sE, cE - e.e);
    const double phi = nu + e.omega;
    const double s2 = std::sin(2 * phi), c2 = std::cos(2 * phi);
    const double du = e.cus * s2 + e.cuc * c2;
    const double dr = e.crs * s2 + e.crc * c2;
    const double di = e.cis * s2 + e.cic * c2;
    const double u = phi + du;
    const double r = A * (1.0 - e.e * cE) + dr;
    const double inc = e.i0 + di + e.idot * tk;
    const double xp = r * std::cos(u), yp = r * std::sin(u);
    const double Om = e.omega0 + (e.omegaDot - kEarthRate) * tk - kEarthRate * e.toe;
    const double cO = std::cos(Om), sO = std::sin(Om), ci = std::cos(inc), si = std::sin(inc);
    pos[0] = xp * cO - yp * ci * sO;
    pos[1] = xp * sO + yp * ci * cO;
    pos[2] = yp * si;
    if (clockS) {
        const double dtc = gpsWrap(t - e.toc);
        *clockS = e.af0 + e.af1 * dtc + e.af2 * dtc * dtc + kRelF * e.e * e.sqrtA * sE;
    }
    if (eccAnom) *eccAnom = Ek;
}

void gpsAlmanacPos(const GpsAlmanac& a, double t, double pos[3]) {
    const double A = a.sqrtA * a.sqrtA;
    const double n = std::sqrt(kGpsMu / (A * A * A));
    const double tk = gpsWrap(t - a.toa);
    const double Ek = solveKepler(a.m0 + n * tk, a.e);
    const double nu = std::atan2(std::sqrt(1.0 - a.e * a.e) * std::sin(Ek), std::cos(Ek) - a.e);
    const double u = nu + a.omega;
    const double r = A * (1.0 - a.e * std::cos(Ek));
    const double xp = r * std::cos(u), yp = r * std::sin(u);
    const double Om = a.omega0 + (a.omegaDot - kEarthRate) * tk - kEarthRate * a.toa;
    const double cO = std::cos(Om), sO = std::sin(Om), ci = std::cos(a.i0), si = std::sin(a.i0);
    pos[0] = xp * cO - yp * ci * sO;
    pos[1] = xp * sO + yp * ci * cO;
    pos[2] = yp * si;
}

// ------------------------------------------------------------------ atmosphere
double klobucharDelay(const GpsIono& io, double latDeg, double lonDeg, double azDeg, double elDeg, double tow) {
    const double E = elDeg / 180.0, phiU = latDeg / 180.0, lamU = lonDeg / 180.0;   // semicircles
    const double A = azDeg * kPi / 180.0;
    const double psi = 0.0137 / (E + 0.11) - 0.022;
    double phiI = phiU + psi * std::cos(A);
    if (phiI > 0.416) phiI = 0.416; else if (phiI < -0.416) phiI = -0.416;
    const double lamI = lamU + psi * std::sin(A) / std::cos(phiI * kPi);
    const double phiM = phiI + 0.064 * std::cos((lamI - 1.617) * kPi);
    const double F = 1.0 + 16.0 * std::pow(0.53 - E, 3);
    double t = 4.32e4 * lamI + tow;
    t = std::fmod(t, 86400.0);
    if (t < 0) t += 86400.0;
    double amp = 0, per = 0, p = 1;
    for (int i = 0; i < 4; i++) { amp += io.alpha[i] * p; per += io.beta[i] * p; p *= phiM; }
    if (amp < 0) amp = 0;
    if (per < 72000.0) per = 72000.0;
    const double x = 2 * kPi * (t - 50400.0) / per;
    if (std::fabs(x) < 1.57) return F * (5e-9 + amp * (1.0 - x * x / 2 + x * x * x * x / 24));
    return F * 5e-9;
}

double troposphereDelay(double latDeg, double h, double elDeg) {
    if (h < -100.0 || h > 1e4 || elDeg <= 0) return 0;
    if (h < 0) h = 0;
    const double lat = latDeg * kPi / 180.0;
    const double pres = 1013.25 * std::pow(1.0 - 2.2557e-5 * h, 5.2568);
    const double temp = 15.0 - 6.5e-3 * h + 273.16;
    const double e = 6.108 * 0.7 * std::exp((17.15 * temp - 4684.0) / (temp - 38.45));
    const double z = kPi / 2 - elDeg * kPi / 180.0;
    const double cz = std::cos(z);
    const double trph = 0.0022768 * pres / (1.0 - 0.00266 * std::cos(2 * lat) - 0.00028 * h / 1e3) / cz;
    const double trpw = 0.002277 * (1255.0 / temp + 0.05) * e / cz;
    return trph + trpw;
}

// ------------------------------------------------------------------ coordinates and time
void llaToEcef(double latDeg, double lonDeg, double h, double x[3]) {
    const double la = latDeg * kPi / 180.0, lo = lonDeg * kPi / 180.0;
    const double e2 = kWgsF * (2 - kWgsF);
    const double sl = std::sin(la), cl = std::cos(la);
    const double N = kWgsA / std::sqrt(1 - e2 * sl * sl);
    x[0] = (N + h) * cl * std::cos(lo);
    x[1] = (N + h) * cl * std::sin(lo);
    x[2] = (N * (1 - e2) + h) * sl;
}

void ecefToLla(const double x[3], double* latDeg, double* lonDeg, double* h) {
    const double e2 = kWgsF * (2 - kWgsF);
    const double p = std::sqrt(x[0] * x[0] + x[1] * x[1]);
    double lat = std::atan2(x[2], p * (1 - e2)), ht = 0;
    for (int i = 0; i < 8; i++) {
        const double sl = std::sin(lat);
        const double N = kWgsA / std::sqrt(1 - e2 * sl * sl);
        ht = p / std::cos(lat) - N;
        lat = std::atan2(x[2], p * (1 - e2 * N / (N + ht)));
    }
    if (p < 1e-3) { lat = x[2] >= 0 ? kPi / 2 : -kPi / 2; ht = std::fabs(x[2]) - kWgsA * (1 - kWgsF); }
    *latDeg = lat * 180.0 / kPi;
    *lonDeg = std::atan2(x[1], x[0]) * 180.0 / kPi;
    *h = ht;
}

void azElFromEcef(const double rx[3], const double sat[3], double* azDeg, double* elDeg) {
    double lat, lon, h;
    ecefToLla(rx, &lat, &lon, &h);
    const double la = lat * kPi / 180.0, lo = lon * kPi / 180.0;
    const double d[3] = {sat[0] - rx[0], sat[1] - rx[1], sat[2] - rx[2]};
    const double e = -std::sin(lo) * d[0] + std::cos(lo) * d[1];
    const double n = -std::sin(la) * std::cos(lo) * d[0] - std::sin(la) * std::sin(lo) * d[1] + std::cos(la) * d[2];
    const double u = std::cos(la) * std::cos(lo) * d[0] + std::cos(la) * std::sin(lo) * d[1] + std::sin(la) * d[2];
    double az = std::atan2(e, n) * 180.0 / kPi;
    if (az < 0) az += 360.0;
    *azDeg = az;
    *elDeg = std::atan2(u, std::sqrt(e * e + n * n)) * 180.0 / kPi;
}

// days from 1970-01-01 of a calendar date (proleptic Gregorian), and back
static long daysFromCivil(long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static void civilFromDays(long z, int* y, int* m, int* d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = (long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

void gpsTimeToCalendar(int week, double tow, double leap, int* year, int* month, int* day, int* hour, int* minute, double* second) {
    double t = (double)week * 604800.0 + tow - leap;      // seconds since 1980-01-06 00:00:00 UTC
    double days = std::floor(t / 86400.0);
    double sod = t - days * 86400.0;
    civilFromDays(daysFromCivil(1980, 1, 6) + (long)days, year, month, day);
    *hour = (int)(sod / 3600.0); sod -= *hour * 3600.0;
    *minute = (int)(sod / 60.0); sod -= *minute * 60.0;
    *second = sod;
}

} // namespace dect2
