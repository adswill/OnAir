// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham operating helpers: the sun (grayline, day / night), great-circle paths for the map, standard dial frequencies of the digital modes,
// the IARU region band edges for the band bar, contest exchange / serial numbers / dupe sheet and the Cabrillo export, the heard-stations
// (spots) list and the operator's macros. Pure logic; nothing transmits (the Ham view sends only through HamSlotTx and its gate).
#pragma once
#include "ham_log.h"
#include <map>
#include <string>
#include <vector>

namespace dect2::ham {

// ---- the sun ----
void subsolarPoint(double utc, double& lat, double& lon);    // degrees, east positive
double sunElevation(double lat, double lon, double utc);     // degrees above the horizon (no refraction)
// the terminator: for each of n longitudes from -180 to 180, the latitude where the sun is on the horizon
std::vector<std::pair<double, double>> terminator(double utc, int n = 181);   // (lat, lon)
bool inGrayline(double lat, double lon, double utc, double bandDeg = 6);   // the sun within bandDeg of the horizon

// ---- paths ----
// n + 1 points along the great circle from 1 to 2 (short path; longPath: the other way round the Earth)
std::vector<std::pair<double, double>> gcPath(double lat1, double lon1, double lat2, double lon2, int n = 64, bool longPath = false);
struct PathInfo { double km = 0, bearing = 0, longKm = 0, longBearing = 0; };
bool pathBetween(const std::string& fromGrid, const std::string& toGrid, PathInfo& out);

// ---- the band bar ----
struct BandDial { const char* band; double ft8, ft4, jt65, jt9, wspr; };   // MHz, 0 where the mode has no usual frequency
const std::vector<BandDial>& bandDials();
double dialFor(const std::string& band, int mode);           // hfdig mode number (0 FT8, 1 FT4, 2 FT2, 3 WSPR, 4 JT65, 5 JT9, 10 JT4); 0 none
// IARU region 1, 2, 3: whether the signal lies inside that region's band (narrower than the union the TX gate uses; shown as a warning)
bool regionAllows(int region, double hz, double bwHz);

// ---- contests ----
enum class Exch { Serial, CqZone, ItuZone, State, Grid, Free };
struct ContestDef { const char* id; const char* name; Exch exch; bool perBand, perMode; };
const std::vector<ContestDef>& contestDefs();                // id is the Cabrillo CONTEST: name
class ContestSession {
public:
    int def = 0;                  // index into contestDefs()
    int nextSerial = 1;
    std::string myExch;           // my zone / state / grid / free text (the serial is counted)
    std::string sent(const std::string& rst) const;           // "599 001", "599 15", "KO24" ...
    void taken() { nextSerial++; }                            // a QSO with the current serial was logged
    // the dupe check of this contest: the QSOs with this contest id, by band (and mode when the contest counts modes apart)
    bool dupe(const Logbook& log, const std::string& call, const std::string& band, const std::string& mode) const;
    // the dupe sheet: the calls worked in the contest per band
    std::map<std::string, std::vector<std::string>> sheet(const Logbook& log) const;
    // serials go on from the highest sent in the log
    void resume(const Logbook& log);
};
struct CabrilloHeader { std::string call, contest, category = "SINGLE-OP ALL LOW", operators, location, grid, name, club, soapbox; };
std::string cabrilloMode(const std::string& adifMode);       // CW, PH, RY, DG, FM
// The log of one contest (the QSOs with that contest id; all QSOs when contest is empty) as a Cabrillo 3.0 file
std::string cabrilloExport(const std::vector<LogQso>& log, const CabrilloHeader& h);

// ---- heard stations (the spots panel) ----
struct Spot { std::string call, grid, band, mode, lastMsg; double lastUtc = 0, hz = 0, dialHz = 0; int snr = -99, bestSnr = -99, count = 0; bool cq = false; };
class SpotList {
public:
    void heard(const std::string& call, const std::string& grid, const std::string& band, const std::string& mode, double utc, double dialHz, double hz,
               int snr, bool cq, const std::string& msg);
    void expire(double nowUtc, double maxAgeSec = 1800);
    const std::vector<Spot>& spots() const { return s_; }   // newest first
    void clear() { s_.clear(); }
private:
    std::vector<Spot> s_;
};

// ---- macros (free-text messages and keyboard texts) ----
// {MYCALL} {MYGRID} {CALL} {GRID} {RST} {SNR} {SERIAL} {EXCH} {NAME} {QTH} are filled in; unknown names stay as they are
struct MacroVars { std::string myCall, myGrid, call, grid, rst, snr, serial, exch, name, qth; };
std::string expandMacro(const std::string& text, const MacroVars& v);

}  // namespace dect2::ham
