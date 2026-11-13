// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Exporters: pure writer functions that turn what a panel shows into a file's text (KML, GPX, GeoJSON, CSV, JSON, plain text, ADIF) and the
// small ExportSink interface a panel hands the result to. No UI, no files and no clock in the writers: the same input is the same output, so
// the tests compare text. Numbers are written with '.' whatever the locale; times are UTC (ISO 8601 "2026-10-22T08:30:00Z", ADIF dates).
//
// Coordinates: KML and GeoJSON are lon,lat[,alt] (x before y); GPX names them lat= and lon= attributes. A point with a latitude outside
// -90..90, a longitude outside -180..180, or a NaN is left out (never written as 0,0). Altitudes are metres.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::xport {

struct GeoPoint {
    std::string name, desc;
    double lat = 0, lon = 0;
    bool hasAlt = false;
    double altM = 0;
    double time = 0;          // unix seconds UTC, 0 = unknown
};
struct GeoTrack {
    std::string name, desc;
    std::vector<GeoPoint> pts;   // in time order; at least two valid points make a line
};

// a table of text cells (a message log, a pass list ...); epoch is optional, one unix time per row
struct Table {
    std::vector<std::string> cols;
    std::vector<std::vector<std::string>> rows;
    std::vector<double> epoch;
    void add(std::vector<std::string> r, double t = 0) { rows.push_back(std::move(r)); epoch.push_back(t); }
};

// one logbook contact for ADIF
struct Qso {
    std::string call, band, mode, grid, rstSent, rstRcvd, name, comment, propMode, satName;
    double freqMHz = 0;
    double timeOn = 0;        // unix seconds UTC
};

// ---- text helpers (public so that the tests and the panels use the same escaping)
std::string xmlEscape(const std::string& s);       // & < > " ' escaped, control characters other than tab / CR / LF dropped
std::string jsonEscape(const std::string& s);      // the text between the quotes of a JSON string
std::string csvCell(const std::string& s);         // RFC 4180 quoting only when needed
std::string numText(double v, int decimals = 6);   // '.', trailing zeros trimmed, "-0" -> "0"
std::string isoUtc(double unixSeconds);            // 2026-10-22T08:30:00Z
bool validLatLon(double lat, double lon);

// ---- geographic files
std::string toKml(const std::vector<GeoPoint>& points, const std::vector<GeoTrack>& tracks = {}, const std::string& docName = "OnAir");
std::string toGpx(const std::vector<GeoPoint>& points, const std::vector<GeoTrack>& tracks = {}, const std::string& docName = "OnAir");
std::string toGeoJson(const std::vector<GeoPoint>& points, const std::vector<GeoTrack>& tracks = {});

// ---- tables
std::string toCsv(const Table& t);                 // UTF-8, header row, CRLF
std::string toJson(const Table& t);                // array of objects keyed by column; numeric-looking cells stay numbers
std::string toText(const Table& t);                // aligned columns for reading, one line per row; a time column first when epoch is set
std::string toAdif(const std::vector<Qso>& q, const std::string& programId = "OnAir");   // ADIF 3.1.4 (.adi)

// ---- sinks
struct ExportItem { std::string name, mime, data; };   // name: a suggested file name with extension
class ExportSink {
public:
    virtual ~ExportSink() = default;
    // false: could not be delivered (error() says why)
    virtual bool put(const ExportItem& item) = 0;
    virtual std::string error() const { return {}; }
};
// writes into a directory (atomic: name.tmp, then rename); a name is reduced to its last path component
class FileSink : public ExportSink {
public:
    explicit FileSink(std::string dir) : dir_(std::move(dir)) {}
    bool put(const ExportItem& item) override;
    std::string error() const override { return err_; }
    std::string lastPath;
private:
    std::string dir_, err_;
};
// keeps the items (tests, and the clipboard / share paths)
class MemorySink : public ExportSink {
public:
    bool put(const ExportItem& item) override { items.push_back(item); return true; }
    std::vector<ExportItem> items;
};

enum class Format { Csv, Json, Txt, Kml, Gpx, GeoJson, Adif };
const char* formatExt(Format f);                   // ".kml"
const char* formatMime(Format f);                  // "application/vnd.google-earth.kml+xml"
Format formatFromPath(const std::string& path, Format def = Format::Csv);
// "<base>-<stamp>.ext": a file name that is safe on every system (letters, digits, - _ . only)
std::string safeFileName(const std::string& base, const std::string& stamp, Format f);

// the one call a panel makes: renders the data in the format and puts it in the sink. Geographic formats take points / tracks, the table
// formats take the table, ADIF takes the contacts. A format the data cannot fill (KML without a valid point) delivers nothing and returns false
// with `why` set.
bool exportGeo(ExportSink& sink, const std::string& base, const std::string& stamp, Format f, const std::vector<GeoPoint>& pts, const std::vector<GeoTrack>& tracks, std::string* why = nullptr);
bool exportTable(ExportSink& sink, const std::string& base, const std::string& stamp, Format f, const Table& t, std::string* why = nullptr);
bool exportQsos(ExportSink& sink, const std::string& base, const std::string& stamp, const std::vector<Qso>& q, std::string* why = nullptr);

}  // namespace dect2::xport
