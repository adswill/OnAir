// Mode S / ADS-B message layer. See adsb_msg.h for the sources.
#include "dect2/adsb_msg.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace dect2::adsb {

// ---------------------------------------------------------------- CRC-24

static const uint32_t* crcTable() {
    static uint32_t t[256];
    static const bool init = [] {
        for (uint32_t b = 0; b < 256; b++) {
            uint32_t c = b << 16;
            for (int i = 0; i < 8; i++) c = (c & 0x800000) ? ((c << 1) ^ kCrcGenerator) : (c << 1);
            t[b] = c & 0xFFFFFF;
        }
        return true;
    }();
    (void)init;
    return t;
}

// The table runs the division over the data bits (with 24 zero bits behind them): that is the parity a transmitter appends. The remainder of the
// whole message, as Annex 10 defines it, is that value XOR the 24 bits that were received.
static uint32_t crcOfData(const uint8_t* m, int nbytes) {
    const uint32_t* t = crcTable();
    uint32_t c = 0;
    for (int i = 0; i < nbytes; i++) c = ((c << 8) ^ t[((c >> 16) ^ m[i]) & 0xFF]) & 0xFFFFFF;
    return c;
}

uint32_t crc24(const uint8_t* m, int nbits) {
    const int nb = nbits / 8;
    return crcOfData(m, nb - 3) ^ ((uint32_t)m[nb - 3] << 16 | (uint32_t)m[nb - 2] << 8 | m[nb - 1]);
}

uint32_t crcParity(const uint8_t* m, int nbits) { return crcOfData(m, nbits / 8 - 3); }

int dfLength(int df) {
    switch (df) {
    case 0: case 4: case 5: case 11: return 56;
    case 16: case 17: case 18: case 19: case 20: case 21: return 112;
    case 24: case 25: case 26: case 27: return 112;   // extended length message (ELM): bit 3 is a spare bit and 0, so DF 28 - 31 are not one
    default: return 0;
    }
}

uint32_t getBits(const uint8_t* m, int first, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++) {
        const int b = first - 1 + i;
        v = (v << 1) | ((m[b >> 3] >> (7 - (b & 7))) & 1u);
    }
    return v;
}

void setBits(uint8_t* m, int first, int count, uint32_t value) {
    for (int i = 0; i < count; i++) {
        const int b = first - 1 + i;
        const uint8_t mask = (uint8_t)(0x80 >> (b & 7));
        if ((value >> (count - 1 - i)) & 1u) m[b >> 3] |= mask; else m[b >> 3] &= (uint8_t)~mask;
    }
}

namespace {
struct SynTable {
    struct E { uint32_t syn; int pos; };
    std::vector<E> e56, e112;
    SynTable() {
        for (int len : {56, 112}) {
            std::vector<E>& v = len == 56 ? e56 : e112;
            for (int p = 0; p < len; p++) {
                uint8_t m[14] = {};
                m[p >> 3] = (uint8_t)(0x80 >> (p & 7));
                v.push_back({crc24(m, len), p});
            }
            std::sort(v.begin(), v.end(), [](const E& a, const E& b) { return a.syn < b.syn; });
        }
    }
};
const SynTable& synTable() { static const SynTable t; return t; }
}

uint32_t singleBitSyndrome(int nbits, int pos) {
    uint8_t m[14] = {};
    m[pos >> 3] = (uint8_t)(0x80 >> (pos & 7));
    return crc24(m, nbits);
}

int singleBitPosition(int nbits, uint32_t rem) {
    const auto& v = nbits == 56 ? synTable().e56 : synTable().e112;
    auto it = std::lower_bound(v.begin(), v.end(), rem, [](const SynTable::E& e, uint32_t s) { return e.syn < s; });
    return (it != v.end() && it->syn == rem) ? it->pos : -1;
}

// ---------------------------------------------------------------- altitude, identity, characters

// Gillham code. Bits D2 D4 A1 A2 A4 B1 B2 B4 are a reflected binary (Gray) count of 500 ft steps above -1000 ft; C1 C2 C4 split each step into
// five 100 ft levels, in the order 001 011 010 110 100 for even steps and reversed for odd steps (ICAO Annex 10 Vol. IV; the layout as described
// at en.wikipedia.org/wiki/Gillham_code, which also lists 0 ft = 000 000 011 010, 100 ft = ...110 and 700 ft = 000 000 010 001 for D A B C).
static const int kCOrder[5] = {1, 3, 2, 6, 4};   // C1 C2 C4 as a 3 bit number, lowest level first

