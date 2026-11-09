// DRM receiver: DRM30 (long, medium and short wave, robustness modes A to D) and DRM+ (mode E, 100 kHz, VHF bands I, II and III) digital radio;
// the mode is found by itself (DRM+ needs an input rate of 100 ksps or more).
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread (they must be safe while feed() runs).
#pragma once
#include "dect2/dab/dab_journaline.h"
#include "drm_data.h"
#include "drm_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
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
    // Channel filter on the DRM30 input: keeps +-passHz around the channel (0 = off, the default; 7000 suits a 10 kHz channel, 12000 a 20 kHz one). Forgets the signal when changed.
    void setChannelFilterHz(int passHz);
    int channelFilterHz() const;
    // Which audio service plays (short id 0 .. 3); -1: the first audio service of the multiplex
    void selectService(int shortId);
    // Tests and tools: every logical frame of every stream as it comes out of the channel decoder (part A then part B), called from feed()
    void setStreamCallback(std::function<void(int stream, const uint8_t* bytes, int len, int lenA, const DrmTelemetry& state)> cb);

    // The Journaline data service of the multiplex (SDC application information, packet mode, DAB domain user application 0x44a)
    JournalineDecoder& journaline();
    bool hasJournaline() const;
    // The other data services (MOT slide show, broadcast website, EPG, TPEG; the others by name): what the data service with this short id has received.
    // False when the multiplex has no application information for it. Safe to call while feed() runs.
    bool dataView(int shortId, DrmDataView& out) const;
    // Slide show pictures that came since the last call (any service), oldest first; the app saves them to the gallery
    bool takeSlide(int& shortId, DrmSlide& out);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning drmTuning();

} // namespace dect2
