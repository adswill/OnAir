// AIS test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makeAisSynth(): a small port area as the engine's synthetic source. 15 vessels in and around Jebel Ali (25.0 N, 55.05 E) that move along simple
// tracks and send the messages of ITU-R M.1371-5 at the reporting intervals of table 1 (class A: 2, 6 or 10 s by speed; class B: 30 s), alternating
// between AIS 1 (161.975 MHz) and AIS 2 (162.025 MHz) on a 2250 slot per minute grid, no two bursts on one channel at once. Static and voyage data
// (messages 5 and 24, 19) go out every 30 s instead of every 6 minutes so a screen fills quickly. Also: a base station (message 4 every 10 s, 14 and
// 8 once a minute), aids to navigation (message 21: a lit buoy and a virtual aid), and a search and rescue aircraft (message 9) circling.
// The channels sit at -25 kHz and +25 kHz of the user's frequency (162.000 MHz), the user's frequency itself at -tuneOffsetHz from 0 Hz.
//
// SynthConfig:
//   snrDb       signal to noise ratio of a vessel at level 1 in 48 kHz of bandwidth (a 25 kHz channel sees 2.8 dB more)
//   cfoHz       carrier offset of every transmitter, added to a fixed per-vessel error of up to +-200 Hz
//   sroPpm      bit clock error of every transmitter
//   modeOpt[0]  number of vessels, 1 to 40 (0 = 15); beyond 15 they are made up from the same kinds
//   modeOpt[1]  seed (0 = 1): start times, slots, vessel errors
//   modeOpt[2]  0: levels follow the distance to the receiver (down to -20 dB); 1: all stations at level 1
//   modeOpt[3]  1: vessels only, without the base station, the aids and the aircraft
//   modeVal[0]  speed factor of the vessels' movement (0 = 1): 10 shows a track in a minute
#pragma once
#include "ais_proto.h"
#include "mode_synth.h"
#include <memory>
#include <vector>

namespace dect2 {

std::unique_ptr<ModeSynth> makeAisSynth(const SynthConfig& cfg, double sampleRate);

// What the synthetic port sent (for tests): turn the log on right after makeAisSynth, before the first generate().
struct AisSentBurst { double startSec = 0; char channel = 'A'; ais::Bits payload; };
void aisSynthLog(ModeSynth& s, bool on);
std::vector<AisSentBurst> aisSynthSent(const ModeSynth& s);

// ---- bursts at chosen times, for tests and the tool
struct AisBurstSpec {
    ais::Bits payload;       // message bits without FCS
    char channel = 'A';      // 'A' = AIS 1 (-25 kHz), 'B' = AIS 2 (+25 kHz)
    double startSec = 0;     // when the burst starts
    double level = 1.0;      // amplitude relative to the reference burst
    double cfoHz = 0;        // carrier error of this transmitter
    double phase = 0;        // carrier phase at the start, radians
};

struct AisRenderConfig {
    double rate = 2e6;
    double durationSec = 1;
    double snrDb = 30;       // reference burst power to noise power in 48 kHz; < -100 means no noise
    double cfoHz = 0;        // added to every burst
    double sroPpm = 0;       // bit clock error
    double tuneOffsetHz = 0; // the channels are placed at +-25 kHz - tuneOffsetHz
    double dcOffset = 0;     // added to I and Q
    int quantBits = 0;       // 8: round to 8 bits like a HackRF (0 = not)
    uint32_t seed = 1;
};

// The baseband of the bursts plus noise. The reference burst has an amplitude of 0.3 before the scaling that keeps the noise peaks below 0.9.
std::vector<cf32> renderAisBursts(const std::vector<AisBurstSpec>& bursts, const AisRenderConfig& cfg);

} // namespace dect2
