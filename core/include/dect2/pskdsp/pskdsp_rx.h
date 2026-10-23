// PSK demodulator and analyser receiver (family 202): finds the strongest narrow signal near the dial, measures its symbol rate, modulation and
// pulse shape blindly (pskdsp.h analyse), then demodulates it continuously and hands out the bits, the constellation and the quality figures.
// EXPERIMENTAL: no real signal has been tested; the tests use generated signals.
#pragma once
#include "pskdsp_tel.h"
#include "dect2/common/ring.h"
#include "dect2/engine/mode_tuning.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {
class PskdReceiver {
public:
    PskdReceiver();
    ~PskdReceiver();
    void configure(double rate);
    void setSignalOffset(double hz);   // where the dial sits in the input
    void setCenterHz(double hz);
    bool ready() const;
    void reset();                      // after a retune: forget the signal, keep the settings
    void clearResults();               // forget the bits
    void feed(const cf32* iq, size_t n);
    bool telemetry(PskdTelemetry& out, uint64_t lastSeq);
    // settings (the app's panel); 0 / Auto / empty mean automatic
    void setOrder(pskd::Order o);
    void setBaud(double baud);                 // 0: blind
    void setAlpha(double a);                   // -2 automatic, -1 rectangular, 0..1 RRC roll-off
    void setSnapStandard(bool on);
    void setSearchHz(double hz);               // width searched around the dial; 0: automatic
    void setManualCarrier(bool on, double hzFromDial, double bwHz);
    void setBits(pskd::Diff d, pskd::Scrambler s, const std::string& syncHexOrBits, bool invert, bool autoAmbiguity, int rotation, bool conj);
    void analyseNow();                         // run the blind analysis at once (it also repeats by itself)
    void setLogCallback(std::function<void(const std::string&)> cb);
    std::string saveResults() const { return {}; }
    bool loadResults(const std::string&) { return false; }
    bool busy() const { return false; }
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
ModeTuning pskdTuning();
} // namespace dect2
