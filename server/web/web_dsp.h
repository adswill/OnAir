// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web DSP: one wideband capture, many virtual receivers.
//
//   capture (cf32, rate R) --> SharedFft: overlap-save forward FFT of N points, hop L = 3N/4, computed ONCE for all users
//                          \-> Waterfall: Hann-windowed power spectrum (shared), each user cuts its own zoom window and bin count
//   per user / decoder:  SliceIq: the M FFT bins around the tuned frequency (M << N, a gentle anti-alias taper), an M-point inverse FFT,
//                        the overlap discarded -> complex baseband at R*M/N; then ChannelFilter (core/src/engine/channelizer.cpp) removes the
//                        residual offset (less than half a bin) and resamples exactly to the demodulator's rate
//                        VirtualRx: + the passband filter (time domain, at 24 kHz or 240 kHz), the demodulator (AM, NFM, WFM, USB, LSB, CW),
//                        squelch, AGC, de-emphasis and the resampler to the listener's audio rate.
// The per-user cost therefore does not grow with the capture's rate (one small inverse FFT a block), which is what lets one radio serve many
// listeners. Nothing here can transmit or write files: it only reads samples.
#pragma once
#include "web_proto.h"
#include "dect2/common/ring.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/engine/channelizer.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace onair_web {

using dect2::cf32;

// The shared forward transform. Bins are in FFT order (bin k is k*R/N for k < N/2, (k-N)*R/N above), scaled so a full-scale tone gives 1.
class SharedFft {
public:
    bool configure(double rateHz);              // picks N (bins of about 150 Hz, 4096 .. 262144 points)
    double rate() const { return rate_; }
    int size() const { return n_; }
    int hop() const { return hop_; }            // new samples per block (3N/4)
    double binHz() const { return rate_ / n_; }
    // Feeds samples; for each completed block calls onBlock(re, im) (N values each). Blocks are numbered from 0 since configure.
    template <class F> void push(const cf32* x, size_t n, F&& onBlock) {
        size_t i = 0;
        while (i < n) {
            const size_t take = std::min(n - i, (size_t)(n_ - fill_));
            for (size_t k = 0; k < take; k++) { buf_[fill_ + k] = x[i + k]; }
            fill_ += (int)take; i += take;
            if (fill_ == n_) { transform(); onBlock((const float*)re_.data(), (const float*)im_.data()); block_++; shift(); }
        }
    }
    uint64_t block() const { return block_; }   // the number of the next block

private:
    double rate_ = 0;
    int n_ = 0, hop_ = 0, log2n_ = 0, fill_ = 0;
    uint64_t block_ = 0;
    std::vector<cf32> buf_;
    std::vector<float> re_, im_;
    void transform();
    void shift();
};

// Where a slice sits and how it is cut, derived from the SharedFft and the wanted rate
struct SlicePlan {
    int n = 0, hop = 0, m = 0;   // the shared N and hop, the slice's M
    int k0 = 0;                  // centre bin (signed)
    double sliceRate = 0;        // R*M/N
    double residualHz = 0;       // what ChannelFilter still removes
};
bool planSlice(const SharedFft& f, double offsetHz, double outRate, SlicePlan& p, std::string& err);

// Complex baseband at outRate around offsetHz from the capture centre (decoders get this; VirtualRx is built on it)
class SliceIq {
public:
    bool configure(const SharedFft& f, double offsetHz, double outRate, std::string& err);
    // one block of the shared FFT (blockNo = SharedFft::block() at the time of the callback); appends samples at outRate
    void process(const float* re, const float* im, uint64_t blockNo, std::vector<cf32>& out);
    double outRate() const { return outRate_; }
    const SlicePlan& plan() const { return p_; }

private:
    SlicePlan p_;
    double outRate_ = 0;
    int log2m_ = 0, keepFrom_ = 0;
    std::shared_ptr<const std::vector<cf32>> h_;   // the filter over the M bins (FFT order), shared between slices of the same plan
    std::vector<float> yr_, yi_;
    std::vector<cf32> tmp_;
    dect2::ChannelFilter fine_;
};

struct RxSettings {
    double offsetHz = 0;            // tuned frequency minus the capture centre
    Demod demod = Demod::Nfm;
    double lowHz = 0, highHz = 0;   // passband relative to the tuned frequency; both 0: the demodulator's default
    double squelchDb = -150;        // <= -150: open
    AgcMode agc = AgcMode::Slow;
    double gainDb = 0;              // with AGC off
    double deemphUs = 50;           // WFM
    int audioRate = 12000;
};
void defaultPassband(Demod d, double& lo, double& hi);
// the widest passband edges a demodulator accepts (relative to the tuned frequency)
void passbandLimits(Demod d, double& lo, double& hi);

class VirtualRx {
public:
    bool configure(const SharedFft& f, const RxSettings& s, std::string& err);
    const RxSettings& settings() const { return s_; }
    void process(const float* re, const float* im, uint64_t blockNo, std::vector<float>& audio);   // appends audio (-1 .. 1) at audioRate
    float levelDb() const { return levelDb_; }   // channel power in the passband, dBFS
    bool squelched() const { return sqClosed_; }
    static double demodRate(Demod d) { return d == Demod::Wfm ? 240000.0 : 24000.0; }

private:
    RxSettings s_;
    SliceIq slice_;
    double chRate_ = 24000;
    // passband filter: shift by -fc, real low-pass, shift back
    std::vector<float> taps_;
    std::vector<cf32> hist_;
    size_t histPos_ = 0;
    double fc_ = 0, ncoPh_ = 0, ncoStep_ = 0;
    double bfoPh_ = 0, bfoStep_ = 0;
    cf32 prev_{1, 0};
    float dcAcc_ = 0, deState_ = 0, deA_ = 0, agcEnv_ = 1e-3f;
    float levelDb_ = -200;
    bool sqClosed_ = false;
    int sqAbove_ = 0;
    // WFM: 240 kHz -> 48 kHz by 5 before the audio resampler
    std::vector<float> wfmTaps_, wfmHist_;
    size_t wfmPhase_ = 0;
    dect2::ExactResampler audioRs_;
    std::vector<cf32> iq_, demod_, aout_;
};

// The shared waterfall: averaged Hann-windowed power (dBFS) of the capture, DC in the middle, refreshed at most `fps` times a second
class Waterfall {
public:
    bool configure(double rateHz, int fps);
    void push(const cf32* x, size_t n);
    bool fresh(uint64_t& seq) const { if (seq == seq_) return false; seq = seq_; return true; }
    uint64_t seq() const { return seq_; }
    const std::vector<float>& db() const { return db_; }
    int size() const { return n_; }
    double rate() const { return rate_; }
    // A row of `bins` values over [startHz, stopHz] relative to the capture centre (clamped to the capture); peaks kept when decimating.
    void row(double startHz, double stopHz, int bins, std::vector<float>& out) const;

private:
    double rate_ = 0;
    int n_ = 0, log2n_ = 0, every_ = 1;   // a transform every `every_` samples
    std::vector<float> win_, re_, im_, acc_, db_;
    std::vector<cf32> buf_;
    int fill_ = 0, nAcc_ = 0;
    long gap_ = 0;
    uint64_t seq_ = 0;
};

// dB row to bytes with a floor / top that follow the noise slowly (state kept by the caller per user)
struct RowScale { float floorDb = -120, topDb = -10; bool init = false; };
void quantizeRow(const std::vector<float>& db, RowScale& sc, std::vector<uint8_t>& out);

} // namespace onair_web
