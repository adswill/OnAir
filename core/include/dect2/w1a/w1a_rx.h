// W1A voiceband modes receiver (families 800 - 802, receive only; see w1a_dsp.h and docs/W1A-3.9.md). One class for the three kinds: the kind says
// which decoder runs. The channel is mixed to 0 Hz (marine::Front) and brought to the 12 kHz complex stream the decoders read.
// Simulated signals only: no real recording went through these modes.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_synth.h"
#include "dect2/engine/mode_tuning.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct W1aTelemetry;   // w1a_tel.h

class W1aReceiver {
public:
    W1aReceiver(int kind = 0);   // not explicit: the engine keeps an array {0, 1, 2}
    ~W1aReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                        // after a retune
    void feed(const cf32* x, size_t n);
    bool telemetry(W1aTelemetry& out, uint64_t lastSeq);
    void clearResults();
    void setForced(double fcHz);         // voice inversion, Advanced: the carrier to descramble with (0 = automatic)
    void setRecordPath(const std::string& wavPath);   // voice inversion: write the descrambled audio (12 kHz, 16 bit mono) to this WAV file; empty: stop
    void setLogCallback(std::function<void(const std::string&)> cb);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning w1aTuning(int kind);
// The test signal of a kind (engine standard 802 + kind): looped, at the radio rate, the dial 20 kHz below the centre (tuneOffsetHz).
// Packet: four frames. Inversion: speech-like audio, modeOpt[0] = 1: not inverted (default inverted at 3000 Hz; modeVal[0] sets fc).
// ELT: modeVal[0] sets the sweeps per second (default 3).
std::unique_ptr<ModeSynth> makeW1aSynth(int kind, const SynthConfig& cfg, double sampleRate);

} // namespace dect2
