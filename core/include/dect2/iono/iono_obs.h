// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ionosphere from my own receptions: a local observation log of what this receiver actually heard (FT8/FT4/WSPR spots with
// locators, HFDL ground stations, beacons, time stations) turned into propagation evidence: a heard grid per band and hour,
// a maximum-usable-frequency ESTIMATE per distance bin (the highest band with decodes there, no physics) and a comparison with
// the open bands the propagation tool predicts. Pure functions plus a small file-backed history; nothing here touches the network.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace dect2::ionoobs {

enum class Source : uint8_t { Ft8 = 0, Ft4, Wspr, Hfdl, Beacon, TimeStation, Other };
const char* sourceName(Source s);
bool sourceFromName(const std::string& s, Source& out);

struct Obs {
    int64_t utc = 0;                 // seconds since 1970
    double fHz = 0;                  // dial / carrier frequency
    Source src = Source::Other;
    float snrDb = 0;
    double rxLat = 0, rxLon = 0;     // this receiver
    double txLat = 0, txLon = 0;     // the station heard
    std::string id;                  // callsign / ground station / beacon name (dedupe key, local only)
};

// ---- geometry (the same spherical earth as prop::greatCircleKm)
double distanceKm(double lat1, double lon1, double lat2, double lon2);
double bearingDeg(double lat1, double lon1, double lat2, double lon2);   // 0..360, initial bearing
double pathKm(const Obs& o);

// ---- bins
constexpr int kDistBins = 9;
int distBin(double km);                          // 0..kDistBins-1, -1 for a negative / NaN distance
const char* distBinName(int bin);                // "0-500 km" ...
double distBinMidKm(int bin);
constexpr int kBearingBins = 8;
int bearingBin(double deg);                      // 0 N, 1 NE ... 7 NW (45 degree sectors centred on the compass points)

// ---- HF bands, 160 m .. 10 m
constexpr int kBands = 10;
int bandOf(double fHz);                          // -1 outside every band
const char* bandName(int band);                  // "160m" ...
double bandCentreMHz(int band);

// ---- windows
enum class Window { H24 = 0, D7, D30 };
int64_t windowSeconds(Window w);
std::vector<Obs> inWindow(const std::vector<Obs>& all, int64_t nowUtc, Window w);

// ---- heard grid: counts per band x UTC hour of day
struct HeardGrid {
    int count[kBands][24] = {};
    float bestSnr[kBands][24];                   // -999 when nothing heard
    HeardGrid() { for (auto& r : bestSnr) for (float& v : r) v = -999.f; }
    int total() const;
};
HeardGrid heardGrid(const std::vector<Obs>& obs);
int utcHourOf(int64_t utc);

// ---- MUF-style estimate per distance bin: the centre frequency (MHz) of the highest band that has at least minDecodes in that
// bin, or -1. This is an observation, not a model.
struct MufRow { double mufMHz[kDistBins]; int bandCount[kDistBins][kBands]; };
MufRow mufEstimate(const std::vector<Obs>& obs, int minDecodes = 1);

// ---- predicted vs observed. mufAt(distKm, utcHour) gives the propagation tool's MUF in MHz (the app passes prop::mufMHz).
enum class Verdict : uint8_t { NoData = 0, BothOpen, BothClosed, ObservedOnly, PredictedOnly };
const char* verdictName(Verdict v);
struct PredObs { Verdict v[kBands][kDistBins]; int agree = 0, disagree = 0; };
// A band is predicted open on a distance when its centre frequency is at or below the MUF. "Not heard" only counts as closed on
// bands where this receiver heard at least minListen decodes anywhere in the window (otherwise it was probably not listening).
PredObs compare(const std::vector<Obs>& obs, const std::function<double(double distKm, int utcHour)>& mufAt, int utcHour = -1,
                int minListen = 3);

// ---- sun / terminator
void subSolarPoint(int64_t utc, double& latDeg, double& lonDeg);
bool isNight(double latDeg, double lonDeg, int64_t utc);
// Terminator longitudes at a latitude (two, or none in polar day/night): returns the count written to lonOut[2].
int terminatorLons(double latDeg, int64_t utc, double lonOut[2]);
std::vector<std::pair<double, double>> terminatorLine(int64_t utc, int steps = 91);   // lat, lon points, -90..90 (the sunset side)

// ---- colour: red (weak) .. yellow .. green (strong), 0xAABBGGRR for ImGui
uint32_t snrColor(float snrDb);

// ---- history: one TSV line per observation, a size cap, 30 day retention
std::string toLine(const Obs& o);
bool fromLine(const std::string& line, Obs& o);
// Add, skipping a duplicate (same id, source, band, utc within 30 s). Returns true when stored.
bool addObs(std::vector<Obs>& v, const Obs& o);
// Drop everything older than 30 days before nowUtc, then the oldest until at most maxBytes of file text / maxCount records remain.
void trim(std::vector<Obs>& v, int64_t nowUtc, size_t maxBytes = 4u << 20, size_t maxCount = 200000);
bool loadFile(const std::string& path, std::vector<Obs>& out);
bool saveFile(const std::string& path, const std::vector<Obs>& v);
std::string exportCsv(const std::vector<Obs>& v);

}  // namespace dect2::ionoobs
