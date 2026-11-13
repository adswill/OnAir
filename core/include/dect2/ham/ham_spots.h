// Spots from outside the station (4.0), all opt-in and off by default: a DX cluster (telnet), the Reverse Beacon Network (telnet, the same
// line format), POTA and SOTA activator spots (their public JSON APIs) and PSK Reporter ("who hears me": its public query). This file is the
// pure part: the parsers, the split read from a spot comment ("UP 2", "QSX 14.205"), the mode guess, and the spot book that merges every
// source with the station's own decodes. The fetching is in ham_net.h. Nothing here touches the network.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace dect2::ham {

enum class SpotSrc { Own = 0, Cluster = 1, Rbn = 2, Pota = 3, Sota = 4, PskReporter = 5 };
constexpr int kSpotSrcs = 6;
const char* spotSrcName(SpotSrc s);   // "own", "cluster", "RBN", "POTA", "SOTA", "PSK Reporter"

struct ExtSpot {
    SpotSrc src = SpotSrc::Cluster;
    std::string call;              // the station spotted (PSK Reporter: the station that was heard: you)
    std::string spotter;           // who spotted it (PSK Reporter: the receiver)
    double freqHz = 0;             // RF frequency
    std::string mode;              // "CW", "FT8", "SSB", ... ("" when not known)
    std::string comment;
    double utc = 0;                // when spotted (Unix seconds)
    int snr = -99;                 // dB (RBN, PSK Reporter); -99 not known
    int wpm = 0;                   // RBN CW speed
    std::string ref, refName;      // POTA park "US-0001" / SOTA summit "W7W/LC-001" and its name
    std::string grid;              // the spotted station's grid (POTA), or the receiver's (PSK Reporter)
    std::string spotterGrid;
    std::string band() const;      // "20m" ("" outside the amateur bands)
};

// ---- DX cluster and RBN lines ----
// "DX de W3LPL:     14025.0  JA1ABC       CW 599 up 2                 1234Z FN20"
// "DX de DK8NE-#:   14025.0  JA1ABC       CW    12 dB  25 WPM  CQ      1234Z"  (RBN: the spotter ends in -#)
// The time HHMMZ is placed on the UTC day of nowUtc (a time in the future by more than 10 minutes is yesterday's). False when the line is not a spot.
bool parseClusterLine(const std::string& line, double nowUtc, ExtSpot& out);
// The mode a spot names in its comment ("FT8", "CW", "RTTY", "USB" -> "SSB", ...), else a guess from the band plan (ham_station.h); "" none
std::string spotModeGuess(const std::string& comment, double freqHz, int region);

// ---- POTA and SOTA (JSON arrays of spots) ----
// POTA https://api.pota.app/spot/activator: activator, frequency (kHz), mode, reference, name, spotTime, spotter, comments, grid6 / grid4
std::vector<ExtSpot> parsePotaSpots(const std::string& json);
// SOTA https://api2.sota.org.uk/api/spots/<n>/all: activatorCallsign, frequency (MHz), mode, associationCode, summitCode, summitDetails,
// timeStamp, callsign (the spotter), comments
std::vector<ExtSpot> parseSotaSpots(const std::string& json);
// PSK Reporter https://retrieve.pskreporter.info/query: <receptionReport receiverCallsign= receiverLocator= senderCallsign= senderLocator=
// frequency= flowStartSeconds= mode= sNR= />
std::vector<ExtSpot> parsePskReporterXml(const std::string& xml);
double isoUtc(const std::string& t);     // "2026-11-12T10:15:30" / "...Z" / "... 10:15:30" -> Unix seconds, 0 when it does not read

// ---- the split a DX station asks for ----
// From a spot comment or decoded text: "UP" (1 kHz), "UP 2", "UP2", "UP 5-10" (the middle), "DOWN 1", "DWN 3", "QSX 14.205" (MHz),
// "QSX 205" (kHz on the same MHz), "QSX 14205". txHz: where to transmit; false when the text asks for no split.
bool parseSplit(const std::string& text, double dxHz, double& txHz);

// ---- the spot book: every source together, newest first ----
struct SpotNeed { int level = 0; std::string why; };   // level 0 worked, 1 not worked on this band, 2 a new slot (band/mode), 3 new DXCC ... see ham_needed.h
class SpotBook {
public:
    double maxAgeSec = 1800;
    // a spot of the same call, band and source within mergeSec replaces the older one (and counts the spotters)
    double mergeSec = 600;
    void add(const ExtSpot& s);
    void addAll(const std::vector<ExtSpot>& v) { for (const auto& s : v) add(s); }
    void expire(double nowUtc);
    void clear() { v_.clear(); }
    struct Row { ExtSpot s; int spotters = 1; };
    const std::vector<Row>& rows() const { return v_; }
    // filtered: band "" any, mode "" any; srcMask bit (1 << SpotSrc) set = shown
    std::vector<const Row*> filter(const std::string& band, const std::string& mode, unsigned srcMask) const;
private:
    std::vector<Row> v_;
};

}  // namespace dect2::ham
