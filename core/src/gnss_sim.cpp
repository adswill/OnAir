// Simulated sky for the GNSS test signal (see gnss_sim.h).
#include "dect2/gnss_sim.h"
#include "dect2/gen_util.h"
#include "dect2/gnss_codes.h"
#include "dect2/gnss_msg.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {

namespace {

const double kPiS = 3.14159265358979323846;
const int kWeek = 2400;                 // the simulated GPS week (any week works: the receiver resolves the 10 bit number against its clock)
const double kToe = 352800.0;           // ephemeris reference time of every satellite
const double kToa = 356352.0;           // almanac reference time (a multiple of 4096 s)

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) { next(); next(); }
    uint64_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; uint64_t x = s; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return x; }
    double u() { return (double)(next() >> 11) / 9007199254740992.0; }          // [0, 1)
    double r(double a, double b) { return a + (b - a) * u(); }
};

// ------------------------------------------------------------------ the data words of the satellites
struct Words { uint32_t d[10] = {}; };

void putBits(Words& w, int first, int len, uint32_t v) {
    for (int k = 0; k < len; k++) {
        const int p = first + k - 1;                    // 0 based over the 300 bits
        const uint32_t bit = (v >> (len - 1 - k)) & 1u;
        const int word = p / 30, off = p % 30;
        if (bit) w.d[word] |= 1u << (23 - off); else w.d[word] &= ~(1u << (23 - off));
    }
}
void putSplit(Words& w, int b1, int l1, int b2, int l2, uint32_t v) {
    putBits(w, b1, l1, (v >> l2) & ((l1 >= 32) ? ~0u : ((1u << l1) - 1)));
    putBits(w, b2, l2, v & ((1u << l2) - 1));
}
// round to the nearest integer of the field, clamped to its range
int64_t qs(double v, double lsb, int bits) {
    int64_t q = (int64_t)std::llround(v / lsb);
    const int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -((int64_t)1 << (bits - 1));
    return std::min(std::max(q, lo), hi);
}
uint32_t qu(double v, double lsb, int bits) {
    int64_t q = (int64_t)std::llround(v / lsb);
    const int64_t hi = ((int64_t)1 << bits) - 1;
    return (uint32_t)std::min(std::max(q, (int64_t)0), hi);
}
uint32_t twos(int64_t v, int bits) { return (uint32_t)((uint64_t)v & (((uint64_t)1 << bits) - 1)); }

// The parity bits of a word, from the equations of IS-GPS-200 Table 20-XIV written as lists of source bit numbers. d is d1..d24 (index 1..24).
void wordParity(const int* d, int p29, int p30, int* par) {
    static const char* eq[6] = {"1 2 3 5 6 10 11 12 13 14 17 18 20 23", "2 3 4 6 7 11 12 13 14 15 18 19 21 24", "1 3 4 5 7 8 12 13 14 15 16 19 20 22",
                                "2 4 5 6 8 9 13 14 15 16 17 20 21 23", "1 3 5 6 7 9 10 14 15 16 17 18 21 22 24", "3 5 6 8 9 10 11 13 15 19 22 23 24"};
    const int lead[6] = {p29, p30, p29, p30, p30, p29};
    for (int k = 0; k < 6; k++) {
        int x = lead[k];
        const char* s = eq[k];
        while (*s) {
            int n = 0;
            while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
            x ^= d[n];
            while (*s == ' ') s++;
        }
        par[k] = x;
    }
}

// the 300 transmitted bits of ten data words; the two non-information bits of words 2 and 10 are chosen so that the word ends in 00
void encodeSubframe(const Words& in, uint8_t* bits) {
    int p29 = 0, p30 = 0;
    for (int w = 0; w < 10; w++) {
        int d[25] = {};
        int par[6];
        uint32_t word = in.d[w];
        if (w == 1 || w == 9) {
            for (int t = 0; t < 4; t++) {
                word = (word & ~3u) | (uint32_t)t;
                for (int i = 1; i <= 24; i++) d[i] = (word >> (24 - i)) & 1;
                wordParity(d, p29, p30, par);
                if (par[4] == 0 && par[5] == 0) break;
            }
        }
        for (int i = 1; i <= 24; i++) d[i] = (word >> (24 - i)) & 1;
        wordParity(d, p29, p30, par);
        for (int i = 1; i <= 24; i++) bits[30 * w + i - 1] = (uint8_t)(d[i] ^ p30);
        for (int k = 0; k < 6; k++) bits[30 * w + 24 + k] = (uint8_t)par[k];
        p29 = par[4]; p30 = par[5];
    }
}

// ------------------------------------------------------------------ orbit (the generator's own arrangement of the IS-GPS-200 model)
void simPosition(const GpsEphemeris& e, double t, double out[3], double* sinE) {
    const double A = e.sqrtA * e.sqrtA;
    const double n = std::sqrt((e.galileo ? kGalMu : kGpsMu) / (A * A * A)) + e.dn;
    double tk = t - e.toe;
    tk = gpsWrap(tk);
    const double M = e.m0 + n * tk;
    // Kepler's equation by bisection, then a few Newton steps
    const double Mr = M - 2 * kPiS * std::floor(M / (2 * kPiS) + 0.5);   // -pi .. pi
    double lo = Mr - 1.0, hi = Mr + 1.0;
    for (int i = 0; i < 60; i++) {
        const double mid = 0.5 * (lo + hi);
        if (mid - e.e * std::sin(mid) - Mr > 0) hi = mid; else lo = mid;
    }
    double E = 0.5 * (lo + hi);
    for (int i = 0; i < 3; i++) E -= (E - e.e * std::sin(E) - Mr) / (1 - e.e * std::cos(E));
    const double nu = 2 * std::atan2(std::sqrt(1 + e.e) * std::sin(E / 2), std::sqrt(1 - e.e) * std::cos(E / 2));
    const double phi = nu + e.omega;
    const double u = phi + e.cus * std::sin(2 * phi) + e.cuc * std::cos(2 * phi);
    const double r = A * (1 - e.e * std::cos(E)) + e.crs * std::sin(2 * phi) + e.crc * std::cos(2 * phi);
    const double inc = e.i0 + e.idot * tk + e.cis * std::sin(2 * phi) + e.cic * std::cos(2 * phi);
    // node: inertial right ascension minus the earth's rotation since the start of the week
    const double node = e.omega0 + e.omegaDot * tk - kEarthRate * (tk + e.toe);
    // R3(-node) * R1(-inc) * (r cos u, r sin u, 0)
    const double px = r * std::cos(u), py = r * std::sin(u);
    const double y1 = py * std::cos(inc), z1 = py * std::sin(inc);
    out[0] = px * std::cos(node) - y1 * std::sin(node);
    out[1] = px * std::sin(node) + y1 * std::cos(node);
    out[2] = z1;
    if (sinE) *sinE = std::sin(E);
}

} // namespace

