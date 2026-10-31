// ATIS receiver (ETSI EN 300 698-1 annex B): one VHF marine channel (the inland channels 156.3 - 162.0 MHz) received as narrow FM. The voice
// of the channel plays on the sound card (Listen, on by default: the ATIS is sent after the voice) and the 1300 / 2100 Hz tones in the same
// audio are decoded: front end to 24 kHz (marine::Front), FM discriminator and tone slicer at 1200 baud (marine::VhfSlicer) in a few small
// carrier-offset branches (a radio's carrier is rarely exact; the tone slicer wants the tones within a few hundred hertz), both polarities, and
// the DSC decoder (marine_dsc.h), which finds the phasing, merges the two copies of every symbol and repairs with the check symbol.
// A message is the ten digit identity (atis_proto.h).
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

struct AtisTelemetry;   // atis_tel.h

class AtisReceiver {
public:
    AtisReceiver();
    ~AtisReceiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the channel sits in the samples
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(AtisTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void flush();                        // tests: report now

    // ---- the sound ("Listen": the voice of the channel, narrow FM)
    void setListen(bool on);             // default on
    bool listening() const;
    void setVolume(float v);             // 0 .. 1
    void setMuted(bool m);
    void setSilent(bool s);              // do not open the sound device (tests, command line)
    void setListenTap(std::function<void(const float* x, size_t n)> cb);   // tests: the 8 kHz audio Listen plays
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning atisTuning();

} // namespace dect2
