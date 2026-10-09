// Pagers test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// makePagerSynth(): an endless loop of POCSAG and FLEX transmissions on one 25 kHz channel, FM modulated. One cycle sends, with half a second
// of silence between the transmissions: POCSAG 512, 1200 and 2400 baud (numeric, alphanumeric and, at 1200, tone-only pages) and one FLEX frame
// each at 1600/2, 3200/2, 3200/4 and 6400/4 (four pages per frame: alphanumeric, numeric, tone-only and a second alphanumeric, spread over the
// phases). pagerTestMessages() lists what a cycle carries, with obviously fake content. A cycle takes about 16 s.
//
// The carrier sits at the tune offset of the mode (pagerTuning().tuneOffsetHz below the radio's centre) plus SynthConfig::cfoHz. The signal to
// noise ratio (snrDb) is carrier power over noise power in 25 kHz.
//
// SynthConfig::modeOpt:
//   [0]  0 the mix, 1 POCSAG only, 2 FLEX only
//   [1]  1 inverted polarity (the frequency shift of every symbol reversed, as some transmitters do)
//   [2]  0 every speed, otherwise only the speed PagerSpeed + 1 (1 = POCSAG 512 ... 7 = FLEX 6400/4)
//   [3]  noise seed, 0 = 1
#pragma once
#include "mode_synth.h"
#include "pager_tel.h"
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct PagerTestMessage {
    int speed = 0;               // PagerSpeed
    bool flex = false;
    uint32_t address = 0;
    int function = -1;           // POCSAG function, -1 for FLEX
    int type = kPagerTone;       // PagerMsgType
    std::string text;
};

// The pages one cycle carries for the options above (modeOpt[0] and modeOpt[2])
std::vector<PagerTestMessage> pagerTestMessages(int onlyKind = 0, int onlySpeed = 0);

// Duration of one cycle in seconds for the options above
double pagerCycleSeconds(int onlyKind = 0, int onlySpeed = 0);

std::unique_ptr<ModeSynth> makePagerSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