struct GnssSim::Impl {
    GnssSimConfig cfg;
    double fs = 0;
    std::vector<GnssSimSat> sats;
    std::vector<Words> sf1, sf2, sf3, alm;        // per satellite (index as in `sats`): the fixed subframes, and its almanac page content
    std::vector<std::vector<uint8_t>> galWords;   // Galileo satellites: I/NAV words 1-4 (4 x 128 bits), by index in `sats`
    double ggtoS = 25e-9;                         // GST minus GPS time that the simulation applies and word type 10 broadcasts
    Words page18, page25s5;
    double rx[3] = {0, 0, 0};
    double tStart = 0;                            // true GPS time of week of sample 0
    int64_t kBase = 0;                            // chips at tStart (an integer)
    double rateErr = 0;                           // sample clock error, fractional
    double lsegS = 0.002;
    GpsIono iono;
    GpsUtc utc;
    // rendering
    struct Chan {
        size_t sat = 0;
        uint8_t code[kGalE1Len];
        int sys = GnssGps;
        int mult = 1;                             // code elements per chip: 2 for Galileo's BOC(1,1) (the half chips are counted)
        int64_t base = 0;                         // kBase in code elements
        float amp = 0;
        int64_t sfCached = -1;
        uint8_t bits[300];
        int64_t msgCached = -1;                   // SBAS message / Galileo page part held in `syms`
        uint8_t syms[500];
        // segment
        double p = 0, dp = 0;                     // chips (relative to kBase) at the end of the next sample interval, per sample
        double cyc = 0, dcyc = 0;                 // carrier cycles at the centre of the next sample, per sample
        double pEnd = 0, cEnd = 0;                // values at the end of the segment
        int64_t lastIdx = INT64_MIN;
        float cur = 0, prev = 0;
    };
    std::vector<Chan> ch;
    int64_t segLeft = 0;
    int64_t n = 0;                                // samples generated
    double tSegEnd = 0;
    genutil::NoiseSource noise;
    float noiseSigma = 0;
    double jamPhase = 0;
    Impl(const GnssSimConfig& c, double rate) : cfg(c), fs(rate), noise(c.noiseSeed ? c.noiseSeed : c.seed) {}

    double timeAt(int64_t idx) const { return tStart + (double)idx / (fs * (1.0 + rateErr)); }

    struct Geo { double tauGeo, tauCode, tauCarr, dtsv, ionoS, tropoS, el, az; };
    Geo geometry(size_t i, double tRx) const {
        const GpsEphemeris& e = sats[i].eph;
        double tau = 0.075, p[3], sE = 0;
        for (int k = 0; k < 4; k++) {
            simPosition(e, tRx - tau, p, &sE);
            const double a = kEarthRate * tau, ca = std::cos(a), sa = std::sin(a);
            const double x = p[0] * ca + p[1] * sa, y = -p[0] * sa + p[1] * ca;
            const double dx = x - rx[0], dy = y - rx[1], dz = p[2] - rx[2];
            tau = std::sqrt(dx * dx + dy * dy + dz * dz) / kC;
        }
        Geo g{};
        g.tauGeo = tau;
        // elevation and azimuth from the rotated position (the receiver's local frame by its geodetic latitude and longitude)
        {
            const double a = kEarthRate * tau, ca = std::cos(a), sa = std::sin(a);
            const double s[3] = {p[0] * ca + p[1] * sa, -p[0] * sa + p[1] * ca, p[2]};
            azElFromEcef(rx, s, &g.az, &g.el);
        }
        if (cfg.realAtmosphere) {
            // ionosphere: a thin shell at 350 km, vertical delay with a daily cycle peaking at 14 h local time (not the Klobuchar form)
            const double lt = std::fmod(tRx + cfg.lonDeg / 15.0 * 3600.0, 86400.0);
            const double vert = 3e-9 + 12e-9 * 0.5 * (1 + std::cos(2 * kPiS * (lt - 50400.0) / 86400.0));
            const double ce = std::cos(g.el * kPiS / 180.0);
            const double q = 6378137.0 / (6378137.0 + 350e3) * ce;
            g.ionoS = vert / std::sqrt(1 - q * q);
            // troposphere: zenith delay by height, mapped with 1.001 / sqrt(0.002001 + sin^2 el)
            const double se = std::sin(g.el * kPiS / 180.0);
            const double zen = 2.3 * std::exp(-cfg.heightM / 8500.0) + 0.1;
            g.tropoS = zen * 1.001 / std::sqrt(0.002001 + se * se) / kC;
        }
        g.tauCode = g.tauGeo + g.ionoS + g.tropoS;
        g.tauCarr = g.tauGeo - g.ionoS + g.tropoS;
        const double dtc = (tRx - tau) - e.toc;
        g.dtsv = e.af0 + e.af1 * dtc + e.af2 * dtc * dtc + (e.galileo ? kGalRelF : kRelF) * e.e * e.sqrtA * sE - e.tgd;
        // a Galileo satellite keeps Galileo time (GST): its signal is ahead of GPS time by the offset between the two
        if (sats[i].sys == GnssGalileo) g.dtsv += ggtoS;
        return g;
    }
    // chips (relative to kBase) of the satellite's code at the receiver time t, and the carrier phase in cycles
    void phases(size_t i, double t, double* chips, double* cyc) const {
        const Geo g = geometry(i, t);
        *chips = kGpsCaChipRate * (sats[i].sys == GnssGalileo ? 2.0 : 1.0) * (t - tStart - g.tauCode + g.dtsv);
        *cyc = -kGpsL1Hz * g.tauCarr + cfg.cfoHz * (t - tStart);
    }

    // the subframe `s` (index since the start of the week) of satellite i
    void buildSubframe(size_t i, int64_t s, uint8_t* bits) const {
        Words w;
        const int id = (int)(s % 5) + 1;
        const int page = (int)((s / 5) % 25) + 1;
        if (id == 1) w = sf1[i]; else if (id == 2) w = sf2[i]; else if (id == 3) w = sf3[i];
        else if (sats[i].sys != GnssGps) {
            // QZSS: subframes 4 and 5 carry its own almanac and other pages (IS-QZSS-PNT): not simulated, only the reserved pattern with data ID 3
            for (int k = 2; k < 10; k++) w.d[k] = 0xAAAAAA;
            putBits(w, 61, 2, 3);
            putBits(w, 63, 6, 0);
        } else {
            const int svId = id == 5 ? (page <= 24 ? page : 51) : (page == 2 ? 25 : page == 3 ? 26 : page == 4 ? 27 : page == 5 ? 28 : page == 7 ? 29 : page == 8 ? 30 : page == 9 ? 31 : page == 10 ? 32 :
                                                                  page == 12 ? 62 : page == 13 ? 52 : page == 14 ? 53 : page == 15 ? 54 : page == 17 ? 55 : page == 18 ? 56 : page == 19 ? 58 :
                                                                  page == 20 ? 59 : page == 21 ? 60 : page == 22 ? 61 : page == 23 ? 62 : page >= 24 ? 63 : 57);
            if (svId >= 1 && svId <= 30) w = alm[svId - 1];
            else if (id == 4 && svId == 56) w = page18;
            else if (id == 5 && svId == 51) w = page25s5;
            else if (svId == 31 || svId == 32) { /* satellites that are not in service: an empty almanac */ }
            else { for (int k = 2; k < 10; k++) w.d[k] = 0xAAAAAA; }            // reserved pages carry alternating bits
            putBits(w, 61, 2, 1);                                                 // data ID
            putBits(w, 63, 6, (uint32_t)svId);
        }
        // telemetry and handover words
        putBits(w, 1, 8, kLnavPreamble);
        putBits(w, 9, 14, 0x1A2B & 0x3FFF);
        putBits(w, 23, 2, 0);
        putBits(w, 31, 17, (uint32_t)((s + 1) % 100800));
        putBits(w, 48, 1, 0);
        putBits(w, 49, 1, 1);
        putBits(w, 50, 3, (uint32_t)id);
        encodeSubframe(w, bits);
    }

