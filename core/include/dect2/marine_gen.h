// Marine test signal: NAVTEX, DSC (MF/HF and VHF channel 70) or weather fax, as complex baseband at any rate. Nothing is transmitted.
//
// The wanted channel sits at -tuneOffsetHz (20 kHz) from 0 Hz, the carrier / dial frequency at that place; the receiver tunes the
// radio tuneOffsetHz above the user's frequency and mixes it out. The level is set so that the total rms is 0.2, whatever the SNR.
//
// SynthConfig use:
//   snrDb    signal to noise ratio in a 3 kHz band, or in 12.5 kHz for the VHF channel (noise is added over the whole sample rate), 30 default
//   cfoHz    carrier offset: the whole signal moves by this many Hz (a real radio: +-10 ppm of the carrier)
//   sroPpm   sample clock error (fax slant)
//   modeOpt[0]  service: 0 or 1 NAVTEX (SITOR-B, 100 bd, 170 Hz) station A with three messages in a loop (navigational warning, gale
//               warning, forecast), 2 DSC MF/HF (100 bd, 170 Hz): distress alert with position, all-ships safety call, individual call,
//               distress acknowledgement, 3 weather fax (USB, 1500/2300 Hz) a synthetic chart, 4 DSC VHF channel 70 (FM, AFSK 1300/2100 Hz, 1200 bd)
//   modeOpt[1]  HF fading: 0 none, 1 two-path channel (1 ms delay, 0.5 Hz Doppler spread, Rayleigh paths of equal power)
//   modeOpt[2]  NAVTEX: 0 the three messages in a loop, 1 / 2 / 3 only that one, 4 a short test message (about 8 s)
//   modeOpt[3]  fax lines per minute (0 = 120)
//   modeOpt[4]  fax IOC (0 = 576, or 288)
//   modeOpt[5]  fax picture lines (0 = 800)
//   modeVal[0]  mistuning in Hz: the signal (all services) is shifted by this much on top of cfoHz
//   modeVal[1]  NAVTEX idle seconds before each message (0 = 6)
//   modeVal[2]  fax phasing seconds (0 = 30)
#pragma once
#include "mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeMarineSynth(const SynthConfig& cfg, double sampleRate);
// The same with the channel at an explicit place in the output (a recording is made with 0)
std::unique_ptr<ModeSynth> makeMarineSynthAt(const SynthConfig& cfg, double sampleRate, double channelOffsetHz);

} // namespace dect2
