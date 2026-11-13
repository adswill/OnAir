// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham science event monitors (3.9, docs/W1B-3.9.md): Schumann resonance monitor, whistler and tweek detector, Moon-echo (EME) test, Radio Jove,
// e-CALLISTO style solar burst monitor and the aurora / sporadic-E / TEP beacon event log. One class, constructed with the kind; each kind turns the
// samples into a spectrogram, a list of timed events and a trend. Receive only. Everything here is new code written from the public descriptions
// cited in docs/W1B-3.9.md; nothing was checked against an on-air recording (simulated signals only).
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {
namespace hamsci {

enum EvtKind { kEvtSchumann = 0, kEvtWhistler, kEvtEme, kEvtJove, kEvtCallisto, kEvtAurora, kEvtKinds };

struct EvtEvent {
    double timeSec = 0;        // signal time of the start
    double durSec = 0;
    std::string type;          // "whistler", "tweek", "echo", "burst", "opening", "transient", ...
    double freqHz = 0;         // the frequency that characterises the event (tweek cutoff, echo Doppler, burst centre offset, ...)
    double peakDb = 0;         // strength over the background, dB
    double aux1 = 0, aux2 = 0; // kind specific: whistler dispersion D (s sqrt(Hz)) / echo delay (s) and spread (Hz) / burst drift (Hz/s) and bandwidth (Hz) / opening spread (Hz), flutter (dB)
    std::string note;
};

struct EvtState {
    int kind = 0;
    double timeSec = 0;
    float levelDb = -200;            // background level, dB re full scale
    // spectrogram, newest row last: dB over the background, `bins` values each, bin 0 at f0Hz (an offset from the radio centre for the RF kinds)
    std::vector<std::vector<float>> rows;
    double rowSec = 0, binHz = 0, f0Hz = 0;
    // trend, one value per trendStepSec, newest last (at most 24 h)
    std::vector<float> trend, trend2;
    double trendStepSec = 60;
    std::string trendName, trend2Name;
    // Schumann: the five lowest resonance peaks found in the long average (0 where none), and the average spectrum itself
    std::vector<float> peakHz, peakDb, peakQ;
    std::vector<float> avgSpec;      // dB over the baseline, binHz apart from avgF0Hz
    double avgF0Hz = 0, avgBinHz = 0;
    // counts
    std::vector<int> hourly;         // events per UTC hour of the capture clock (24 values), newest day only
    int eventsTotal = 0;
    // EME: own transmission found
    double txFreqHz = 0;
    int txPulses = 0;
    std::vector<std::string> info;   // short status lines for the receiver pane
};

class EvtMonitor {
public:
    explicit EvtMonitor(int kind);
    ~EvtMonitor();
    void configure(double rateHz);
    void setSignalOffset(double hz);
    void setStartUnix(double t);
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    std::vector<EvtEvent> take();            // new events since the last call
    const EvtState& state() const;
    int kind() const { return kind_; }
    // options (Auto when never set)
    void setThresholdDb(double db);          // detection threshold over the background, dB
    void setBeaconOffsetHz(double hz);       // aurora / EME: the line to follow (otherwise the strongest line is found)
    void setAutoLine(bool on);
private:
    struct Impl;
    Impl* p_;
    int kind_;
};

std::string evtCsv(const std::vector<EvtEvent>& ev, int64_t startUtc);    // one row per event (UTC time, seconds, type, frequency, peak, duration, aux columns)
std::string evtTrendCsv(const EvtState& s, int64_t startUtc);              // the trend columns, one row per step
const char* evtKindName(int kind);

// generators (simulated signals; the receiver's own test signals, nothing is transmitted). All return complex samples at `rate`.
struct EvtGenOpt {
    double seconds = 60;
    double noiseDb = 0;      // see each generator
    double offsetHz = 0;     // tuning offset / frequency error
    double driftHz = 0;      // slow frequency drift over the run
    uint32_t seed = 1;
    double level = 1.0;      // event strength (kind specific)
};
std::vector<std::complex<float>> evtGenSchumann(double rate, const EvtGenOpt& o, double fundamentalHz = 7.83);
std::vector<std::complex<float>> evtGenWhistler(double rate, const EvtGenOpt& o, const std::vector<double>& whistlerAtSec, const std::vector<double>& tweekAtSec, double dispersion = 60.0, double cutoffHz = 1800.0);
std::vector<std::complex<float>> evtGenEme(double rate, const EvtGenOpt& o, const std::vector<double>& txStartSec, double txLenSec, double delaySec, double dopplerHz, double spreadHz, double echoSnrDb);
std::vector<std::complex<float>> evtGenBursts(double rate, const EvtGenOpt& o, const std::vector<double>& burstAtSec, double burstSec, double driftHzPerSec, double burstBwHz);   // Jove and CALLISTO
std::vector<std::complex<float>> evtGenBeacon(double rate, const EvtGenOpt& o, const std::vector<double>& openAtSec, double openSec, double spreadHz, double flutterDb);

}  // namespace hamsci
}  // namespace dect2