    // ---- SBAS: one 250 bit message a second (message m starts at second m of the week), types in a fixed rotation, random contents
    void sbasMessage(size_t i, int64_t m, uint8_t* out) const {
        static const int kTypes[16] = {9, 2, 3, 4, 5, 18, 26, 25, 7, 10, 12, 17, 24, 27, 28, 1};
        const int type = kTypes[((m % 16) + 16) % 16];
        Rng r((uint64_t)sats[i].prn * 1000003ull + (uint64_t)m);
        uint8_t d[212];
        for (auto& v : d) v = (uint8_t)(r.next() >> 63);
        sbasBuildMessage((int)(m % 3), type, d, out);
    }
    void sbasSymbols(size_t i, int64_t m, uint8_t* out500) const {
        uint8_t prev[250], cur[250];
        sbasMessage(i, m - 1, prev);
        sbasMessage(i, m, cur);
        int st = 0;
        for (int k = 244; k < 250; k++) st = (st >> 1) | (prev[k] << 5);     // the last six bits, the newest in bit 5
        convEncode(cur, 250, false, st, out500);
    }

    // ---- Galileo I/NAV: the word of the page that begins (even part) at GST second s0 of the week, sub-frame layout of ICD Table 38 on E1-B
    void galWord(size_t i, int64_t s0, uint8_t* w) const {
        std::memset(w, 0, 128);
        const int t = (int)(s0 % 30);
        const int64_t sub = s0 / 30;
        int type = 0;
        switch (t) {
        case 1: type = 2; break;
        case 3: type = 4; break;
        case 5: type = 6; break;
        case 7: type = (sub & 1) ? 9 : 7; break;
        case 9: type = (sub & 1) ? 10 : 8; break;
        case 21: type = 1; break;
        case 23: type = 3; break;
        case 25: type = 5; break;
        default: type = 0; break;              // the spare word in place of the reduced and FEC2 words, which are not simulated
        }
        const int wn = (kWeek - 1024) & 4095;
        const uint32_t tow = (uint32_t)(s0 % 604800);
        if (type >= 1 && type <= 4) { std::memcpy(w, &galWords[i][(size_t)(type - 1) * 128], 128); return; }
        inavPut(w, 0, 6, (uint32_t)type);
        if (type == 0) { inavPut(w, 6, 2, 2); inavPut(w, 96, 12, (uint32_t)wn); inavPut(w, 108, 20, tow); }
        else if (type == 5) {
            inavPut(w, 6, 11, 120); inavPut(w, 17, 11, twos(5, 11)); inavPut(w, 28, 14, twos(-3, 14));
            const int64_t bgd = qs(sats[i].eph.tgd, std::ldexp(1.0, -32), 10);
            inavPut(w, 47, 10, twos(bgd, 10)); inavPut(w, 57, 10, twos(bgd, 10));
            inavPut(w, 73, 12, (uint32_t)wn); inavPut(w, 85, 20, tow);
        } else if (type == 6) {
            inavPut(w, 6, 32, twos(-2, 32)); inavPut(w, 38, 24, twos(3, 24)); inavPut(w, 62, 8, 18); inavPut(w, 70, 8, 15);
            inavPut(w, 78, 8, (uint32_t)(wn & 255)); inavPut(w, 86, 8, (uint32_t)(wn & 255)); inavPut(w, 94, 3, 7); inavPut(w, 97, 8, 18);
            inavPut(w, 105, 20, tow);
        } else if (type >= 7 && type <= 10) {
            inavPut(w, 6, 4, 5);                  // IODa; the almanac fields are left zero (not simulated)
            if (type == 10) {
                inavPut(w, 86, 16, twos(qs(ggtoS, std::ldexp(1.0, -35), 16), 16));
                inavPut(w, 102, 12, 0);
                inavPut(w, 114, 8, (uint32_t)((tStart / 3600.0)) & 0xFF);
                inavPut(w, 122, 6, (uint32_t)(wn & 63));
            }
        }
    }
    // the 250 symbols of the page part sent during GST second `sec` of the week: even parts at odd seconds
    void galPart(size_t i, int64_t sec, uint8_t* out250) const {
        const int64_t s0 = (sec & 1) ? sec : sec - 1;
        uint8_t w[128], ev[120], od[120];
        galWord(i, s0, w);
        const int ssp = (int)((((s0 + 1) / 2 - 1) % 3 + 3) % 3);
        inavBuildPage(w, nullptr, ssp, ev, od);
        inavEncodePart((sec & 1) ? ev : od, out250);
    }

    // the value (+-1) of code element `idx` (chips, Galileo half chips, counted from the start of the week) with the data on it
    float element(Chan& c, int64_t idx) {
        if (c.sys == GnssGalileo) {
            const int64_t sym = idx / 8184, part = sym / 250;
            if (part != c.msgCached) { galPart(c.sat, part, c.syms); c.msgCached = part; }
            const float d = c.syms[sym % 250] ? -1.f : 1.f;
            const int chip = (int)((idx / 2) % kGalE1Len);
            const float sc = (idx & 1) ? -1.f : 1.f;          // BOC(1,1), sine phase: + then - within each chip
            return d * sc * (c.code[chip] ? -1.f : 1.f);
        }
        if (c.sys == GnssSbas) {
            const int64_t sym = idx / 2046, m = sym / 500;
            if (m != c.msgCached) { sbasSymbols(c.sat, m, c.syms); c.msgCached = m; }
            const float d = c.syms[sym % 500] ? -1.f : 1.f;
            return d * (c.code[idx % 1023] ? -1.f : 1.f);
        }
        const int64_t bit = idx / 20460;
        const int64_t sf = bit / 300;
        if (sf != c.sfCached) { buildSubframe(c.sat, sf, c.bits); c.sfCached = sf; }
        const float d = c.bits[bit % 300] ? -1.f : 1.f;
        return d * (c.code[idx % 1023] ? -1.f : 1.f);
    }
};

