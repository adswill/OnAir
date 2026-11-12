// What the Tools > Propagation window edits (one scenario, Simple or Advanced), turned into model Params; presets, receiver (SDR) noise
// figures, input checks, the plain-language verdict, the link-budget table, sweeps and CSV. Pure logic, tested by tests/prop/.
#pragma once
#include "dect2/prop/prop.h"
#include "dect2/prop/mode_snr.h"
#include "dect2/prop/hf_p533.h"
#include <string>
#include <vector>

namespace dect2::prop {

enum class PowerUnit { W = 0, dBW, dBm, kW };

struct Scenario {
    int preset = 4;                          // presets() index (4: ham 2 m FM)
    double txLat = 54.687, txLon = 25.280;
    double power = 25; PowerUnit powerUnit = PowerUnit::W;
    double txGainDbi = 2.15, txFeederDb = 0, txHeightM = 10, txAzDeg = 0;
    std::string txPatternPath;               // antenna pattern file (the antenna designer's export), empty: the scalar gain
    double fMHz = 145.5, bandwidthHz = 12500, requiredSnrDb = 12;
    double rxGainDbi = 2.15, rxFeederDb = 0, rxHeightM = 5;
    int radio = 0;                           // radios() index; 0 = enter the noise figure
    double radioGainPct = 70;                // the radio's gain setting, 0..100 % of its range
    double rxNfDb = 6;                       // used when radio == 0
    bool preamp = false; double preampGainDb = 20, preampNfDb = 1;
    Pol txPol = Pol::Vertical, rxPol = Pol::Vertical;
    Env env = Env::Residential;
    bool atmosphericNoise = true;
    Ground ground = Ground::Average;
    Clutter rxClutter = Clutter::Open;
    PathModel pathModel = PathModel::Auto;
    HfModel hfModel = HfModel::Both;
    double sfi = 120, kIndex = 2;
    double rainMmH = 0; bool gas = true;
    bool useNow = true; int year = 2026, month = 1, day = 1, hour = 12, minute = 0;
    // 4.0 (docs/PROPAGATION-4.0.md)
    double timePct = 50, locPct = 50, sigmaLocDb = 5.5;   // P.1812 time / location percentages and location variability
    double deltaN = 45, n0 = 325;                          // refractivity (P.453): lapse rate N/km, sea-level N
    bool hfLegacy = false;                                 // HF: the 3.9 estimate instead of P.533
    double r12 = -1;                                       // HF sunspot number (< 0: from the SFI)
    std::string decodeMode;                                // an OnAir mode id (mode_snr.h): its measured threshold sets SNR and bandwidth
    int decodeCondition = 1;                               // 0 white noise, 1 real-world impairments, 2 fading
};

struct Preset {
    const char* name;
    const char* note;              // one line for the Simple screen
    double fMHz, bandwidthHz, requiredSnrDb, powerW, txGainDbi, txHeightM, rxGainDbi, rxHeightM;
    Pol pol;
    double fMinMHz, fMaxMHz;       // the band, for the input check (0, 0: any)
};
const std::vector<Preset>& presets();      // the last one is "Custom"
void applyPreset(Scenario& s, int index);  // sets frequency, bandwidth, SNR, power, gains, heights and polarisation

struct Radio {
    const char* name;
    double nfDb;          // noise figure at full gain, 30 MHz and up (dB)
    double nfHfDb;        // below 30 MHz (its HF input or direct sampling)
    double fMinMHz, fMaxMHz;
    const char* note;
};
const std::vector<Radio>& radios();        // index 0: "enter the noise figure"
// The radio's noise figure at a gain setting: low gain costs noise figure (rough: + 16 dB (1 - g)^2)
double radioNfDb(const Scenario& s, double fMHz);

double powerDbw(double power, PowerUnit u);
Params toParams(const Scenario& s, const Terrain* terrain = nullptr, const AntennaPattern* pattern = nullptr);

struct Issue { int level; std::string text; };   // 0 note, 1 warning, 2 error (no result until fixed)
std::vector<Issue> validate(const Scenario& s);
bool hasErrors(const std::vector<Issue>& v);

// Simple mode: a plain-language answer
int rating(double marginDb);               // 2 good (>= 10 dB to spare), 1 fair (0..10), 0 poor (not heard)
const char* ratingName(int r);
std::string kmText(double km);             // "4.2 km", "42 km", "420 km", "4200 km"
struct Verdict {
    int rating = 0;
    std::string headline;                  // "You should hear it up to about 42 km."
    std::string atRx;                      // "At the receiver (12 km away): good, 25 dB to spare."
    std::vector<std::string> reasons;      // one line each
};
Verdict verdict(const Params& p, const Range& r, const Point* atRx, const char* rxLabel);

// Advanced mode: the link budget, term by term, each with an explanation for its tooltip
struct BudgetRow { std::string label; double value; std::string unit; std::string help; bool total = false; };
std::vector<BudgetRow> budget(const Params& p, const Point& q);

// Sweeps (cheap: findRange and evaluate only)
struct Series { std::string name, xLabel, yLabel; std::vector<double> x, y; bool logX = false; };
Series sweepSignalVsDistance(const Params& p, double maxKm, int n, double bearing);   // received SNR (dB) against distance
Series sweepRangeVsPower(const Params& p, int n, double bearing);                     // range (km) against power -20..+20 dB around the current
Series sweepRangeVsFrequency(const Params& p, double f0MHz, double f1MHz, int n, double bearing);
struct HfDay { std::vector<double> hour, muf, fot, luf; };                            // luf 0: no usable frequency that hour
HfDay hfDay(const Params& p, double distKm, double bearing);

// 4.0 views
// basic path loss against distance for several models side by side: free space, exact two-ray, P.1812 median, P.1812 for 90 % of locations,
// P.1812 for 1 % of the time (the enhanced / interference case)
std::vector<Series> sweepLossByModel(const Params& p, double maxKm, int n, double bearing);
// the terrain profile toward a point with the line of sight, the first Fresnel zone and the earth's bulge for the median k-factor
struct ProfileView {
    std::vector<double> dKm, groundM;   // ground height above sea level, bent by the earth's curvature (k-factor) relative to the chord
    std::vector<double> losM, fresnelM; // line of sight between the antenna tops and the first Fresnel zone radius at each point
    double kFactor = 4.0 / 3.0;
    double worstClearancePct = 100;     // smallest (line of sight - ground) / first Fresnel radius, % (60 % or more: no diffraction loss)
    double worstKm = 0;
    bool terrain = false;               // false: no elevation data under the path (flat sea level)
};
ProfileView profileView(const Params& p, double rxLat, double rxLon, int n = 200);
// the P.533 inputs of a scenario's path and its hour x band chart
hf::Path hfPathFor(const Params& p, double rxLat, double rxLon);
hf::Chart hfChartFor(const Params& p, double rxLat, double rxLon, const std::vector<double>& freqMHz);

std::string budgetCsv(const std::vector<BudgetRow>& a, const std::vector<BudgetRow>* b);   // b: the second scenario (compare)
std::string seriesCsv(const std::vector<Series>& s);
std::string hfDayCsv(const HfDay& d);

} // namespace dect2::prop
