// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inmarsat all-carriers mode, stage 1: finds the carriers of the L-band satellite segment in ONE wide capture and tells the narrow
// ones (P 600 / 1200 bit/s, bare carriers) from the medium ones (Inmarsat-C, about 2 kHz) and the wide ones (P 10500 bit/s, C channels).
// Design: docs/INMARSAT-REVAMP.md. Width alone cannot tell a 10500 bit/s P channel from a C channel: the class only picks which decoder
// is tried first, and the decoders confirm by frame sync.
// Method (the same idea as the Aero receiver's own band search, now for any rate up to 20 Msps): averaged Hann-window spectra spread
// over each second, the median as the noise floor, regions above floor + threshold, and a carrier must show in two searches in a row.
// Not thread-safe: the manager (inmsat_manager.h) serialises the calls.
#pragma once
#include "dect2/common/ring.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2::inmsat {

enum class CarrierClass { Narrow, Medium, Wide };
const char* carrierClassName(CarrierClass c);
CarrierClass classifyWidth(double widthHz);   // < 1.5 kHz narrow, < 4.5 kHz medium, else wide

struct DetectedCarrier {
    int id = 0;
    double offsetHz = 0;       // against the centre of the capture
    double widthHz = 0;       // occupied width above the threshold
    float snrDb = 0;          // peak over the noise floor
    CarrierClass cls = CarrierClass::Narrow;
    bool confirmed = false;   // seen in two searches in a row
    double firstSeen = 0;     // stream seconds
    double lastSeen = 0;
    uint64_t seen = 0;        // searches that found it
};

struct DetectorConfig {
    double rate = 0;              // input sample rate
    double binHz = 250;           // wanted FFT bin width (the real one is the next power of two that gives this or finer)
    double searchSec = 1.0;       // one evaluation per this many seconds
    int slices = 32;              // spectra averaged per evaluation (spread over the period)
    double thresholdDb = 3.0;     // over the median floor
    double holdSec = 8.0;         // a carrier that is not seen any more is dropped after this
    double dcExcludeHz = 2000;    // around 0 Hz: the radio's DC spike
    double edgeFraction = 0.45;   // only this part of +-rate/2 is used (the radio's filter roll-off)
    double maxWidthHz = 40000;    // wider regions are not carriers (a bump, a filter edge)
};

class CarrierDetector {
public:
    CarrierDetector();
    ~CarrierDetector();
    void configure(const DetectorConfig& cfg);
    void reset();
    void feed(const cf32* x, size_t n);
    const std::vector<DetectedCarrier>& carriers() const;   // confirmed and pending ones, strongest first
    uint64_t searches() const;                              // evaluations done so far
    double floorDb() const;                                 // median power of the last evaluation, dB (arbitrary reference)
    double nowSec() const;
    // the last evaluation's smoothed spectrum in dB over the floor: bin 0 is startHz, the step is binHz
    void spectrum(std::vector<float>& dB, double& startHz, double& binHz) const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::inmsat
