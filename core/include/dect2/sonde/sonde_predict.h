// Radiosonde landing prediction, 4.0 (docs/SONDE-PREDICT-4.0.md). Replaces the constant-wind estimate of sonde_land.h in the app; the
// old functions stay for their callers and tests.
//
// What goes in, best first:
//  1. the wind the sonde itself measured on the way up: the ascent track cut into layers (200 m), each layer's wind the least squares
//     slope of position against time, GPS glitches thrown out first. Its error grows with the time since it was measured (1 m/s per hour
//     by default) and with the distance from where it was measured.
//  2. a public wind forecast (Open-Meteo GFS pressure-level winds, or the SondeHub / Tawhiri prediction turned into a wind profile) for
//     the layers the ascent did not measure (above the highest point heard, or all of them when the ascent was missed). Opt-in, cached.
//  3. the descent measured so far: the parachute's sea-level descent rate K = v * sqrt(rho / rho0) is fitted to the measured vertical speed
//     in an atmosphere built from the sonde's own temperatures (ISA where it sent none), refitted on every call, recent points weighted
//     most. A burst with no parachute is the same model with a large K. The difference between the wind the descent measured and the
//     prior (ascent / forecast) in the layers already passed is carried down as the change of the wind since the ascent.
//  4. the final approach: a Kalman filter on position and velocity over the newest fixes gives the drift now; it replaces the prior in
//     the layers just below and fades with depth (driftCorrLenM, longer below approachBelowM).
//  5. the ground under the predicted point: the descent stops where it meets the terrain model (SRTM tiles), else the fixed ground value.
// The uncertainty is a Monte Carlo ensemble over the descent rate, wind errors correlated in height, the start position and the ground:
// an ellipse (1 sigma) and the 50 % and 95 % radii.
#pragma once
#include "dect2/sonde/sonde_land.h"
#include "dect2/sonde/sonde_tel.h"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
namespace sondepred {

// ---- atmosphere ------------------------------------------------------------------------------------------------------------------
double isaTempK(double hM);       // ISA (1976) to 51 km, geopotential height taken as geometric
double isaPressurePa(double hM);
double isaDensity(double hM);

// Density profile from layer temperatures (the sonde's own; ISA plus the nearest measured offset where none), hydrostatic.
struct Atmosphere {
    double baseM = -500, stepM = 50;
    std::vector<double> rho;       // kg/m3 on the grid
    bool fromSonde = false;        // at least one measured layer went in
    void buildIsa();
    // tempBaseM / tempC: layer bottoms (ascending) and mean temperatures; layerM their thickness
    void build(const std::vector<double>& tempBaseM, const std::vector<double>& tempC, double layerM);
    double density(double hM) const;
    double rho0() const { return density(0); }
};

// EGM96 geoid height above the WGS 84 ellipsoid (metres; a 2 degree table, within about 1 m mostly, 4 m in 99 % of places). Terrain
// models give heights above the geoid (mean sea level); RS41, RS92 and M10 send heights above the ellipsoid.
double geoidUndulationM(double lat, double lon);

// ---- winds -------------------------------------------------------------------------------------------------------------------------
struct WindLayer {
    bool ok = false;
    double ve = 0, vn = 0;         // air motion, m/s east / north
    double sigma = 0;              // 1-sigma error of the fit, m/s (vector component)
    double tMid = 0;               // when it was measured (unix s)
    double latMid = 0, lonMid = 0; // where
    int n = 0;
};
struct LayerWinds {
    double layerM = 200;
    int firstLayer = 0;            // layers[i] spans (firstLayer + i) * layerM .. + layerM (metres above sea level)
    std::vector<WindLayer> layers;
    int okCount() const;
    double bottomM() const;        // lowest / highest height with a fit (0 when none)
    double topM() const;
    // linear between layer centres; the nearest fit within maxGapM otherwise; false when none that close
    bool at(double hM, WindLayer& w, double maxGapM = 1000) const;
};

// Forecast winds: columns (lat, lon), each with times and levels (height above sea level, wind).
struct ForecastWinds {
    struct Level { double zM = 0, ve = 0, vn = 0; };
    struct Column { double lat = 0, lon = 0; std::vector<double> times; std::vector<std::vector<Level>> levels; };   // levels[time], by height
    std::vector<Column> cols;
    std::string source;            // "Open-Meteo GFS", "Tawhiri"
    double fetchedUnix = 0;
    bool empty() const { return cols.empty(); }
    // inverse-distance weighting over the columns, linear in time (held at the ends) and height (held at the ends)
    bool at(double lat, double lon, double hM, double unixT, double& ve, double& vn) const;
    double topM() const;
};

// Open-Meteo (https://open-meteo.com, CC BY 4.0) pressure-level winds of the GFS model for a few points around the flight.
std::vector<int> openMeteoLevelsHpa();
std::string openMeteoUrl(const std::vector<std::pair<double, double>>& latLon, double unixT, const std::string& base = "https://api.open-meteo.com");
bool parseOpenMeteo(const std::string& json, ForecastWinds& out, std::string& err);
// The points to ask for: the sonde, the first-guess landing and the midpoint, rounded to 0.25 degrees (the GFS grid; also the cache key).
std::vector<std::pair<double, double>> forecastPoints(double lat, double lon, double landLat, double landLon);
// A predicted trajectory (Tawhiri): the wind of each step is its displacement over its time, at the step's mean height and place.
struct PathPoint { double lat = 0, lon = 0, altM = 0, unixT = 0; };
ForecastWinds windsFromPath(const std::vector<PathPoint>& path, const std::string& source = "Tawhiri");

// ---- the fixes -------------------------------------------------------------------------------------------------------------------
struct Fix { double t = 0, lat = 0, lon = 0, alt = 0, tempC = -999; };

// winds of the layers crossed by fixes [i0, i1)
LayerWinds layerWinds(const std::vector<Fix>& f, size_t i0, size_t i1, double layerM, double minSpanS = 30);

struct DescentFit {
    bool ok = false;
    double K = 0;                  // sea-level descent rate, m/s (v = K * sqrt(rho0 / rho))
    double slopePerKm = 0;         // K changes by this fraction per km of height (fitted, limited)
    double sigmaRel = 0;           // 1-sigma relative error of K
    bool noParachute = false;      // K above 11 m/s
    int n = 0;
    double atH = 0;                // the height the slope is taken from (the newest fix)
    double kAt(double hM) const;   // K at a height below atH
};
// tauS: weight exp(-(tNow - t) / tauS); the first 30 s after the burst are skipped (the sonde is still speeding up)
DescentFit fitDescent(const std::vector<Fix>& f, size_t burstIdx, const Atmosphere& atm, double tauS);

struct Drift {
    bool ok = false;
    double t = 0, lat = 0, lon = 0, alt = 0;    // filtered position now
    double ve = 0, vn = 0, vz = 0;
    double sigmaV = 0, sigmaPos = 0;            // 1-sigma horizontal velocity (per component) and position
    int used = 0, rejected = 0;
};
// Kalman filter, constant velocity, over the fixes of the last windowS * 3 seconds. The process noise is set so the velocity follows
// changes with a time constant of windowS / 5; gpsSigmaM is the position noise (GPS plus the swing under the parachute).
Drift kalmanDrift(const std::vector<Fix>& f, size_t i0, double windowS, double gpsSigmaM);

}  // namespace sondepred