bool gillhamToAltitude(unsigned code, int& altFt) {
    // code11: C1 A1 C2 A2 C4 A4 B1 B2 D2 B4 D4, bit 10 first
    const unsigned c1 = (code >> 10) & 1, a1 = (code >> 9) & 1, c2 = (code >> 8) & 1, a2 = (code >> 7) & 1, c4 = (code >> 6) & 1, a4 = (code >> 5) & 1;
    const unsigned b1 = (code >> 4) & 1, b2 = (code >> 3) & 1, d2 = (code >> 2) & 1, b4 = (code >> 1) & 1, d4 = code & 1;
    unsigned gray = (d2 << 7) | (d4 << 6) | (a1 << 5) | (a2 << 4) | (a4 << 3) | (b1 << 2) | (b2 << 1) | b4;
    unsigned n = gray;
    for (unsigned s = 1; s < 8; s <<= 1) n ^= n >> s;   // Gray to binary
    const unsigned c = (c1 << 2) | (c2 << 1) | c4;
    int r = -1;
    for (int i = 0; i < 5; i++) if ((unsigned)kCOrder[i] == c) r = i;
    if (r < 0) return false;
    const int level = (n & 1) ? 4 - r : r;   // odd steps run the other way
    altFt = -1200 + 500 * (int)n + 100 * level;
    // the lowest code of step n is -1200 + 500 n: step 0 starts at -1200 ft
    return altFt >= -1200 && altFt <= 126700;
}

bool altitudeToGillham(int altFt, unsigned& code) {
    if (altFt < -1200 || altFt > 126700) return false;
    int a = altFt + 1200;
    a = (a + 50) / 100;      // 100 ft steps
    if (a < 0 || a > 1279) return false;
    const unsigned n = (unsigned)(a / 5);
    const int level = a % 5;
    const int r = (n & 1) ? 4 - level : level;
    const unsigned c = (unsigned)kCOrder[r];
    const unsigned gray = n ^ (n >> 1);
    const unsigned d2 = (gray >> 7) & 1, d4 = (gray >> 6) & 1, a1 = (gray >> 5) & 1, a2 = (gray >> 4) & 1, a4 = (gray >> 3) & 1, b1 = (gray >> 2) & 1, b2 = (gray >> 1) & 1, b4 = gray & 1;
    const unsigned c1 = (c >> 2) & 1, c2 = (c >> 1) & 1, c4 = c & 1;
    code = (c1 << 10) | (a1 << 9) | (c2 << 8) | (a2 << 7) | (c4 << 6) | (a4 << 5) | (b1 << 4) | (b2 << 3) | (d2 << 2) | (b4 << 1) | d4;
    return true;
}

// AC13 (DF0 / 4 / 16 / 20, bits 20-32): C1 A1 C2 A2 C4 A4 M B1 Q B2 D2 B4 D4
bool decodeAc13(unsigned ac, int& altFt) {
    ac &= 0x1FFF;
    if (ac == 0) return false;                  // no altitude information
    if (ac & 0x0040) return false;              // M = 1: metric altitude, not used in practice and not decoded
    if (ac & 0x0010) {                          // Q = 1: 25 ft steps
        const unsigned n = ((ac & 0x1F80) >> 2) | ((ac & 0x0020) >> 1) | (ac & 0x000F);
        altFt = (int)n * 25 - 1000;
        return true;
    }
    // Gillham: C1 A1 C2 A2 C4 A4 B1 B2 D2 B4 D4 (M and Q removed)
    const unsigned code = ((ac & 0x1F80) >> 2) | ((ac & 0x0020) >> 1) | (ac & 0x000F);
    return gillhamToAltitude(code, altFt);
}

bool decodeAlt12(unsigned alt12, int& altFt) {
    alt12 &= 0xFFF;
    return decodeAc13(((alt12 & 0xFC0) << 1) | (alt12 & 0x3F), altFt);
}

// 13 bit identity: C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4
int decodeId13(unsigned id) {
    const unsigned c1 = (id >> 12) & 1, a1 = (id >> 11) & 1, c2 = (id >> 10) & 1, a2 = (id >> 9) & 1, c4 = (id >> 8) & 1, a4 = (id >> 7) & 1;
    const unsigned b1 = (id >> 5) & 1, d1 = (id >> 4) & 1, b2 = (id >> 3) & 1, d2 = (id >> 2) & 1, b4 = (id >> 1) & 1, d4 = id & 1;
    return (int)((a4 * 4 + a2 * 2 + a1) * 1000 + (b4 * 4 + b2 * 2 + b1) * 100 + (c4 * 4 + c2 * 2 + c1) * 10 + (d4 * 4 + d2 * 2 + d1));
}

