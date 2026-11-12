// MPT 1327 trunking control channel in the engine: the decoder of mpt1327_rx.h behind the engine's interface (configure, offset, feed, telemetry).
// Engine stdMode 129, active standard 128 (family 127, "MPT1327 trunking (WIP)"). Receive only. The traffic channels inside the capture are followed
// as analog FM slices of it (single-radio trunking); nothing encrypted is decoded.
#pragma once
#include "dect2/mpt1327/mpt1327_rx.h"
#include "dect2/engine/mode_tuning.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct MptMessageRow {
    double t = 0;              // seconds since the receiver started
    std::string type;          // GTC, ALH, ACK, AHY, BCAST ...
    std::string called;        // PFIX/IDENT1
    std::string caller;        // IDENT2 where the message has one
    int channel = -1;          // channel number the message carries, -1 none
    std::string detail;
    int carrierId = 0;          // whole-span decoding: 1 when the row came from a carrier of the span (carrierHz: its offset from the centre)
    double carrierHz = 0;
};

struct MptChannelRow {
    int channel = 0;
    bool controlChannel = false;   // announced by a BCAST (as a control channel of a system), otherwise allocated by a GTC
    uint32_t count = 0;            // times seen
    double lastT = 0;
    std::string called, caller;    // of the last GTC for it
};

// A traffic channel call the control channel allocated (GTC), followed inside the capture when it can be (single-radio trunking, docs/TRUNK1-3.9.md)
struct MptCall {
    uint64_t id = 0;
    int called = -1, caller = -1;  // PFIX << 13 | IDENT1, and IDENT2
    int channel = -1;
    double freqHz = 0;             // from the channel plan, 0 when the plan is not known
    bool active = false, followed = false;
    int voiceSlot = -1;            // the slot decoding it (0 ...), -1 not followed
    double durationSec = 0;
    std::string state, note;
};

struct MptTelemetry {
    uint64_t seq = 0;
    int state = 0;                 // 0 no input, 1 searching, 2 locked
    double cfoHz = 0;              // not measured (0)
    float snrDb = 0;               // not measured (0)
    bool dataValid = false;        // locked: a good codeword within the last 3 s
    uint64_t blocksOk = 0;         // = good
    uint64_t blocksBad = 0;        // = bad
    bool locked = false;
    bool syncTolerant = false;     // the sync word check is off (Tolerant setting, or the Auto fallback)
    float levelDb = -200;          // level of the last block, dBFS
    uint64_t good = 0;             // codewords with good check bits
    uint64_t bad = 0;              // codewords after a sync whose check bits failed
    uint64_t messages = 0;         // address codewords decoded
    int sysId = -1;                // system identity from the control channel system codeword, -1 unknown
    std::vector<MptMessageRow> rows;        // the last messages, oldest first
    std::vector<MptChannelRow> channels;    // channels seen, by channel number
    double inputRate = 0;
    double centerHz = 0;
    // single-radio trunking: the traffic channels inside the capture are followed as FM slices of it, several at once
    bool follow = true;
    int voiceSlots = 4;
    double controlHz = 0;          // where the control channel is (the capture's centre plus the receiver's offset)
    int planControlChannel = -1;   // the channel plan: the control channel's number and the spacing (Hz), -1 not given
    double planSpacingHz = 12500;
    std::vector<MptCall> calls;    // active first, then the newest
    std::string followStatus;      // what the follower does, or why it cannot follow
    std::string followNote;        // the newest notice (a call outside the capture with the bandwidth it needs ...)
};

class MptReceiver {
public:
    MptReceiver();
    ~MptReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the control channel sits relative to 0 Hz in the input (about -tuneOffsetHz)
    void setCenterHz(double hz);
    void setSyncMode(mpt1327::SyncMode m);   // Standard / Tolerant (French networks) / Auto
    mpt1327::SyncMode syncMode() const;
    bool ready() const;
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    bool telemetry(MptTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // ---- single-radio trunking (docs/TRUNK1-3.9.md): the traffic channels the GTCs name are followed inside the capture, as FM, several at once
    void setWeakSignalPath(bool on);                      // 4.0: 5.5 kHz channel filter with a carrier follow-up before the discriminator (default on; off = the 3.9 receiver)
    void setFollow(bool on);                              // follow calls inside the capture (on by default; needs the channel plan)
    void setVoiceSlots(int n);                            // calls followed at once, 1 .. 16 (default 4)
    void setChannelPlan(int controlChannel, double spacingHz);   // the control channel's number and the channel spacing: channel n is at control + (n - control) x spacing
    void setRecordDir(const std::string& dir);            // one WAV + JSON per call; empty: no files
    void setListenTg(int called);                         // the call whose sound is played (PFIX << 13 | IDENT1); -1 the newest
    void setSilent(bool s);                               // follow but never open the sound device
    void setVolume(float v);
    void setMuted(bool m);
    void setAudioTap(std::function<void(int called, const float* x, size_t n, int rate)> cb);   // tests: the sound of each followed call

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning mpt1327Tuning();

// One line for the top bar and the receiver tab
std::string mpt1327Summary(const MptTelemetry& t);

}  // namespace dect2
