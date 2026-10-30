// nRF24L01(+) Enhanced ShockBurst sniffer. The radio is tuned to one channel (2400 + N MHz, N = 0..125); a 2.4 GHz SDR is needed (HackRF, PlutoSDR
// with the extended range, bladeRF, LimeSDR, USRP B2xx): an RTL-SDR, Airspy R2/Mini, SDRplay RSP1A/RSPduo/RSPdx and sound cards do not reach 2.4 GHz.
//
// The channel (radio 1 MHz above, the receiver mixes it out) is received by three demodulators at once, one per data rate (250 kbit/s, 1 and 2 Mbit/s):
// each is a short box filter, an FM discriminator and, at the end of a burst found from the signal power, a slicer with a clock loop over the stored
// samples of the burst (pre-roll included, so the preamble is complete). The bits go to the sniffer (nrf24_proto.h): candidate address lengths
// and positions, a frame is accepted when the CRC is valid. CRC-8 frames wait for a third sight of the same frame.
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

struct Nrf24Telemetry;   // nrf24_tel.h

class Nrf24Receiver {
public:
    Nrf24Receiver();
    ~Nrf24Receiver();
    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the channel sits in the samples
    bool ready() const;
    void reset();
    void clearResults();
    void feed(const cf32* x, size_t n);
    bool telemetry(Nrf24Telemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    void flush();                        // tests: close the burst that is open
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning nrf24Tuning();

} // namespace dect2