// ------------------------------------------------------------------ the constellation
namespace {

struct Kep {
    double a, e, inc, raan, argp, m0;
};

// the fields of the ephemeris and the almanac as integers, to words, and the decoded values that those integers stand for
double wrapPi(double a) { return a - 2 * kPiS * std::floor(a / (2 * kPiS) + 0.5); }

void quantiseAndPack(GnssSimSat& s, const Kep& kIn, Rng& rng, Words& f1, Words& f2, Words& f3, Words& al, bool healthy) {
    Kep k = kIn;
    k.m0 = wrapPi(k.m0); k.raan = wrapPi(k.raan); k.argp = wrapPi(k.argp);
    GpsEphemeris& e = s.eph;
    e.prn = s.prn;
    e.has1 = e.has2 = e.has3 = true;
    // values that make the orbit more than a bare ellipse
    const double cuc = rng.r(-6e-6, 6e-6), cus = rng.r(-6e-6, 6e-6), crc = rng.r(-250, 250), crs = rng.r(-60, 60), cic = rng.r(-1.5e-7, 1.5e-7), cis = rng.r(-1.5e-7, 1.5e-7);
    const double dn = rng.r(3e-9, 6e-9), idot = rng.r(-6e-11, 6e-11);
    const double omegaDot = -7.9e-9 + rng.r(-4e-10, 4e-10);            // the nodal regression of a 55 degree orbit at this height
    const double af0 = rng.r(-9e-5, 9e-5), af1 = rng.r(-9e-12, 9e-12), tgd = rng.r(-9e-9, 9e-9);
    const double sqrtA = std::sqrt(k.a);
    // integers
    const uint32_t rSqrtA = qu(sqrtA, std::ldexp(1.0, -19), 32);
    const uint32_t rE = qu(k.e, std::ldexp(1.0, -33), 32);
    const int64_t rM0 = qs(k.m0 / kPiS, std::ldexp(1.0, -31), 32), rO0 = qs(k.raan / kPiS, std::ldexp(1.0, -31), 32);
    const int64_t rI0 = qs(k.inc / kPiS, std::ldexp(1.0, -31), 32), rW = qs(k.argp / kPiS, std::ldexp(1.0, -31), 32);
    const int64_t rDn = qs(dn / kPiS, std::ldexp(1.0, -43), 16), rOd = qs(omegaDot / kPiS, std::ldexp(1.0, -43), 24), rId = qs(idot / kPiS, std::ldexp(1.0, -43), 14);
    const int64_t rCuc = qs(cuc, std::ldexp(1.0, -29), 16), rCus = qs(cus, std::ldexp(1.0, -29), 16), rCic = qs(cic, std::ldexp(1.0, -29), 16), rCis = qs(cis, std::ldexp(1.0, -29), 16);
    const int64_t rCrc = qs(crc, std::ldexp(1.0, -5), 16), rCrs = qs(crs, std::ldexp(1.0, -5), 16);
    const int64_t rAf0 = qs(af0, std::ldexp(1.0, -31), 22), rAf1 = qs(af1, std::ldexp(1.0, -43), 16), rTgd = qs(tgd, std::ldexp(1.0, -31), 8);
    const uint32_t rToe = (uint32_t)(kToe / 16.0), rToc = rToe;
    const uint32_t iode = (uint32_t)(0x41 + s.prn), iodc = iode;
    // the values the integers stand for
    e.sqrtA = rSqrtA * std::ldexp(1.0, -19); e.e = rE * std::ldexp(1.0, -33);
    e.m0 = (double)rM0 * std::ldexp(1.0, -31) * kPiS; e.omega0 = (double)rO0 * std::ldexp(1.0, -31) * kPiS;
    e.i0 = (double)rI0 * std::ldexp(1.0, -31) * kPiS; e.omega = (double)rW * std::ldexp(1.0, -31) * kPiS;
    e.dn = (double)rDn * std::ldexp(1.0, -43) * kPiS; e.omegaDot = (double)rOd * std::ldexp(1.0, -43) * kPiS; e.idot = (double)rId * std::ldexp(1.0, -43) * kPiS;
    e.cuc = rCuc * std::ldexp(1.0, -29); e.cus = rCus * std::ldexp(1.0, -29); e.cic = rCic * std::ldexp(1.0, -29); e.cis = rCis * std::ldexp(1.0, -29);
    e.crc = rCrc * std::ldexp(1.0, -5); e.crs = rCrs * std::ldexp(1.0, -5);
    e.af0 = rAf0 * std::ldexp(1.0, -31); e.af1 = rAf1 * std::ldexp(1.0, -43); e.af2 = 0; e.tgd = rTgd * std::ldexp(1.0, -31);
    e.toe = rToe * 16.0; e.toc = rToc * 16.0;
    e.wn = kWeek & 1023; e.uraIndex = 2; e.health = healthy ? 0 : 63; e.iodc = (int)iodc; e.iode2 = e.iode3 = (int)(iode & 0xFF); e.fitFlag = 0;
    // subframe 1
    putBits(f1, 61, 10, (uint32_t)(kWeek & 1023));
    putBits(f1, 71, 2, 1);
    putBits(f1, 73, 4, 2);
    putBits(f1, 77, 6, healthy ? 0 : 63);
    putBits(f1, 83, 2, iodc >> 8);
    putBits(f1, 91, 24, 0x800000 | 0xAAAAA);       // reserved
    for (int w = 4; w <= 6; w++) putBits(f1, 30 * (w - 1) + 1, 24, 0xAAAAAA);
    putBits(f1, 181, 16, 0xAAAA);
    putBits(f1, 197, 8, twos(rTgd, 8));
    putBits(f1, 211, 8, iodc & 0xFF);
    putBits(f1, 219, 16, rToc);
    putBits(f1, 241, 8, 0);
    putBits(f1, 249, 16, twos(rAf1, 16));
    putBits(f1, 271, 22, twos(rAf0, 22));
    // subframe 2
    putBits(f2, 61, 8, iode & 0xFF);
    putBits(f2, 69, 16, twos(rCrs, 16));
    putBits(f2, 91, 16, twos(rDn, 16));
    putSplit(f2, 107, 8, 121, 24, twos(rM0, 32));
    putBits(f2, 151, 16, twos(rCuc, 16));
    putSplit(f2, 167, 8, 181, 24, rE);
    putBits(f2, 211, 16, twos(rCus, 16));
    putSplit(f2, 227, 8, 241, 24, rSqrtA);
    putBits(f2, 271, 16, rToe);
    putBits(f2, 287, 1, 0);
    putBits(f2, 288, 5, 31);
    // subframe 3
    putBits(f3, 61, 16, twos(rCic, 16));
    putSplit(f3, 77, 8, 91, 24, twos(rO0, 32));
    putBits(f3, 121, 16, twos(rCis, 16));
    putSplit(f3, 137, 8, 151, 24, twos(rI0, 32));
    putBits(f3, 181, 16, twos(rCrc, 16));
    putSplit(f3, 197, 8, 211, 24, twos(rW, 32));
    putBits(f3, 241, 24, twos(rOd, 24));
    putBits(f3, 271, 8, iode & 0xFF);
    putBits(f3, 279, 14, twos(rId, 14));

    // almanac: the same orbit without the harmonic terms, referred to toa
    const double A = e.sqrtA * e.sqrtA, n0 = std::sqrt(kGpsMu / (A * A * A)) + e.dn;
    GpsAlmanac& a = s.alm;
    a.prn = s.prn; a.valid = true; a.health = healthy ? 0 : 63;
    const double dta = kToa - e.toe;
    const double m0a = e.m0 + n0 * dta;
    const double om0a = e.omega0 + e.omegaDot * dta;
    const double dI = e.i0 - 0.3 * kPiS;
    const uint32_t aE = qu(e.e, std::ldexp(1.0, -21), 16), aToa = (uint32_t)(kToa / 4096.0);
    const int64_t aDi = qs(dI / kPiS, std::ldexp(1.0, -19), 16), aOd = qs(e.omegaDot / kPiS, std::ldexp(1.0, -38), 16);
    const uint32_t aSq = qu(e.sqrtA, std::ldexp(1.0, -11), 24);
    const int64_t aO0 = qs(om0a / kPiS - 2 * std::floor(om0a / kPiS / 2 + 0.5), std::ldexp(1.0, -23), 24);
    const int64_t aW = qs(e.omega / kPiS, std::ldexp(1.0, -23), 24);
    const int64_t aM = qs((m0a - 2 * kPiS * std::floor(m0a / (2 * kPiS) + 0.5)) / kPiS, std::ldexp(1.0, -23), 24);
    const int64_t aAf0 = qs(e.af0, std::ldexp(1.0, -20), 11), aAf1 = qs(e.af1, std::ldexp(1.0, -38), 11);
    a.e = aE * std::ldexp(1.0, -21); a.toa = aToa * 4096.0; a.i0 = (0.3 + aDi * std::ldexp(1.0, -19)) * kPiS;
    a.omegaDot = aOd * std::ldexp(1.0, -38) * kPiS; a.sqrtA = aSq * std::ldexp(1.0, -11);
    a.omega0 = (double)aO0 * std::ldexp(1.0, -23) * kPiS; a.omega = (double)aW * std::ldexp(1.0, -23) * kPiS; a.m0 = (double)aM * std::ldexp(1.0, -23) * kPiS;
    a.af0 = aAf0 * std::ldexp(1.0, -20); a.af1 = aAf1 * std::ldexp(1.0, -38);
    putBits(al, 69, 16, aE);
    putBits(al, 91, 8, aToa);
    putBits(al, 99, 16, twos(aDi, 16));
    putBits(al, 121, 16, twos(aOd, 16));
    putBits(al, 137, 8, healthy ? 0 : 63);
    putBits(al, 151, 24, aSq);
    putBits(al, 181, 24, twos(aO0, 24));
    putBits(al, 211, 24, twos(aW, 24));
    putBits(al, 241, 24, twos(aM, 24));
    putSplit(al, 271, 8, 290, 3, twos(aAf0, 11));
    putBits(al, 279, 11, twos(aAf1, 11));
}

} // namespace

