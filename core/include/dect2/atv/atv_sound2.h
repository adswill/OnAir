// Analogue TV: the second sound systems of terrestrial television.
//   A2 / Zweikanalton (IRT, ITU-R BS.707): two FM sound carriers. The first carries L+R (mono) or sound A, the second carrier, 242.1875 kHz (15.5 line
//   rates) above it, carries R (stereo) or sound B (dual). A pilot of 54.6875 kHz (3.5 line rates) rides on the second carrier, amplitude modulated with
//   117.5 Hz for stereo and 274.1 Hz for dual sound (not modulated: mono).
//   NICAM-728 (ETSI EN 300 163): a differentially encoded QPSK carrier of 364 ksymbol/s 5.85 MHz (system I: 6.552 MHz) above the vision carrier, 728 bit
//   frames every millisecond, 14 bit sound at 32 kHz companded to 10 bits in blocks of 32 samples, stereo or two independent mono channels.
// Both come as a receiver and a transmitter (the transmitters make the test signals and the "test signal" of the app; nothing is transmitted by them).
// Everything here works on complex samples at any rate of at least 8 Msps, the carriers are given in Hz from the centre of the input.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// ------------------------------------------------------------------------------------------------ NICAM-728

namespace nicam {
constexpr double kSymbolRate = 364000.0;
constexpr int kFrameBits = 728;
constexpr double kCarrierSpacingMhz = 5.85;       // B, G, H, K1, L
constexpr double kCarrierSpacingIMhz = 6.552;     // I
// the energy dispersal sequence (x^9 + x^4 + 1, all ones at the start of every frame): the 720 bits after the frame alignment word
void scrambler(uint8_t out[720]);
// the bit of transmission order t (0 .. 703) of the sound block is the bit j of the frame structure (before interleaving): j = (t / 16) + 44 * (t % 16)
int deinterleavedIndex(int t);
// companding: the scale factor word R2R1R0 of a block, and back: the number of bits dropped from the 14 bit samples
int shiftFromScaleFactor(int r);
int scaleFactorForPeak(int peak14);               // peak: largest magnitude of the block's 14 bit samples
int shiftForPeak(int peak14);
// pre-emphasis ITU-T J.17 as in table 2 of the standard (|H|^2 = (1 + x^2) / (75 + x^2), x = 2 pi f / 3000), for 32 kHz samples, 0 dB at 2 kHz;
// the receiver uses the inverse. `state` holds the filter memory.
struct Emphasis { double x1 = 0, y1 = 0; };
float preEmphasis(Emphasis& s, float x);
float deEmphasis(Emphasis& s, float x);
} // namespace nicam

struct NicamStatus {
    bool locked = false;          // frame alignment found and held
    int mode = -1;                // -1 unknown, 0 stereo, 1 two mono channels (dual sound), 2 one mono channel plus data, 3 data or undefined
    bool reserveFlag = false;     // C4: the analogue sound carries the same programme
    uint64_t frames = 0;          // frames decoded since the lock
    uint64_t parityBad = 0;       // sound samples whose parity check failed (the sample is replaced by the one before)
    uint64_t samples = 0;         // sound samples decoded
    float errorRate = 0;          // parity failures over samples, recent
    float levelDb = -120;         // level of the received carrier, dB re full scale
    double carrierErrHz = 0;      // measured offset of the carrier from the one asked for
};

class NicamRx {
public:
    void configure(double fs);
    void reset();
    // Consumes the input; the carrier is at `carrierHz` from the centre. wide: system I (roll-off 100 %), else 40 %.
    // Appends 48 kHz stereo to `l` and `r` (stereo: left and right; dual: `dualChannel` (0 or 1) on both; mono + data: the mono channel on both).
    void process(const cf32* x, size_t n, double carrierHz, bool wide, int dualChannel, std::vector<float>& l, std::vector<float>& r);
    NicamStatus status() const;
    bool audioValid() const;       // locked, the frames are mostly intact and the mode carries sound
private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

// Test signal: L and R tones (or two mono channels). Adds the carrier to `out` (complex samples at the output rate).
struct NicamTxConfig {
    double rate = 10e6;
    bool wide = false;            // system I
    int mode = 0;                 // 0 stereo, 1 dual (M1 in odd frames, M2 in even)
    double toneLHz = 1000, toneRHz = 1500;    // the two channels: tone frequencies (0: silence)
    double amp = 0.25;            // of the 14 bit full scale, before the pre-emphasis
    double level = 0.02;          // carrier amplitude of the QPSK signal (peak, in the same scale as the vision carrier 0.4)
};

class NicamTx {
public:
    explicit NicamTx(const NicamTxConfig& c);
    ~NicamTx();
    // adds n samples of the carrier at `carrierHz` (from the centre of the output); consecutive calls continue the stream
    void add(cf32* out, size_t n, double carrierHz);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ------------------------------------------------------------------------------------------------ A2 (Zweikanalton)

namespace a2 {
constexpr double kCarrier2OffsetHz = 242187.5;    // above the first sound carrier (15.5 line rates)
constexpr double kPilotHz = 54687.5;              // 3.5 line rates
constexpr double kStereoToneHz = 117.5;
constexpr double kDualToneHz = 274.1;
constexpr double kDevHz = 50000.0;
} // namespace a2

struct A2Status {
    bool carrier2 = false;        // a second carrier with the pilot is there
    int mode = -1;                // -1 not known, 0 mono, 1 stereo, 2 dual
    float pilotDb = -120;         // pilot against the noise next to it, dB
    float levelDb = -120;
};

class A2Rx {
public:
    void configure(double fs);
    void reset();
    // carrier1Hz: the first sound carrier from the centre of the input. Appends 48 kHz audio: stereo: left, right; dual: sound A on `l` and sound B on `r`;
    // mono: the first carrier on both. Nothing is appended until both carriers were processed for the same stretch.
    void process(const cf32* x, size_t n, double carrier1Hz, double devKhz, double preEmphUs, std::vector<float>& l, std::vector<float>& r);
    A2Status status() const;
private:
    struct Impl;
    std::shared_ptr<Impl> p_;
};

struct A2TxConfig {
    double rate = 10e6;
    int mode = 1;                 // 0 mono (pilot not modulated), 1 stereo, 2 dual
    double toneLHz = 1000, toneRHz = 1500;    // left / sound A and right / sound B
    double amp = 0.25;            // of full deviation (50 kHz), before the pre-emphasis gain
    double preUs = 50;
    double level2 = 0.02;         // amplitude of the second carrier
    double level1 = 0.02;         // amplitude of the first carrier (the generator of the vision signal has its own; here for tests of the receiver alone)
    bool withFirst = false;       // also make the first carrier (L+R or sound A)
};

class A2Tx {
public:
    explicit A2Tx(const A2TxConfig& c);
    ~A2Tx();
    // adds the second carrier (and the first if asked for); carrier1Hz from the centre of the output
    void add(cf32* out, size_t n, double carrier1Hz);
    // the modulating signals (fractions of full deviation) of the first carrier at time t (the vision generator uses it for its own sound carrier)
    static double firstCarrierAudio(const A2TxConfig& c, double t);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