struct SondePredictOpts {
    double layerM = 200;
    bool useAscent = true, useFit = true, useKalman = true, useTerrain = true, useForecast = true;
    double approachBelowM = 2000;   // height above ground below which the final-approach weighting is used
    double approachWindowS = 60;    // the drift of about the last 30..120 s counts (60 s: best of 30..120 in the harness)
    double driftCorrLenM = 600;     // how far below the sonde the drift now still says something about the wind
    double windChangeMsPerH = 1.0;  // the ascent wind's error grows by this per hour since it was measured
    double windChangeMsPer100Km = 2.0;   // and by this per 100 km from where it was measured
    double forecastSigmaMs = 2.5;
    double gpsSigmaM = 4;
    int members = 200;              // ensemble size (0: no ensemble)
    unsigned seed = 1;
    double groundM = 0;             // ground when there is no terrain model
    bool altEllipsoid = false;      // the sonde's heights are above the ellipsoid: the geoid height is added to the terrain's
    double defaultSeaLevelDescentMs = 5.0;
    double burstM = 30000;          // assumed burst while still climbing
    double descentTauS = 300;       // weight of the descent fit (shorter in the final approach)
};

struct SondePredictEnv {
    std::function<bool(double lat, double lon, double& hM)> ground;   // terrain model; empty or false: opts.groundM
    const sondepred::ForecastWinds* forecast = nullptr;
};

