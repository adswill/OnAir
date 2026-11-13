// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Receiver frequency self-calibration: measure the radio's oscillator error (ppm) against references of known frequency and give the
// number to put into the radio's "ppm" setting (docs/self-calibration.md).
//
// Sign convention (the same as source.h radioPpm): positive ppm = the radio's clock runs fast, a signal shows up below its real frequency.
// One oscillator sets the LO and the sample clock, so a tone of true frequency fnom is measured at fnom / (1 + ppm 1e-6) in absolute terms:
//   ppm = (fnom / (centerHz + basebandHz) - 1) * 1e6
// (centerHz = 0 for a tone found in demodulated audio, which only the sample clock moves).
//
// Pieces: ToneMeter (a steady carrier or pilot in IQ, integrated for as long as it is fed), FmPilotMeter (the 19 kHz stereo pilot of an FM
// station), fromFreqError (a frequency error some other decoder measured: cell PSS/SSS, GPS carrier, DVB-T pilots), combine (several
// references into one), Store (per radio serial history, warm-up drift, the ppm to apply), Wizard (the stages and progress of the dialog).
#pragma once
#include <complex>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace dect2::selfcal {

struct Estimate {
    bool valid = false;
    double ppm = 0;         // the radio's error (positive = clock fast)
    double sigmaPpm = 0;    // 1-sigma uncertainty of ppm (the reference's own accuracy is not included)
    double seconds = 0;     // signal time integrated
    double snrDb = 0;       // tone power against the noise in the 1/(block time) bandwidth, rough
    std::string source;     // which reference
};

// A steady tone in IQ. fs: sample rate. expectedHz: where the tone sits in the baseband for a perfect clock (nominalHz - centerHz).
// nominalHz: its absolute frequency (the denominator of the ppm); centerHz: the radio's tuning. searchPpm: how far from nominal to look
// (the radio's worst error, default +-100).
class ToneMeter {
public:
    ToneMeter(double fs, double expectedHz, double centerHz, double searchPpm = 100.0);
    void push(const std::complex<float>* x, size_t n);
    void push(const float* real, size_t n);          // a real signal (demodulated audio / MPX)
    Estimate estimate() const;                       // can be called at any time; valid after about a second of a clear tone
    bool acquired() const { return acquired_; }
    double seconds() const { return seconds_; }
private:
    void process(const std::complex<double>& d);     // one decimated, mixed sample
    void acquire();
    void track(const std::complex<double>& d);
    double fs_, exp_, ctr_, nom_, searchPpm_;
    int dec_ = 1;
    double fsd_ = 0, blockSec_ = 0.05;
    size_t blockLen_ = 1, acqLen_ = 0;
    std::complex<double> upAcc_, downAcc_, prevDown_, rot_{1, 0}, stepRot_{1, 0};   // the triangular decimator and the mixer
    int decN_ = 0; bool haveFirst_ = false; unsigned long long mixN_ = 0;
    double ph_ = 0, mixStep_ = 0;                    // mixer on the decimated stream (rad, rad per sample)
    double seconds_ = 0;
    bool acquired_ = false;
    double fine_ = 0;                                // tone offset from expectedHz found by the acquisition (Hz)
    std::vector<std::complex<double>> acq_;          // decimated samples until the acquisition is done
    std::complex<double> blk_{0, 0}; size_t blkN_ = 0;
    std::vector<std::complex<double>> z_;            // one sum per block
};

// FM broadcast stereo pilot (19 kHz +-2 Hz by the standard, in practice locked to a good reference by most stations).
class FmPilotMeter {
public:
    explicit FmPilotMeter(double fs);                // fs >= 76 kHz of baseband IQ, centered on the station
    void push(const std::complex<float>* x, size_t n);
    Estimate estimate() const;
private:
    ToneMeter tone_;
    double fs_;
    std::complex<float> prev_{1.f, 0.f};
};

// A frequency error measured by another decoder: the carrier should be nominalHz and was found at measuredHz (absolute), +- sigmaHz.
Estimate fromFreqError(const std::string& source, double nominalHz, double measuredHz, double sigmaHz, double seconds = 0);

