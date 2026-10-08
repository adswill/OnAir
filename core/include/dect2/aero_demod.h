// Inmarsat Aero P channel demodulation: one carrier at a time, already moved near 0 Hz.
//   600 / 1200 bit/s: MSK seen as offset QPSK with half-sine pulses; the bits are the changes between successive arm decisions
//                     (JAERO mskdemodulator.cpp: DiffDecode of imag as is, of real negated, i.e. the signs of consecutive arm values compared with (-1)^k)
//   10500 bit/s:      offset QPSK, root raised cosine with roll-off 1 (JAERO oqpskdemodulator.cpp rrc.design(1, .., 10500/2));
//                     the Q arm leads, bits alternate Q, I (pushed imag then real)
// Carrier: the squared signal has two lines at 2 df +- Rb/2; they give the offset (and confirm the bit rate) before the loops start.
#pragma once
#include "aero_phy.h"
#include "ring.h"
#include <functional>
#include <memory>

namespace dect2 {

struct AeroFrameEvent {
    int bitRate = 0;
    uint16_t header = 0;
    std::vector<uint8_t> bytes;        // the frame's information bytes (72 or 312)
    int susOk = 0, susBad = 0;
    int uwErrors = 0;                  // bit errors in the unique word, -1 when it was not found and the frame was taken on timing alone
    float channelBer = 0;
    float ebn0Db = 0;                  // per channel bit
    double freqHz = 0;                 // carrier relative to the demodulator's input centre
    uint64_t bitIndex = 0;             // channel bit count at the frame start
};

// Soft channel bits -> frames: unique-word search, a flywheel from frame to frame, header, decoding.
class AeroFramer {
public:
    explicit AeroFramer(int bitRate);
    ~AeroFramer();
    void reset();
    void push(float soft);             // one channel bit, positive = 1
    void setCallback(std::function<void(AeroFrameEvent&)> cb);
    bool synced() const;               // a frame start is known
    bool dataLock() const;             // the SU checks have been good lately (JAERO's data carrier detect)
    uint64_t frames() const;
    uint64_t uwMisses() const;
    uint64_t uwBitErrors() const;
    uint64_t falseSyncs() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

class AeroDemod {
public:
    // fsIn: rate of the samples handed to feed(); searchHz: how far from 0 Hz the carrier may be
    AeroDemod(int bitRate, double fsIn, double searchHz);
    ~AeroDemod();
    void reset();
    void feed(const cf32* x, size_t n);
    void setCallback(std::function<void(AeroFrameEvent&)> cb);
    int bitRate() const;
    bool acquired() const;             // the two lines were found: the loops run
    bool synced() const;
    bool dataLock() const;
    double freqHz() const;             // current carrier estimate relative to the input centre
    float ebn0Db() const;
    float acqMetric() const;           // strength of the last line search (0 = none yet)
    const AeroFramer& framer() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Modulator for the test signal: channel bits -> complex baseband at fs (unit power, carrier at 0 Hz).
// The bit clock may run off nominal by clockPpm.
class AeroModulator {
public:
    AeroModulator(int bitRate, double fs, double clockPpm = 0);
    ~AeroModulator();
    void pushBits(const uint8_t* bits, size_t n);      // queue channel bits (what the frame encoder makes)
    size_t queuedBits() const;
    void generate(cf32* out, size_t n);                // needs a few bits queued ahead; runs on zeros when starved
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// Root raised cosine pulse, symbol period 1 (for the modulator and the matched filter)
double aeroRrc(double t, double alpha);

} // namespace dect2
