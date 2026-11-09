// Channelised analog voice receiver: CB, FRS / GMRS, PMR446, amateur FM repeaters, HFGCS / HF aviation voice, MDC-1200 on analog FM and a UHF
// wireless microphone finder, all in one class (one mode of the program per service, setService() picks it).
//
// The carrier finder (dsp/carrierfind.h) looks for carriers on the raster of the service's channel plan in the captured span (the plan's channels
// are also given to it as known channels, so they are found at a lower threshold). Each carrier gets a channelizer and its own channel receiver:
//   FM       narrow-FM discriminator (+-5 kHz full scale), 300 - 3000 Hz voice band, optional 750 us de-emphasis, quieting squelch (as the marine VHF mode)
//   AM       envelope detector on the carrier level, AGC'd audio
//   USB/LSB  the sideband selected by a complex filter around the channel frequency, AGC'd audio
//   Auto     CB: the signal decides (a constant envelope is FM, a strong carrier with a varying envelope is AM, no carrier is SSB; the side the energy
//            is on gives USB or LSB), re-decided whenever the channel opens
//   WFM      the wireless microphone finder: 192 kHz channel, wide FM, 15 kHz audio
// A channel plays when it opens: the one the user picked (listen()), or the scan's (whole span: the first active channel in order, held until quiet;
// single tuner, HF: the scan asks the app to retune, tuneRequest in the telemetry), or the strongest open channel. Every channel keeps a call log,
// a CTCSS tone or DCS code reading (FM) and can be recorded (one WAV per transmission). In the MDC-1200 service the discriminator audio of every open
// carrier also goes to an MDC-1200 decoder.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks), telemetry() and the
// setters from the interface thread.
#pragma once
#include "dect2/chanvoice/chv_plan.h"
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

struct ChvTelemetry;   // chv_tel.h

constexpr double kChvAudioRate = 48000.0;
constexpr double kChvMicRate = 192000.0;

class ChanVoiceReceiver {
public:
    ChanVoiceReceiver();
    ~ChanVoiceReceiver();

    // ---- the engine's contract
    void configure(double inputRateHz);
    void setSignalOffset(double hz);                  // where the dial frequency sits relative to the middle of the samples
    void setCenterHz(double hz);                      // the radio's centre frequency (0: not known, files)
    void setDialHz(double hz);                        // the dial frequency (places the channels when the centre is not known)
    bool ready() const;                               // false when the input rate is too low
    void reset();                                     // after a retune: the carriers are new, the call log stays
    void feed(const cf32* x, size_t n);
    bool telemetry(ChvTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void clearResults();                              // the call log, the MDC list and the activity counts

    // ---- the service and its plan
    void setService(chv::Service s);                  // resets the carriers and the scan; the region keeps its last value per service
    chv::Service service() const;
    void setRegion(int region);
    int region() const;
    void setDemodOverride(int demod);                 // -1: the plan's choice (CB: automatic); else chv::Demod for every channel
    void listen(int key);                             // play this plan channel (-1: automatic: the scan, else the strongest open channel)
    void setScan(bool on);
    void setScanKeys(const std::vector<int>& keys);   // the channels to scan (empty: every channel of the plan)
    void setScanTiming(double dwellSec, double hangSec);
    void setSingleTuner(bool on);                     // the scan retunes the radio (HF): update asks for one channel at a time
    void setRecording(bool on, const std::string& dir, double minSec = 0.5);

    // ---- the receiver
    void setSquelch(int percent);                     // 0 (open at the weakest) .. 100 (needs a clean signal); default 50
    void setDeemphasis(bool on);
    void setCarrierLimit(int n);                      // carriers followed at once (1 .. 24, default 10)
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);                           // decode but do not open the sound device (tests, command line)
    // tests: the 48 kHz audio of each carrier after the squelch (the gated stream), by plan key (-1 off the plan) and the demodulator in use
    void setChannelTap(std::function<void(int key, int demod, const float* x, size_t n)> cb);
    int carrierCount() const;
    int playingKey() const;
    // wireless microphone finder
    void setMicListen(int id);                        // play the candidate with this id (0: none)
    void setMicBand(int index);                       // -1: every band of the region

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning chanVoiceTuning(chv::Service s);

} // namespace dect2
