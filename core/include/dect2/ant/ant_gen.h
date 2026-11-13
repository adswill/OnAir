// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ANT+ test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Sensors broadcasting on one channel, each with its own period (the heart rate profile every 8070/32768 s), 1 Mbit/s GFSK with 160 kHz
// deviation, complex white noise and a carrier offset from the config. The heart rate strap cycles its pages like a real one (page 0 with
// the background pages 1, 2, 3 and 7 every 65th message) with a beat count and event time that follow the heart rate.
//
// makeAntSynth (SynthConfig): snrDb = carrier to noise in 1 MHz; cfoHz = carrier offset; modeOpt[0] = 0 a heart rate strap, a bike power
// meter and a speed and cadence sensor, 1 the heart rate strap alone, 2 noise only.
#pragma once
#include "ant_proto.h"
#include "dect2/engine/mode_synth.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {

struct AntGenSensor {
    uint16_t deviceNumber = 0x6941;
    int type = ant::HeartRate;
    uint8_t transType = 1;
    uint16_t marker = ant::kAntPlusMarker;
    double periodSec = ant::kHrmPeriodSec;
    double cfoHz = 0;                 // this sensor's own carrier error
    double ampScale = 1;
    int heartRate = 72;               // heart rate strap: the rate it reports (changes slowly when sweep is set)
    bool sweep = true;                // the heart rate walks between 60 and 99
    int powerW = 180, cadenceRpm = 85; // power meter
    double speedKmh = 25;             // speed and cadence sensor
    int badCrcEvery = 0;              // every n-th frame gets a bad CRC (0: never)
};

struct AntGenConfig {
    double rate = 4e6;
    double snrDb = 30;                // carrier to noise in 1 MHz
    double cfoHz = 0;
    double dialOffsetHz = 0;          // where the channel sits in the capture (the radio's tune offset is negative of it)
    double clockPpm = 0;              // bit clock error
    double amp = 0.5;
    uint32_t seed = 1;
    bool noiseOnly = false;
    double leadSec = 0.05;
    std::vector<AntGenSensor> sensors;
};

class AntGenerator {
public:
    explicit AntGenerator(const AntGenConfig& c);
    ~AntGenerator();
    void generate(cf32* out, size_t n);
    static double noiseSigma(const AntGenConfig& c);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::vector<AntGenSensor> antTestSensors(int option);   // 0 three sensors, 1 the strap alone
std::unique_ptr<ModeSynth> makeAntSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