GnssSim::GnssSim(const GnssSimConfig& cfg, double sampleRate) : p_(std::make_unique<Impl>(cfg, sampleRate)) {
    Impl& p = *p_;
    llaToEcef(cfg.latDeg, cfg.lonDeg, cfg.heightM, p.rx);
    p.rateErr = -cfg.cfoHz / kGpsL1Hz + cfg.sroPpm * 1e-6;
    // the frame at the start carries page 17 of subframes 4 and 5, so the ionosphere and UTC page (subframe 4, page 18) follows in the next frame
    p.tStart = 354480.0 + (cfg.warmStart ? 22.0 : 13.0);
    p.kBase = (int64_t)std::llround(kGpsCaChipRate * p.tStart);
    p.noiseSigma = (float)cfg.noiseRms;

    // choose the orbital phasing so that 9 to 10 satellites are above the mask at the start, spread around the sky
    Rng rng(cfg.seed * 7919u + 13u);
    struct Slot { double e, argp, a, inc; };
    Slot slots[30];
    for (int i = 0; i < 30; i++) slots[i] = {rng.r(0.001, 0.015), rng.r(0, 2 * kPiS), 26560e3 + rng.r(-6e3, 6e3), (55.0 + rng.r(-0.8, 0.8)) * kPiS / 180.0};
    auto kepOf = [&](int idx, double raanOff, double mOff) {
        const int plane = idx / 5, slot = idx % 5;
        Kep k;
        k.a = slots[idx].a; k.e = slots[idx].e; k.inc = slots[idx].inc; k.argp = slots[idx].argp;
        k.raan = (raanOff + 60.0 * plane) * kPiS / 180.0;
        k.m0 = (mOff + 72.0 * slot + 24.0 * plane) * kPiS / 180.0;
        return k;
    };
    // count of satellites above the mask and the PDOP of the geometry, for a phasing of the planes
    auto geometryOf = [&](double raanOff, double mOff, int* count, double* pdop) {
        double G[16] = {};
        int cnt = 0;
        for (int i = 0; i < 30; i++) {
            GpsEphemeris e;
            const Kep k = kepOf(i, raanOff, mOff);
            e.sqrtA = std::sqrt(k.a); e.e = k.e; e.i0 = k.inc; e.omega = k.argp; e.m0 = k.m0; e.omega0 = k.raan; e.toe = p.tStart; e.omegaDot = -7.9e-9;
            double pos[3], az, el;
            simPosition(e, p.tStart, pos, nullptr);
            azElFromEcef(p.rx, pos, &az, &el);
            if (el < cfg.maskDeg) continue;
            cnt++;
            const double dx = pos[0] - p.rx[0], dy = pos[1] - p.rx[1], dz = pos[2] - p.rx[2], r = std::sqrt(dx * dx + dy * dy + dz * dz);
            const double h[4] = {-dx / r, -dy / r, -dz / r, 1.0};
            for (int a2 = 0; a2 < 4; a2++) for (int b2 = 0; b2 < 4; b2++) G[a2 * 4 + b2] += h[a2] * h[b2];
        }
        *count = cnt;
        *pdop = 99;
        if (cnt < 5) return;
        // invert G by Gauss-Jordan
        double M[4][8];
        for (int a2 = 0; a2 < 4; a2++) for (int b2 = 0; b2 < 8; b2++) M[a2][b2] = b2 < 4 ? G[a2 * 4 + b2] : (b2 - 4 == a2 ? 1.0 : 0.0);
        for (int c = 0; c < 4; c++) {
            int piv = c;
            for (int r2 = c + 1; r2 < 4; r2++) if (std::fabs(M[r2][c]) > std::fabs(M[piv][c])) piv = r2;
            if (std::fabs(M[piv][c]) < 1e-12) return;
            for (int b2 = 0; b2 < 8; b2++) std::swap(M[c][b2], M[piv][b2]);
            const double d = M[c][c];
            for (int b2 = 0; b2 < 8; b2++) M[c][b2] /= d;
            for (int r2 = 0; r2 < 4; r2++) if (r2 != c) { const double f = M[r2][c]; for (int b2 = 0; b2 < 8; b2++) M[r2][b2] -= f * M[c][b2]; }
        }
        *pdop = std::sqrt(M[0][4] + M[1][5] + M[2][6]);
    };
    double bestR = 0, bestM = 0, bestScore = -1e9;
    for (int ro = 0; ro < 360; ro += 5)
        for (int mo = 0; mo < 360; mo += 10) {
            int c; double pd;
            geometryOf((double)ro, (double)mo, &c, &pd);
            const double score = (c >= 9 && c <= 10 ? 100.0 : 0.0) - pd * 10.0 - std::abs(c - 10);
            if (score > bestScore) { bestScore = score; bestR = ro; bestM = mo; }
        }

    p.sats.resize(30);
    p.sf1.resize(30); p.sf2.resize(30); p.sf3.resize(30); p.alm.resize(30);
    for (int i = 0; i < 30; i++) {
        GnssSimSat& s = p.sats[i];
        s.sys = GnssGps; s.prn = i + 1;
        Rng r2(cfg.seed * 104729u + (uint64_t)i * 31u + 5u);
        quantiseAndPack(s, kepOf(i, bestR, bestM), r2, p.sf1[i], p.sf2[i], p.sf3[i], p.alm[i], true);
        double pos[3];
        // the elevation at the start, geometry with the transmit time of this satellite
        const Impl::Geo g = p.geometry((size_t)i, p.tStart);
        (void)pos;
        s.azDeg = g.az; s.elDeg = g.el;
        s.transmitted = g.el >= cfg.maskDeg && (cfg.systems & 1u);
        const double R = g.tauGeo * kC;
        const double rZen = 26560e3 - 6378137.0;
        s.cn0 = cfg.cn0Top - 20.0 * std::log10(R / rZen) - 8.0 * (1.0 - std::sin(g.el * kPiS / 180.0));
    }
    if (cfg.maxSats > 0) {
        std::vector<int> idx;
        for (int i = 0; i < 30; i++) if (p.sats[i].transmitted) idx.push_back(i);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return p.sats[a].elDeg > p.sats[b].elDeg; });
        for (size_t k = (size_t)cfg.maxSats; k < idx.size(); k++) p.sats[idx[k]].transmitted = false;
    }
    // ---- the other systems, after the GPS satellites (whose numbers and random draws stay as they were)
    auto cn0Of = [&](const Impl::Geo& g, double rZen) {
        return cfg.cn0Top - 20.0 * std::log10(g.tauGeo * kC / rZen) - 8.0 * (1.0 - std::sin(std::max(0.0, g.el) * kPiS / 180.0));
    };
    if (cfg.systems & gnssSystemBit(GnssQzss)) {
        // three quasi-zenith orbits (IGSO, 41 degrees, e 0.075, perigee at 270 degrees) on one ground track; the track is placed so that the
        // satellites are in view of the receiver (the real ones serve Japan and Australia)
        double bestOff = 0, bestScore = -1e9;
        for (int off = 0; off < 360; off += 10) {
            double score = 0;
            for (int k = 0; k < 3; k++) {
                GpsEphemeris e;
                e.sqrtA = std::sqrt(42164e3); e.e = 0.075; e.i0 = 41.0 * kPiS / 180.0; e.omega = 270.0 * kPiS / 180.0;
                e.omega0 = (off + 120.0 * k) * kPiS / 180.0 + kEarthRate * p.tStart; e.m0 = -120.0 * k * kPiS / 180.0; e.toe = p.tStart;
                double pos[3], az, el;
                simPosition(e, p.tStart, pos, nullptr);
                azElFromEcef(p.rx, pos, &az, &el);
                score += el > cfg.maskDeg + 5 ? 10 + el * 0.1 : 0;
            }
            if (score > bestScore) { bestScore = score; bestOff = off; }
        }
        for (int k = 0; k < 3; k++) {
            GnssSimSat q;
            q.sys = GnssQzss; q.prn = 193 + k;
            Rng r2(cfg.seed * 7717u + (uint64_t)k * 17u + 3u);
            Kep kp{42164e3, 0.075, 41.0 * kPiS / 180.0, (bestOff + 120.0 * k) * kPiS / 180.0 + kEarthRate * p.tStart, 270.0 * kPiS / 180.0, -120.0 * k * kPiS / 180.0};
            // the mean anomaly at toe that puts the satellite where the search placed it at the start
            const double n0 = std::sqrt(kGpsMu / (kp.a * kp.a * kp.a));
            kp.m0 += n0 * (kToe - p.tStart);
            p.sats.push_back(q);
            p.sf1.emplace_back(); p.sf2.emplace_back(); p.sf3.emplace_back(); p.alm.emplace_back();
            quantiseAndPack(p.sats.back(), kp, r2, p.sf1.back(), p.sf2.back(), p.sf3.back(), p.alm.back(), true);
        }
    }
    if (cfg.systems & gnssSystemBit(GnssSbas)) {
        // geostationary: three of the PRNs in view of the Gulf (at their real longitudes); a circular equatorial orbit that turns with the earth
        const int prns[3] = {123, 127, 128};
        const double lons[3] = {31.5, 55.0, 83.0};
        for (int k = 0; k < 3; k++) {
            GnssSimSat g;
            g.sys = GnssSbas; g.prn = prns[k];
            GpsEphemeris& e = g.eph;
            e.prn = g.prn;
            const double A = 42164.17e3;
            e.sqrtA = std::sqrt(A); e.e = 0.0; e.i0 = 0.0005; e.omega = 0; e.m0 = 0; e.toe = kToe; e.toc = kToe;
            e.dn = kEarthRate - std::sqrt(kGpsMu / (A * A * A));
            e.omega0 = lons[k] * kPiS / 180.0 + kEarthRate * kToe;
            e.has1 = e.has2 = e.has3 = true;
            p.sats.push_back(g);
            p.sf1.emplace_back(); p.sf2.emplace_back(); p.sf3.emplace_back(); p.alm.emplace_back();
        }
    }
    if (cfg.systems & gnssSystemBit(GnssGalileo)) {
        // a Walker 24/3/1 constellation like Galileo's: 29600 km, 56 degrees; the phasing of the planes chosen for 7 or 8 satellites in view
        double bestR = 0, bestM = 0, bestScore = -1e9;
        for (int ro = 0; ro < 120; ro += 5)
            for (int mo = 0; mo < 45; mo += 5) {
                int c = 0;
                for (int k = 0; k < 24; k++) {
                    GpsEphemeris e;
                    e.galileo = true;
                    e.sqrtA = std::sqrt(29600e3); e.i0 = 56.0 * kPiS / 180.0; e.omega0 = (ro + 120.0 * (k / 8)) * kPiS / 180.0 + kEarthRate * p.tStart;
                    e.m0 = (mo + 45.0 * (k % 8) + 15.0 * (k / 8)) * kPiS / 180.0; e.toe = p.tStart;
                    double pos[3], az, el;
                    simPosition(e, p.tStart, pos, nullptr);
                    azElFromEcef(p.rx, pos, &az, &el);
                    c += el > cfg.maskDeg + 3;
                }
                const double score = -std::fabs(c - 7.5);
                if (score > bestScore) { bestScore = score; bestR = ro; bestM = mo; }
            }
        for (int k = 0; k < 24; k++) {
            GnssSimSat g;
            g.sys = GnssGalileo; g.prn = k + 1;
            Rng r2(cfg.seed * 30011u + (uint64_t)k * 13u + 7u);
            GpsEphemeris& e = g.eph;
            e.prn = g.prn; e.galileo = true;
            const double A = 29600e3 + r2.r(-3e3, 3e3), n0 = std::sqrt(kGalMu / (A * A * A));
            const double m0t = (bestM + 45.0 * (k % 8) + 15.0 * (k / 8)) * kPiS / 180.0 + n0 * (kToe - p.tStart);
            const double raan = (bestR + 120.0 * (k / 8)) * kPiS / 180.0 + kEarthRate * p.tStart;
            // the transmitted integers (OS SIS ICD Tables 65, 68, 70) and the values they stand for
            const uint32_t rSq = qu(std::sqrt(A), std::ldexp(1.0, -19), 32), rE = qu(r2.r(0.0001, 0.0006), std::ldexp(1.0, -33), 32);
            const int64_t rM0 = qs(wrapPi(m0t) / kPiS, std::ldexp(1.0, -31), 32), rO0 = qs(wrapPi(raan) / kPiS, std::ldexp(1.0, -31), 32);
            const int64_t rI0 = qs((56.0 + r2.r(-0.5, 0.5)) / 180.0, std::ldexp(1.0, -31), 32), rW = qs(wrapPi(r2.r(0, 2 * kPiS)) / kPiS, std::ldexp(1.0, -31), 32);
            const int64_t rId = qs(r2.r(-5e-11, 5e-11) / kPiS, std::ldexp(1.0, -43), 14), rOd = qs((-5.6e-9 + r2.r(-3e-10, 3e-10)) / kPiS, std::ldexp(1.0, -43), 24);
            const int64_t rDn = qs(r2.r(2e-9, 4e-9) / kPiS, std::ldexp(1.0, -43), 16);
            const int64_t rCuc = qs(r2.r(-4e-6, 4e-6), std::ldexp(1.0, -29), 16), rCus = qs(r2.r(-4e-6, 4e-6), std::ldexp(1.0, -29), 16);
            const int64_t rCrc = qs(r2.r(-200, 200), std::ldexp(1.0, -5), 16), rCrs = qs(r2.r(-50, 50), std::ldexp(1.0, -5), 16);
            const int64_t rCic = qs(r2.r(-1e-7, 1e-7), std::ldexp(1.0, -29), 16), rCis = qs(r2.r(-1e-7, 1e-7), std::ldexp(1.0, -29), 16);
            const int64_t rAf0 = qs(r2.r(-5e-4, 5e-4), std::ldexp(1.0, -34), 31), rAf1 = qs(r2.r(-5e-12, 5e-12), std::ldexp(1.0, -46), 21);
            const int64_t rBgd = qs(r2.r(-5e-9, 5e-9), std::ldexp(1.0, -32), 10);
            const uint32_t rToe = (uint32_t)(kToe / 60.0), iod = (uint32_t)(100 + k);
            e.sqrtA = rSq * std::ldexp(1.0, -19); e.e = rE * std::ldexp(1.0, -33);
            e.m0 = (double)rM0 * std::ldexp(1.0, -31) * kPiS; e.omega0 = (double)rO0 * std::ldexp(1.0, -31) * kPiS;
            e.i0 = (double)rI0 * std::ldexp(1.0, -31) * kPiS; e.omega = (double)rW * std::ldexp(1.0, -31) * kPiS;
            e.idot = (double)rId * std::ldexp(1.0, -43) * kPiS; e.omegaDot = (double)rOd * std::ldexp(1.0, -43) * kPiS; e.dn = (double)rDn * std::ldexp(1.0, -43) * kPiS;
            e.cuc = rCuc * std::ldexp(1.0, -29); e.cus = rCus * std::ldexp(1.0, -29); e.cic = rCic * std::ldexp(1.0, -29); e.cis = rCis * std::ldexp(1.0, -29);
            e.crc = rCrc * std::ldexp(1.0, -5); e.crs = rCrs * std::ldexp(1.0, -5);
            e.af0 = rAf0 * std::ldexp(1.0, -34); e.af1 = rAf1 * std::ldexp(1.0, -46); e.af2 = 0; e.tgd = rBgd * std::ldexp(1.0, -32);
            e.toe = rToe * 60.0; e.toc = e.toe; e.iode2 = e.iode3 = e.iodc = (int)iod; e.has1 = e.has2 = e.has3 = true; e.health = 0;
            std::vector<uint8_t> w(4 * 128, 0);
            uint8_t* w1 = &w[0]; uint8_t* w2 = &w[128]; uint8_t* w3 = &w[256]; uint8_t* w4 = &w[384];
            inavPut(w1, 0, 6, 1); inavPut(w1, 6, 10, iod); inavPut(w1, 16, 14, rToe); inavPut(w1, 30, 32, twos(rM0, 32)); inavPut(w1, 62, 32, rE); inavPut(w1, 94, 32, rSq);
            inavPut(w2, 0, 6, 2); inavPut(w2, 6, 10, iod); inavPut(w2, 16, 32, twos(rO0, 32)); inavPut(w2, 48, 32, twos(rI0, 32)); inavPut(w2, 80, 32, twos(rW, 32)); inavPut(w2, 112, 14, twos(rId, 14));
            inavPut(w3, 0, 6, 3); inavPut(w3, 6, 10, iod); inavPut(w3, 16, 24, twos(rOd, 24)); inavPut(w3, 40, 16, twos(rDn, 16)); inavPut(w3, 56, 16, twos(rCuc, 16));
            inavPut(w3, 72, 16, twos(rCus, 16)); inavPut(w3, 88, 16, twos(rCrc, 16)); inavPut(w3, 104, 16, twos(rCrs, 16)); inavPut(w3, 120, 8, 107);
            inavPut(w4, 0, 6, 4); inavPut(w4, 6, 10, iod); inavPut(w4, 16, 6, (uint32_t)g.prn); inavPut(w4, 22, 16, twos(rCic, 16)); inavPut(w4, 38, 16, twos(rCis, 16));
            inavPut(w4, 54, 14, rToe); inavPut(w4, 68, 31, twos(rAf0, 31)); inavPut(w4, 99, 21, twos(rAf1, 21)); inavPut(w4, 120, 6, 0);
            p.sats.push_back(g);
            p.sf1.emplace_back(); p.sf2.emplace_back(); p.sf3.emplace_back(); p.alm.emplace_back();
            p.galWords.resize(p.sats.size());
            p.galWords.back() = w;
        }
    }
    p.galWords.resize(p.sats.size());
    for (size_t i = 30; i < p.sats.size(); i++) {
        GnssSimSat& s = p.sats[i];
        const Impl::Geo g = p.geometry(i, p.tStart);
        s.azDeg = g.az; s.elDeg = g.el;
        s.transmitted = g.el >= cfg.maskDeg;
        // Galileo: the E1-B data component carries half the E1 power (the pilot E1-C is not simulated); SBAS a little weaker
        s.cn0 = cn0Of(g, s.sys == GnssGalileo ? 29600e3 - 6378137.0 : 35786e3) - (s.sys == GnssGalileo ? 3.0 : s.sys == GnssSbas ? 2.0 : 0.0);
    }

    // broadcast parameters of the ionosphere and UTC: typical values (the integers of the fields)
    {
        const int a[4] = {13, 2, -1, -1}, b[4] = {44, 3, -2, -4};
        const double aS[4] = {std::ldexp(1.0, -30), std::ldexp(1.0, -27), std::ldexp(1.0, -24), std::ldexp(1.0, -24)};
        const double bS[4] = {std::ldexp(1.0, 11), std::ldexp(1.0, 14), std::ldexp(1.0, 16), std::ldexp(1.0, 16)};
        for (int k = 0; k < 4; k++) { p.iono.alpha[k] = a[k] * aS[k]; p.iono.beta[k] = b[k] * bS[k]; }
        p.iono.valid = true;
        Words& w = p.page18;
        putBits(w, 69, 8, twos(a[0], 8)); putBits(w, 77, 8, twos(a[1], 8)); putBits(w, 91, 8, twos(a[2], 8)); putBits(w, 99, 8, twos(a[3], 8));
        putBits(w, 107, 8, twos(b[0], 8)); putBits(w, 121, 8, twos(b[1], 8)); putBits(w, 129, 8, twos(b[2], 8)); putBits(w, 137, 8, twos(b[3], 8));
        const int a1 = 3, a0 = -2, tot = 15, wnt = kWeek & 255, dtls = 18;
        putBits(w, 151, 24, twos(a1, 24));
        putSplit(w, 181, 24, 211, 8, twos(a0, 32));
        putBits(w, 219, 8, (uint32_t)tot); putBits(w, 227, 8, (uint32_t)wnt);
        putBits(w, 241, 8, (uint32_t)dtls); putBits(w, 249, 8, (uint32_t)wnt); putBits(w, 257, 8, 7); putBits(w, 271, 8, (uint32_t)dtls);
        p.utc.valid = true; p.utc.a1 = a1 * std::ldexp(1.0, -50); p.utc.a0 = a0 * std::ldexp(1.0, -30); p.utc.tot = tot * 4096; p.utc.wnt = wnt;
        p.utc.dtls = dtls; p.utc.wnlsf = wnt; p.utc.dn = 7; p.utc.dtlsf = dtls;
        // subframe 5 page 25: almanac reference time, week and health of the satellites 1..24
        Words& h = p.page25s5;
        putBits(h, 69, 8, (uint32_t)(kToa / 4096.0)); putBits(h, 77, 8, (uint32_t)(kWeek & 255));
        for (int k = 0; k < 24; k++) putBits(h, 30 * (3 + k / 4) + 1 + 6 * (k % 4), 6, 0);   // four 6 bit health fields to a word
    }
    // the channels
    for (size_t i = 0; i < p.sats.size(); i++) {
        if (!p.sats[i].transmitted) continue;
        Impl::Chan c;
        c.sat = i;
        c.sys = p.sats[i].sys;
        if (c.sys == GnssGalileo) { galE1bChips(p.sats[i].prn, c.code); c.mult = 2; }
        else l1caChips(p.sats[i].prn, c.code);
        c.base = p.kBase * c.mult;
        const double N0 = 2.0 * cfg.noiseRms * cfg.noiseRms / sampleRate;
        c.amp = (float)std::sqrt(std::pow(10.0, p.sats[i].cn0 / 10.0) * N0);
        p.ch.push_back(c);
    }
    p.lsegS = 0.002;
    // the first segment starts at sample 0: the centre of sample 0 is at time 0
    for (auto& c : p.ch) {
        double ch0, cy0;
        p.phases(c.sat, p.timeAt(0), &ch0, &cy0);
        c.pEnd = ch0; c.cEnd = cy0;
    }
}