unsigned encodeId13(int sq) {
    const unsigned a = (unsigned)(sq / 1000) & 7, b = (unsigned)(sq / 100) % 10 & 7, c = (unsigned)(sq / 10) % 10 & 7, d = (unsigned)sq % 10 & 7;
    return ((c >> 0 & 1) << 12) | ((a >> 0 & 1) << 11) | ((c >> 1 & 1) << 10) | ((a >> 1 & 1) << 9) | ((c >> 2 & 1) << 8) | ((a >> 2 & 1) << 7) |
           ((b >> 0 & 1) << 5) | ((d >> 0 & 1) << 4) | ((b >> 1 & 1) << 3) | ((d >> 1 & 1) << 2) | ((b >> 2 & 1) << 1) | (d >> 2 & 1);
}

// Annex 10 Vol. IV 3.1.2.9 / DO-260B: # = not used. 1 - 26 A - Z, 32 space, 48 - 57 digits.
char charFrom6bit(unsigned c) {
    if (c >= 1 && c <= 26) return (char)('A' + c - 1);
    if (c == 32) return ' ';
    if (c >= 48 && c <= 57) return (char)('0' + c - 48);
    return '#';
}

int charTo6bit(char ch) {
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 1;
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 1;
    if (ch == ' ') return 32;
    if (ch >= '0' && ch <= '9') return 48 + (ch - '0');
    return -1;
}

std::string categoryCode(int tc, int ca) {
    if (tc < 1 || tc > 4 || ca < 0 || ca > 7) return "";
    char b[3] = {(char)('A' + (4 - tc)), (char)('0' + ca), 0};
    return b;
}

// Emitter categories: DO-260B, as listed at adsbexchange.com (Emitter Category). Codes the standard leaves reserved are not named.
std::string categoryName(int tc, int ca) {
    if (tc < 1 || tc > 4) return "";
    if (ca == 0) return "No information";
    static const char* a[8] = {"", "Light (< 15500 lb)", "Small (15500 - 75000 lb)", "Large (75000 - 300000 lb)", "High vortex large", "Heavy (> 300000 lb)", "High performance", "Rotorcraft"};
    static const char* b[8] = {"", "Glider", "Lighter than air", "Parachutist", "Ultralight", "", "Unmanned aircraft", "Space vehicle"};
    static const char* c[8] = {"", "Surface emergency vehicle", "Surface service vehicle", "Point obstacle", "", "", "", ""};
    const char* s = tc == 4 ? a[ca] : tc == 3 ? b[ca] : tc == 2 ? c[ca] : "";
    return *s ? s : "Reserved";
}

const char* emergencyName(int s) {
    static const char* n[8] = {"None", "General emergency", "Medical", "Minimum fuel", "No communications", "Unlawful interference", "Downed aircraft", "Reserved"};
    return (s >= 0 && s < 8) ? n[s] : "";
}

// ---------------------------------------------------------------- CPR

static double mod(double x, double y) { double r = std::fmod(x, y); return r < 0 ? r + y : r; }

// Transition latitudes: NL >= n up to |lat| = T[n] (Annex 10 / DO-260B table A-1; the formula gives the table's 10.47047130, 14.82817437, ... 87.0)
static const double* nlThresholds() {
    static double t[60];
    static const bool init = [] {
        for (int n = 2; n <= 59; n++)
            t[n] = std::acos(std::sqrt((1.0 - std::cos(M_PI / 30.0)) / (1.0 - std::cos(2.0 * M_PI / n)))) * 180.0 / M_PI;
        return true;
    }();
    (void)init;
    return t;
}

int cprNl(double lat) {
    const double a = std::fabs(lat);
    if (a > 90.0) return 1;
    const double* t = nlThresholds();
    int nl = 59;
    for (int n = 59; n >= 2; n--) {
        if (a > t[n]) nl = n - 1; else break;
    }
    return nl;
}

static const double kCprScale = 131072.0;   // 2^17

bool cprGlobalAirborne(int latE, int lonE, int latO, int lonO, bool oddLatest, double& lat, double& lon) {
    const double le = latE / kCprScale, lo = latO / kCprScale;
    const double j = std::floor(59.0 * le - 60.0 * lo + 0.5);
    double ve = (360.0 / 60.0) * (mod(j, 60.0) + le);
    double vo = (360.0 / 59.0) * (mod(j, 59.0) + lo);
    if (ve >= 270.0) ve -= 360.0;
    if (vo >= 270.0) vo -= 360.0;
    if (ve < -90.0 || ve > 90.0 || vo < -90.0 || vo > 90.0) return false;
    const int nl = cprNl(ve);
    if (nl != cprNl(vo)) return false;           // the two frames are in different longitude zone layouts: no solution
    const double rlat = oddLatest ? vo : ve;
    const int ni = std::max(nl - (oddLatest ? 1 : 0), 1);
    const double m = std::floor((lonE / kCprScale) * (nl - 1) - (lonO / kCprScale) * nl + 0.5);
    double rlon = (360.0 / ni) * (mod(m, ni) + (oddLatest ? lonO : lonE) / kCprScale);
    if (rlon >= 180.0) rlon -= 360.0;
    lat = rlat; lon = rlon;
    return true;
}

