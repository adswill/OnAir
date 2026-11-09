// PSK31 / PSK63 receiver (BPSK and QPSK): decodes every PSK signal in the audio passband of an upper sideband receiver at once.
//
// The channel is mixed to 0 Hz and brought to 24 kHz complex (marine::Front). A 4096-point spectrum every 2048 samples (5.9 Hz bins),
// held with a fast attack and a slow release and smoothed over the signal's width, finds the signals between dial + 100 Hz and
// dial + 3.9 kHz: peaks 8 dB over the median (the noise floor), at least one and a half signal widths apart. Each signal gets its own
// path: mixed to 0 Hz, CIC to 1 kHz, a matched filter (the raised-cosine pulse of PSK31, two symbols long), the symbol clock from the
// envelope (the amplitude dips between symbols: the peak of the per-phase average is followed one sample at a time), differential
// detection, AFC (the remaining phase step per symbol, BPSK squared, QPSK to the fourth), a DCD from how clean the phase decisions
// are, then the varicode (BPSK) or the K = 5 Viterbi decoder and the varicode (QPSK). A signal silent for 40 s is dropped (not the focus).
//
// The focus (setFocusHz, a click on the waterfall) is the signal of the big text view.
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

struct PskTelemetry;   // psk_tel.h

class PskReceiver {
public:
    PskReceiver();
    ~PskReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the dial frequency sits relative to 0 Hz in the input
    bool ready() const;
    void reset();                                        // after a retune (the signals' text stays: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(PskTelemetry& out, uint64_t lastSeq);
    // The signals and their text (also of signals gone quiet) stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setAuto(bool on);               // Auto: the modulation follows the signal (BPSK31 .. QPSK125, tried on the last seconds of the band). Off by default; setMode alone does not change it
    bool autoOn() const;
    void setMode(int m);                 // kPskModes: BPSK31 (default), BPSK63, QPSK31, QPSK63; forgets the signals
    void setFocusHz(double offsetHz);    // the signal nearest this offset from the dial becomes the focus (a new one when none is near)
    void clearSignals();
    void setThresholdDb(float db);       // how far over the noise floor a signal must be to get a decoder (default 8)
    // "Listen" (off by default): the upper sideband, 200 .. 3800 Hz above the dial, as audio on the sound card; volume and mute are the
    // app's shared sound controls. The decoding does not change with it.
    void setListen(bool on);
    bool listening() const;
    void setVolume(float v);             // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);              // listen without opening the sound device (tests, command line)
    // Whole span (the "wide decode" switch): decode the PSK signals of the visible span (up to 250 kHz around the dial frequency), not only dial + 100 Hz
    // .. 3.9 kHz: the span is cut by a WideDecimator (widespan.h) and tiled by one receiver per 3.7 kHz slice; offsets are from the dial.
    void setWide(bool on, double spanHz = 250000);
    bool wide() const;
    void setSpotShiftHz(double hz);      // internal (slice receivers): added to a signal's offset for the PSK Reporter spot
    void setListenTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz audio Listen plays, called from feed()

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning pskTuning();

// The PSK calling frequencies (dial, upper sideband): where the presets tune
struct PskPreset { const char* name; double mhz; };
const PskPreset* pskPresets(int& count);

} // namespace dect2
