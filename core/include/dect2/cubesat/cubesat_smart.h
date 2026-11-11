// Smart CubeSat decoder: finds cubesat and small-satellite packet and telemetry signals and decodes them without setup.
//
//   span samples ---> CarrierScan (spectrum, narrow carriers: centre, width, SNR)
//        |                |
//        |   pass planner (TLE + transmitter table: which satellites are overhead, where their carriers are with Doppler)
//        |                v
//        |        Track per carrier  -> Channelizer (follows the carrier, 96 kHz) -> probe (about 1.5 s): modulation class + baud estimate
//        |                                  -> chains: the table's hypothesis first, then the ranked blind ones
//        |                                       FSK / AFSK / BPSK / CW, each with every framing that fits (AX.25 incl. G3RUH, AX100, CCSDS,
//        |                                       AO-40 FEC, ASCII, Morse); a frame that validates (CRC, Golay, Reed-Solomon, Viterbi) is reported
//        v                                       with the trial that found it
//   LoRa (the LoRa satellite receiver) for table entries that use it.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks), status() and
// the getters from the interface thread. All frequencies are absolute Hz (the centre frequency of the capture is known), Doppler is applied
// by moving the channelizers, the radio is not retuned.
#pragma once
#include "cubesat_chain.h"
#include "cubesat_db.h"
#include "cubesat_telem.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {
namespace cubesat {

struct SmartSettings {
    bool usePasses = true;          // follow the satellites of the table that are overhead (needs the observer and TLEs)
    bool blind = true;              // look for carriers the table does not explain
    bool tryLora = true;            // run the LoRa satellite receiver for LoRa entries of the table
    double minSnrDb = 8.0;          // a carrier must stand this far above the noise (in its own width)
    int maxChannels = 4;            // simultaneous carriers followed (CPU budget)
    double minElDeg = 0.0;          // a table entry is followed above this elevation
    double searchHz = 5000.0;       // how far from its predicted place a carrier is looked for
    // Advanced overrides (0 / ModUnknown: automatic). Any value, no clamps.
    int forceMod = ModUnknown;      // try only this modulation on blind carriers
    int forceBaud = 0;              // and this baud rate
    double channelHz = 0;           // the width the channelizer keeps (0: from the measured occupied width)
    double scanBinHz = 0;           // spectrum resolution of the carrier scan (0: automatic)
};

struct SmartPacket {
    double timeSec = 0;             // capture time of the frame
    int64_t wallTime = 0;           // the computer's clock (0: a file)
    std::string sat;                // satellite name ("" unknown)
    int norad = 0;
    bool satGuess = false;          // the name comes from the carrier's frequency, not from the table entry that was followed
    double freqHz = 0;              // the carrier's frequency at the frame (Doppler included)
    int mod = 0;                    // ModKind
    int baud = 0;
    std::string framing;            // "AX.25 G3RUH", "AO-40 FEC", ...
    std::string how;                // what validated it
    int rsErrors = -1;
    float snrDb = 0;
    std::string from, to, path, type;
    bool hasCsp = false;
    CspHeader csp;
    std::vector<uint8_t> data, payload;
    std::string text;
    TelResult tel;                  // parsed telemetry (kind empty: none)
    int framingId = 0;              // Framing / kFramingAo40 / kFramingText / kFramingCw
};

struct SmartTrial {
    std::string label;
    uint64_t heard = 0, valid = 0;
};

struct SmartCarrier {
    int id = 0;
    double freqHz = 0;              // absolute
    double widthHz = 0;
    double snrDb = 0;
    bool fromTable = false;         // started from a table entry of a satellite overhead
    std::string sat;                // table entry or best guess
    int norad = 0;
    double elDeg = 0;               // of the table satellite, when known
    std::string state;              // "searching", "carrier found", "analysing", "decoding", "carrier only", "no known signal"
    int mod = ModUnknown;           // best hypothesis so far
    int baud = 0;
    double estBaud = 0;             // measured (0 before the probe)
    std::string modText;            // "FSK / GMSK 9600 baud", "unknown"
    std::string framing;            // the framing of the last good frame ("" none yet)
    uint64_t frames = 0;
    double lastFrameSec = -1e9;
    std::vector<SmartTrial> trials;
};

struct SmartStatus {
    int state = 0;                  // 0 searching, 1 carrier found, 2 decoding
    double centerHz = 0, rateHz = 0;
    double timeSec = 0;
    double noiseDb = -200;
    std::vector<SmartCarrier> carriers;
    uint64_t frames = 0, scans = 0;
    std::string note;               // what the decoder is doing in words ("waiting for AO-73 at 19:42 ...")
};

class SmartCubesat {
public:
    SmartCubesat();
    ~SmartCubesat();
    void configure(double rateHz);
    void setCenterHz(double hz);                              // the capture's centre frequency (absolute); 0: unknown (carriers keep span-relative names)
    void setObserver(const Observer& o);
    void setSettings(const SmartSettings& s);
    SmartSettings settings() const;
    // Time for the pass planner: wall clock (default), or, for a file, a fixed start time that advances with the samples (unix seconds)
    void setSimTime(double startUnix);
    void setWallClock(bool on);
    // The satellite knowledge: TLEs (a CelesTrak text file merged over the built-in snapshot) and the user's own transmitters. Any thread.
    size_t addTleText(const std::string& text);
    void setUserTransmitters(const std::vector<TxEntry>& v);
    // The next passes of satellites with a decodable transmitter (computed on a copy of the planner: does not hold up feed()). Interface thread.
    std::vector<PlannedPass> upcoming(double nowUnix, double hours, double minElDeg, size_t max) const;
    std::vector<TxEntry> transmitters() const;
    size_t satelliteCount() const;
    double tleNewestEpochUnix() const;
    bool look(int norad, double unixSec, Look& l) const;
    void reset();
    void clearPackets();
    void feed(const cf32* x, size_t n);
    bool ready() const;
    SmartStatus status() const;
    std::vector<SmartPacket> packets() const;                 // newest first
    void setLogCallback(std::function<void(const std::string&)> cb);

    // Which tune would cover the satellites that are up (or the next ones): the centre and the width needed, with a reason. valid = false when
    // nothing is expected within the horizon.
    struct TuneAdvice { bool valid = false; double centerHz = 0, spanHz = 0; std::string why; double startUnix = 0; };
    TuneAdvice adviseTune(double nowUnix, double maxSpanHz) const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Export formats of the packet list. Nothing is sent anywhere.
std::string exportSatnogsJson(const std::vector<SmartPacket>& v, const std::string& stationCall, double latDeg, double lonDeg);   // one SatNOGS telemetry record per line
// Live forwarding to SatNOGS DB (the SiDS form gr-satellites' telemetry forwarder posts to https://db.satnogs.org/api/telemetry/): noradID, source,
// timestamp (UTC, ms, Z), frame (upper-case hex), locator=longLat, longitude "25.28E", latitude "54.69N", version. "" when the packet cannot go
// (no NORAD number, no bytes, a file rather than a live capture).
std::string sidsForm(const SmartPacket& p, const std::string& callsign, double latDeg, double lonDeg, int64_t wallMs = 0);
constexpr const char* kSidsSatnogsUrl = "https://db.satnogs.org/api/telemetry/";
std::string exportCsv(const std::vector<SmartPacket>& v);
std::string exportKiss(const std::vector<SmartPacket>& v);                                                                  // AX.25 / data frames as KISS
std::string packetSummary(const SmartPacket& p);

} // namespace cubesat
} // namespace dect2
