// Shared "Save results..." export: columns + rows -> CSV or JSON. No GUI includes here, so tests can link it.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace resexp {

struct Table {
    std::vector<std::string> cols;
    std::vector<std::vector<std::string>> rows;
    std::vector<double> epoch;   // optional, one UTC unix time per row (0 = unknown); the logbook formats and the "since start" filter use it
    void add(std::vector<std::string> r, double t = 0) { rows.push_back(std::move(r)); epoch.push_back(t); }
};

// Logbook formats. Columns are found by name (case-insensitive): call/callsign, grid/locator, frequency_hz/freq_hz/frequency_mhz, band,
// mode, rst/rst_sent/snr/snr_db, lat/latitude, lon/longitude, alt_ft/alt_m, name/icao/mmsi/id, time.
enum class Fmt { Csv, Json, Adif, Kml, Gpx, GeoJson };

// locale-independent number text with '.', trimmed to `dec` decimals
std::string num(double v, int dec = 3);
std::string toCsv(const Table& t);    // UTF-8 header row, RFC 4180 quoting, CRLF
std::string toJson(const Table& t);   // array of objects keyed by column name; numeric-looking cells stay numbers
std::string toAdif(const Table& t);   // ADIF 3 (.adi): rows with a call; CALL, QSO_DATE, TIME_ON, BAND, FREQ, MODE, GRIDSQUARE, RST_RCVD
std::string toKml(const Table& t);    // KML 2.2 placemarks for rows with a lat/lon
std::string toGpx(const Table& t);    // GPX 1.1 waypoints for rows with a lat/lon
std::string toGeoJson(const Table& t);   // GeoJSON FeatureCollection (points, [lon,lat,alt]) for rows with a lat/lon; the writer is core/src/export
std::string render(const Table& t, Fmt f);
Table since(const Table& t, double t0);   // rows seen at or after t0 (rows without a time are dropped)
Fmt fmtFromPath(const std::string& path); // by extension: .adi/.adif, .kml, .gpx, .json, else CSV
const char* fmtExt(Fmt f);
std::string bandOf(double mhz);       // "20m"... or ""
std::string stamp();                  // YYYYmmdd-HHMM, local time

// GUI half (results_export_ui.cpp): floppy-icon "Save results..." button; kind = "lte", "dvb"...
// Asks for a file (.csv default, .json by file name), writes it, puts a status line in `note`.
bool saveButton(const Table& t, const char* kind, std::string& note, const char* id = "saveres");
// The same, but the table is only built (by `make`) when it is needed: once a second for the clip log, and when the button is used.
// For tables with many rows and cells (the ADS-B table: every aircraft, 34 columns, built every frame before).
bool saveButtonLazy(const std::function<Table()>& make, const char* kind, std::string& note, const char* id = "saveres");

}  // namespace resexp
