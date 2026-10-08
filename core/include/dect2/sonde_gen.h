// Radiosonde test signal: several sondes at once in one captured band, centred on 403.0 MHz (the app's default frequency), each sending
// frames the way its type does (checks, FEC and calibration as a real one; the content is made up).
//
// Default (three sondes), flying from Dubai:
//   0  RS41-SG   403.000 MHz  25.20 N 55.36 E, 12 km, climbing 5 m/s, calibration subframes valid (51 frames to complete)
//   1  DFM-17    402.200 MHz  after burst, descending 15 m/s from 22 km
//   2  M10       404.100 MHz  climbing 5 m/s from 8 km
// More entries for modeOpt[1] > 3: 3 = RS41 at +450 kHz (modeVal[2] changes the offset), 4 = M20 at 401.6 MHz, 5 = DFM-09 at 405.0 MHz,
// 6 = RS92 at 405.3 MHz (only with bit 4 of the mask).
// Sondes outside +-45 % of the sample rate are left out (at 2 Msps: the RS41, and the DFM at -800 kHz).
//
// Options (SynthConfig):
//   modeOpt[0]  type mask: bit 0 RS41, bit 1 DFM, bit 2 M10, bit 3 M20, bit 4 RS92 (0 = RS41, DFM and M10)
//   modeOpt[1]  number of sondes, 1 to 7, taken in the order above among the types in the mask (0 = three)
//   modeOpt[2]  seed (0 = 1)
//   modeVal[0]  drift to the east in m/s (default 10; 0 is used as 10: pass a tiny value for none)
//   modeVal[1]  carrier drift in Hz per minute, applied to every sonde (default 0: the sonde TCXO and the radio error are tested with cfoHz)
//   modeVal[2]  offset in Hz of the second RS41 (entry 3) from the first, default 450000
// snrDb is the signal to noise ratio of every sonde in 10 kHz of bandwidth; cfoHz shifts everything; sroPpm changes the clocks.
// The signal is a burst per frame (RS41: 0.6 s of preamble and frame per second) with the carrier off in between, a Gaussian-like
// frequency shaping, and deviations of 2.4 kHz (RS41, DFM) and 4.8 kHz (M10, M20): assumed values, not measured on the air.
#pragma once
#include "mode_synth.h"
#include <memory>

namespace dect2 {

std::unique_ptr<ModeSynth> makeSondeSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
