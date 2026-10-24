// VDL Mode 2 receiver: every VDL2 channel of the list that lies inside the sample band, decoded at once.
//
// Per channel: mixed to 0 Hz (the radio's tuning error, measured from the bursts, taken out), a three-stage moving sum and a FIR filter
// bring it to about 83 kHz (8 samples per symbol). A wider filter (+-12 kHz) feeds the burst search: the 16 training symbols are looked
// for with a differential correlation, which does not mind a carrier offset of up to +-5 kHz. A found burst is turned by its own carrier
// offset, filtered to the channel (+-8.6 kHz), its timing refined on the training symbols, and the D8PSK symbols are taken with a phase
// locked loop (coherent decisions, differential decoding) and a Gardner timing loop. Then: descrambling, the header and its FEC, the
// Reed-Solomon blocks, the AVLC frames and their check sequences (vdl2_proto.h).
//
// Written to the engine's contract: configure() once the sample rate is known, feed() from the analysis thread, telemetry() and the setters
// from the interface thread.
#pragma once
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include "vdl2_tel.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// The European VDL2 channels: 136.975 (CSC), 136.725, 136.775, 136.825, 136.875, 136.900 MHz
std::vector<double> vdl2EuropeChannels();
// Every VDL2 channel in use worldwide (the Whole span switch decodes those inside the capture)
std::vector<double> vdl2WorldChannels();

class Vdl2Receiver {
public:
    Vdl2Receiver();
    ~Vdl2Receiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);                     // where the dial frequency sits relative to 0 Hz in the input (0 for this mode)
    void setCenterHz(double hz);                         // the radio's centre frequency (0: a file, whose centre is taken as 136.85 MHz)
    bool ready() const;                                  // false when the input rate is too low for this mode
    void reset();                                        // after a retune: forget the signal (the tuning error and the decoded lists are kept: clearResults)
    void feed(const cf32* x, size_t n);
    bool telemetry(Vdl2Telemetry& out, uint64_t lastSeq);
    // The messages, aircraft and ground stations stay through reset(), configure() and retunes: only clearResults() empties them
    void clearResults();
    std::string saveResults() const;                     // for the results file (results_io.h)
    bool loadResults(const std::string& blob);           // false: not this build's format, nothing changes
    void setLogCallback(std::function<void(const std::string&)> cb);

    void setChannels(const std::vector<double>& hz);     // absolute channel frequencies; empty = the European set
    void setWholeSpan(bool on);                          // Whole span: the worldwide channel set (vdl2WorldChannels) in the capture plus the list; off: the list or the European set
    void setTuneErrorHz(double hz);                      // a starting guess of the radio's tuning error (measured from then on)
    void setTrackTuning(bool on);                        // follow the tuning error from the decoded bursts (default on)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning vdl2Tuning();

} // namespace dect2
