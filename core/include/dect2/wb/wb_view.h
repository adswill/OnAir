// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Signal workbench: what the screen shows of a recording of any size, computed on background threads:
//  - Overview: one pass over the file that keeps min / max / power per block (a decimated waveform and the power over time),
//  - SpecEngine: the spectrogram of what is in view, one FFT (or a few, averaged or max-held) per pixel column, so the cost follows the
//    window, not the file; zooming and scrolling ask for a new picture and the old one stays on screen until it is ready.
#pragma once
#include "wb_dsp.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace dect2 {
namespace wb {

struct OverviewBlock { float minI, maxI, minQ, maxQ, maxMag, power; };

class Overview {
public:
    // Starts the pass on its own thread; the blocks fill in over time (progress() reaches 1)
    explicit Overview(std::shared_ptr<Source> s, size_t blocks = 8192);
    ~Overview();
    double progress() const { return prog_.load(); }
    bool done() const { return prog_.load() >= 1; }
    size_t blocks() const { return nb_; }
    uint64_t blockLen() const { return bl_; }
    std::vector<OverviewBlock> snapshot() const;    // a copy of the blocks done so far
    double peak() const { return peak_.load(); }
    double rmsDb() const;                           // the whole file
    cf32 mean() const;                              // the DC offset
    std::vector<float> powerDb() const;             // per block, dBFS
private:
    std::shared_ptr<Source> s_;
    size_t nb_;
    uint64_t bl_ = 1;
    mutable std::mutex mu_;
    std::vector<OverviewBlock> b_;
    std::vector<double> pwAcc_;
    std::atomic<double> prog_{0}, peak_{0};
    double sumPower_ = 0; cf32 sumMean_;
    std::atomic<bool> stop_{false};
    std::thread th_;
};

// min / max of I and Q per pixel column of [t0, t1) read from the source (or from the overview when the range is too long to read)
struct WaveColumns { std::vector<float> minI, maxI, minQ, maxQ, mag; };
void waveformColumns(const Source& s, const Overview* ov, uint64_t t0, uint64_t t1, int cols, WaveColumns& out);

struct SpecParams {
    uint64_t t0 = 0, t1 = 0;      // samples in view
    int cols = 800;               // time columns wanted
    int fftSize = 1024;
    int window = WinHann;
    double overlap = 0.5;         // 0 .. 0.95
    double f0 = -1e30, f1 = 1e30; // band in view, Hz from the source's DC (clipped to what the signal has)
    int maxRows = 1024;
    bool operator==(const SpecParams& o) const {
        return t0 == o.t0 && t1 == o.t1 && cols == o.cols && fftSize == o.fftSize && window == o.window && overlap == o.overlap && f0 == o.f0 && f1 == o.f1 && maxRows == o.maxRows;
    }
};

struct SpecImage {
    SpecParams p;
    int w = 0, h = 0;             // columns, rows (row 0 is the highest frequency)
    std::vector<float> db;        // w * h, dBFS per FFT bin
    double fLo = 0, fHi = 0;      // the frequencies of the image's bottom and top edge, Hz from DC
    double binHz = 0;             // width of one FFT bin
    float p5 = -100, p995 = 0;    // 5 % and 99.5 % points of the levels (the automatic colour range)
    uint64_t seq = 0;
    double secs = 0;              // how long it took
};

class SpecEngine {
public:
    explicit SpecEngine(std::shared_ptr<Source> s);
    ~SpecEngine();
    void request(const SpecParams& p);              // newest request wins; a running one is abandoned
    bool poll(SpecImage& out, uint64_t& seq);       // true: a picture newer than seq is there (copied)
    bool busy() const { return busy_.load(); }
    const SpecParams& last() const { return req_; }
private:
    void run();
    void compute(const SpecParams& p, uint64_t gen);
    std::shared_ptr<Source> s_;
    SpecParams req_;
    std::mutex mu_;
    std::condition_variable cv_;
    uint64_t gen_ = 0, doneSeq_ = 0;
    bool havePending_ = false, quit_ = false;
    SpecParams pending_;
    SpecImage img_;
    std::atomic<bool> busy_{false};
    std::atomic<uint64_t> curGen_{0};
    std::thread th_;
};

} // namespace wb
} // namespace dect2
