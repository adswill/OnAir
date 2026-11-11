// CW (Morse) receiver: a skimmer that decodes every CW signal in a window around the dial frequency at once.
//
// The channel is mixed to 0 Hz and brought to 24 kHz complex (marine::Front). A 2048-point spectrum every 1024 samples (11.7 Hz bins),
// held with a fast attack and a slow release so a keyed carrier stays visible between its elements, finds the carriers: peaks 10 dB over
// the median of the window (the noise floor), at least 60 Hz apart. Each carrier gets its own signal path: mixed to 0 Hz, CIC to 1 kHz,
// a 65-tap low-pass (+-50 Hz), the envelope, and an adaptive threshold between a mark level (fast attack, follows the fading) and a noise
// level, with hysteresis. The key-down / key-up durations go to the timing decoder (cw_morse.h): speed 5 - 50 WPM followed from the
// dot / dash clusters, Farnsworth spacing, prosigns. A signal that stays silent for 30 s is dropped (unless it is the focus).
//
// The focus (setFocusHz, a click on the waterfall) is the signal of the big text view; it can have a sidetone: a tone (400 - 1000 Hz, 700 default) keyed by its
// decoder, off by default (setSidetone).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct CwTelemetry;   // cw_tel.h

class CwReceiver {
public:
    CwReceiver();
    ~CwReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the dial frequency sits relative to 0 Hz in the input
    bool ready() const;                  // false when the input rate is too low
    void reset();                                        // after a retune (the signals' text stays: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(CwTelemetry& out, uint64_t lastSeq);
    // The signals and their text (also of signals gone quiet) stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setFocusHz(double offsetHz);    // the signal nearest this offset from the dial becomes the focus (a new one when none is near)
    void clearSignals();                 // forget every signal and its text
    void setThresholdDb(float db);       // how far over the noise floor a carrier must be to be followed (default 10)
    void setSidetone(bool on);           // a tone for the focused signal (off by default)
    void setSidetoneHz(float hz);
    void setVolume(float v);
    void setMuted(bool m);
    void setSilent(bool s);              // decode but do not open the sound device
    // Whole span (the "wide decode" switch): skim the visible span (up to 250 kHz around the dial frequency) instead of +-7 kHz. The span is
    // cut by a WideDecimator (widespan.h) and tiled by one skimmer per 12 kHz slice; the signals are listed with their offset from the dial.
    void setWide(bool on, double spanHz = 250000);
    bool wide() const;
    void setSpotShiftHz(double hz);      // internal (slice skimmers): added to a signal's offset for the PSK Reporter spot
    void setSidetoneTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz sidetone (mono)
    // The timing decoder: 0 automatic (the default: both run on every signal and the text comes from the one that suits the sender),
    // 1 the fixed clustering of 0.2.7 (cw_morse.h Timing), 2 the adaptive one (cw_decoder.h FistDecoder)
    void setTimingDecoder(int which);
    int timingDecoder() const;
    void setKeyTap(std::function<void(int id, double offsetHz, bool key)> cb);   // tests: every signal's detector output, once a millisecond

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning cwTuning();

// The CW segments of the HF bands (IARU Region 1): where the band presets tune
struct CwBand { const char* name; double loMhz, hiMhz; };
const CwBand* cwBands(int& count);

} // namespace dect2
