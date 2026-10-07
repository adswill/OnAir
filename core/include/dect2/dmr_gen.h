// DMR test signal: the generator the synthetic source plays and dmrtool writes to a file. Nothing is transmitted.
//
// What it makes: the downlink of a two slot base station (a continuous 4FSK carrier at 4800 symbols/s with CACH, idle bursts, group and private
// voice calls with header, six burst superframes with embedded link control and talker alias, and terminator, control blocks, short data text
// messages) or, in direct mode, a handset that transmits now and then in one time slot. The voice bits are random: AMBE+2 is not part of it.
//
// SynthConfig::modeOpt and modeVal carry this mode's options. Zero always means the default.
//   modeOpt[0]  colour code: 0 = 1 (default), 1..15 = that code, -1 = colour code 0
//   modeOpt[1]  talkgroups in use, 1..8 (0 = 3)
//   modeOpt[2]  link: 0 base station (both slots, continuous), 1 direct mode (bursts of single calls, silence between),
//               2 mobile: a handset talking to a repeater (the uplink: as direct mode, with the mobile station sync patterns)
//   modeOpt[3]  traffic: 0 normal, 1 quiet, 2 busy
//   modeOpt[4]  random seed (0 = 1)
//   modeOpt[5]  bit 0: no text messages, bit 1: no talker alias, bit 2: no sync pattern in the voice headers (embedded signalling instead, which
//               older editions of the standard allowed), bit 3: reverse channel signalling in place of the sync pattern in every second data block of
//               a text message (the current standard allows it in data continuation bursts)
//   modeOpt[6]  hang time: terminator bursts a base station repeats after a call, 0 = 6 (a real repeater sends them for seconds), -1 = none
//   modeVal[0]  deviation as a factor of the nominal 1944 Hz of the outer level (0 = 1.0)
//   modeVal[1]  DC offset of the radio as a fraction of full scale (0 = none; the spike a HackRF puts at the centre)
//   modeVal[2]  IQ gain imbalance in dB (a phase error of 3 degrees per dB goes with it)
//   modeVal[3]  unused
// SynthConfig::snrDb is the signal to noise ratio in a 12.5 kHz channel, cfoHz the carrier offset, sroPpm the clock error of the sample clock.
#pragma once
#include "dmr_proto.h"
#include "mode_synth.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct DmrGenConfig {
    double rate = 2.4e6;
    int cc = 1;
    int talkgroups = 3;
    bool direct = false;         // a handset: single calls in one time slot, silence in between
    bool mobile = false;         // with direct: the sync patterns of a handset transmitting to a repeater (MS sourced) instead of direct mode
    int traffic = 0;             // 0 normal, 1 quiet, 2 busy
    uint32_t seed = 1;
    bool textMessages = true, talkerAlias = true;
    int hangBursts = 6;          // base station: terminators repeated after a voice call (hang time)
    bool headerSync = true;      // false: the voice headers carry embedded signalling in the centre instead of a sync pattern
    bool blockSync = true;       // false: every second data block of a message has reverse channel signalling in the centre instead of a sync pattern
    double snrDb = 40;           // in 12.5 kHz
    double cfoHz = 0;
    double sroPpm = 0;
    double devScale = 1.0;
    double dcOffset = 0;
    double iqImbalanceDb = 0;
    double rms = 0.22;           // total power of signal plus noise, as an rms of the complex samples
};

// What the generator has put on the air (for tests)
struct DmrTruth {
    double startSec = 0, endSec = 0;
    int slot = 0;                // 1 or 2 (direct mode: the time slot used)
    int kind = 0;                // as DmrCall::kind: 0 group voice, 1 private voice, 2 all call, 3 data message, 4 control
    uint32_t src = 0, dst = 0;
    int voiceBursts = 0;
    int cc = 0;
    int csbkOpcode = -1;
    int dataRate = 0;            // data type of the blocks of a message
    std::string alias, text;
    bool finished = false;
};

class DmrSignal {
public:
    explicit DmrSignal(const DmrGenConfig& c);
    ~DmrSignal();
    void generate(cf32* out, size_t n);
    const std::vector<DmrTruth>& truth() const;
    double seconds() const;      // signal time generated so far (symbol time)
    // Insert nothing: silence for `secs` seconds of the *symbol* clock (tests of the carrier going away); the carrier is off, noise continues
    void setCarrier(bool on);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

DmrGenConfig dmrGenConfigFrom(const SynthConfig& cfg, double sampleRate);
std::unique_ptr<ModeSynth> makeDmrSynth(const SynthConfig& cfg, double sampleRate);   // nullptr only for a rate below 1 Msps or above 20 Msps

} // namespace dect2
