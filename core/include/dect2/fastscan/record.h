// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Recording of channels from one wide capture without retuning: WolaBank (a 4x oversampled FFT filterbank, so the cost does not grow with the
// number of channels), a small per-channel tap (fine mix, low-pass, decimate, FM/AM demodulator) and RecorderBank that starts and stops
// recordings by hit, up to 100 at once, guarded by CpuGuard. Files are named by frequency and time into one folder with a size budget.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {
namespace fastscan {

// Weighted overlap-add analysis filterbank: M channels spaced rate/M, each delivered at 4 x rate/M (hop M/4), passband +-0.75 bin, stopband from
// 1.25 bin (Kaiser prototype of P x M taps). Channel k is the span centred on k x rate/M relative to the capture centre (k < M/2 above, the rest below).
class WolaBank {
public:
    static constexpr int kOver = 4;
    bool configure(double rateHz, int m, int p = 8);
    int channels() const { return M_; }
    double binHz() const { return rate_ / M_; }
    double outRate() const { return kOver * rate_ / M_; }
    void reset() { pend_.clear(); frame_ = 0; }
    // Feeds n samples; for every frame produced calls emit(frameIndex, X) where X[k] is the (phase-corrected) output of channel k.
    template <class F> void process(const cf32* x, size_t n, F&& emit) {
        pend_.insert(pend_.end(), x, x + n);
        const size_t L = (size_t)P_ * M_, D = (size_t)M_ / kOver;
        size_t pos = 0;
        while (pend_.size() - pos >= L) {
            analyse(&pend_[pos]);
            emit(frame_, re_.data(), im_.data());
            frame_++; pos += D;
        }
        if (pos) pend_.erase(pend_.begin(), pend_.begin() + (long)pos);
    }
    uint64_t frames() const { return frame_; }
private:
    void analyse(const cf32* x);
    double rate_ = 0;
    int M_ = 0, P_ = 8, log2m_ = 0;
    std::vector<float> w_, re_, im_, fr_, fi_;
    std::vector<cf32> pend_;
    uint64_t frame_ = 0;
};
int wolaChannelsFor(double rateHz, double chanBwHz);   // the power of two M that leaves a channel of chanBwHz inside the +-0.25 bin it needs

enum class Demod { FM, AM, IQ };
Demod demodFor(const std::string& modeKey);

// One channel's chain after the filterbank.
class ChannelTap {
public:
    // bin: the filterbank channel, residualHz: the channel's centre minus the bin's (|res| <= bin/2), inRate: WolaBank::outRate()
    void configure(int bin, double residualHz, double inRate, double bwHz, Demod d);
    int bin() const { return bin_; }
    // z: the filterbank output of this channel (inRate); appends audio (int16) or IQ (cf32) at outRate()
    void process(const cf32* z, size_t n, std::vector<int16_t>& audio, std::vector<cf32>& iq);
    double outRate() const { return inRate_ / dec_; }
    float level() const { return level_; }   // recent output level (0..1)
private:
    int bin_ = 0, dec_ = 1;
    double inRate_ = 0, res_ = 0, ph_ = 0, step_ = 0;
    Demod d_ = Demod::FM;
    std::vector<float> taps_;
    std::vector<cf32> hist_;
    size_t phase_ = 0;
    cf32 prev_{1, 0};
    float dcAvg_ = 0, level_ = 0, fmScale_ = 1;
};

// The share of the thread the recorders may use: updated from measured busy time, answers how many channels may run.
class CpuGuard {
public:
    void configure(double maxLoad, int maxChannels) { maxLoad_ = maxLoad; maxCh_ = maxChannels; cap_ = maxChannels; }
    void update(double busySec, double spanSec, int active);   // one block: time spent, time covered by the samples, channels running
    int cap() const { return cap_; }               // channels allowed now
    double load() const { return load_; }
    bool throttled() const { return cap_ < maxCh_; }
private:
    double maxLoad_ = 0.7, load_ = 0, grow_ = 0;
    int maxCh_ = 100, cap_ = 100;
};

// ---- files
std::string scanRecName(double freqHz, std::time_t when, const std::string& ext);   // "scan_145.500MHz_20261023-101500.wav"
struct RecFileInfo { std::string path; uint64_t bytes = 0; std::time_t mtime = 0; };
// The files to delete (oldest first) so that the total stays within budgetBytes; keepNewest files are never picked.
std::vector<size_t> pickDeletions(const std::vector<RecFileInfo>& files, uint64_t budgetBytes, size_t keepNewest = 1);
uint64_t trimRecordingFolder(const std::string& dir, uint64_t budgetBytes);   // deletes scan_* files; returns the bytes left

struct RecSettings {
    std::string dir;                 // scan-recordings folder
    uint64_t budgetBytes = 2ull << 30;
    double chanBwHz = 12500;         // channel width of every recording
    bool iqSlice = false;            // false: demodulated audio (WAV), true: the channel's IQ (cf32 + SigMF)
    int maxChannels = 100;
    double maxSecPerRecording = 900;
    double maxLoad = 0.7;            // CPU guard
};

struct RecStats {
    int active = 0, cap = 100;
    uint64_t started = 0, refused = 0, bytes = 0;
    double load = 0;
    bool throttled = false;
};

class RecorderBank {
public:
    ~RecorderBank() { closeAll(); }
    bool configure(double rateHz, double centerHz, const RecSettings& s);
    bool ready() const { return rate_ > 0; }
    double centerHz() const { return center_; }
    bool retarget(double centerHz);   // the capture is now centred elsewhere; only while nothing is recording
    // start a recording of the channel at freqHz (absolute); false: outside the capture, the limit or the CPU guard refused
    bool start(uint64_t key, double freqHz, const std::string& modeKey, std::time_t now);
    void stop(uint64_t key);
    void closeAll();
    bool recording(uint64_t key) const;
    // the wide capture (the same stream the spectrum is made from); runs the filterbank only while a recording is open
    void feed(const cf32* x, size_t n);
    RecStats stats() const;
    const RecSettings& settings() const { return s_; }
private:
    struct Rec {
        uint64_t key = 0;
        ChannelTap tap;
        std::vector<cf32> in;
        std::vector<int16_t> audio;
        std::vector<cf32> iq;
        FILE* f = nullptr;
        std::string path;
        uint64_t samples = 0, bytes = 0;
        double startedSec = 0, freqHz = 0;
        bool iqMode = false;
    };
    void finish(Rec& r);
    double rate_ = 0, center_ = 0, elapsed_ = 0;
    RecSettings s_;
    WolaBank bank_;
    CpuGuard guard_;
    std::vector<std::unique_ptr<Rec>> recs_;
    uint64_t started_ = 0, refused_ = 0, bytesDone_ = 0;
    mutable std::mutex m_;
};

}  // namespace fastscan
}  // namespace dect2
