// DRM receiver: DRM30 (long, medium and short wave) and, as far as it goes, DRM+ (VHF band II) digital radio.
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
#pragma once
#include "drm_tel.h"
#include "mode_tuning.h"
#include "ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

class DrmReceiver {
public:
    DrmReceiver();
    ~DrmReceiver();

    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal, drop queued output
    void feed(const cf32* x, size_t n);
    bool telemetry(DrmTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);   // events for the log pane; may be called from the receiver thread
    // Sound (volume, mute) as in FmReceiver; the app pushes its shared controls here.
    void setVolume(float v);                             // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);                              // decode but do not open the sound device (tests, command line)
    void setAudioTap(std::function<void(const float* left, const float* right, size_t n)> cb);   // tests: the 48 kHz sound, called from feed()
    // Which audio service plays (short id 0 .. 3); -1: the first audio service of the multiplex
    void selectService(int shortId);
    // Tests and tools: every logical frame of every stream as it comes out of the channel decoder (part A then part B), called from feed()
    void setStreamCallback(std::function<void(int stream, const uint8_t* bytes, int len, int lenA, const DrmTelemetry& state)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning drmTuning();

} // namespace dect2
