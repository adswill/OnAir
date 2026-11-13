// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hydrogen line toolkit (21 cm radio astronomy), pure functions, all in km/s, K, degrees unless a name says otherwise:
//  - calibration: Y-factor (hot / cold load) -> receiver temperature and gain, antenna temperature from on/off, median baselines, RFI flagging
//    (robust residual, spectral kurtosis) with mask-and-interpolate
//  - the velocity of the observer towards a direction (Earth rotation, Earth orbit, the Sun's motion) -> radial velocity in the LSR frame
//  - sky: time, sidereal time, equatorial / galactic / horizontal, Sun, Moon and the usual calibrators, transit / rise / set, 24 h visibility
//  - Gaussian line fits with several components and uncertainties; a longitude-velocity map with terminal velocities and a rotation curve
//  - FITS (image and binary table) writer and reader, CSV and SigMF annotation text
// Everything is written from the published formulas (see the .cpp files for the references); no third-party code.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace astro {
namespace hl {

// ---------------------------------------------------------------------------------------------------------------- calibration
struct YFactor { double y = 0, trx = 0, gainPerK = 0; bool ok = false; };
// hot / cold load powers (linear, same units) with their physical temperatures: Y = Phot / Pcold, Trx = (Thot - Y Tcold) / (Y - 1),
// gainPerK = (Phot - Pcold) / (Thot - Tcold) (power units per kelvin of system noise)
YFactor yFactor(double pHot, double pCold, double tHotK, double tColdK);
// the same per bin; trx and gain per bin are returned in the vectors
struct YFactorSpec { std::vector<double> trx, gainPerK; double trxMedian = 0; bool ok = false; };
YFactorSpec yFactorSpectrum(const std::vector<double>& hot, const std::vector<double>& cold, double tHotK, double tColdK);
// Tsys = Trx + Tsky + Tspill (the cold sky about 3 K + atmosphere + ground spill; the caller says how much)
inline double tsysFromTrx(double trx, double tSkyAndSpill) { return trx + tSkyAndSpill; }

// position switching: Ta = Tsys (on - off) / off per bin
std::vector<double> antennaTemperature(const std::vector<double>& on, const std::vector<double>& off, double tsysK);
// total power calibrated with a Y-factor gain: Ta = (P - Poff) / gainPerK
std::vector<double> antennaTemperaturePower(const std::vector<double>& p, const std::vector<double>& off, const std::vector<double>& gainPerK);
// dB above the baseline (10 log10 of the ratio of y to its baseline)
std::vector<double> dbAboveBaseline(const std::vector<double>& y, const std::vector<double>& baseline);
// running median of window w (odd, edges shortened); the median-filter baseline is this with a wide window
std::vector<double> medianFilter(const std::vector<double>& y, int w);
// per-bin gain (ripple) from a flat-field spectrum (a noise source or a long off-source average): flat / medianFilter(flat, smoothW), 1 = flat
std::vector<double> perBinGain(const std::vector<double>& flat, int smoothW);
std::vector<double> applyGain(const std::vector<double>& s, const std::vector<double>& gain);   // s[i] / gain[i]
// frequency switching with two phases a, b (b = shifted by shiftBins): (a - b) / b, then folded: the line appears positive at its place and
// negative a shift away; the fold adds the negative image back (at +shiftBins), halving noise power. Returns the folded spectrum.
std::vector<double> frequencySwitchFold(const std::vector<double>& a, const std::vector<double>& b, int shiftBins);

// ------------------------------------------------------------------------------------------------------------------------ RFI
struct RfiParams { int medianW = 31; double sigma = 5.0; double skLimit = 5.0; };
// one spectrum: flag bins whose residual from the median baseline exceeds sigma robust standard deviations (1.4826 MAD)
std::vector<bool> flagRobust(const std::vector<double>& s, const RfiParams& p);
// many spectra (rows = time, equal length): spectral kurtosis of the power per channel, SK = (M+1)/(M-1) (M S2 / S1^2 - 1), where M is the
// number of FFT frames averaged into each row (the flagged channels are those whose SK is further from 1 than skLimit expected sigmas)
std::vector<bool> flagKurtosis(const std::vector<std::vector<double>>& rows, int framesPerRow, const RfiParams& p);
// the flagged bins replaced by a straight line between the nearest good ones
std::vector<double> maskInterpolate(const std::vector<double>& s, const std::vector<bool>& flag);

// ------------------------------------------------------------------------------------------------- time, coordinates, velocity
struct Utc { int y = 2000, mo = 1, d = 1, h = 12, mi = 0; double s = 0; };
double julianDate(const Utc& t);                    // Meeus ch. 7 (Gregorian calendar)
Utc fromJulianDate(double jd);
double gmstDeg(double jd);                          // Greenwich mean sidereal time, Meeus (12.4)
double lmstDeg(double jd, double lonEastDeg);

struct Eq { double raDeg = 0, decDeg = 0; };
struct Gal { double lDeg = 0, bDeg = 0; };
struct Hor { double azDeg = 0, elDeg = 0; };        // azimuth from north through east
Gal galacticFromJ2000(const Eq& e);                 // IAU 1958 galactic system via the J2000 (ICRS) pole 192.85948 +27.12825, node 122.93192
Eq j2000FromGalactic(const Gal& g);
Eq precessJ2000ToDate(const Eq& e, double jd);      // IAU 1976 precession
Hor horizontal(const Eq& eDate, double jd, double latDeg, double lonEastDeg, bool refraction = false);

// velocity (km/s) of the observer along the unit vector towards the source (positive = moving towards it), by part
struct ObserverVelocity { double rotation = 0, orbit = 0, sun = 0, total = 0; };
enum class Lsr { Kinematic /* IAU standard solar motion */, Dynamical /* Schoenrich et al. 2010 */ };
ObserverVelocity observerVelocity(const Eq& srcJ2000, double jd, double latDeg, double lonEastDeg, Lsr lsr = Lsr::Kinematic);
// the radio-convention radial velocity measured at the antenna (v_topo, positive receding) -> LSR frame
inline double lsrVelocity(double vTopoKms, const ObserverVelocity& o) { return vTopoKms + o.total; }
// Earth's barycentric-ish velocity (heliocentric orbit, Sun's reflex neglected: <= 0.015 km/s) as equatorial components, km/s
void earthVelocity(double jd, double v[3]);
Eq solarApexJ2000(Lsr lsr = Lsr::Kinematic, double* speedKms = nullptr);

// ------------------------------------------------------------------------------------------------------------------ pointing
enum class Body { Galactic, Sun, Moon, CasA, CygA, SgrA, Custom };
const char* bodyName(Body b);
// where a body is, in equatorial coordinates of the date (galactic and custom bodies from `target`)
Eq bodyPosition(Body b, double jd, const Eq& targetJ2000, double* distanceKm = nullptr);
Hor bodyHorizontal(Body b, double jd, double latDeg, double lonEastDeg, const Eq& targetJ2000, bool refraction = false);
struct Transit { bool transits = false, rises = false; double transitJd = 0, riseJd = 0, setJd = 0, transitElDeg = 0; };
// events in the 24 h from jd0 (fixed object: the position at jd0 is used; Sun and Moon are evaluated at each step)
Transit events(Body b, double jd0, double latDeg, double lonEastDeg, const Eq& targetJ2000, double horizonDeg = 0);
std::vector<double> visibility(Body b, double jd0, double latDeg, double lonEastDeg, const Eq& targetJ2000, int steps = 144);   // elevation every 24h/steps
extern const Eq kCasA, kCygA, kSgrA;

// -------------------------------------------------------------------------------------------------------------------- line fit
struct GaussComp { double amp = 0, centre = 0, sigma = 1; double dAmp = 0, dCentre = 0, dSigma = 0;
                   double fwhm() const { return 2.354820045 * sigma; } double dFwhm() const { return 2.354820045 * dSigma; }
                   double area() const; double dArea = 0; };
struct FitResult { std::vector<GaussComp> comps; double offset = 0, dOffset = 0, rms = 0; int iterations = 0; bool ok = false; };
// least squares (Levenberg-Marquardt) through y(x) with a constant offset; the uncertainties come from the covariance scaled by the residual rms
FitResult fitGaussians(const std::vector<double>& x, const std::vector<double>& y, std::vector<GaussComp> start, bool fitOffset = true);
// start values: the n strongest separated peaks of the (lightly smoothed) data above `snr` times the robust noise, widths from the half maximum
std::vector<GaussComp> guessComponents(const std::vector<double>& x, const std::vector<double>& y, int maxComps, double snr = 4);

// ------------------------------------------------------------------------------------------------------------- l-v map, rotation
struct LvMap {
    double vLo = -250, vHi = 250, vStep = 1;
    std::vector<double> l;                           // longitudes added, degrees (the order they came in)
    std::vector<std::vector<double>> rows;           // temperature on the LSR velocity grid, one row per l
    int nv() const { return (int)((vHi - vLo) / vStep + 1.5); }
    // adds a spectrum given on its own LSR velocity axis (any order); linear resampling onto the grid
    void add(double lDeg, const std::vector<double>& vLsr, const std::vector<double>& t);
};
struct Terminal { double l = 0, vTerm = 0, noise = 0; bool ok = false; };
// the terminal (tangent point) velocity of each row in the inner galaxy: the extreme velocity (highest for l 0..90, lowest for 270..360)
// where the temperature stays above `sigma` times the row's robust noise for at least `minWidth` channels
std::vector<Terminal> terminalVelocities(const LvMap& m, double sigma = 5, int minWidth = 3);
struct RotPoint { double rKpc = 0, vKms = 0; };
// tangent-point method: R = R0 |sin l|, V(R) = |vTerm| + V0 |sin l|
std::vector<RotPoint> rotationCurve(const std::vector<Terminal>& t, double r0Kpc = 8.5, double v0Kms = 220);

// -------------------------------------------------------------------------------------------------------------------- export
struct Hdu {
    std::map<std::string, std::string> cards;        // keyword -> value text (strings unquoted)
    std::vector<std::string> cardOrder;
    int bitpix = 0;                                  // image: -32 / -64 / 8 / 16 / 32
    std::vector<int> naxis;
    std::vector<double> image;                       // image data (row-major, NAXIS1 fastest)
    std::vector<std::string> colNames, colUnits;     // binary table: columns of doubles (D) or floats (E)
    std::vector<std::vector<double>> cols;
    std::vector<char> colType;                       // 'D' or 'E'
    bool isTable = false;
};
std::string fitsImage(const std::vector<float>& data, int nx, int ny, const std::vector<std::pair<std::string, std::string>>& cards, bool primary = true);
std::string fitsTable(const std::vector<std::string>& names, const std::vector<std::string>& units, const std::vector<std::vector<double>>& cols,
                      const std::vector<std::pair<std::string, std::string>>& cards, bool doublePrecision = true);
// a whole file: a primary header (no data) followed by the HDUs given (each from fitsImage(..., false) or fitsTable)
std::string fitsFile(const std::vector<std::pair<std::string, std::string>>& primaryCards, const std::vector<std::string>& extensions);
bool fitsRead(const std::string& bytes, std::vector<Hdu>& out);

// one calibrated spectrum as CSV: freq_hz, vel_topo_kms, vel_lsr_kms, ta_k, flagged
std::string spectrumCsv(const std::vector<double>& freqHz, const std::vector<double>& vTopo, double lsrCorrKms, const std::vector<double>& taK,
                        const std::vector<bool>& flag, const std::string& comment);
// SigMF "annotations" array text and a complete small .sigmf-meta for a spectrum recording (centre frequency, rate, datatype cf32_le)
std::string sigmfMeta(double centreHz, double rateHz, const std::string& description, const std::vector<GaussComp>& fits,
                      const std::string& utcIso, double latDeg, double lonDeg, uint64_t sampleCount);
// a saved session: key=value lines then the rows (see the .cpp)
struct Session { std::map<std::string, std::string> info; std::vector<std::vector<double>> spectra; std::vector<double> times; };
std::string sessionText(const Session& s);
bool parseSession(const std::string& text, Session& s);

} // namespace hl
} // namespace astro
} // namespace dect2
