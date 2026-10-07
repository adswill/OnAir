// ADS-B simulation harness: transmissions from the generator (or a list of frames at chosen times) -> channel impairments -> receiver, with the
// list of what was sent kept so that every frame can be checked off. The tests and adsbtool use it.
#pragma once
#include "adsb_gen.h"
#include "adsb_rx.h"
#include <vector>

namespace dect2 {

struct AdsbSimConfig {
    double rate = 2e6;
    double seconds = 10;
    double snrDb = 30;                  // pulse-to-noise ratio of an aircraft at 50 NM, noise in 2 MHz (as SynthConfig::snrDb)
    int aircraft = 12;
    uint32_t seed = 1;
    double mult = 1;
    double filterMHz = 0;               // analogue filter of the radio, 0 = automatic
    double cfoHz = 0, sroPpm = 0;
    bool replies = true;
    double refLat = 25.25, refLon = 55.36;
    bool quantise = true;               // round to 8 bits like a HackRF
    double dcOffset = 0;                // added to I and Q, fraction of full scale
    double iqGainDb = 0, iqPhaseDeg = 0;   // imbalance of the Q branch
    std::vector<size_t> chunks = {65536};  // block sizes handed to feed(), repeated in turn
    double gapAtSec = -1, gapMs = 0;    // a stretch of zero samples (the radio dropped data)
    double resetAtSec = -1;             // call reset() here
    double gainStepAtSec = -1, gainStepDb = 0;   // the level of everything (signal and noise, as an AGC step or a gain change) changes here
    double clipFactor = 1;              // everything multiplied by this before the converter, which clips at full scale (strong signals)
    double cwDb = -999, cwHz = 0;       // a carrier next to the signal, its power relative to the pulse power of an aircraft at 50 NM (-999 = none)
    std::vector<AdsbTx> custom;         // when not empty: these transmissions instead of the airspace
    float custAmpToSnr = 0;
    int fixBits = 1;
    float kPulse = 0, kGap = 0;         // preamble thresholds (0 = the receiver's own)
    bool setRef = false;                // give the receiver the reference position
    double tailSec = 0.01;              // silence after the last transmission, so that the last frame is looked at
};

struct AdsbSimFrame {                   // one transmission and what happened to it
    double t = 0;
    std::string hex;
    uint32_t icao = 0;
    int df = 0;
    float amp = 0;
    float snrDb = 0;                    // pulse-to-noise ratio in 2 MHz
    bool overlapped = false;            // another burst was on the air during part of it
    bool decoded = false;
    float levelDbfs = 0;                // what the receiver measured, when decoded
    float rxSnrDb = 0;
    int corrected = 0;
};

struct AdsbSimResult {
    std::vector<AdsbSimFrame> sent;
    size_t decoded = 0;                 // transmissions found
    size_t phantom = 0;                 // good messages that match no transmission
    std::vector<std::string> phantoms;
    AdsbTelemetry tel;                  // the last report
    double cpuSec = 0;                  // CPU time spent in feed()
    double signalSec = 0;
    double noiseSigma = 0;
};

AdsbSimResult adsbSimulate(const AdsbSimConfig& cfg, AdsbReceiver& rx);
// the detection rate of the frames in an SNR range (frames not overlapped by another burst unless includeOverlapped)
double adsbDetectionRate(const AdsbSimResult& r, double snrLo, double snrHi, bool includeOverlapped, size_t* count = nullptr);

} // namespace dect2
