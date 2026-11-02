// DGPS beacons in the engine: the receiver of dgps_rx.h behind the engine's interface (configure, offset, feed, telemetry).
// Engine standard 122 (family 120, "DGPS beacon"). Receive only; nothing is transmitted. Three receivers run side by side (200, 100 and 50 bit/s) and the one that
// finds messages is shown.
#pragma once
#include "dect2/dgps/dgps_rx.h"
#include "dect2/engine/mode_tuning.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct DgpsMessageRow {
    int type = 0, station = 0, zcount = 0, seq = 0, length = 0, health = 0;
    bool complete = false;
    std::string text;               // what the message says: satellites and corrections, position, text, almanac ...
};

struct DgpsBeacon {                 // from a type 7 message
    double latitude = 0, longitude = 0, frequencyKhz = 0;
    int rangeKm = 0, stationId = 0, bitRate = 0, health = 0;
};

struct DgpsTelemetry {
    uint64_t seq = 0;
    int state = 0;                  // 0 no input, 1 listening, 2 a frame with a good header seen
    double cfoHz = 0;               // carrier offset from the nominal beacon frequency
    float snrDb = 0;                // not measured (0)
    float levelDb = -200;           // level of the last block, dBFS
    bool dataValid = false;         // at least one frame with a good preamble and parity
    uint64_t blocksOk = 0;          // frames with a good header
    uint64_t blocksBad = 0;         // reserved: blocks without a frame
    int stationId = 0;              // reference station ID of the last frame
    int lastType = 0;               // message type of the last frame
    int zcount = 0;                 // modified Z-count of the last frame (0.6 s units)
    int health = 0;                 // station health of the last frame (raw)
    std::vector<int> typesSeen;     // message types seen so far, ascending
    double bitRate = 200;           // the beacon's bit rate (the receiver that locked)
    bool inverted = false;          // channel polarity found
    uint64_t messagesComplete = 0, wordsBad = 0;   // messages whose words all passed their parity / words that failed
    std::vector<DgpsMessageRow> rows;   // newest first
    bool hasPosition = false;       // type 3
    double lat = 0, lon = 0, height = 0;
    std::vector<DgpsBeacon> almanac;    // the last type 7 message
    std::string specialText;        // the last type 16 message
    int satellites = 0;             // satellites in the last type 1 / 9 message
    double inputRate = 0;
    double centerHz = 0;
};

class DgpsReceiver {
public:
    DgpsReceiver();
    ~DgpsReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the beacon sits relative to 0 Hz in the input (about -tuneOffsetHz)
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    bool telemetry(DgpsTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning dgpsTuning();

// One line for the top bar and the receiver tab
std::string dgpsSummary(const DgpsTelemetry& t);

}  // namespace dect2