struct SondePrediction {
    bool valid = false;
    double lat = 0, lon = 0;        // the nominal landing point
    double secondsLeft = 0;
    double landUnix = 0;
    double groundM = 0; bool groundFromTerrain = false;
    bool climbing = false;          // still going up: the burst height is assumed
    // uncertainty (ensemble)
    bool hasSpread = false;
    double r50M = 0, r95M = 0;      // radius around the nominal point holding 50 % / 95 % of the members
    double ellMajorM = 0, ellMinorM = 0, ellAngleDeg = 0;   // 1-sigma ellipse, major axis from north, clockwise
    double secondsSigma = 0;
    std::vector<std::pair<double, double>> members;    // landing points of the members (lat, lon)
    std::vector<std::pair<double, double>> path;       // nominal ground track, every ~20 s
    // what went in
    double seaLevelDescentMs = 0; bool fitted = false, noParachute = false;
    int ascentLayers = 0; bool usedForecast = false; bool usedDrift = false;
    double driftVe = 0, driftVn = 0;
    std::string text;               // one line for the panel
    SondeLanding legacy() const { SondeLanding l; l.valid = valid; l.lat = lat; l.lon = lon; l.secondsLeft = secondsLeft; return l; }
};

// One sonde's fixes at the full frame rate (the receiver's track is thinned to 600 points). Feed it every new frame; seed() fills it
// from the thinned track when the sonde was heard before this object existed.
class SondeLandTracker {
public:
    void addFix(double t, double lat, double lon, double altM, double tempC = -999);
    void seed(const std::vector<SondeTrackPoint>& track);
    const std::vector<sondepred::Fix>& fixes() const { return f_; }
    size_t size() const { return f_.size(); }
    double lastT() const { return f_.empty() ? 0 : f_.back().t; }
    int rejected() const { return rejected_; }
    size_t burstIndex() const;      // highest fix
    bool descending() const;        // fell 150 m below the highest fix, still falling
    sondepred::LayerWinds ascentWinds(double layerM) const;
    sondepred::LayerWinds descentWinds(double layerM) const;
    sondepred::Atmosphere atmosphere(double layerM = 250) const;
    SondePrediction predict(const SondePredictOpts& o, const SondePredictEnv& env) const;
private:
    std::vector<sondepred::Fix> f_;
    int rejected_ = 0, pendingBad_ = 0;
    sondepred::Fix pending_;
};

// Stateless form for code that has only the receiver's track (thinned): builds a tracker from it.
SondePrediction predictSondeLanding40(const std::vector<SondeTrackPoint>& track, const SondePredictOpts& o, const SondePredictEnv& env);

// Open-Meteo wind forecasts in the background, one request at a time, only when the user turned the network on. The answers are kept
// in memory and, when cacheDir is set, as files named after the points and the 3-hour slot, so the same flight never asks twice.
class SondeWindFetcher {
public:
    SondeWindFetcher();
    ~SondeWindFetcher();
    SondeWindFetcher(const SondeWindFetcher&) = delete;
    SondeWindFetcher& operator=(const SondeWindFetcher&) = delete;
    std::string base = "https://api.open-meteo.com";
    std::string cacheDir;
    double minGapS = 120;              // between two network requests
    static std::string cacheKey(const std::vector<std::pair<double, double>>& pts, double unixT);
    // true when a fetch started or the answer was already here (memory or file)
    bool request(const std::vector<std::pair<double, double>>& pts, double unixT, double nowSteady);
    bool get(const std::vector<std::pair<double, double>>& pts, double unixT, sondepred::ForecastWinds& out) const;
    bool busy() const;
    std::string status() const;        // the last error or "forecast from ..." line
    std::string lastUrl() const;
private:
    struct Impl;
    Impl* d_;
};

}  // namespace dect2
