// PSK demodulator receiver (family 202, engine standard 204, issue #282): a general BPSK / QPSK / OQPSK / 8PSK demodulator with a blind
// analyser, for satellite and other PSK signals of any symbol rate the radio's sample rate holds (about 50 Bd to a few hundred kBd).
//
// The span around the dial is cut out (pskd::Channeliser), the last seconds of it are kept, and while nothing is locked the analyser
// (pskd::analyse) looks at them twice a second: the signal nearest the dial, its symbol rate, order, pulse shape and carrier. Once it locks,
// a second channel is cut around the carrier at about six samples per symbol and demodulated continuously (pskd::Demod), starting from the
// analysed block so a burst is not lost; the carrier is followed through the Doppler of a pass (the channel moves with it). The bits go
// through the chosen differential decoding / descrambler and a sync-word hunt (pskd::BitDecoder), and can be saved as bytes. When the
// constellation stays bad for a few seconds the receiver goes back to searching.
// Experimental: checked only against generated signals (tests/pskdsp), not against a real satellite.
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread (never blocks),
// telemetry() and the setters from the interface thread.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/mode_synth.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct PskdTelemetry;
struct PskdSettings;

class PskdReceiver {
public:
    PskdReceiver();
    ~PskdReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the dial sits in the input
    bool ready() const;
    void reset();                        // after a retune: search again
    void feed(const cf32* x, size_t n);
    bool telemetry(PskdTelemetry& out, uint64_t lastSeq);
    void clearResults();                 // the bits, sync counts and log
    void reanalyse();                    // drop the lock and analyse again now
    void setSettings(const PskdSettings& s);
    PskdSettings settings() const;
    void setLogCallback(std::function<void(const std::string&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning pskdTuning();   // "pskd": the PSK demodulator (pskTuning() is PSK31, PskReceiver its receiver)

// Test signal of the mode: frames with the sync word 1ACFFC1D at the dial.
// SynthConfig::modeOpt [0] order 0 BPSK, 1 QPSK, 2 OQPSK, 3 8PSK  [1] differential (pskd::Diff)  [2] scrambler (pskd::Scrambler)
//                modeVal [0] symbol rate (0: 1200 Bd)  [1] roll-off (0: 0.35, below 0: rectangular)  [2] Doppler ramp Hz/s
std::unique_ptr<ModeSynth> makePskdSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