// The cell decoders (core/src/cell, nr): a cell's carrier is on the 100 kHz raster at centerHz, the decoder found it cfoHz away (CellTelemetry::cfoHz,
// carrier offset against the candidate centre). The operators' carriers are good to about 0.05 ppm. Only meaningful when cfoHz is within the
// range a clock error can explain (< 60 ppm of the carrier); otherwise the estimate is not valid.
Estimate fromCell(double centerHz, double cfoHz, double sigmaHz = 0);
// GPS L1 C/A (1575.42 MHz): residualHz = the tracked carrier minus its predicted Doppler (and minus the satellite clock term), i.e. what the
// radio's clock leaves over.
Estimate fromGpsCarrier(double residualHz, double sigmaHz = 5.0);

// Several estimates (stations, cells, ...) into one: the median of the valid ones is taken, those more than 3 sigma-or-0.3-ppm away are
// dropped as bad references (a station that is off), the rest are averaged with weight 1 / sigma^2.
Estimate combine(const std::vector<Estimate>& v);

// Known reference tones.
double atscPilotHz(int usChannel);                   // US ATSC channels 2..36: lower edge + 309.440559 kHz (0 for others)

// Per radio history, kept in a text file (one line per measurement).
struct Entry {
    std::string serial;
    double utc = 0, ppm = 0, sigmaPpm = 0, tempC = -999;   // tempC -999 = not known
    std::string source;
};
class Store {
public:
    void add(const Entry& e);
    std::vector<Entry> history(const std::string& serial) const;   // oldest first
    bool latest(const std::string& serial, Entry& out) const;
    // warm-up drift: slope of ppm against time (ppm per minute) over the measurements of the last windowSec before the newest one;
    // false with fewer than 3 points
    bool drift(const std::string& serial, double windowSec, double& ppmPerMin) const;
    // the number to apply: the newest measurement, or the inverse-variance mean of the last few within an hour when the clock has settled
    bool value(const std::string& serial, double& ppm) const;
    bool save(const std::string& path) const;
    bool load(const std::string& path);
    std::string toText() const;                   // the same lines as the file (the app keeps them in its preferences)
    void fromText(const std::string& text);
    size_t size() const { return e_.size(); }
private:
    std::vector<Entry> e_;
};
// Put a ppm into a radio settings map (key "ppm", "" = removed when 0), the format radioPpm() reads.
void applyPpm(std::map<std::string, std::string>& radio, double ppm);

// The live feed of the Calibrate dialog: raw blocks straight from the radio (any rate, any centre) into the chosen reference.
//   FmStation: the station at absoluteHz is mixed to the centre, filtered to about 200 kHz and given to an FmPilotMeter.
//   Carrier:   a steady carrier at absoluteHz (an ATSC pilot, a beacon, a test oscillator) goes to a ToneMeter.
// A change of rate or centre starts the measurement over. Thread safe: push() from the analysis thread, estimate() from the UI.
class LiveCal {
public:
    enum Kind { FmStation, Carrier };
    LiveCal(Kind k, double absoluteHz);
    ~LiveCal();
    void push(const std::complex<float>* x, size_t n, double rateHz, double centerHz);
    Estimate estimate() const;
    Kind kind() const { return kind_; }
    double seconds() const;
private:
    struct Impl;
    Kind kind_;
    Impl* d_;
};

// The stages of the Calibrate dialog.
class Wizard {
public:
    enum Stage { Idle, Searching, Integrating, Done, Failed };
    explicit Wizard(double targetSigmaPpm = 0.2, double giveUpSec = 120);
    void start();
    // feed the running estimate and the seconds since start; returns the stage
    Stage update(const Estimate& e, double secondsSinceStart);
    Stage stage() const { return stage_; }
    double progress() const { return progress_; }          // 0..1
    const Estimate& result() const { return res_; }
    std::string status() const;                           // one plain-words line
private:
    double target_, giveUp_, progress_ = 0;
    Stage stage_ = Idle;
    Estimate res_;
};

}   // namespace dect2::selfcal