GnssSim::~GnssSim() = default;
double GnssSim::sampleRate() const { return p_->fs; }
const std::vector<GnssSimSat>& GnssSim::sats() const { return p_->sats; }
const double* GnssSim::receiverEcef() const { return p_->rx; }
int GnssSim::week() const { return kWeek; }
double GnssSim::startTow() const { return p_->tStart; }
GpsIono GnssSim::iono() const { return p_->iono; }
GpsUtc GnssSim::utc() const { return p_->utc; }
double GnssSim::geometricDelay(size_t i, double tRx) const { return p_->geometry(i, tRx).tauGeo; }
void GnssSim::atmosphereDelays(size_t i, double tRx, double* ionoS, double* tropoS) const {
    const Impl::Geo g = p_->geometry(i, tRx);
    *ionoS = g.ionoS; *tropoS = g.tropoS;
}
double GnssSim::trueTime(double rxSeconds) const { return p_->tStart + rxSeconds / (1.0 + p_->rateErr); }
double GnssSim::svTransmitTime(size_t i, double tRx) const {
    const Impl::Geo g = p_->geometry(i, tRx);
    return tRx - g.tauCode + g.dtsv;
}
void GnssSim::subframeBits(size_t i, uint32_t s, uint8_t* bits) const { p_->buildSubframe(i, (int64_t)s, bits); }

