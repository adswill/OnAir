// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Burst edges for the transmit generators that key their carrier hard on and off (a burst starts at full amplitude the sample after silence).
// A hard key-up spreads like a sinc: tens of kHz of splatter around a 12.5 kHz channel. The shaper gives every burst a raised-cosine ramp over
// its first and last rampSec, and the start of the transmission too; inside a burst and in the silence it changes nothing. It looks rampSec
// ahead to see a burst's end coming, so the output is that much later than the generator's. Nothing is made louder: the gain is 0 .. 1.
//
// A burst is a run of samples that are not exactly zero: the generators of these modes write exact zeros between their bursts.
#pragma once
#include "dect2/tx/tx.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// The modes whose generator keys hard (measured by tests/tx/test_tx_quality_b.cpp: envelope step 1.0 at the burst edges) and the ramp each
// gets, about one to two symbols of the mode (0: no shaping). The receivers lock on preambles that are longer than this.
inline double txBurstRampSec(const std::string& id) {
    struct R { const char* id; double sec; };
    static const R kRamps[] = {
        {"m17", 1e-3}, {"nxdn", 1e-3}, {"p25", 1e-3},          // 4800 / 2400 symbol/s 4FSK in 12.5 / 6.25 kHz
        {"multitone", 5e-3},                                    // analog tone paging: the carrier keyed on and off
        {"loraraw", 0.25e-3}, {"loraaprs", 0.25e-3},            // LoRa: chirps of a quarter of a millisecond (SF7 at 500 kHz) and more
        {"vdl4", 0.1e-3},                                       // 19.2 kbit/s GFSK
        {"wmbus", 50e-6}, {"zwave", 50e-6},                     // 32.768 .. 100 kchip/s FSK
        {"uat", 4e-6},                                          // 1.041667 Mbit/s CPFSK
        {"ism", 20e-6},                                         // OOK / FSK sensor bursts, pulses of 100 us and more
        {"wifi", 0.5e-6},                                       // 802.11 frames (the standard's own window is 0.1 us; the DSSS ramp 2 us)
    };
    for (const R& r : kRamps) if (id == r.id) return r.sec;
    return 0;
}

class TxBurstShaper : public TxGenerator {
public:
    TxBurstShaper(std::unique_ptr<TxGenerator> in, double rampSec) : in_(std::move(in)) {
        R_ = std::max<size_t>(2, (size_t)std::lround(rampSec * in_->sampleRate()));
        w_.resize(R_ + 1);
        for (size_t k = 0; k <= R_; k++) w_[k] = (float)(0.5 - 0.5 * std::cos(3.14159265358979 * (double)k / (double)R_));
        delay_.assign(R_, cf32(0.f, 0.f));   // the look-ahead: the first R outputs are the silence before the transmission
        for (size_t j = 0; j < R_; j++) zeros_.push_back(j);
        inIdx_ = R_;
    }
    double sampleRate() const override { return in_->sampleRate(); }
    bool finished() const override { return in_->finished(); }
    void generate(cf32* out, size_t n) override {
        if (buf_.size() < n) buf_.resize(n);
        in_->generate(buf_.data(), n);
        for (size_t i = 0; i < n; i++) {
            // input sample number inIdx_ enters, sample number inIdx_ - R leaves
            const cf32 x = buf_[i];
            if (x.real() == 0.f && x.imag() == 0.f) zeros_.push_back(inIdx_);
            const uint64_t o = inIdx_ - R_;
            const cf32 y = delay_[pos_];
            delay_[pos_] = x;
            pos_ = pos_ + 1 == R_ ? 0 : pos_ + 1;
            inIdx_++;
            while (!zeros_.empty() && zeros_.front() < o) zeros_.pop_front();
            const bool silent = !zeros_.empty() && zeros_.front() == o;
            if (silent) { since_ = 0; out[i] = y; continue; }
            since_++;   // samples of this burst so far, this one included
            const uint64_t until = zeros_.empty() ? R_ + 1 : zeros_.front() - o;   // samples to the burst's end, this one included
            const uint64_t d = std::min<uint64_t>(since_, until);
            out[i] = d >= R_ ? y : y * w_[(size_t)d];
        }
    }
private:
    std::unique_ptr<TxGenerator> in_;
    size_t R_ = 2, pos_ = 0;
    uint64_t inIdx_ = 0, since_ = 0;
    std::vector<float> w_;
    std::vector<cf32> delay_, buf_;
    std::deque<uint64_t> zeros_;   // input sample numbers of the exact zeros inside the look-ahead window
};

} // namespace dect2