bool cprLocalAirborne(int latCpr, int lonCpr, bool odd, double refLat, double refLon, double& lat, double& lon) {
    const double i = odd ? 1.0 : 0.0;
    const double dLat = 360.0 / (60.0 - i);
    const double lc = latCpr / kCprScale, xc = lonCpr / kCprScale;
    const double j = std::floor(refLat / dLat) + std::floor(0.5 + mod(refLat, dLat) / dLat - lc);
    const double rlat = dLat * (j + lc);
    if (rlat < -90.0 || rlat > 90.0) return false;
    const double dLon = 360.0 / std::max(cprNl(rlat) - i, 1.0);
    const double m = std::floor(refLon / dLon) + std::floor(0.5 + mod(refLon, dLon) / dLon - xc);
    double rlon = dLon * (m + xc);
    if (rlon >= 180.0) rlon -= 360.0;
    if (rlon < -180.0) rlon += 360.0;
    lat = rlat; lon = rlon;
    return true;
}

static double wrapLon(double l) { l = mod(l + 180.0, 360.0) - 180.0; return l; }

bool cprGlobalSurface(int latE, int lonE, int latO, int lonO, bool oddLatest, double refLat, double refLon, double& lat, double& lon) {
    const double le = latE / kCprScale, lo = latO / kCprScale;
    const double j = std::floor(59.0 * le - 60.0 * lo + 0.5);
    // the zones repeat every 90 degrees: the northern solution is in [0, 90), the southern one 90 lower; the reference picks
    auto hemisphere = [&](double v) { return std::fabs(v - refLat) <= std::fabs(v - 90.0 - refLat) ? v : v - 90.0; };
    const double ve = hemisphere((90.0 / 60.0) * (mod(j, 60.0) + le));
    const double vo = hemisphere((90.0 / 59.0) * (mod(j, 59.0) + lo));
    const int nl = cprNl(ve);
    if (nl != cprNl(vo)) return false;
    const double rlat = oddLatest ? vo : ve;
    const int ni = std::max(nl - (oddLatest ? 1 : 0), 1);
    const double m = std::floor((lonE / kCprScale) * (nl - 1) - (lonO / kCprScale) * nl + 0.5);
    const double base = (90.0 / ni) * (mod(m, ni) + (oddLatest ? lonO : lonE) / kCprScale);
    double best = 0, bestD = 1e9;
    for (int k = 0; k < 4; k++) {
        const double c = wrapLon(base + 90.0 * k);
        const double d = std::fabs(wrapLon(c - refLon));
        if (d < bestD) { bestD = d; best = c; }
    }
    lat = rlat; lon = best;
    return true;
}

bool cprLocalSurface(int latCpr, int lonCpr, bool odd, double refLat, double refLon, double& lat, double& lon) {
    const double i = odd ? 1.0 : 0.0;
    const double dLat = 90.0 / (60.0 - i);
    const double lc = latCpr / kCprScale, xc = lonCpr / kCprScale;
    const double j = std::floor(refLat / dLat) + std::floor(0.5 + mod(refLat, dLat) / dLat - lc);
    const double rlat = dLat * (j + lc);
    if (rlat < -90.0 || rlat > 90.0) return false;
    const double dLon = 90.0 / std::max(cprNl(rlat) - i, 1.0);
    const double m = std::floor(refLon / dLon) + std::floor(0.5 + mod(refLon, dLon) / dLon - xc);
    lat = rlat; lon = wrapLon(dLon * (m + xc));
    return true;
}

void cprEncodeAirborne(double lat, double lon, bool odd, int& latCpr, int& lonCpr) {
    const double dLat = 360.0 / (odd ? 59.0 : 60.0);
    const double yz = std::floor(kCprScale * mod(lat, dLat) / dLat + 0.5);
    const double rlat = dLat * (yz / kCprScale + std::floor(lat / dLat));
    const double dLon = 360.0 / std::max(cprNl(rlat) - (odd ? 1 : 0), 1);
    const double xz = std::floor(kCprScale * mod(lon, dLon) / dLon + 0.5);
    latCpr = (int)mod(yz, kCprScale);
    lonCpr = (int)mod(xz, kCprScale);
}

void cprEncodeSurface(double lat, double lon, bool odd, int& latCpr, int& lonCpr) {
    const double dLat = 90.0 / (odd ? 59.0 : 60.0);
    const double yz = std::floor(kCprScale * mod(lat, dLat) / dLat + 0.5);
    const double rlat = dLat * (yz / kCprScale + std::floor(lat / dLat));
    const double dLon = 90.0 / std::max(cprNl(rlat) - (odd ? 1 : 0), 1);
    const double xz = std::floor(kCprScale * mod(lon, dLon) / dLon + 0.5);
    latCpr = (int)mod(yz, kCprScale);
    lonCpr = (int)mod(xz, kCprScale);
}

