// Mercury test signal. Nothing is transmitted: the samples only exist in memory or in a file (the test source, the TX studio's IQ
// file, the tests). The modulator is OnAir's own (mercury_phy.h), built from Mercury's mode definitions; its output is the upper
// sideband a radio carrying Mercury's audio sends.
//
// A script of bursts: a CQ, a CALL and its ACCEPT, carousel rounds both ways (DATA on DATAC3, DATAC15, DATAC17 and QAM16C2, POLLs
// on DATAC16, all with the session's seeded CRC), the DISCONNECT, a broadcast message and a broadcast file (RaptorQ, with source
// symbols left out so that repair symbols are needed). Callsigns LY2ONA (caller) and LY1HRM (callee). Through a channel model:
// noise (SNR in 3 kHz), frequency offset, sample clock offset, and the HF fading channels (Watterson, two equal paths with Gaussian
// Doppler spectra: CCIR good 0.5 ms / 0.1 Hz, moderate 1 ms / 0.5 Hz, poor 2 ms / 1 Hz; ITU-R F.1487 low latitude moderate
// 2 ms / 1.5 Hz; flutter 0.5 ms / 10 Hz).
//
// SynthConfig (makeMercurySynth):
//   snrDb       SNR in 3 kHz; >= 99: no noise
//   cfoHz       the radio's tuning error
//   sroPpm      the sender's sample clock error
//   modeOpt[0]  0 the whole script, 1 broadcast only, 2 one mode over and over (modeOpt[1]: OfdmModeId, MFSK included), 3 every mode
//               once, 4 a link at the MFSK floor (CALL, ACCEPT, a data round, a control frame behind the floor mark, DISCONNECT)
//   modeOpt[2]  channel: 0 none, 1 CCIR good, 2 CCIR moderate, 3 CCIR poor, 4 ITU-R low latitude moderate, 5 flutter
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/mercury/mercury_phy.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace mercury {

struct GenBurst {
    int mode = kDatac16;
    std::vector<uint8_t> frame;   // as makeFrame() gives it (payload and CRC)
    double gapSec = 1.0;          // silence after it
    double cfoHz = 0;             // this sender's own frequency error (two radios of a link differ)
};

// the scripts (see above)
std::vector<GenBurst> demoScript(int which = 0, int singleMode = kDatac3);
// the bytes the demo session's caller and callee send, and the broadcast file (tests compare what the monitor reassembles)
std::string demoCallerText();
std::string demoCalleeText();
std::string demoFloorText();   // the caller's data of the floor link (script 4)
std::string demoFileName();
std::vector<uint8_t> demoFileData();

enum Fading { kFadeNone = 0, kCcirGood, kCcirModerate, kCcirPoor, kItuLowModerate, kFlutter, kNumFadings };
const char* fadingName(int f);

struct ChannelConfig {
    double snrDb = 99;            // in 3 kHz; >= 99: no noise
    double cfoHz = 0;
    double sroPpm = 0;            // the sender's clock: its 8000 Hz is 8000 * (1 + ppm 1e-6)
    int fading = kFadeNone;
    uint64_t seed = 1;
    double refPower = 0;          // the signal power the SNR is measured against (0: from the signal itself)
};
// The script as 8 kHz complex samples of the sideband (the audio's analytic signal, carriers at their audio frequencies), through the channel
std::vector<cf> renderScript(const std::vector<GenBurst>& script, const ChannelConfig& ch, double leadSec = 0.5);
// One channel applied to analytic 8 kHz samples, in place (the same models as renderScript)
void applyChannel(std::vector<cf>& x, const ChannelConfig& ch);

} // namespace mercury

std::unique_ptr<ModeSynth> makeMercurySynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
