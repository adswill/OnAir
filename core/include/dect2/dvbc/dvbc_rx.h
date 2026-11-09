// DVB-C receiver (EN 300 429 / J.83 annex A and C): carrier and symbol rate from the spectrum, an interpolating matched filter with a Gardner
// timing loop, a fractionally spaced equaliser (CMA, then decision directed LMS with a carrier loop), the QAM order and the quarter turn found from
// the transport stream sync bytes (the differential coding makes the data immune to a quarter turn), then the DVB-T outer decoder: de-interleaver,
// Reed-Solomon, energy dispersal. Transport stream packets go to the packet callback, like DVB-T and DVB-S.
#pragma once
#include "dvbc_tel.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/common/ring.h"
#include <functional>
#include <memory>
#include <string>

namespace dect2 {

struct SynthConfig;
class ModeSynth;

class DvbcReceiver {
public:
    DvbcReceiver();
    ~DvbcReceiver();
    void configure(double inputRateHz);
    bool ready() const;                                  // false when the input rate is too low
    void reset();
    void feed(const cf32* x, size_t n);
    bool telemetry(DvbcTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void setPacketCallback(std::function<void(const uint8_t* packets, size_t nPackets, double secs)> cb);

    void setSymbolRate(double hz);                       // 0 = measure it (a manual rate that does not lock gives way to the measured one)
    void setQam(int q);                                  // -1 automatic, 0..4 16..256-QAM
    void setAnnexC(bool c);                              // roll-off 0.13 instead of 0.15
    void setAnnexB(bool b);                              // ITU-T J.83 annex B (clear QAM): 64-QAM or 256-QAM with its own FEC (j83b.h)
    void setSignalOffset(double hz) { (void)hz; }       // centred channel: nothing to mix out
    void setBlocking(bool b);                            // recordings and tests: wait for the decoder instead of dropping input
    void flush();                                        // waits until everything fed so far is decoded

    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

ModeTuning dvbcTuning();
double dvbcSynthNetBitrate(const SynthConfig& cfg, double sampleRate);   // the transport stream bit rate these options carry
std::unique_ptr<ModeSynth> makeDvbcSynth(const SynthConfig& cfg, double sampleRate);
std::unique_ptr<ModeSynth> makeDvbcSynth(const SynthConfig& cfg, double sampleRate, std::function<void(uint8_t*)> ts);
// SynthConfig for the test signal: modeOpt[0] QAM (0 = 64-QAM, 1 16, 2 32, 3 128, 4 256), modeOpt[1] 1 = annex C (roll-off 0.13),
// modeOpt[2] 1 = spectrum inverted; modeVal[0] symbol rate (0: 6.952 Msym/s, or what the sample rate allows), modeVal[1] clock offset ppm;
// snrDb is Es/N0, cfoHz the carrier offset, echoDb / echoDelay (samples) one echo.

// European cable raster: channel centre of a frequency in 114..862 MHz, snapped to the 8 MHz raster (centre = 114 + 8 k MHz ... 858 MHz)
double dvbcChannelCentreMhz(double mhz);

} // namespace dect2
