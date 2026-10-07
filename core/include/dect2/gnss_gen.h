// GNSS test signal: a simulated sky (gnss_sim.h) rendered as complex baseband. Nothing is transmitted; the samples only exist in memory or in a file.
// The satellites are synthetic: orbits with the shape of real GPS orbits, valid navigation messages (parity, TOW, ephemeris, almanac, ionosphere and
// UTC parameters), range and Doppler from the geometry to a fixed receiver, ionosphere and troposphere delays, a satellite clock error, 8 bit friendly levels.
// They are not the satellites of today.
//
// SynthConfig options (makeGnssSynth):
//   cfoHz       the receiver oscillator's error as seen at L1: every signal appears this many Hz higher, and the sample clock is off by the same fraction
//   sroPpm      extra sample clock error
//   modeOpt[0]  systems mask (bit 0 GPS L1 C/A; the others are not simulated yet), 0 = GPS
//   modeOpt[1]  number of satellites, 0 = all above the mask (8 to 11), otherwise the highest ones
//   modeOpt[2]  0 = warm start: the stream begins 22 s into a navigation frame, so subframe 1 starts after 8 s and the ephemeris is complete after 26 s; 1 = cold: begin 13 s into the frame (35 s)
//   modeOpt[3]  bit 0: a continuous-wave jammer at +400 kHz (amplitude 0.3); bit 1: a DC offset of 0.02 in I and Q (the radio's spike)
//   modeOpt[4]  random seed for the orbits, 0 = 1
//   modeVal[0], modeVal[1], modeVal[2]  latitude, longitude (degrees) and height (m) of the receiver; both angles 0 = Dubai, 25.2 N 55.36 E, 10 m
//   modeVal[3]  C/N0 of a satellite at the zenith in dB-Hz, 0 = 44 (satellites lower in the sky are weaker: about 35 dB-Hz at 5 degrees)
// snrDb is not used: the signal level is set by modeVal[3].
#pragma once
#include "gnss_sim.h"
#include "mode_synth.h"
#include <memory>

namespace dect2 {

GnssSimConfig gnssSimConfigFrom(const SynthConfig& cfg);
std::unique_ptr<ModeSynth> makeGnssSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
