// Iridium test signal: the satellites of a simple Iridium constellation model (66 satellites, 6 planes, 780 km, 86.4 degrees) seen from
// Dubai, each sending ring alerts (IRA) on the simplex channels, broadcast (IBC) and sync (ISY) bursts on its broadcast channel, pager
// messages (MSG), ACARS in short burst data (IDA) and voice-like traffic bursts, with the Doppler shift and Doppler rate of its pass. The frames come from the frame
// layer's builders (iridium_frame.h); the bursts are DQPSK at 25 ksym/s with a root raised cosine (0.4) as gr-iridium expects.
//
// SynthConfig use:
//   snrDb    Es/N0 of a burst from a satellite at the zenith (lower satellites a few dB less)
//   cfoHz    carrier offset of the receiving radio (added to every burst)
//   sroPpm   sample clock offset of the receiving radio
//   modeOpt[0]  number of satellites sending at once, 1 to 4 (0 = 3): the highest ones above 1 degree
//   modeOpt[1]  seed (data, traffic and the start time in the constellation)
//   modeOpt[2]  1 = no voice-like traffic bursts
//   modeOpt[3]  1 = no Doppler (every burst exactly on its channel; for tests)
//   modeOpt[4]  1 = no ACARS messages in short burst data (IDA frames; Phase B, the SBD layout as iridium_sbd.h reads it)
//   modeVal[0]  C/N0 in dB-Hz of a zenith burst; 0 = use snrDb (C/N0 = Es/N0 + 44 dB at 25 ksym/s)
//   modeVal[1]  centre frequency of the band the output covers, MHz; 0 = 1622 at 9.5 Msps and more, else 1626.25 (the simplex channels)
//   modeVal[2]  seconds into the pass at the start (added to the start time the seed picks, and to the Iridium time)
#pragma once
#include "mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

std::unique_ptr<ModeSynth> makeIridiumSynth(const SynthConfig& cfg, double sampleRate);

// One burst for tests and the generator.
struct IridiumTestBurst {
    std::vector<uint8_t> bits;       // after the unique word (iridium_frame.h convention)
    bool downlink = true;
    int preamble = 16;               // symbols: 16 (traffic, broadcast) or 64 (simplex channels)
    double startSec = 0;             // time of the first preamble symbol
    double freqHz = 0;               // centre relative to 0 Hz of the output
    double dopplerRate = 0;          // Hz/s during the burst
    float amplitude = 0.1f;          // rms of the burst
    double phase = 0;                // carrier phase at the start, radians
};

// Renders bursts into a buffer (adds to it). Sample i of out is at time t0 + i / (rate (1 + sroPpm 1e-6)).
void iridiumRenderBursts(const std::vector<IridiumTestBurst>& bursts, double rate, double t0, cf32* out, size_t n, double sroPpm = 0);

// Plays a list of bursts as an endless stream (shapes each burst shortly before it starts, forgets it after it ends).
class IridiumBurstPlayer {
public:
    explicit IridiumBurstPlayer(double rate, double sroPpm = 0);
    ~IridiumBurstPlayer();
    void add(const IridiumTestBurst& b);       // any order
    void render(cf32* out, size_t n);          // adds the bursts of the next n samples to out
    double time() const;                       // time of the next sample
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// The constellation model as the generator uses it (for tests and the tool).
struct IridiumSkySat {
    int id = 0;                      // the satellite number the frames carry (0..127)
    double lat = 0, lon = 0, altKm = 0;   // sub-point (geocentric) and height
    double elevDeg = 0, azDeg = 0, rangeKm = 0;
    double dopplerHz = 0, dopplerRate = 0;   // at 1626 MHz
};
// All satellites above minElevDeg seen from Dubai at time t (seconds of the model), highest first.
std::vector<IridiumSkySat> iridiumSky(double t, double minElevDeg = 5, double carrierHz = 1626e6);
// The start time in the model the generator uses for this seed and number of satellites.
double iridiumSkyStart(uint32_t seed, int nSats);

constexpr double kIridiumSynthEpoch = 1791374400.0;   // 2026-10-07 12:00:00 UTC: Iridium time of the generator's first sample

} // namespace dect2
