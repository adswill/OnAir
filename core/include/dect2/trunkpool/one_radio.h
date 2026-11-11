// Single-radio trunking (docs/TRUNK1-3.9.md), the part every trunking mode shares: one wide capture holds the control channel and the voice
// channels; the mode decodes its control channel and hands each grant here, and the calls inside the capture are followed at once as slices of
// the same samples (CaptureVoice), several at a time (the voice slots), each with its own sound, recording and call row (TrunkPool). A grant
// outside the capture is listed as "outside the capture (needs X MHz)". The channels the grants named are learned (ChannelUsage) for the
// coverage plan (coverage.h). Encrypted grants are listed, never followed. Plain logic on the mode's analysis thread; time is signal time.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/trunkpool/capture_voice.h"
#include "dect2/trunkpool/coverage.h"
#include "dect2/trunkpool/trunkpool.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace tpool {

// One followed or listed call, for the mode's telemetry
struct OneRadioCall {
    uint64_t id = 0;
    int tg = -1, src = -1, slot = -1, voiceSlot = -1;   // voiceSlot: the slice decoding it (0 ...), -1 not followed
    double freqHz = 0, durSec = 0;
    bool active = false, followed = false, encrypted = false, emergency = false;
    std::string state, note, audioPath;
};

class OneRadio {
public:
    explicit OneRadio(const std::string& system, int slots = 4);
    ~OneRadio();
    void configure(double rateHz);                      // the capture's rate (calls end)
    void setCentreHz(double hz);                        // the capture's centre (0: not known, nothing is followed)
    void setControlHz(double hz);                       // the control channel the mode decodes
    void setSlots(int n);                               // voice slots, 1 .. 16
    int slots() const;
    void setRecordDir(const std::string& dir);          // a WAV + JSON per call (empty: none)
    void setEnabled(bool on);                           // off: grants are only counted for the channel list
    bool enabled() const { return on_; }
    void setMonitorPrefix(const std::string& p);        // digital slices: their own voice monitors (capture_voice.h)
    void setP25p2System(int wacn, int sysId, int nac);
    void setDcMode(int m);
    void setAudioTap(std::function<void(int tg, const float* x, size_t n, int rate)> cb);   // the sound of each followed call (tests, the mode's listen)
    void setListenTg(int tg) { listenTg_ = tg; }        // the call whose sound goes to listenAudio (-1: the newest)

    void onControlSeen();                               // a good control channel message (signal time of the last feed)
    void onGrant(const GrantEvent& g);                  // at the signal time of the last feed
    void feed(const cf32* x, size_t n);                 // the capture's samples (after the control channel decoder had them)

    double nowSec() const;
    std::vector<OneRadioCall> calls(size_t max = 64) const;   // active ones first, then the newest ended
    int activeCalls() const;
    std::string status() const;                         // the pool's status line
    std::string lastNote() const;                       // the newest message (an outside call, a busy slot ...)
    const ChannelUsage& usage() const { return use_; }
    double cpuShare() const { return voice_->cpuShareAll(); }
    TrunkPool& pool() { return *pool_; }
    CaptureVoice& voice() { return *voice_; }

private:
    std::string system_;
    std::unique_ptr<CaptureVoice> voice_;
    std::unique_ptr<TrunkPool> pool_;
    ChannelUsage use_;
    double fs_ = 0, centre_ = 0, cc_ = 0;
    uint64_t n_ = 0;
    bool on_ = true;
    std::string dir_;
    int listenTg_ = -1;
    std::function<void(int, const float*, size_t, int)> tap_;
    void rebuild();
};

} // namespace tpool
} // namespace dect2
