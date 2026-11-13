// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Antenna designer tools around the solver (antd.h): parametric templates, calculators, the optimiser, NEC deck and pattern file I/O.
// Pure functions; docs/ANTENNA.md has the formulas and the file formats.
#pragma once
#include "dect2/antd/antd.h"
#include <string>
#include <vector>

namespace dect2::antd {

// ---- templates -----------------------------------------------------------------------------------------------------------------------------
enum class PKind { Length = 0, Count, Angle, Ratio, Ohm, Pf, Freq };
struct TParam {
    std::string key, label;
    double value = 0, lo = 0, hi = 1;
    PKind kind = PKind::Length;
    std::string tip;
};
struct TemplateInfo {
    std::string id, name, blurb;
    double z0 = 50;             // the reference impedance it is usually fed with
    std::string trim;           // the parameter that moves the resonance ("Trim to resonance"); empty = none
};
const std::vector<TemplateInfo>& templates();
const TemplateInfo* findTemplate(const std::string& id);
std::vector<TParam> templateParams(const std::string& id, double fHz);   // defaults from the textbook formulas for this design frequency
// the model; note: a line for the user on what the formula assumes (may be empty)
// ground: the ground to build for (nullptr: the template's usual one); heights are kept clear of it
Model buildTemplate(const std::string& id, double fHz, const std::vector<TParam>& params, std::string* note = nullptr, const Ground* ground = nullptr);
double paramValue(const std::vector<TParam>& p, const std::string& key, double dflt = 0);

// ---- calculators (all SI: metres, hertz, ohms, watts) ----------------------------------------------------------------------------------------
double wavelength(double fHz, double vf = 1);
struct WireLengths { double halfDipole, quarterVertical, fullLoop, efhw, fiveEighths, inverted_v; };
WireLengths wireLengths(double fHz);   // the usual practical lengths (end effect included): 142.65 / f MHz and so on

struct Cable { const char* name; double z0, vf, k1, k2; const char* note; };   // matched loss dB/100 m = k1 sqrt(f MHz) + k2 f MHz
const std::vector<Cable>& cables();
double cableLossDb(const Cable& c, double fHz, double lengthM);
// loss with a mismatched load: the matched loss a (dB) and the SWR at the load -> the total loss (dB) and the SWR seen at the input
struct LineLoss { double matchedDb = 0, totalDb = 0, swrIn = 1, swrLoad = 1; };
LineLoss mismatchedLineLoss(double matchedDb, double swrLoad);

struct Mismatch { double gamma = 0, swr = 1, returnLossDb = 0, mismatchLossDb = 0, reflectedPct = 0; };
Mismatch fromSwr(double swr);
Mismatch fromZ(cd z, double z0);
Mismatch fromReturnLoss(double rlDb);

// an L network matching a load to a resistive source; up to two solutions. series / shunt: reactances (ohms) and parts at f
struct LMatch { bool ok = false; bool shuntAtLoad = false; double xSeries = 0, xShunt = 0; std::string series, shunt; };
std::vector<LMatch> lNetwork(cd zLoad, double r0, double fHz);
std::string reactancePart(double x, double fHz);   // "123 nH" or "45.6 pF"
struct StubMatch { double dLambda = 0, lLambda = 0, dM = 0, lM = 0; bool open = false; };
std::vector<StubMatch> stubMatch(cd zLoad, double z0, double fHz, double vf, bool openStub);   // a single shunt stub: distance from the load, stub length
struct QuarterWave { double z = 0, lengthM = 0; };
QuarterWave quarterWave(double rLoad, double z0, double fHz, double vf);
struct Gamma { double rodLengthM = 0, rodSpacingM = 0, rodDiameterRatio = 0, capPf = 0; };
Gamma gammaMatch(double fHz, double elementDiameterM);   // the usual starting dimensions (ARRL rules of thumb), to be trimmed
struct Choke { double inductanceH = 0, turnsRatio = 0, reactance = 0; };
Choke balunChoke(double fHz, double zHigh, double zLow, double chokeOhms = 1000);   // turns ratio sqrt(zHigh / zLow); L for the choke reactance
cd lineTransform(cd zLoad, double z0, double lengthM, double fHz, double vf, double lossDbPerM = 0);   // input impedance of a line
void seriesToParallel(cd zs, double& rp, double& xp);   // R + jX in series -> Rp || jXp
cd parallelToSeries(double rp, double xp);
struct Eirp { double eirpW = 0, eirpDbm = 0, erpW = 0, erpDbm = 0; };
Eirp eirp(double txW, double cableLossDb, double gainDbi);
double antennaFactorDb(double fHz, double gainDbi, double z0 = 50);   // dB(1/m)
double gainFromAperture(double apertureM2, double fHz);              // dBi

struct BandPreset { const char* name; double f0, f1; const char* note; };
const std::vector<BandPreset>& bandPresets();   // ham (IARU region 1), FM broadcast, DVB-T UHF, airband, marine, ISM...

// ---- optimiser ----------------------------------------------------------------------------------------------------------------------------
struct OptVar { std::string key; double lo = 0, hi = 1; };   // a template parameter to vary (within lo..hi)
struct OptGoal {
    double f0Hz = 0, f1Hz = 0; int points = 3;   // the band the goals hold over (points across it)
    double z0 = 50;
    double swrTarget = 1.5, swrWeight = 1;       // the worst SWR over the band down to the target
    double gainTarget = 0, gainWeight = 0;       // dBi toward the main direction (phi 0, at the given elevation)
    double fbTarget = 0, fbWeight = 0;           // front to back, dB
    double elevationDeg = 0;                     // where the gain / FB are measured (0 = horizon; over ground e.g. 15)
};
struct OptStep { int iter = 0; double cost = 0; std::vector<double> x; double swr = 0, gain = 0, fb = 0; };
struct OptResult { bool ok = false; std::vector<TParam> params; double cost = 0, worstSwr = 0, gainDbi = 0, fbDb = 0; int evals = 0; std::string msg; };
// Nelder-Mead on the template parameters; progress gets each better step and may stop the search by returning false
OptResult optimise(const std::string& templateId, double designHz, const std::vector<TParam>& start, const std::vector<OptVar>& vars,
                   const OptGoal& goal, const Ground& ground, int maxEvals = 200,
                   const std::function<bool(const OptStep&)>& progress = nullptr);
// one parameter moved until the feed reactance is zero at fHz (bisection when it changes sign in lo..hi, else the lowest SWR against z0)
bool trimParam(const std::string& templateId, double fHz, std::vector<TParam>& params, const std::string& key, const Ground* ground, double z0,
               cd* zOut = nullptr);
// a generic Nelder-Mead: minimise f over x in the box lo..hi
std::vector<double> nelderMead(const std::function<double(const std::vector<double>&)>& f, std::vector<double> x0,
                               const std::vector<double>& lo, const std::vector<double>& hi, int maxEvals, double tol, int* evals = nullptr,
                               const std::function<bool(int, double, const std::vector<double>&)>& progress = nullptr);

// ---- files --------------------------------------------------------------------------------------------------------------------------------
// NEC-2 card deck: CM/CE, GW, GA, GH, GS, GM (translate / rotate only), GE, GN, EX 0, LD 0/1/4/5, TL, FR, RP, EN
std::string writeNec(const Model& m, double f0Hz, double f1Hz, int nFreq);
bool readNec(const std::string& text, Model& m, double* f0Hz, double* f1Hz, int* nFreq, std::string* err);
// the OnAir pattern file (docs/ANTENNA.md): comment header, then "az_deg,el_deg,gain_dbi,gain_v_dbi,gain_h_dbi"; azimuth is a compass
// bearing (0 = +y "north", 90 = +x "east"), elevation above the horizon
std::string writePatternCsv(const Model& m, const Solution& s, double azStep = 5, double elStep = 5);
struct PatternFile {
    std::string name;
    double fMHz = 0, maxDbi = -999;
    double azStep = 0, elStep = 0, elMin = 0, elMax = 0;
    int nAz = 0, nEl = 0;
    std::vector<float> g;   // nEl x nAz, row = elevation from elMin
};
bool readPatternCsv(const std::string& text, PatternFile& out, std::string* err = nullptr);
double patternGainDbi(const PatternFile& p, double azDeg, double elDeg);   // bilinear; -999 outside the file's elevation range
// currents and sweep tables for a spreadsheet
std::string writeSweepCsv(const std::vector<SweepPoint>& pts, double z0);
std::string writeCurrentsCsv(const Model& m, const Solution& s);

} // namespace dect2::antd