double distanceNm(double lat1, double lon1, double lat2, double lon2) {
    const double r = M_PI / 180.0;
    const double dl = (lat2 - lat1) * r, dn = (lon2 - lon1) * r;
    const double a = std::sin(dl / 2) * std::sin(dl / 2) + std::cos(lat1 * r) * std::cos(lat2 * r) * std::sin(dn / 2) * std::sin(dn / 2);
    return 2.0 * 3440.065 * std::asin(std::min(1.0, std::sqrt(a)));   // mean earth radius 6371.0088 km = 3440.065 NM
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
    const double r = M_PI / 180.0;
    const double dn = (lon2 - lon1) * r;
    const double y = std::sin(dn) * std::cos(lat2 * r);
    const double x = std::cos(lat1 * r) * std::sin(lat2 * r) - std::sin(lat1 * r) * std::cos(lat2 * r) * std::cos(dn);
    return mod(std::atan2(y, x) * 180.0 / M_PI, 360.0);
}

// ---------------------------------------------------------------- Comm-B inference

static int signedBits(const uint8_t* mb, int first, int count) {
    int v = (int)getBits(mb, first, count);
    if (v >= (1 << (count - 1))) v -= 1 << count;
    return v;
}

// status bit followed by a value of `n` bits: when the status is 0 the value must be 0
static bool statusOk(const uint8_t* mb, int statusBit, int first, int n) {
    return getBits(mb, statusBit, 1) || getBits(mb, first, n) == 0;
}