void GnssSim::generate(cf32* out, size_t count) {
    Impl& p = *p_;
    const int64_t segLen = std::max<int64_t>(16, (int64_t)std::llround(p.lsegS * p.fs));
    size_t done = 0;
    // the jammer
    const double jw = 2 * kPiS * 400e3 / p.fs;
    while (done < count) {
        if (p.segLeft == 0) {
            // set up the next segment from p.n: the values at its end are the next segment's start
            const int64_t n1 = p.n + segLen;
            const double t1 = p.timeAt(n1);
            for (auto& c : p.ch) {
                double ch1, cy1;
                p.phases(c.sat, t1, &ch1, &cy1);
                c.p = c.pEnd; c.cyc = c.cEnd;
                c.dp = (ch1 - c.pEnd) / (double)segLen;
                c.dcyc = (cy1 - c.cEnd) / (double)segLen;
                c.pEnd = ch1; c.cEnd = cy1;
            }
            p.segLeft = segLen;
        }
        const size_t m = (size_t)std::min<int64_t>(p.segLeft, (int64_t)(count - done));
        cf32* o = out + done;
        for (size_t k = 0; k < m; k++) o[k] = cf32(0.f, 0.f);
        for (auto& c : p.ch) {
            const double dpf = c.dp;
            float* of = reinterpret_cast<float*>(o);
            double pe = c.p + 0.5 * c.dp;                 // end of the interval of the first sample
            double cy = c.cyc;
            // carrier by recurrence, renormalised every 256 samples
            const double w = 2 * kPiS * c.dcyc;
            const cf32 rot((float)std::cos(w), (float)std::sin(w));
            const double ph0 = 2 * kPiS * (cy - std::floor(cy));
            cf32 z((float)std::cos(ph0), (float)std::sin(ph0));
            for (size_t k = 0; k < m; k++) {
                const double fl = std::floor(pe);
                const int64_t idx = c.base + (int64_t)fl;
                const double frac = pe - fl;
                if (idx != c.lastIdx) {
                    // the element before this one, for the sample interval that straddles the boundary
                    c.prev = idx == c.lastIdx + 1 ? c.cur : p.element(c, idx - 1);
                    c.cur = p.element(c, idx);
                    c.lastIdx = idx;
                }
                // the sample integrates the chip stream over its own interval
                const float v = frac < dpf ? (float)((c.cur * frac + c.prev * (dpf - frac)) / dpf) : c.cur;
                const float a = c.amp * v;
                of[2 * k] += a * z.real();
                of[2 * k + 1] += a * z.imag();
                z *= rot;
                pe += dpf;
                if ((k & 255) == 255) z /= std::abs(z);
            }
            c.p += (double)m * c.dp;
            c.cyc += (double)m * c.dcyc;
        }
        // noise, jammer, DC
        p.noise.add(o, m, p.noiseSigma);
        if (p.cfg.jammer || p.cfg.dcOffset != 0) {
            for (size_t k = 0; k < m; k++) {
                if (p.cfg.jammer) {
                    o[k] += cf32(0.3f * (float)std::cos(p.jamPhase), 0.3f * (float)std::sin(p.jamPhase));
                    p.jamPhase += jw;
                    if (p.jamPhase > 2 * kPiS) p.jamPhase -= 2 * kPiS;
                }
                o[k] += cf32((float)p.cfg.dcOffset, (float)p.cfg.dcOffset);
            }
        }
        p.n += (int64_t)m;
        p.segLeft -= (int64_t)m;
        done += m;
    }
}

} // namespace dect2
