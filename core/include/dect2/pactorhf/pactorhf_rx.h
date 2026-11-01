// PACTOR-II / -III / -IV receiver (receive only, physical layer; see pactorhf_dsp.h and docs/pactorhf.md). One class for the three modes: the
// variant says what the mode looks for (2, 3 or 4); the analysis reports what it finds (so PACTOR-II mode tells you when the signal is PACTOR-III).
//
// The channel is mixed to 0 Hz and brought to 24 kHz complex (marine::Front), then the upper sideband becomes 8 kHz audio (hfdig::UsbAudio). The last
// 40 s of audio are analysed every few seconds on a worker thread (carriers, symbol clock, phase steps, repeating frame structure).
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
#include <vector>

namespace dect2 {

struct PactorHfTelemetry;   // pactorhf_tel.h

class PactorHfReceiver {
public:
    explicit PactorHfReceiver(int variant = 2);
    ~PactorHfReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);
    bool ready() const;
    void reset();                        // after a retune
    void feed(const cf32* x, size_t n);
    bool telemetry(PactorHfTelemetry& out, uint64_t lastSeq);
    void clearResults();
    void setLogCallback(std::function<void(const std::string&)> cb);
    void flush();                        // tests: analyse what is buffered now and wait for it
    void setAnalysisInterval(double sec); // tests: how often the buffer is analysed (default 4 s)

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning pactorHfTuning(int variant);
inline ModeTuning pactor2Tuning() { return pactorHfTuning(2); }
inline ModeTuning pactor3Tuning() { return pactorHfTuning(3); }
inline ModeTuning pactor4Tuning() { return pactorHfTuning(4); }

} // namespace dect2
