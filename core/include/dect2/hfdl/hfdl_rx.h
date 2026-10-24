// HFDL receiver: every HFDL channel of the list that lies inside the sample band, decoded at once.
//
// Per channel: the carrier (1440 Hz above the dial frequency) mixed to 0 Hz, a two-stage moving sum and a FIR filter bring it down,
// a cubic interpolator to exactly 4 samples per symbol (7200 Hz), the receive root raised cosine filter. The burst search correlates
// A1 + A2 in 8-symbol pieces added without their phase (a carrier offset of a few hundred Hz does not matter); a found burst gets its
// carrier offset from a coherent search over +-250 Hz refined by the phase step from A1 to A2, its timing from the correlation peak,
// and its rate from M1 (8 cyclic shifts). An adaptive equaliser (fractionally spaced, 25 taps at half a symbol, and 6 decision feedback
// taps, normalised LMS) is trained on the preamble and the 15-symbol probes and runs on decisions in the data, with a phase loop.
// Then: descrambling, soft bits, deinterleaving, Viterbi, the PDU and its check sequences (hfdl_proto.h).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters
// from the interface thread.
#pragma once
#include "hfdl_tel.h"
#include "../dsp/baseband_filter.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// the dial frequencies of the built-in station table inside centreHz +- halfSpanHz (each once, sorted)
std::vector<double> hfdlChannelsNear(double centreHz, double halfSpanHz);

class HfdlReceiver {
public:
    HfdlReceiver();
    ~HfdlReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the dial frequency sits relative to 0 Hz in the input (0 for this mode)
    void setCenterHz(double hz);                         // the radio's centre frequency (0: a file, whose centre is taken as 8.9 MHz)
    bool ready() const;
    void reset();                                        // after a retune (the decoded lists stay: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(HfdlTelemetry& out, uint64_t lastSeq);
    // The messages, aircraft and ground stations stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    void setParallel(bool on);                           // channels in parallel on the shared pool (default) or one after the other; same results either way (tests)
    void setChanneliser(int mode);                       // tests: 0 = the per-channel path, 1 = the shared FFT channeliser, -1 (default) = by the rule (wide captures, see hfdl_rx.cpp)
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setBasebandFilter(const BasebandFilterCfg& c);  // the channel filter (default: 3.4 kHz Blackman); a change restarts the channel chains
    void setWholeSpan(bool on);                          // Whole span: the station table in the capture plus the list; off: the list replaces the table
    void setChannels(const std::vector<double>& dialHz); // empty: every frequency of the station table inside the band

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning hfdlTuning();

} // namespace dect2
