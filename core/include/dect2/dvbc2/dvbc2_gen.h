// DVB-C2 test signal (EN 302 769): one C2 system of up to one L1 block width (3 409 carriers, 7.61 MHz) with its data slices, PLPs and
// notches, built in the frequency domain symbol by symbol (preamble with the L1 part 2, data symbols with scattered / continual / edge
// pilots), 4K IFFT at 64/7 Msample/s, guard interval, then an echo, the sample clock error, the carrier offset and noise, resampled to the
// requested rate. The transport streams come from the per-PLP packet sources (the small test stream when none is given).
// Simulated only: this generator and the receiver of dvbc2_rx.h were checked against each other, never against a cable network.
#pragma once
#include "dvbc2.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/mode_synth.h"
#include <functional>
#include <memory>
#include <random>

namespace dect2 {

struct SynthConfig;

namespace dvbc2 {

struct PlpConfig {
    int id = 0;
    int fecType = 1, mod = 3, cod = 2;          // PLP_FEC_TYPE, PLP_MOD, PLP_COD
    int type = 2, payloadType = 3, groupId = 0; // normal data PLP carrying a TS
    int tsId = 1, onId = 1;
    std::function<void(uint8_t*)> ts;           // 188-byte packets; empty = the small test stream
};
struct SliceConfig {
    int id = 0;
    int tunePos = 0;                 // multiples of D_X from START_FREQUENCY
    int offsetLeft = 0, offsetRight = 0;
    int type = 0;                    // 0 Type 1 (one PLP, pointer in L1), 1 Type 2 (FECFrame headers)
    int fecHeaderType = 0;           // Type 2: 0 robust QPSK header, 1 high efficiency 16QAM header
    int tiDepth = 0;                 // 0 none, 1 four symbols, 2 eight, 3 sixteen
    std::vector<PlpConfig> plps;
};
struct NotchConfig { int start = 0, width = 0; };   // NOTCH_START / NOTCH_WIDTH in multiples of D_X

struct GenConfig {
    int gi = 1;                      // 0 = 1/128, 1 = 1/64
    int64_t startFrequency = 0;      // K_min, absolute carrier index (multiple of D_X)
    int bandwidth = 0;               // C2_BANDWIDTH: K_max = K_min + bandwidth D_X
    int64_t tuneCarrier = 0;         // the absolute carrier at the centre of the output (the receiver's tuning position)
    int networkId = 0x3001, systemId = 1;
    int l1TiMode = 0;                // L1_TI_MODE 0 .. 3
    std::vector<SliceConfig> slices;
    std::vector<NotchConfig> notches;
    double sampleRate = kSampleRate; // output rate
    double snrDb = 200;              // Es/N0 per data cell; 200 = no noise
    double cfoHz = 0;
    double clockPpm = 0;             // the transmitter's clock fast (positive) or slow against the receiver's
    double echoDb = 0, echoUs = 0, echoDeg = 0;   // one echo: attenuation (0 = none), delay, phase
    double level = 0.22;             // rms of the output
    unsigned seed = 1;
};

// A complete C2 system centred on `tuneCarrier`, 7.61 MHz wide, with one data slice over the whole width (the usual single-channel case)
GenConfig defaultSystem(int gi, int64_t tuneCarrier);

class Generator {
public:
    explicit Generator(const GenConfig& cfg);
    ~Generator();
    const GenConfig& config() const;
    void generate(cf32* out, size_t n);
    const L1Part2& l1() const;                   // the L1 of the frame being generated
    uint64_t frames() const;                     // C2 frames started
    uint64_t packets() const;                    // transport stream packets taken from the sources
    double netBitrate(size_t slice, size_t plp) const;   // bit/s of that PLP's transport stream (its share of the slice)
    struct Impl;
private:
    std::unique_ptr<Impl> p_;
};

} // namespace dvbc2

ModeTuning dvbc2Tuning();
// SynthConfig for the test signal: modeOpt[0] modulation and code rate (0 256-QAM 5/6, 1 16-QAM 4/5, 2 64-QAM 2/3, 3 256-QAM 3/4,
// 4 64-QAM 9/10, 5 256-QAM 9/10), modeOpt[1] guard interval (0 1/64, 1 1/128), modeOpt[2] data slice type (0 Type 1, 1 Type 2 robust
// header, 2 Type 2 high efficiency header), modeOpt[3] time interleaving depth (0 none, 1 four, 2 eight, 3 sixteen symbols),
// modeOpt[4] 1 = a narrowband notch in the slice, modeOpt[5] 1 = 16K FEC frames, modeOpt[6] 1 = two PLPs (Type 2);
// modeVal[0] clock offset ppm; snrDb Es/N0 per cell, cfoHz the carrier offset, echoDb / echoDelay (samples) one echo.
std::unique_ptr<ModeSynth> makeDvbc2Synth(const SynthConfig& cfg, double sampleRate);
std::unique_ptr<ModeSynth> makeDvbc2Synth(const SynthConfig& cfg, double sampleRate, std::function<void(uint8_t*)> ts);
double dvbc2SynthNetBitrate(const SynthConfig& cfg);   // the transport stream bit rate those options carry

} // namespace dect2
