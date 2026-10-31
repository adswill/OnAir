// ATIS test signal: narrow FM on a VHF marine channel, a voice-like transmission (a few audio tones) and, when it ends, the ATIS burst
// (dot pattern, DSC phasing, format 121, identity, check symbol; atis_proto.h) as 1300 / 2100 Hz AFSK at 1200 baud. As on the air the
// tones are on a phase-modulated carrier (modulation index 1.0), so the 2100 Hz tone has more deviation than the 1300 Hz one; phaseMod = false
// gives flat FM. Nothing is transmitted: the samples only exist in memory or in a file.
// SynthConfig (makeAtisSynth): snrDb is the carrier to noise ratio in 12.5 kHz, cfoHz the carrier offset, modeOpt[0] 1 = flat FM instead of
// phase modulation, modeOpt[1] 1 = no voice before the burst, modeOpt[2] 1 = the dot pattern is left out, modeOpt[3] = clock error in ppm.
#pragma once
#include "dect2/engine/mode_synth.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AtisGenConfig {
    double rate = 1e6;
    double dialOffsetHz = 0;         // where the channel sits in the samples
    double cfoHz = 0;
    double snrDb = 99;               // carrier to noise in 12.5 kHz; >= 99: no noise
    double amp = 0.4;
    bool phaseMod = true;
    double modIndex = 1.0;           // radians of phase deviation for a full-scale tone (phase modulation) or kHz/1 (flat FM: deviation = modIndex * 1.7 kHz)
    double voiceSec = 1.2;           // 0: no voice, the burst follows a silent carrier
    double gapSec = 1.0;             // no carrier between transmissions
    double afterVoiceSec = 0.04;     // silent carrier between the voice and the dot pattern
    int dotBits = 20;
    double clockPpm = 0;
    bool silent = false;             // never transmit: noise only (negative tests)
    std::vector<std::string> ids;    // ten digits each, sent one per transmission, then the list starts again
    uint32_t seed = 1;
};

class AtisGenerator {
public:
    explicit AtisGenerator(const AtisGenConfig& c);
    ~AtisGenerator();
    void generate(cf32* out, size_t n);
    // the whole transmission of one identity (voice, burst) without noise and without the gap, for tests
    static std::vector<cf32> transmission(const std::string& id, const AtisGenConfig& c);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::vector<std::string> atisTestIds();
std::unique_ptr<ModeSynth> makeAtisSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
