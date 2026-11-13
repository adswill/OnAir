// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Orbcomm test transmitter: one or two subscriber downlinks (orbcomm.h framing, SDPSK, RRC 0.4) with Doppler (fixed or a ramp),
// symbol clock offset, noise and packets with a broken checksum.
#pragma once
#include "orbcomm.h"
#include "dect2/common/ring.h"
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace dect2::orbcomm {

struct GenChannel {
    double offsetHz = 0;         // nominal carrier against the centre
    int satId = 23;
    double dopplerHz = 0;        // fixed Doppler, or the start of the ramp
    double dopplerEndHz = 0;     // ramp end (used when rampSec > 0): a triangle start -> end -> start, one leg per rampSec
    double rampSec = 0;
    double raanDeg = 0, phaseDeg = 0;   // the satellite's orbit (circular, 775 km, 45 degrees)
};

struct SignalConfig {
    double sampleRate = 250e3;
    GenChannel ch[2];
    int channels = 1;
    double snrDb = 20;           // Es/N0 of each downlink
    double clockPpm = 0;
    double badFraction = 0;      // share of the packets sent with a broken checksum
    double amplitude = 0.2;      // rms of the whole output
    uint32_t seed = 1;
    double startSec = 600;       // signal time of the first sample (orbit and ephemeris clock)
};

// the satellite's earth fixed position (km) at time t (s) for the generator's orbit
void genOrbit(const GenChannel& c, double t, double& x, double& y, double& z, double& argLatDeg);

class Signal {
public:
    explicit Signal(const SignalConfig& c);
    ~Signal();
    void generate(cf32* out, size_t n);
    const SignalConfig& config() const { return c_; }
    uint64_t packetsSent(int ch) const;
    uint64_t packetsBroken(int ch) const;
    uint64_t framesSent(int ch) const;
    double dopplerAt(int ch, double t) const;     // what channel ch carries at signal time t (s since the first sample)
    struct Impl;
private:
    SignalConfig c_;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2::orbcomm
