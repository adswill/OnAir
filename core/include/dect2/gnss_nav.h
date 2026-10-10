// GPS navigation message (LNAV) and the orbit, clock and atmosphere models of IS-GPS-200, plus the coordinate and time helpers every
// system needs. Facts (field layouts, scale factors, algorithms) are from IS-GPS-200 (sections 20.3.3, 20.3.5 and Tables 20-I to 20-IV, 20-XIV).
#pragma once
#include <cstdint>
#include <cstddef>

namespace dect2 {

constexpr double kC = 299792458.0;               // speed of light, m/s
constexpr double kGpsMu = 3.986005e14;           // WGS-84 earth gravitational constant used by GPS, m^3/s^2
constexpr double kEarthRate = 7.2921151467e-5;   // rad/s
constexpr double kRelF = -4.442807633e-10;       // relativistic clock constant, s/sqrt(m)
constexpr double kWgsA = 6378137.0, kWgsF = 1.0 / 298.257223563;
constexpr double kPi = 3.1415926535897932;       // the GPS value of pi (IS-GPS-200 20.3.3.3.3.1)
constexpr double kGalMu = 3.986004418e14;        // Galileo OS SIS ICD Table 66
constexpr double kGalRelF = -4.442807309e-10;    // Galileo OS SIS ICD 5.1.4

// ------------------------------------------------------------------ the data bit layer
// 30 bit words: 24 source bits d1..d24 then 6 parity bits D25..D30. The words are kept as the 24 source bits (d1 in bit 23).
struct LnavSubframe {
    uint32_t w[10] = {};
};
// the 6 parity bits (D25 in bit 5) of the word whose source bits are d24 and whose predecessor ended in D29*, D30*
uint32_t lnavParity(uint32_t d24, int d29prev, int d30prev);
// received[0..29] are the 30 received bits D1..D30 (0/1) of a word, prevD29/prevD30 the last two received bits of the word before.
// On success the 24 source bits are returned in *d24. Works on a stream with the polarity inverted: the previous bits must then be inverted too.
bool lnavCheckWord(const uint8_t* received, int prevD29, int prevD30, uint32_t* d24);
// 300 received bits. All ten parities must hold.
bool lnavDecodeSubframe(const uint8_t* bits300, int prevD29, int prevD30, LnavSubframe& out);

// The same for a stream whose polarity may flip between words (a carrier loop that slipped by half a cycle): each word is tried as received and inverted.
// The word before the first one ended in 00 (the last two bits of every subframe are 0 by design), so no state is needed from the previous subframe.
bool lnavDecodeSubframeAuto(const uint8_t* bits300, LnavSubframe& out);

constexpr unsigned kLnavPreamble = 0x8B;        // 10001011
unsigned lnavSubframeId(const LnavSubframe& s);  // HOW bits 50..52
unsigned lnavTowCount(const LnavSubframe& s);    // HOW bits 31..47: the time of week of the next subframe in units of 6 s
// a field of the subframe by its ICD bit position (1 based, counted over the 300 bits including the parity: the bits of a word are 30*(w-1)+1 .. 30*(w-1)+24)
uint32_t lnavField(const LnavSubframe& s, int firstBit, int len);
int32_t lnavFieldSigned(const LnavSubframe& s, int firstBit, int len);
// a field made of two parts (an MSB part in one word and the LSBs in the next): the 8+24 bit fields of the ephemeris
uint32_t lnavField2(const LnavSubframe& s, int bit1, int len1, int bit2, int len2);
int32_t lnavField2Signed(const LnavSubframe& s, int bit1, int len1, int bit2, int len2);

// ------------------------------------------------------------------ decoded data
struct GpsEphemeris {
    int prn = 0;
    bool has1 = false, has2 = false, has3 = false;
    int wn = 0;                 // transmitted week number (10 bits)
    int uraIndex = 0, health = 0;   // health: the 6 bit field, 0 = all signals ok
    int iodc = 0, iode2 = 0, iode3 = 0;
    double tgd = 0, toc = 0, af0 = 0, af1 = 0, af2 = 0;
    double toe = 0, sqrtA = 0, e = 0, m0 = 0, dn = 0, omega0 = 0, i0 = 0, omega = 0, omegaDot = 0, idot = 0;
    double cuc = 0, cus = 0, crc = 0, crs = 0, cic = 0, cis = 0;   // radians, metres
    int fitFlag = 0;
    bool galileo = false;       // a Galileo ephemeris (I/NAV words 1-4): its gravitational constant and relativity factor; tgd holds BGD(E1,E5b)
    bool complete() const { return has1 && has2 && has3 && iode2 == iode3 && (iodc & 0xFF) == iode2; }
    int iode() const { return iode2; }
};
struct GpsAlmanac {
    int prn = 0;
    bool valid = false;
    int health = 0;             // 8 bits; 0 = healthy
    double e = 0, toa = 0, i0 = 0, omegaDot = 0, sqrtA = 0, omega0 = 0, omega = 0, m0 = 0, af0 = 0, af1 = 0;
};
struct GpsIono {
    bool valid = false;
    double alpha[4] = {0, 0, 0, 0}, beta[4] = {0, 0, 0, 0};
};
struct GpsUtc {
    bool valid = false;
    double a0 = 0, a1 = 0;
    int tot = 0, wnt = 0, dtls = 0, wnlsf = 0, dn = 0, dtlsf = 0;
};

// Subframes 1..3 into the ephemeris (marks has1..has3). Return false if the subframe is not of that kind.
bool lnavParseSf1(const LnavSubframe& s, GpsEphemeris& e);
bool lnavParseSf2(const LnavSubframe& s, GpsEphemeris& e);
bool lnavParseSf3(const LnavSubframe& s, GpsEphemeris& e);
// Subframes 4 and 5. `svId` is the page's SV ID. Almanac pages fill `alm` (1..32); page 18 of subframe 4 (SV ID 56) fills iono and utc.
// Returns 0 when the page has nothing for us, 1 for an almanac, 2 for the iono/UTC page, 3 for the almanac reference time page of subframe 5 (toa in *toaOut, week in *wnaOut).
int lnavParseAlmanacPage(const LnavSubframe& s, int subframe, int* svId, GpsAlmanac& alm, GpsIono& iono, GpsUtc& utc, double* toaOut, int* wnaOut);

// ------------------------------------------------------------------ orbits and clocks
// position (ECEF at the transmit time, in the frame of that instant) of the satellite at GPS time of week t (the signal's transmit time, seconds)
// and the satellite clock offset in seconds (af0 + af1 dt + af2 dt^2 + the relativistic term; the group delay is not in it)
void gpsEphemerisState(const GpsEphemeris& e, double t, double pos[3], double* clockS, double* eccAnom = nullptr);
// satellite position and clock rate for the Doppler: same, plus velocity by differencing over 0.5 s
// almanac position (the same algorithm without the harmonic terms)
void gpsAlmanacPos(const GpsAlmanac& a, double t, double pos[3]);
// the difference t - ref wrapped to +-302400 s (half a week)
double gpsWrap(double dt);
// the full week from the 10 bit number, nearest to the reference week
int gpsResolveWeek(int wn10, int referenceWeek);

// ------------------------------------------------------------------ atmosphere
// Klobuchar ionosphere delay on L1 in seconds (IS-GPS-200 20.3.3.5.2.5). latDeg/lonDeg: receiver, azDeg/elDeg: satellite, tow: GPS time of week.
double klobucharDelay(const GpsIono& iono, double latDeg, double lonDeg, double azDeg, double elDeg, double tow);
// troposphere delay in metres: Saastamoinen with a standard atmosphere at the receiver height (the formulas of RTKLIB's tropmodel())
double troposphereDelay(double latDeg, double heightM, double elDeg);

// ------------------------------------------------------------------ coordinates and time
void ecefToLla(const double x[3], double* latDeg, double* lonDeg, double* h);
void llaToEcef(double latDeg, double lonDeg, double h, double x[3]);
// azimuth (from north, clockwise) and elevation in degrees of a satellite as seen from a receiver position
void azElFromEcef(const double rx[3], const double sat[3], double* azDeg, double* elDeg);
// days since 1980-01-06 and the seconds of the day into a calendar date
void gpsTimeToCalendar(int week, double tow, double leapSeconds, int* year, int* month, int* day, int* hour, int* minute, double* second);

} // namespace dect2
