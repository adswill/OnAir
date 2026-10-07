// Analog TV test signal: a live test card (colour bars, grey ramp, multiburst, a moving box, a running clock, the text ONAIR TEST) as composite
// video with the timing and levels of the chosen system (BT.470-6), amplitude modulated with a negative vision carrier through a
// vestigial-sideband filter, plus an FM sound carrier that plays a 1 kHz tone with short interruptions or a melody.
// Nothing is transmitted: the samples exist in memory or in a file.
//
// What SynthConfig carries for this mode (engine standard code 10):
//   snrDb      carrier-to-noise ratio: the peak (sync tip) vision carrier against the noise in 5 MHz; 100 and above = no noise
//   cfoHz      the whole signal is shifted by this much (the receiver has to find the carrier)
//   sroPpm     sample clock error of the "radio"
//   echoDb     0 = no ghost, otherwise a ghost this many dB below the signal; delay modeVal[2] us (default 1.5), carrier phase modeVal[3] degrees
//   modeOpt[0] system: 0 B/G (8 MHz channel), 1 B (7 MHz), 2 I, 3 D/K, 4 M, 5 N
//   modeOpt[1] colour: 0 the usual one for the system (PAL, NTSC for M), 1 PAL, 2 NTSC, 3 SECAM, 4 none (monochrome)
//   modeOpt[2] picture: 0 test card, 1 colour bars, 2 grey ramp
//   modeOpt[3] sound: 0 1 kHz tone with short interruptions, 1 melody, 2 unmodulated carrier, 3 no sound carrier, 4 continuous 1 kHz tone
//   modeOpt[4] hum: 0 off, 1 50 Hz, 2 60 Hz (level modeVal[0] percent of the carrier, default 4)
//   modeOpt[5] 1 = no group-delay pre-correction of the colour (BT.470 Table 3 item 14: 170 ns)
//   modeOpt[6] 1 = the transmitter already has the Nyquist slope (a modulator with a SAW filter) instead of a flat vestigial region
//   modeOpt[7] 1 = no black-level setup in the 525-line systems (Japan); with SECAM instead: how the lines are identified, 0 both (the subcarrier at its rest
//              frequency on the back porch of every line, and the 9 lines of field identification signals), 1 the back porch only, 2 the field lines only
//   modeVal[1] sync compression in percent: the sync pulses lose this share of their height
#pragma once
#include "mode_synth.h"
#include "atv_std.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {

std::unique_ptr<ModeSynth> makeAtvSynth(const SynthConfig& cfg, double sampleRate);   // nullptr: unsupported combination

struct AtvGenConfig {
    int sys = kAtvG;
    int colour = kAtvPal;
    double rate = 10e6;           // output sample rate
    double cnrDb = 40;            // 100 and above: no noise
    double cfoHz = 0;
    double sroPpm = 0;
    double echoDb = 0, echoDelayUs = 1.5, echoPhaseDeg = 0;
    double syncCompression = 0;   // 0 .. 0.9
    double humPct = 0, humHz = 50;
    int sound = 0;                // see above
    int pattern = 0;              // 0 card, 1 bars, 2 ramp
    bool groupDelay = true;
    bool nyquistTx = false;
    bool setup = true;            // 7.5 IRE black level in M
    int secamIdent = 0;           // SECAM: 0 lead-in on the lines and bottles in the field blanking, 1 lead-in only, 2 bottles only
    double level = 0.4;           // peak carrier amplitude (the complex samples then have an rms of about 0.2)
    double startSec = 0;          // time shown by the clock at the start
    uint64_t seed = 1;
};

class AtvCard;

class AtvGenerator {
public:
    explicit AtvGenerator(const AtvGenConfig& c);
    ~AtvGenerator();
    bool ok() const;
    const AtvFormat& format() const;
    const AtvGenConfig& config() const;
    void generate(cf32* out, size_t n);                 // the next n samples at config().rate
    void generate(size_t n, std::vector<cf32>& out);    // appends
    // The things that may change while it plays (noise, offsets, ghost, hum, compression, sound); false if something else differs
    bool update(const AtvGenConfig& c);
    // The composite video before modulation, at kInternalRate, as it is made (tests); the tap sees every sample once
    static constexpr double kInternalRate = 12e6;
    void setCompositeTap(std::function<void(const float*, size_t)> tap);
    const AtvCard& card() const;
    // the instant (seconds) of the next output sample, and the frame that was being sent then
    double timeSec() const;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
