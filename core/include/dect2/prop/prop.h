// Propagation prediction (pure functions, nothing downloaded). Clean-room from public formulas, see docs/propagation.md:
//  - free space: ITU-R P.525; noise: kTB plus man-made, galactic and atmospheric noise (ITU-R P.372, simplified) and the receive chain (Friis)
//  - 30 MHz and up: smooth-earth diffraction of ITU-R P.526 (with its line-of-sight interpolation, so there is no step at the horizon),
//    the plane-earth (two-ray) ground reflection, receiver clutter (ITU-R P.2108 height-gain correction), rain (P.838 / P.530 style) and gases
//    (a rough P.676 curve); with a user-supplied elevation file the terrain profile adds delta-Bullington knife-edge diffraction (P.526 / P.1812)
//  - below 30 MHz: ground wave (Norton / Terman, capped by P.526 smooth-earth diffraction for the earth's curvature) plus a one-layer F2 skywave
//    (foF2 from time, season, latitude, SFI and K index; D-layer absorption after the CCIR / P.533 form), an ESTIMATE only
#pragma once
#include <atomic>
#include <string>
#include <vector>

namespace dect2::prop {

enum class Env { Quiet = 0, Rural, Residential, City };                     // man-made noise environment at the receiver (P.372)
enum class Ground { Sea = 0, Wet, Average, Dry };                           // ground constants (conductivity, permittivity)
enum class Pol { Vertical = 0, Horizontal, CircularRight, CircularLeft };   // antenna polarisation
enum class Clutter { Open = 0, Suburban, Urban, DenseUrban, Forest };       // what is around the receive antenna (P.2108 classes)
// 30 MHz and up: which model decides the path loss. Auto (4.0) = ITU-R P.1812-8 over the terrain profile (flat sea-level profile when no
// terrain is loaded); Legacy39 = the 3.9 combination (P.526 smooth earth / plane earth / troposcatter), kept for comparison.
enum class PathModel { Auto = 0, FreeSpace, SmoothEarth, TwoRayExact, Legacy39 };
enum class HfModel { Both = 0, GroundOnly, SkyOnly };                       // below 30 MHz
enum class Model { FreeSpace = 0, LineOfSight, Diffraction, HfGround, HfSky, TwoRay, Terrain, Troposcatter, Ducting };   // what decided the loss at a point

struct Terrain;   // prop_terrain.h: elevation tiles the user loaded

// Antenna pattern hook (the antenna designer exports one): gain in dBi relative to the antenna's own boresight, by azimuth (0..359, 1 degree)
// and optionally by elevation (-90..90, 1 degree). An empty pattern means "use the scalar gain".
struct AntennaPattern {
    std::string name;
    std::vector<double> azDb;   // 360 values, dBi at azimuth offset 0..359 from the boresight, elevation 0
    std::vector<double> elDb;   // 181 values, dB relative to the azimuth gain at elevations -90..90 (empty: no elevation dependence)
    bool valid() const { return azDb.size() == 360; }
    double gainDbi(double azOffsetDeg, double elDeg) const;   // linear interpolation
};
// Text format: lines "azimuth_deg,gain_dbi" (any step, sorted or not, at least 2 lines) and optional "el,elevation_deg,relative_db" lines.
// '#' starts a comment. False with a message when the text is not a pattern.
bool parseAntennaPattern(const std::string& text, AntennaPattern& out, std::string& err);

struct Params {
    double txLat = 0, txLon = 0;             // transmitter position, degrees
    double txPowerDbw = 10;                  // power into the feeder, dBW
    double txGainDbi = 0, txFeederDb = 0;    // antenna gain (toward the horizon, or the pattern's peak), feeder loss
    double txAzDeg = 0;                      // boresight azimuth of the pattern (only with a pattern)
    const AntennaPattern* txPattern = nullptr;
    double fMHz = 100;                       // carrier
    double bandwidthHz = 12500;              // noise bandwidth of the receiver
    double rxGainDbi = 0, rxFeederDb = 0;    // receive antenna gain, feeder loss to the radio
    double rxNfDb = 6;                       // the radio's own noise figure
    bool preamp = false;                     // a preamplifier at the antenna (before the feeder)
    double preampGainDb = 20, preampNfDb = 1;
    double requiredSnrDb = 10;               // in the bandwidth above
    Pol txPol = Pol::Vertical, rxPol = Pol::Vertical;
    Env env = Env::Residential;
    bool atmosphericNoise = true;            // add lightning (atmospheric) noise below 30 MHz
    double txHeightM = 10, rxHeightM = 2;    // above the ground
    Ground ground = Ground::Average;
    Clutter rxClutter = Clutter::Open;
    double sfi = 120;                        // 10.7 cm solar flux, sfu (HF)
    double kIndex = 2;                       // planetary K index 0..9 (HF)
    int year = 2026, month = 1, day = 1;
    double utcHour = 12;                     // 0..24
    double rainMmH = 0;                      // rain rate along the path (matters above ~5 GHz)
    bool gas = true;                         // oxygen and water vapour absorption (matters above ~10 GHz)
    PathModel pathModel = PathModel::Auto;
    HfModel hfModel = HfModel::Both;
    const Terrain* terrain = nullptr;        // optional elevation data (no data: smooth earth)
    // 4.0 (docs/PROPAGATION-4.0.md)
    double timePct = 50;                     // P.1812: the loss is not exceeded for this % of the time (1..50; 50 = median, small = ducting)
    double locPct = 50;                      // P.1812: ... at this % of locations (1..99; 90 = the planning value for coverage)
    double sigmaLocDb = 5.5;                 // location variability (P.1812 Table 6)
    double deltaN = 45, n0 = 325;            // refractivity lapse rate (N/km) and sea-level refractivity (P.453 maps; 45 / 325 typical)
    double seaFraction = -1;                 // fraction of the path over water (< 0: 1 for Ground::Sea, else 0)
    bool hfLegacy = false;                   // HF skywave: the 3.9 one-layer estimate instead of P.533 / P.1239
    double r12 = -1;                         // HF: 12-month smoothed sunspot number (< 0: from the SFI, ITU-R P.371)
};
double eirpDbw(const Params& p);             // txPowerDbw + txGainDbi - txFeederDb (the pattern peak)

struct Point {
    double distKm = 0, bearingDeg = 0;
    Model model = Model::FreeSpace;
    // the loss, term by term (dB); pathLossDb is their sum (the basic transmission loss plus the clutter, rain, gas and polarisation terms)
    double fsDb = 0;          // free space over the ground distance (P.525)
    double excessDb = 0;      // the model's loss beyond free space: diffraction, ground reflection, ground wave, ionospheric (can be negative)
    double clutterDb = 0, rainDb = 0, gasDb = 0, polDb = 0;
    double pathLossDb = 0;
    double txGainDbi = 0;     // toward the receiver (pattern) or the scalar gain
    double eirpDbw = 0;
    double rxDbm = 0, fieldDbuvm = 0;
    double faDb = 0;          // external noise factor at the antenna, dB above kTB
    double sysNfDb = 0;       // receive chain noise figure referred to the antenna terminals
    double noiseDbm = 0, snrDb = 0, marginDb = 0;
    double horizonKm = 0;     // radio horizon of the antenna pair (VHF and up)
    bool terrainUsed = false;
    // HF
    double muf = 0;           // MUF of the path, MHz (0 when not HF)
    double absorptionDb = 0;  // D-layer absorption of the skywave (all hops)
    double aboveMufDb = 0;    // the loss for being above the MUF (scatter)
    double elevDeg = 0;       // skywave take-off angle
    int hops = 0;             // 0: no skywave
    double groundLossDb = 0, skyLossDb = 0;   // basic loss of each HF mode (1e9: none)
    // 4.0
    bool flatEarth = false;   // VHF and up with Auto: no terrain under this path, a flat sea-level profile was used
    double kFactor = 0;       // median effective earth radius factor (P.453 / P.1812 eq. 6)
    double diffDb = 0, tropoDb = 0, ductDb = 0;   // P.1812 diffraction loss, troposcatter and ducting basic losses
    double beta0 = 0;         // P.1812 % of time with anomalous refraction
    double hfReliability = -1;   // P.533 basic circuit reliability 0..1 (-1: not HF skywave)
    double hfOwf = 0, hfHpf = 0, hfMufProb = 0;   // MUF exceeded 90 % / 10 % of days, probability the band is supported
    int hfMethod = 0;         // 0 ray paths, 1 long path, 2 interpolated
    char hfLayer = 0;         // 'E' or 'F' of the strongest mode
};

// Building blocks (documented in docs/propagation.md), exposed for tests
double fsplDb(double fMHz, double dKm);                          // P.525: 32.45 + 20 log f + 20 log d
double ktbDbm(double bandwidthHz);                               // -174 dBm/Hz + 10 log B
double manMadeFa(double fMHz, Env env);                          // P.372 man-made noise, dB above kTB
double galacticFa(double fMHz);                                  // P.372 galactic noise (reaches the ground above about 10 MHz)
double atmosphericFa(double fMHz, double lat, double lon, int year, int month, int day, double utcHour);   // rough P.372 median, mid-latitudes
double externalNoiseFa(double fMHz, Env env);                    // man-made and galactic, power sum
double cascadeNfDb(double feederDb, double radioNfDb, bool preamp, double preampGainDb, double preampNfDb);   // Friis at the antenna terminals
double systemNoiseDbm(double bandwidthHz, double faDb, double sysNfDb);   // kTB (fa + f - 1)
double systemNoiseDbm(double fMHz, double bandwidthHz, double nfDb, Env env);   // the old form: man-made + galactic, no feeder or preamp
double fieldDbuvm(double rxDbm, double fMHz, double rxGainDbi);  // E at the receiver from the received power
double dbmToUv50(double dbm);                                    // the voltage into 50 ohms, uV
double radioHorizonKm(double h1M, double h2M);                   // 4/3 earth
double planeEarthLossDb(double dKm, double h1M, double h2M);     // 40 log d - 20 log h1 h2 (two-ray asymptote)
double twoRayLossDb(double fMHz, double dKm, double h1M, double h2M);   // exact two-ray with reflection -1 (nulls and peaks)
double knifeEdgeJ(double v);                                     // P.526 single knife edge, dB (0 below v = -0.78)
double fresnelV(double hM, double d1Km, double d2Km, double fMHz);   // P.526 diffraction parameter for an obstacle h above the line
double smoothEarthResidueDb(double fMHz, double dKm, double h1M, double h2M, double aeKm, Ground g, Pol pol);   // P.526 3.1.1, any distance
double diffractionLossDb(double fMHz, double dKm, double h1M, double h2M, Ground g = Ground::Average, Pol pol = Pol::Vertical);   // P.526 3.2
double troposcatterLossDb(double fMHz, double dKm, double h1M, double h2M, double gainsDbi = 0, double thetaMrad = -1);   // P.452 4.3, 50 % of the time; theta < 0: smooth earth
double clutterLossDb(double fMHz, double hM, Clutter c);         // P.2108 3.1 terminal correction (0 for open ground: the two-ray has it)
double rainSpecificDbKm(double fMHz, double rainMmH);            // gamma_R = k R^alpha (P.838 coefficients, interpolated)
double rainLossDb(double fMHz, double dKm, double rainMmH);      // with the P.530 effective path length
double gasSpecificDbKm(double fMHz);                             // rough sea-level curve (7.5 g/m3), P.676 shape
double polLossDb(Pol tx, Pol rx);
double groundWaveFactor(double fMHz, double dKm, Ground g, Pol pol = Pol::Vertical);   // Norton attenuation A (0..1), Terman's form
double groundWaveLossDb(double fMHz, double dKm, Ground g, Pol pol, double h1M = 2, double h2M = 2);   // flat Norton, curved earth (P.526)
double solarCosZenith(double lat, double lon, int y, int m, int d, double utcHour);
double sfiToSunspot(double sfi);
double foF2(double lat, double lon, const Params& p);            // estimate, MHz
double hopMaxKm(double layerKm, double minElevDeg);              // longest hop with at least that take-off angle
double hopElevationDeg(double hopKm, double layerKm);            // take-off angle of one hop
double hopIncidenceSec(double hopKm, double layerKm);            // sec of the incidence angle at the reflecting layer (MUF factor)
double dLayerSec(double hopKm, double reflectKm, double dKm);    // sec of the angle at the D layer (dKm) of a ray reflected at reflectKm
double dLayerAbsorptionDb(double fMHz, double hopKm, double cosChi, double sunspot);   // one hop
double aboveMufLossDb(double fMHz, double muf);
double mufMHz(double lat, double lon, double distKm, const Params& p);
double greatCircleKm(double lat1, double lon1, double lat2, double lon2);
double bearingDeg(double lat1, double lon1, double lat2, double lon2);
void destination(double lat, double lon, double bearingDeg, double distKm, double& lat2, double& lon2);
void midpoint(double lat1, double lon1, double lat2, double lon2, double& lat, double& lon);

// One point at a ground distance from the transmitter along a bearing
Point evaluate(const Params& p, double distKm, double bearingDeg);
// The path to a latitude / longitude
Point evaluateAt(const Params& p, double lat, double lon);

struct Range {
    double maxKm = 0;       // farthest distance with the margin >= 0 (0: none)
    double minKm = 0;       // nearest such distance (0: heard right from the transmitter)
    bool any = false;
    std::vector<std::pair<double, double>> spans;   // the distances where it is heard, near to far (a skip zone is a gap between two)
    double firstEndKm() const { return spans.empty() ? 0 : spans.front().second; }
};
Range findRange(const Params& p, double bearingDeg = 0, double maxKm = 20000.0);

struct Grid {
    int n = 0;                         // n x n cells
    double lat0 = 0, lon0 = 0, lat1 = 0, lon1 = 0;   // south-west and north-east corners
    double rangeKm = 0;                // half width of the square
    std::vector<float> snr;            // row 0 = south
    std::vector<unsigned char> model;
};
// Square of +-radiusKm around the transmitter, n x n cells. cancel: stop early (returns an empty grid) when it becomes true.
Grid computeGrid(const Params& p, double radiusKm, int n, const std::atomic<bool>* cancel = nullptr);

const char* modelName(const Params& p);          // the model chosen by frequency
std::string modelLimits(const Params& p);        // what it covers and what it does not
const char* pointModelName(Model m);

} // namespace dect2::prop
