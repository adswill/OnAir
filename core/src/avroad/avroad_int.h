// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Inside the aviation / rail / road receivers: the decoder interface and the small DSP pieces they share.
#pragma once
#include "dect2/avroad/avroad_rx.h"
#include "dect2/avroad/avroad_tel.h"
#include "dect2/dmr/dmr_dsp.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/common/mathconst.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dect2::avroad {

using dect2::math::kPi;

// What a decoder reports to the receiver
struct Sink {
    virtual ~Sink() = default;
    virtual void row(const char* kind, const std::string& id, const std::string& text, float levelDb, const double* latLon = nullptr) = 0;
    virtual void good(float snrDb = 0, double cfoHz = 0) = 0;
    virtual void bad() = 0;
    virtual void energy() = 0;               // something is on the channel
    virtual double now() const = 0;
};

class Decoder {
public:
    virtual ~Decoder() = default;
    virtual bool configure(double fs) = 0;
    virtual void reset() = 0;
    virtual void feed(const cf32* x, size_t n) = 0;
    virtual void fill(AvRoadTelemetry& t) const = 0;      // headline and facts
    virtual void setCenterHz(double) {}
    void attach(Sink* s) { out = s; }
protected:
    Sink* out = nullptr;
};

std::unique_ptr<Decoder> makeDecoder(int variant);
std::unique_ptr<Decoder> makeEot();
std::unique_ptr<Decoder> makeFanet();
std::unique_ptr<Decoder> makeOgn();
std::unique_ptr<Decoder> makeErmes();
std::unique_ptr<Decoder> makeRadar(bool weather);
std::unique_ptr<Decoder> makeIts();
std::unique_ptr<Decoder> makeDsrc();
std::unique_ptr<Decoder> makeGbas();
std::unique_ptr<Decoder> makeOfdmDetect(int kind);      // 0 LDACS, 1 AeroMACS, 2 C-V2X

// Low-pass, then resample to exactly workRate
class Chain {
public:
    bool configure(double fs, double workRate, double passHz, double stopHz) {
        const int d = std::max(1, (int)std::floor(fs / workRate));
        lp_.design(dmr::lowpassTaps(passHz, std::min(stopHz, fs / d * 0.5 - 1000.0), fs, 50, 9), d);
        const double r = fs / d;
        pass_ = std::fabs(r - workRate) < 0.01;
        if (!pass_ && !rs_.configure(r, workRate)) return false;
        return true;
    }
    void reset() { lp_.reset(); if (!pass_) rs_.reset(); }
    void process(const cf32* x, size_t n, std::vector<cf32>& out) {
        dec_.clear(); out.clear();
        lp_.process(x, n, dec_);
        if (pass_) out = dec_; else rs_.process(dec_.data(), dec_.size(), out);
    }
private:
    dmr::Fir<cf32> lp_;
    ExactResampler rs_;
    bool pass_ = true;
    std::vector<cf32> dec_;
};

inline std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
inline std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap; va_start(ap, f); vsnprintf(b, sizeof b, f, ap); va_end(ap);
    return b;
}

inline float toDb(float p) { return 10 * std::log10(std::max(p, 1e-14f)); }

} // namespace dect2::avroad
