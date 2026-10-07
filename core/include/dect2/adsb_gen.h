// ADS-B test signal: a simulated airspace. Aircraft fly great-circle tracks around a reference point and send the Mode S / ADS-B message mix at its
// real rates; every message is rendered as 1 us pulse-position modulation (8 us preamble, 56 or 112 bits) at any sample rate from 2 Msps up,
// with overlapping bursts, a carrier frequency error and phase of its own for each transmitter, a level that falls with distance, and noise.
// Nothing is transmitted: the samples only exist in memory or in a file.
//
// SynthConfig options (makeAdsbSynth):
//   snrDb       pulse-to-noise ratio of an aircraft 50 NM away, noise measured in 2 MHz (default 30). Level goes with 1/distance, so a nearby aircraft
//               is 14 dB above that figure at 10 NM and one at 200 NM is 12 dB below it.
//   cfoHz       frequency error of the whole capture, added to each transmitter's own (+-100 kHz)
//   sroPpm      sample clock error
//   modeOpt[0]  number of aircraft, 0 = 12 (at most 300), -1 = none (noise only)
//   modeOpt[1]  1 = ADS-B and DF11 only; 0 = also the replies to a secondary radar (DF0, 4, 5, 16, 20, 21 with Comm-B 2,0 / 4,0 / 5,0 / 6,0)
//   modeOpt[2]  random seed, 0 = 1
//   modeOpt[3]  1 = one aircraft squawks 7700 and sends the emergency status (for a look at the alert)
//   modeVal[0]  message rate multiplier, 0 = 1 (0.1 to 20)
//   modeVal[1]  width of the radio's analogue filter in MHz, 0 = automatic (0.875 x the sample rate, between 1.75 and 5)
//   modeVal[2], modeVal[3]  latitude and longitude of the reference point (both 0 = 25.25 N 55.36 E)
// The first aircraft is on the ground, taxiing near the reference point, when there are four or more.
// Rates: position 2 per second in total (even and odd alternate; DO-260B), velocity 2, identification every 5 s, operational status every 2.5 s,
// target state every 1.25 s on half of the aircraft, DF11 replies about once per 2 s, radar replies about 1 per second together.
#pragma once
#include "adsb_encode.h"
#include "mode_synth.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {

// One transmission
struct AdsbTx {
    double t = 0;                // start of the preamble, seconds of stream time
    adsb::Frame frame;
    float amp = 0.1f;            // pulse amplitude at the receiver (1 = full scale)
    double cfoHz = 0;            // this transmitter's carrier offset
    float phase = 0;             // carrier phase at the start of the burst, radians
    std::vector<std::pair<double, double>> pulses;   // when not empty: these pulses (start and end in us after t) instead of the frame: Mode A / C replies and other interference
};

// Renders transmissions as complex baseband at any sample rate. The pulses are 0.5 us rectangles with a 25 ns Gaussian edge (the transmitter), passed
// through a third-order Butterworth low-pass whose cutoff is half the radio's filter width (the HackRF's filter settings are widths, 1.75 MHz
// and up), and sampled at the instants of the sample clock without any further filtering, as the HackRF does. The sampled shape therefore depends
// on where the burst falls between two samples, and at 2 Msps the samples are aliased a little, as on the real radio.
class AdsbMixer {
public:
    AdsbMixer(double rate, double filterMHz = 0, double sroPpm = 0);
    void add(const AdsbTx& tx);                    // may start before the samples rendered so far only if it is still ahead of them
    void render(cf32* out, size_t n);              // the next n samples: the sum of the bursts (no noise)
    double time() const { return (double)pos_ / fs_; }      // stream time of the next sample
    double rate() const { return rate_; }
    size_t pending() const { return bursts_.size(); }
private:
    struct Burst {
        int64_t first = 0;                         // index of the first sample
        std::vector<float> env;                    // envelope per sample
        float amp = 0;
        cf32 z, w;                                 // carrier phasor now and per sample
        size_t at = 0;                             // samples rendered so far
    };
    double rate_, fs_;
    int64_t pos_ = 0;
    std::vector<Burst> bursts_;
    std::vector<double> step_;                     // step response of the radio, from -umax to +umax microseconds
    double du_ = 0.005, umax_ = 8.0;
};

// The radio's noise: white Gaussian noise through the same third-order low-pass as the signal (when that low-pass is narrower than the sample rate allows
// to leave out), at a level that gives an aircraft of pulse amplitude `ampRef` the signal-to-noise ratio `snrDb` in 2 MHz.
class AdsbNoise {
public:
    AdsbNoise(double rate, double filterMHz, double snrDb, double ampRef, uint64_t seed);
    void setSnr(double snrDb);
    void add(cf32* x, size_t n);       // adds noise to x
    double sigmaPerComponent() const { return sigma_; }
private:
    double rate_, ampRef_, gain_ = 1;  // gain_: power of the filtered noise per unit of white variance
    bool filtered_ = false;
    float sigma_ = 0;
    // two sections: first order, then second order, applied to I and Q
    double b1_[2] = {}, a1_ = 0, b2_[3] = {}, a2_[2] = {};
    double s1_[2] = {}, s2_[2][2] = {};
    uint64_t rng_[2];
    uint64_t next();
};

struct AdsbAirspaceConfig {
    int aircraft = 12;
    uint32_t seed = 1;
    double refLat = 25.25, refLon = 55.36;
    double rateMultiplier = 1.0;
    bool replies = true;                           // secondary radar replies (DF0, 4, 5, 16, 20, 21)
    bool emergencyDemo = false;
    double a50 = 0.1;                              // pulse amplitude of an aircraft at 50 NM
};

class AdsbAirspace {
public:
    explicit AdsbAirspace(const AdsbAirspaceConfig& c);
    ~AdsbAirspace();
    // Every transmission that starts before untilSec, in order of time. The aircraft keep flying.
    void run(double untilSec, std::vector<AdsbTx>& out);
    struct Info { uint32_t icao; std::string callsign; double lat, lon, altFt, gsKt, trackDeg, distNm; bool ground; int squawk; double iasKt; bool airspeedMessage; };
    std::vector<Info> aircraft() const;            // state at the time of the last run
    void setRateMultiplier(double m);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeAdsbSynth(const SynthConfig& cfg, double sampleRate);   // nullptr: sample rate below 2 Msps

} // namespace dect2
