// Inmarsat all-carriers mode, stage 2: one wide capture in, every carrier decoded in parallel.
//   CarrierDetector (inmsat_detect.h) finds the carriers; the manager groups them into windows of about 160 kHz, cuts every window out of the
//   capture with ONE shared forward FFT (lora::FdChannelizer) and runs an Aero receiver (P channels at 600, 1200, 10500 bit/s) and, for
//   windows with a 2 kHz carrier, an Inmarsat-C receiver on it. The results of all windows are merged: one carrier table, one message list,
//   one aircraft list. A CPU budget limits the windows; carriers in a window that does not fit are listed as "parked".
// Windows stay where they are while they hold a carrier; a new carrier outside every window adds a window and rebuilds the channeliser (the
// receivers keep running, they only see a short gap). Design: docs/INMARSAT-REVAMP.md.
// feed() and snapshot() may be called from different threads.
#pragma once
#include "inmsat_detect.h"
#include "dect2/aero/aero_tel.h"
#include "dect2/inmc/inmc_tel.h"
#include "inmsat_satid.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2::inmsat {

enum class CarrierType { Unknown, AeroP, InmarsatC, AeroC };
const char* carrierTypeName(CarrierType t);

// the P channel rate a carrier of this width most likely is (a guess for the carrier list until a decoder holds sync)
int rateGuessForWidth(double widthHz, CarrierClass cls);

struct CarrierRow {
    int id = 0;
    double offsetHz = 0;          // against the centre of the capture
    double freqHz = 0;            // absolute (centre + offset), 0 when the centre is not known
    double widthHz = 0;
    float snrDb = 0;              // carrier over the noise floor in the detector
    CarrierClass cls = CarrierClass::Narrow;
    CarrierType type = CarrierType::Unknown;   // what actually decodes; Unknown while nothing is in sync
    int bitRate = 0;              // P channels: 600 / 1200 / 10500; Inmarsat-C: 1200 (symbols/s)
    int rateGuess = 0;            // while bitRate is 0: the P channel rate the carrier's width suggests (600 narrow, 10500 wide), 0 = no guess
    int state = 0;                // 0 seen, 1 locked, 2 frames in sync, 3 data
    float ebn0Db = 0;
    uint64_t frames = 0;
    double idleSec = -1;          // seconds since the last good frame or SU, -1 never
    std::string region;           // Inmarsat-C: ocean region from the bulletin board
    std::string station;          // Inmarsat-C: station name
    std::string satellite;        // from the satellite identification (inmsat_satid.h), "" while it is not sure
    bool voiceCandidate = false;  // wide, old enough, and no data decoder holds sync: it may be an Aero C (voice) channel (not decodable yet, see inmsat_voice.h)
    bool parked = false;          // no window: the CPU budget is used up
    bool focus = false;
    int window = -1;              // index into Snapshot::windows, -1 parked
};

struct WindowInfo {
    double centerHz = 0, bwHz = 0, outRate = 0;
    bool aero = false, inmc = false;
    double cost = 0;
};

struct Snapshot {
    uint64_t seq = 0;
    double timeSec = 0;
    double rate = 0, centerHz = 0;
    std::vector<CarrierRow> carriers;           // strongest first
    std::vector<WindowInfo> windows;
    double cpuCost = 0, cpuBudget = 0;
    std::vector<AeroMessage> messages;          // newest last, up to 300, all windows
    std::vector<AeroLogonEntry> logons;         // newest last, up to 200
    std::vector<AeroAircraft> aircraft;         // most recent first, up to 200
    std::vector<InmcMessage> egc;               // Inmarsat-C messages, newest first, up to 150
    uint64_t messagesTotal = 0, positionsTotal = 0, susOk = 0, susBad = 0;
    float floorDb = 0;
    SatResult sat;                              // which satellite this capture is looking at, with the evidence
    // the common prefix the engine copies into RxTelemetry (status bar, history plot): same meaning as in every other mode
    int state = 0;                              // 0 searching, 1 carriers found, 2 decoding
    double cfoHz = 0;
    float snrDb = 0;                            // best Eb/N0 of a decoding carrier
    bool dataValid = false;                     // a message was decoded
    uint64_t blocksOk = 0, blocksBad = 0;       // SUs with a good / bad check, all carriers
};

struct ManagerConfig {
    double rate = 0;              // capture rate
    double centerHz = 0;          // absolute frequency of the capture centre (0 unknown)
    double windowBwHz = 160000;   // width of one window
    double budget = 10;           // CPU budget in cost units (an Aero window costs 1, an added Inmarsat-C receiver 0.6)
    bool inmcOn = true;
    DetectorConfig detector;      // rate is set from `rate`
};

class Manager {
public:
    Manager();
    ~Manager();
    void configure(const ManagerConfig& cfg);
    bool ready() const;                          // false when the capture rate is below 600 kHz
    void reset();                                // new signal (retune): the carriers and windows go, the decoded tables stay
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool snapshot(Snapshot& out, uint64_t lastSeq);
    void setCenter(double hz);                   // absolute frequency of the capture centre, for the carrier table
    void setBudget(double units);
    void setInmcOn(bool on);
    void setLocation(bool known, double latDeg, double lonDeg);   // the user's position, for the geometry evidence
    void setSatelliteOverride(const std::string& name);           // "" = automatic
    void setFocus(int carrierId);                // 0 none; a focused carrier's window is never parked
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::inmsat
