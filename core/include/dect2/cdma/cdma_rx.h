// cdma2000 / IS-95 base stations receiver: a block of about one second of the 1.25 MHz channel goes through cdma::search() (pilots, then
// the Sync Channel of each) whenever the previous block is done, and the base stations heard are kept in a table by PN offset.
// Receive only: the pilot and the Sync Channel Message, which every base station broadcasts to all mobiles (cdma.h).
#pragma once
#include "dect2/cdma/cdma.h"
#include "dect2/engine/mode_tuning.h"
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct CdmaCellRow {
    int pilotPn = -1;            // from the Sync Channel Message; -1 when only the pilot was heard
    int relOffset = -1;          // PN offset relative to the strongest pilot of the last block (64 chip units)
    bool hasMsg = false;
    cdma::SyncMsg msg;
    double ecIoDb = -99, pilotDbfs = -200, cfoHz = 0, clockPpm = 0;
    uint64_t blocks = 0, msgs = 0, crcFails = 0;
    double lastSeen = 0;
};
struct CdmaTelemetry {
    uint64_t seq = 0;
    int state = 0;               // 0 waiting for samples, 1 searching
    double centerHz = 0;         // the radio's centre frequency (0 unknown)
    int bandClass = -1;          // from the centre frequency: 0 (800 MHz), 1 (PCS 1900), 4 (Korean PCS), -1 other
    double cfoHz = 0; bool dataValid = false; float snrDb = 0; uint64_t blocksOk = 0, blocksBad = 0;
    uint64_t scans = 0;
    double lastScanMs = 0;
    std::vector<CdmaCellRow> cells;   // strongest first
};
inline std::string cdmaSummary(const CdmaTelemetry& t) {
    size_t id = 0; for (auto& c : t.cells) id += c.hasMsg;
    char b[128]; snprintf(b, sizeof b, "CDMA2000: %zu base station%s, %zu identified", t.cells.size(), t.cells.size() == 1 ? "" : "s", id); return b;
}

class CdmaReceiver {
public:
    CdmaReceiver();
    ~CdmaReceiver();
    void configure(double inputRateHz);
    bool ready() const;
    void setSignalOffset(double hz);
    void setCenterHz(double hz);
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(CdmaTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    bool busy() const;           // a block is being searched (tools: feed a file only as fast as it is searched)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
ModeTuning cdmaTuning();
struct SynthConfig; class ModeSynth;
std::unique_ptr<ModeSynth> makeCdmaSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
