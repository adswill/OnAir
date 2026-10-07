// ADS-B frame validation and the aircraft table: CRC and address checks, error correction, CPR position decoding, expiry.
// No signal processing: the receiver hands in sliced bits, tests can hand in published messages.
#pragma once
#include "adsb_msg.h"
#include "adsb_tel.h"
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

namespace dect2 {

// A sliced frame before any check
struct AdsbRaw {
    uint8_t bytes[14] = {};
    int bits = 112;              // 56 or 112 (112 when the DF is not one that is searched for)
    float conf[112] = {};        // per bit confidence: the difference between the two half-bit energies (larger = surer)
    float levelDbfs = -120;      // mean pulse power of the preamble
    float snrDb = 0;
    double timeSec = 0;          // signal time of the preamble
};

// A frame that passed the checks
struct AdsbFrame {
    uint8_t bytes[14] = {};
    int bits = 0;
    int df = 0;
    uint32_t icao = 0;
    int corrected = 0;           // bits flipped by error correction
    float levelDbfs = 0, snrDb = 0;
    double timeSec = 0;
    adsb::Msg msg;
    bool newAircraft = false;    // the first message from this address
};

struct AdsbOptions {
    int fixBits = 1;             // error correction: 0 off, 1 one bit, 2 up to two bits (taken from the least certain bits only)
    bool fixUnknown = false;     // also accept a repaired DF11 / 17 / 18 from an address that has not been heard cleanly
    double expirySec = 60;       // an aircraft that is silent for this long leaves the table
};

class AdsbTracker {
public:
    AdsbTracker();
    void reset();                                        // forget aircraft, statistics and the frame list (the reference position stays)
    void setReference(double lat, double lon);
    void clearReference();
    void setOptions(const AdsbOptions& o) { opt_ = o; }
    const AdsbOptions& options() const { return opt_; }

    // The checks: DF, CRC, address. On success the frame is repaired if it had to be, applied to the table and described in out.
    // countBad: a frame that fails is counted as a bad one (false when the caller will try the same frame again with other bits)
    bool accept(AdsbRaw& raw, AdsbFrame& out, bool countBad = true);
    // Apply a frame that has been checked elsewhere (tests)
    void apply(const AdsbFrame& f);
    void tick(double now);                               // expire aircraft; call a few times a second
    // Window bookkeeping for the messages-per-second figure: call once per report interval (0.25 s)
    void closeWindow();

    bool knownAddress(uint32_t icao, double now) const;
    void snapshot(AdsbTelemetry& t, double now) const;   // aircraft, frames, statistics (not seq, state or the noise figures)
    size_t aircraftCount() const { return rec_.size(); }
    bool refValid() const { return refValid_; }

    // statistics
    uint64_t good() const { return good_; }
    uint64_t bad() const { return bad_; }
    uint64_t corrected() const { return corrected_; }
    uint64_t dfCount(int df) const { return df >= 0 && df < 32 ? dfCount_[df] : 0; }

private:
    struct Cpr { int lat = 0, lon = 0; double t = -1e9; bool valid = false; };
    struct Rec {
        AdsbAircraft a;
        double firstSeen = 0, lastSeen = 0, lastTrusted = -1e9, lastPos = -1e9, lastTrack = -1e9, lastVel = -1e9;
        Cpr cpr[2], scpr[2];     // last airborne and surface frames, by parity
        double level = 0;        // mean pulse power (linear) of the last messages
        int rejects = 0;
        int emergencyTc = 0;     // from type code 28
        int emergencySq = 0;     // from the squawk
        double altTime = -1e9; int trustedAltFt = 0;      // the last altitude from an ADS-B message
        int pendingSquawk = -1; double pendingTime = 0;   // a changed squawk from a reply, waiting for a second one
    };
    bool known(const Rec* r, double now) const { return r && now - r->lastTrusted <= opt_.expirySec; }
    Rec& touch(uint32_t key, double now);
    void applyMsg(Rec& r, const AdsbFrame& f);
    void airbornePosition(Rec& r, const adsb::Msg& m, double now);
    void surfacePosition(Rec& r, const adsb::Msg& m, double now);
    void commitPosition(Rec& r, double lat, double lon, int kind, double now);
    bool tryCorrect(AdsbRaw& raw, int len, uint32_t rem, double now);
    bool plausibleReply(const AdsbRaw& raw, int df, int len, uint32_t icao) const;

    AdsbOptions opt_;
    std::unordered_map<uint32_t, Rec> rec_;
    std::deque<AdsbFrameInfo> frames_;
    double refLat_ = 0, refLon_ = 0; bool refValid_ = false;
    uint64_t good_ = 0, bad_ = 0, corrected_ = 0, dfCount_[32] = {};
    uint32_t window_ = 0; uint32_t windows_[4] = {};       // good messages per 0.25 s, last four
    float maxRange_ = 0;
    double now_ = 0;
    double snrEma_ = 0, levelEma_ = 0; bool snrInit_ = false;
    bool touchedNew_ = false;
};

std::string adsbDescribe(const adsb::Msg& m);

} // namespace dect2
