// Shortwave test signal (makeShortwaveSynth): a slice of an HF band, so the whole mode can be tried and tested without a radio.
// Nothing is transmitted; the samples only exist in memory or in a file. Positions are from the dial frequency D (the radio sits
// tuneOffsetHz above it, so D is at -tuneOffsetHz in the samples):
//
//   D + 25.000 kHz   an AM broadcast station: carrier, 1 kHz tone at 60 %
//   D + 51.275/.445  an RTTY transmission (upper sideband, mark and space 170 Hz apart, centre D + 51.360 kHz) sending kRttyTestText
//   D + 51.360 kHz   a CW carrier keying "CQ CQ DE ONAIR ONAIR K" at 20 WPM, exactly on the RTTY centre (a parallel-decoder test)
//   D + 53.000 kHz   a BPSK31 signal
//
// SynthConfig: snrDb of the RTTY signal in 3 kHz (default 30); cfoHz shifts the whole slice;
//   modeOpt[0]  which signals: 0 all four, 1 AM only, 2 RTTY only, 3 CW only, 4 PSK31 only, 5 noise only
//   modeVal[0]  Hz added to the RTTY / CW / PSK positions (a station "elsewhere in the band"; 0: as above)
#pragma once
#include "dect2/engine/mode_synth.h"
#include <memory>

namespace dect2 {

constexpr double kSwSynthAmHz = 25000, kSwSynthRttyCentreHz = 51360, kSwSynthPskHz = 53000;
constexpr double kSwSynthRttyLowHz = 1275;     // the lower tone of the RTTY audio (the upper sideband: D + 50.000 kHz + this)

std::unique_ptr<ModeSynth> makeShortwaveSynth(const SynthConfig& cfg, double sampleRate);

}  // namespace dect2
