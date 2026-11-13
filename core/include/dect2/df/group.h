// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A coherent group of N independent receivers (KrakenSDR / KerberosSDR: one RTL-SDR per channel, all sharing one clock and one switchable
// noise source) presented as one MultiChannelSource. Each device is a ChannelFeed (the RTL driver adapter lives outside the core).
// Calibrate while the noise source is on: the integer sample offset between the devices (they start a few samples apart) is found by
// cross-correlation, then the per-channel complex gain (phase, amplitude) from the dominant eigenvector of the covariance. read() then
// returns aligned, gain-corrected frames. Public algorithm descriptions only; no third-party code.
#pragma once
#include "dect2/df/source.h"
#include <memory>

namespace dect2::df {

class ChannelFeed {
public:
    virtual ~ChannelFeed() = default;
    // Fills up to n samples, blocking; returns the count read (0 = the device stopped)
    virtual size_t read(cd* out, size_t n) = 0;
};

struct GroupCalibration {
    bool ok = false;
    std::vector<int> lagSamples;      // arrival lag of each channel relative to channel 0 (+ = later)
    std::vector<cd> gains;            // per-channel gain after alignment, channel 0 phase = 0, mean amplitude 1
    double coherence = 0;             // dominant eigenvalue / trace of the aligned noise covariance (1 = perfectly coherent), > ~0.8 is usable
    size_t consumed = 0;              // aligned samples per channel used up by the calibration (the rest stays queued for read())
};

class CoherentGroup : public MultiChannelSource {
public:
    CoherentGroup(std::vector<std::unique_ptr<ChannelFeed>> feeds, double sampleRate, double centerFreq);
    int channels() const override { return (int)feeds_.size(); }
    double sampleRate() const override { return fs_; }
    double centerFreq() const override { return fc_; }
    // Reads about `n` samples from every device (noise source on), estimates lags and gains, discards the lead so the channels line up, stores the result.
    // maxLag: the largest start offset searched (samples). False when the channels are not coherent enough (coherence < minCoherence).
    bool calibrate(size_t n = 8192, int maxLag = 64, double minCoherence = 0.8);
    const GroupCalibration& calibration() const { return cal_; }
    // Aligned and (once calibrated) gain-corrected samples
    bool read(Frame& out, size_t n) override;
private:
    std::vector<std::unique_ptr<ChannelFeed>> feeds_;
    double fs_, fc_;
    GroupCalibration cal_;
    std::vector<std::vector<cd>> pend_;   // samples read ahead per channel while aligning
    bool fill(size_t ch, size_t n);
};

}   // namespace dect2::df