CommB inferCommB(const uint8_t* mb) {
    CommB out;
    bool zero = true;
    for (int i = 0; i < 7; i++) if (mb[i]) zero = false;
    if (zero) return out;
    // 2,0: the first byte is the code, the rest are eight characters
    bool is20 = mb[0] == 0x20;
    std::string cs;
    if (is20) {
        for (int i = 0; i < 8 && is20; i++) {
            const unsigned c = getBits(mb, 9 + 6 * i, 6);
            const char ch = charFrom6bit(c);
            if (ch == '#') is20 = false;
            cs += ch;
        }
        if (is20 && cs.find_first_not_of(' ') == std::string::npos) is20 = false;
    }
    // 4,0
    int n40 = 0;
    bool is40 = statusOk(mb, 1, 2, 12) && statusOk(mb, 14, 15, 12) && statusOk(mb, 27, 28, 12) && getBits(mb, 40, 8) == 0 && getBits(mb, 52, 2) == 0 &&
                statusOk(mb, 48, 49, 3) && statusOk(mb, 54, 55, 2);
    if (is40) {
        n40 = (int)getBits(mb, 1, 1) + (int)getBits(mb, 14, 1) + (int)getBits(mb, 27, 1);
        if (n40 < 2) is40 = false;
        if (is40 && getBits(mb, 27, 1)) { const double b = 800.0 + 0.1 * getBits(mb, 28, 12); if (b < 900.0 || b > 1100.0) is40 = false; }
    }
    // 5,0
    int n50 = 0;
    bool is50 = statusOk(mb, 1, 2, 10) && statusOk(mb, 12, 13, 11) && statusOk(mb, 24, 25, 10) && statusOk(mb, 35, 36, 10) && statusOk(mb, 46, 47, 10);
    if (is50) {
        n50 = (int)(getBits(mb, 1, 1) + getBits(mb, 12, 1) + getBits(mb, 24, 1) + getBits(mb, 35, 1) + getBits(mb, 46, 1));
        if (n50 < 4) is50 = false;
        if (is50 && getBits(mb, 1, 1) && std::fabs(signedBits(mb, 2, 10) * 45.0 / 256.0) > 50.0) is50 = false;
        if (is50 && getBits(mb, 24, 1) && getBits(mb, 25, 10) * 2 > 600) is50 = false;
        if (is50 && getBits(mb, 46, 1) && getBits(mb, 47, 10) * 2 > 500) is50 = false;
        // a coordinated turn: rate = g tan(roll) / true airspeed (my own check, not in the Riddle; the Riddle's own example, -9.7 degrees at 466 kt, gives -0.40 deg/s as sent)
        if (is50 && getBits(mb, 1, 1) && getBits(mb, 35, 1) && getBits(mb, 46, 1) && getBits(mb, 47, 10) > 20) {
            const double tas = getBits(mb, 47, 10) * 2 * 0.514444;
            const double expect = 9.80665 * std::tan(signedBits(mb, 2, 10) * 45.0 / 256.0 * M_PI / 180.0) / tas * 180.0 / M_PI;
            if (std::fabs(expect - signedBits(mb, 36, 10) * 8.0 / 256.0) > 1.0) is50 = false;
        }
        // ground speed and true airspeed differ by the wind: not by more than 200 kt
        if (is50 && getBits(mb, 24, 1) && getBits(mb, 46, 1) && std::abs((int)getBits(mb, 25, 10) - (int)getBits(mb, 47, 10)) * 2 > 200) is50 = false;
    }
    // 6,0
    int n60 = 0;
    bool is60 = statusOk(mb, 1, 2, 11) && statusOk(mb, 13, 14, 10) && statusOk(mb, 24, 25, 10) && statusOk(mb, 35, 36, 10) && statusOk(mb, 46, 47, 10);
    if (is60) {
        n60 = (int)(getBits(mb, 1, 1) + getBits(mb, 13, 1) + getBits(mb, 24, 1) + getBits(mb, 35, 1) + getBits(mb, 46, 1));
        if (n60 < 4) is60 = false;
        if (is60 && getBits(mb, 13, 1) && getBits(mb, 14, 10) > 500) is60 = false;
        if (is60 && getBits(mb, 24, 1) && getBits(mb, 25, 10) * 0.004 > 1.0) is60 = false;
        if (is60 && getBits(mb, 35, 1) && std::abs(signedBits(mb, 36, 10)) * 32 > 6000) is60 = false;
        if (is60 && getBits(mb, 46, 1) && std::abs(signedBits(mb, 47, 10)) * 32 > 6000) is60 = false;
        // indicated airspeed cannot exceed Mach x the speed of sound at sea level (661.5 kt), with some room for the instrument error
        if (is60 && getBits(mb, 13, 1) && getBits(mb, 24, 1) && getBits(mb, 14, 10) > getBits(mb, 25, 10) * 0.004 * 661.5 + 25.0) is60 = false;
    }
    if (is20 + is40 + is50 + is60 != 1) return out;   // nothing fits, or more than one fits
    if (is20) {
        out.bds = 0x20;
        while (!cs.empty() && cs.back() == ' ') cs.pop_back();
        out.callsign = cs;
    } else if (is40) {
        out.bds = 0x40;
        if (getBits(mb, 1, 1)) { out.hasSelAlt = true; out.selAltMcpFt = (int)getBits(mb, 2, 12) * 16; }
        if (getBits(mb, 14, 1)) { out.hasSelAltFms = true; out.selAltFmsFt = (int)getBits(mb, 15, 12) * 16; }
        if (getBits(mb, 27, 1)) { out.hasBaro = true; out.baroMb = 800.0 + 0.1 * getBits(mb, 28, 12); }
    } else if (is50) {
        out.bds = 0x50;
        if (getBits(mb, 1, 1)) { out.hasRoll = true; out.rollDeg = signedBits(mb, 2, 10) * 45.0 / 256.0; }
        if (getBits(mb, 12, 1)) { out.hasTrack = true; double t = signedBits(mb, 13, 11) * 90.0 / 512.0; if (t < 0) t += 360.0; out.trackDeg = t; }
        if (getBits(mb, 24, 1)) { out.hasGs = true; out.gsKt = (int)getBits(mb, 25, 10) * 2; }
        if (getBits(mb, 35, 1)) { out.hasTrackRate = true; out.trackRateDps = signedBits(mb, 36, 10) * 8.0 / 256.0; }
        if (getBits(mb, 46, 1)) { out.hasTas = true; out.tasKt = (int)getBits(mb, 47, 10) * 2; }
    } else {
        out.bds = 0x60;
        if (getBits(mb, 1, 1)) { out.hasHeading = true; double h = signedBits(mb, 2, 11) * 90.0 / 512.0; if (h < 0) h += 360.0; out.headingDeg = h; }
        if (getBits(mb, 13, 1)) { out.hasIas = true; out.iasKt = (int)getBits(mb, 14, 10); }
        if (getBits(mb, 24, 1)) { out.hasMach = true; out.mach = getBits(mb, 25, 10) * 0.004; }
        if (getBits(mb, 35, 1)) { out.hasBaroRate = true; out.baroRateFpm = signedBits(mb, 36, 10) * 32; }
        if (getBits(mb, 46, 1)) { out.hasInertialRate = true; out.inertialRateFpm = signedBits(mb, 47, 10) * 32; }
    }
    return out;
}

// ---------------------------------------------------------------- message decoding

std::string toHex(const uint8_t* b, int nbits) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < nbits / 8; i++) { s += d[b[i] >> 4]; s += d[b[i] & 15]; }
    return s;
}

