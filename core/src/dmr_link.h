// DMR link layer of the receiver: frame sync search on the demodulated symbols, time slot tracking, burst decoding and the call logic.
// Input is the output of the matched filter at 48 kHz in Hz (ten samples per symbol), one sample at a time. Internal to the DMR receiver.
#pragma once
#include "dect2/dmr_fec.h"
#include "dect2/dmr_proto.h"
#include "dect2/dmr_tel.h"
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace dect2 {
namespace dmr {

class Link {
public:
    Link();
    void reset();                                   // forget the signal; keeps nothing but the counters of telemetry that must not restart
    void push(float y);                             // one sample of the matched filter output
    // after a call to push(): Hz by which the demodulator should move its oscillator (taken once)
    double takeNcoDelta() { const double d = ncoDelta_; ncoDelta_ = 0; return d; }
    void setLog(std::function<void(const std::string&)> cb) { log_ = std::move(cb); }
    // fills everything the link knows; the front end adds its own measurements
    void snapshot(DmrTelemetry& t) const;
    double nowSec() const { return (double)n_ / 48000.0; }
    // a voice callback for a later vocoder: slot, burst position 0..5 (A..F), 216 bits VS(215)..VS(0)
    std::function<void(int, int, const Bits&)> onVoiceBurst;
    uint64_t samples() const { return n_; }
    double cfoResidualHz() const;                   // offset the slicer still corrects for
    bool locked() const;                            // at least one slot is being followed (a track has decoded bursts)
    void shiftOffset(double hz);                    // the front end moved its oscillator by this much (up): the symbol levels move down by it

private:
    static constexpr int kRingBits = 14;
    static constexpr size_t kRing = 1u << kRingBits, kMask = kRing - 1;
    static constexpr int kMaxTracks = 4;

    struct Cal { double off = 0, unit = kDevUnit; bool valid = false; };
    static constexpr double kDevUnit = 648.0;

    struct Assembler {                              // data message in progress
        bool active = false;
        DataHeader hdr;
        int dt = 0;                                 // data type of the blocks
        std::vector<uint8_t> bytes;
        int got = 0, bad = 0, fecErrors = 0;
        double startSec = 0;
        bool confirmed = false;
        bool crcAllOk = true;
    };

    struct Track {
        bool used = false, confirmed = false, probe = false;
        int family = -1;                            // 0 base station, 1 mobile, 2 direct mode
        int slot = 0;                               // 1 or 2 (0 not known)
        int tcVote[2] = {0, 0};                     // clean CACH reports of the TDMA channel bit, for slot 1 and slot 2 (base station)
        double nextT0 = 0, period = 2880;
        double lastAnchor = -1;                     // t0 of the last burst whose sync re-timed the track
        int hits = 0, misses = 0, tentativeOk = 0, voiceRun = 0;
        double birthRho = 0;
        Cal cal;
        int cc = -1;
        int vpos = -1;                              // voice burst position of the last voice burst, -1 none
        uint64_t lastVoiceSample = 0;
        int lcHave = 0;
        Bits lcFrag[4];
        TalkerAlias ta;
        Assembler msg;
        // call state
        bool inCall = false;
        DmrCall call;
        std::string lastBurst;
        int state = 0;                              // as DmrSlot::state
        uint64_t bursts = 0;
        double errEma = 0.3, berNum = 0, berDen = 0;
        int sinceVoice = 0;                         // bursts since the last voice burst
        uint64_t lastCsbkKey = 0;
        double lastCsbkSec = -10;
        uint64_t lastTermKey = 0;                   // the call a terminator ended, to recognise the repeats of hang time
        double lastTermSec = -100;
        double lastSeenSec = 0;
        std::string alias;
    };

    // ---- sync search
    float ring_[kRing] = {};
    uint64_t n_ = 0;                                // samples pushed
    float tmpl_[5][24];
    int patVoice_[5], patData_[5];
    float rho_[5][5] = {};                          // the last five correlation values per pattern
    void searchSync();
    float at(uint64_t i) const { return ring_[i & kMask]; }
    float interp(double pos) const;                 // cubic interpolation of the ring at a (fractional) sample position
    struct Cand { int pat = 0; bool voice = false; double pos = 0, rho = 0; };
    void onCandidate(const Cand& c);
    bool verifySync(int sync, double centre, double& A, double& b, int& diff) const;

    // ---- tracks
    Track tr_[kMaxTracks];
    Track* findTrack(double t0, double tol);
    Track* newTrack(int family, double t0, const Cal& cal, bool probe);
    void dropTrack(Track& t, const char* why);
    void runTracks();
    void decodeBurst(Track& t);

    struct Outcome {
        bool valid = false;                         // the burst had a recognised structure and passed its checks
        bool structure = false;                     // a sync or EMB was recognised even if the payload failed
        bool voice = false;
        bool data = false;
        int dt = -1;
    };
    Outcome decodeData(Track& t, const Bits& burst, const float* z, int sync, int cc, int dt);
    void voiceBurst(Track& t, int pos, const Bits& burst, int lcss, const Bits& emb32, bool hadSync);
    void handleCach(Track& t, const float* zc);

    // ---- calls and messages
    void startCall(Track& t, int kind, uint32_t src, uint32_t dst, uint8_t svc, bool late, bool idsKnown);
    void endCall(Track& t, bool terminated);
    void finishMessage(Track& t);
    void addLog(const DmrCall& c);
    void addMessage(const DmrMessage& m);
    void say(const std::string& s) { if (log_) log_(s); }
    void noteFec(Track& t, double errors, double bits, bool toCall = true);   // bit error statistics; `toCall`: the errors also count for the call
    int slotOf(const Track& t) const { return t.slot ? t.slot : 1; }

    // ---- calibration and statistics
    Cal globalCal_;
    double ncoDelta_ = 0;
    double nco_ = 0;                                // where the front end's oscillator is (the sum of what it was told), for the acceptance limit
    std::function<void(const std::string&)> log_;
    DmrTelemetry tel_;                              // counters and logs accumulate here
    std::vector<float> eye_;
    int cc_ = -1;
    int linkFamily_ = -1;
    double snrEma_ = 0;
    bool snrValid_ = false;
    double berNum_ = 0, berDen_ = 0;
    uint64_t lastLockSample_ = 0;
    DmrSlot lastSlot_[2];
    std::string cachInfo_;
    Bits shortLcPiece_[4];
    int shortLcHave_ = 0;
    double symPpm_ = 0;
    double devHz_ = 1944;
};

} // namespace dmr
} // namespace dect2
