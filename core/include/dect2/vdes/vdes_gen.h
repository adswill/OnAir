// VDES test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// The signal sits around the mode's default dial frequency (161.9 MHz): the two ASM channels (161.950 and 162.000 MHz, GMSK 9.6 kbit/s with the AIS
// framing) at +50 and +100 kHz, and the 100 kHz VDE-TER channel of 161.8 MHz at -100 kHz with bursts of 25, 50 and 100 kHz (pi/4-QPSK, 8PSK and 16QAM,
// root-raised-cosine with roll-off 0.3, 19.2 kbaud per 25 kHz of channel). makeVdesSynth() plays a cycle of both in turn; renderVdes() puts chosen
// bursts at chosen times for tests and the tool.
//
// SynthConfig: snrDb = a 25 kHz VDE-TER burst's power to the noise in 25 kHz (an ASM burst at the same level sees 2.8 dB less in its 48 kHz);
// cfoHz = carrier offset of every transmitter; sroPpm = symbol clock error; modeOpt[0] = 0 ASM and VDE-TER, 1 ASM only, 2 VDE-TER only.
#pragma once
#include "dect2/ais/ais_gen.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <vector>

namespace dect2 {

constexpr double kVdeSymRate25 = 19200.0;      // baud in a 25 kHz channel (x2 for 50 kHz, x4 for 100 kHz)
constexpr double kVdeRolloff = 0.3;

struct VdeBurstSpec {
    int band = 1;               // 0: 157.2 MHz, 1: 161.8 MHz
    int firstSub = 0;           // first 25 kHz sub-channel of the burst, 0 .. 3
    int nSub = 1;               // 1, 2 or 4 sub-channels (25, 50, 100 kHz)
    int mod = 1;                // VdeMod: 1 pi/4-QPSK, 2 8PSK, 3 16QAM
    double startSec = 0;
    double durSec = 0.02;
    double level = 1.0;         // amplitude relative to the reference (power density equal in every sub-channel)
    double cfoHz = 0;           // carrier error of this transmitter
    uint32_t seed = 1;          // the symbols
};

struct VdesRenderConfig {
    double rate = 1e6;
    double durationSec = 1;
    double dialHz = 161.9e6;    // the frequency at the centre of the samples
    double snrDb = 30;          // reference 25 kHz burst power to noise power in 25 kHz; < -100 means no noise
    double cfoHz = 0;           // added to every burst
    double sroPpm = 0;          // symbol clock error
    uint32_t seed = 1;
};

// ASM bursts (channel 'A' = 161.950, 'B' = 162.000 MHz, the payload without FCS) and VDE-TER bursts in one baseband
std::vector<cf32> renderVdes(const std::vector<AisBurstSpec>& asmBursts, const std::vector<VdeBurstSpec>& vdeBursts, const VdesRenderConfig& cfg);
std::unique_ptr<ModeSynth> makeVdesSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