// movement field of a surface position (DO-260B; the ranges as printed in the Riddle, chapter Surface Position). The lower edge of the range is reported.
static bool movementToKt(unsigned c, double& kt) {
    if (c == 0 || c > 124) return false;
    if (c == 1) kt = 0;
    else if (c <= 8) kt = 0.125 * (c - 1);
    else if (c <= 12) kt = 1.0 + 0.25 * (c - 9);
    else if (c <= 38) kt = 2.0 + 0.5 * (c - 13);
    else if (c <= 93) kt = 15.0 + (c - 39);
    else if (c <= 108) kt = 70.0 + 2.0 * (c - 94);
    else if (c <= 123) kt = 100.0 + 5.0 * (c - 109);
    else kt = 175.0;
    return true;
}

static void decodeVelocity(const uint8_t* b, Msg& m) {
    const int st = m.st;
    if (st < 1 || st > 4) return;
    // ME bits (message bit 32 + n): 9 IC, 10 resv, 11-13 NAC, 14 Sew / SH, 15-24 Vew / heading, 25 Sns / airspeed type, 26-35 Vns / airspeed,
    // 36 vertical source, 37 Svr, 38-46 Vr, 49 Sdif, 50-56 Dif
    if (st <= 2) {
        const unsigned vew = getBits(b, 32 + 15, 10), vns = getBits(b, 32 + 26, 10);
        if (vew == 0 || vns == 0) return;
        const double mul = st == 2 ? 4.0 : 1.0;
        const double ve = (vew - 1) * mul * (getBits(b, 32 + 14, 1) ? -1.0 : 1.0);    // 1 = towards the west
        const double vn = (vns - 1) * mul * (getBits(b, 32 + 25, 1) ? -1.0 : 1.0);    // 1 = towards the south
        m.hasVel = true; m.speedKt = std::sqrt(ve * ve + vn * vn); m.speedKind = 0;
        m.hasHeading = true; m.headingIsTrack = true;
        double h = std::atan2(ve, vn) * 180.0 / M_PI;
        if (h < 0) h += 360.0;
        m.headingDeg = h;
    } else {
        if (getBits(b, 32 + 14, 1)) { m.hasHeading = true; m.headingIsTrack = false; m.headingDeg = getBits(b, 32 + 15, 10) * 360.0 / 1024.0; }
        const unsigned as = getBits(b, 32 + 26, 10);
        if (as) { m.hasVel = true; m.speedKt = (as - 1) * (st == 4 ? 4.0 : 1.0); m.speedKind = getBits(b, 32 + 25, 1) ? 2 : 1; }
    }
    const unsigned vr = getBits(b, 32 + 38, 9);
    if (vr) {
        m.hasVrate = true;
        m.vrateFpm = ((int)vr - 1) * 64 * (getBits(b, 32 + 37, 1) ? -1 : 1);
        m.vrateBaro = getBits(b, 32 + 36, 1) != 0;
    }
    const unsigned dif = getBits(b, 32 + 50, 7);
    if (dif) {
        m.hasGnssDiff = true;
        m.gnssDiffFt = ((int)dif - 1) * 25 * (getBits(b, 32 + 49, 1) ? -1 : 1);   // 1 = GNSS height below barometric altitude
    }
}

