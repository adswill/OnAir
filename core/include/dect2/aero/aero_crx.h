// One Inmarsat Aero C channel (8400 bit/s OQPSK, circuit-mode voice) from a wideband stream: mixer + decimator to about 48 kHz, the OQPSK
// demodulator with its own carrier search, the C channel framer (aero_cchan.h). The coded voice frames come out of the callback; what they are
// (AMBE) and how they become sound is the user-installed codec's business (inmsat_voice.h, docs/CODECS.md).
// Feed with the samples of a stream that contains the carrier: a window of the Inmarsat all-carriers mode, or an IQ file.
#pragma once
#include "aero_cchan.h"
#include "aero_demod.h"
#include "aero_front.h"
#include <functional>
#include <memory>

namespace dect2 {

class AeroCReceiver {
public:
    AeroCReceiver();
    ~AeroCReceiver();
    // fsIn: sample rate of the stream handed to feed(); offsetHz: where the carrier sits against 0 Hz; searchHz: how far from there it may be
    void configure(double fsIn, double offsetHz, double searchHz = 2500);
    void reset();                                           // forget the lock, keep the settings
    void feed(const cf32* x, size_t n);
    void setFrameCallback(std::function<void(const AeroCFrameEvent&)> cb);
    bool acquired() const;                                  // the carrier was found
    bool synced() const;                                    // frame start known
    bool dataLock() const;                                  // frames have checked out lately
    double carrierHz() const;                               // measured, against 0 Hz of the input
    float ebn0Db() const;
    uint64_t frames() const;
    uint64_t samplesFed() const;
    uint64_t lastFrameSample() const;                       // samplesFed() when the last frame ended, 0 = none
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