static void decodeExtended(const uint8_t* b, Msg& m) {
    m.tc = (int)getBits(b, 33, 5);
    const int tc = m.tc;
    if (tc >= 1 && tc <= 4) {
        m.catSet = (char)('A' + (4 - tc));
        m.catCode = (int)getBits(b, 38, 3);
        std::string cs;
        for (int i = 0; i < 8; i++) {
            char ch = charFrom6bit(getBits(b, 41 + 6 * i, 6));
            cs += ch == '#' ? ' ' : ch;
        }
        while (!cs.empty() && cs.back() == ' ') cs.pop_back();
        m.hasIdent = true; m.callsign = cs;
    } else if (tc >= 5 && tc <= 8) {
        m.surface = true; m.ground = true;
        m.hasMove = movementToKt(getBits(b, 38, 7), m.moveKt);
        if (getBits(b, 45, 1)) { m.hasTrack = true; m.trackDeg = getBits(b, 46, 7) * 360.0 / 128.0; }
        m.hasCpr = true; m.cprOdd = getBits(b, 54, 1) != 0; m.cprLat = (int)getBits(b, 55, 17); m.cprLon = (int)getBits(b, 72, 17);
    } else if ((tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {
        // The altitude field of TC 20-22 is GNSS height. readsb (and dump1090) decode it like the barometric altitude, 25 ft steps with the Q bit; the
        // Riddle says metres, which cannot be right for a 12 bit field (4095 m = 13435 ft). Not checked against DO-260B: see docs/modes/adsb.md.
        int a = 0;
        if (decodeAlt12(getBits(b, 41, 12), a)) { m.hasAlt = true; m.altFt = a; m.altGnss = tc >= 20; }
        const unsigned ss = getBits(b, 38, 2);
        if (ss == 1 || ss == 2) m.alert = true;
        if (ss == 3) m.spi = true;
        m.hasCpr = true; m.cprOdd = getBits(b, 54, 1) != 0; m.cprLat = (int)getBits(b, 55, 17); m.cprLon = (int)getBits(b, 72, 17);
    } else if (tc == 0) {   // no position information: the altitude field may still be filled
        int a = 0;
        if (decodeAlt12(getBits(b, 41, 12), a)) { m.hasAlt = true; m.altFt = a; }
    } else if (tc == 19) {
        m.st = (int)getBits(b, 38, 3);
        decodeVelocity(b, m);
    } else if (tc == 28) {
        m.st = (int)getBits(b, 38, 3);
        if (m.st == 1) {
            m.emergency = (int)getBits(b, 41, 3);
            const unsigned id = getBits(b, 44, 13);
            if (id) { m.hasSquawk = true; m.squawk = decodeId13(id); }
        }
    } else if (tc == 29) {
        m.st = (int)getBits(b, 38, 2);
        if (m.st == 1) {   // DO-260B target state and status; field positions as in readsb mode_s.c (decodeESTargetState), not read from the standard
            m.selAltFms = getBits(b, 41, 1) != 0;
            const unsigned sa = getBits(b, 42, 11);
            if (sa) { m.hasSelAlt = true; m.selAltFt = ((int)sa - 1) * 32; }
            const unsigned bp = getBits(b, 53, 9);
            if (bp) { m.hasBaro = true; m.baroMb = 800.0 + ((int)bp - 1) * 0.8; }
            if (getBits(b, 62, 1)) {
                m.hasSelHdg = true;
                int h = (int)getBits(b, 63, 9);
                if (h >= 256) h -= 512;
                double d = h * 180.0 / 256.0;
                if (d < 0) d += 360.0;
                m.selHdgDeg = d;
            }
            m.tsNacp = (int)getBits(b, 72, 4);
            if (getBits(b, 79, 1)) {
                m.tsModeValid = true;
                m.autopilot = getBits(b, 80, 1) != 0; m.vnav = getBits(b, 81, 1) != 0; m.altHold = getBits(b, 82, 1) != 0;
                m.approach = getBits(b, 84, 1) != 0; m.lnav = getBits(b, 86, 1) != 0;
            }
        }
    } else if (tc == 31) {
        m.st = (int)getBits(b, 38, 3);
        if (m.st == 0 || m.st == 1) {
            const int ver = (int)getBits(b, 73, 3);
            m.adsbVersion = ver;
            if (ver >= 1) {   // NIC supplement-A, NACp, SIL: bits 76, 77-80, 83-84 (version 1 and 2)
                m.nicSuppA = (int)getBits(b, 76, 1);
                m.nacp = (int)getBits(b, 77, 4);
                m.sil = (int)getBits(b, 83, 2);
                if (m.st == 0) { m.nicBaro = (int)getBits(b, 85, 1); if (ver >= 2) m.gva = (int)getBits(b, 81, 2); }
            }
        }
    }
}

bool decodeMsg(const uint8_t* b, int nbits, Msg& m) {
    m = Msg();
    m.df = (int)getBits(b, 1, 5);
    m.nbits = nbits;
    if (dfLength(m.df) != nbits) return false;
    switch (m.df) {
    case 0: case 16: {
        m.vs = (int)getBits(b, 6, 1);
        m.ground = m.vs == 1;
        int a = 0;
        if (decodeAc13(getBits(b, 20, 13), a)) { m.hasAlt = true; m.altFt = a; }
        break;
    }
    case 4: case 5: case 20: case 21: {
        m.fs = (int)getBits(b, 6, 3);
        m.ground = m.fs == 1 || m.fs == 3;
        m.alert = m.fs >= 2 && m.fs <= 4;
        m.spi = m.fs == 4 || m.fs == 5;
        if (m.df == 4 || m.df == 20) {
            int a = 0;
            if (decodeAc13(getBits(b, 20, 13), a)) { m.hasAlt = true; m.altFt = a; }
        } else {
            m.hasSquawk = true; m.squawk = decodeId13(getBits(b, 20, 13));
        }
        if (m.df >= 20) m.commb = inferCommB(b + 4);
        break;
    }
    case 11:
        m.ca = (int)getBits(b, 6, 3);
        m.icao = getBits(b, 9, 24);
        m.ground = m.ca == 4;
        break;
    case 17: case 18: case 19:
        m.ca = (int)getBits(b, 6, 3);
        m.icao = getBits(b, 9, 24);
        if (m.df == 18 && (m.ca == 4 || m.ca == 7)) return true;   // TIS-B management and reserved control field: no ME decoding
        if (m.df == 19 && m.ca != 0) return true;                  // military: only application field 0 has the extended squitter layout
        decodeExtended(b, m);
        break;
    default: break;
    }
    return true;
}

} // namespace dect2::adsb
