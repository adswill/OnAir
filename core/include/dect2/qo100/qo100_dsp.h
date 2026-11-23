// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// QO-100 (Es'hail-2) maths, shared by the receiver, the app and the tests: the downlink plan, the LNB (local oscillator, IF, inverted
// spectrum above the downlink), decimating channel filters, a phase-exact mixer, the fine frequency estimate of a carrier, the search for
// the two CW beacons of the narrowband transponder, and the drift fit.
//
// Frequencies: "apparent" RF is what the radio's IF gives with the nominal LO (IF + LO, or LO - IF for an inverted LNB). The LNB's own
// oscillator is off by lnbErr (actual LO = nominal + lnbErr), so apparent = true - lnbErr for both kinds; the beacons, whose true
// frequencies are known, measure lnbErr, and true = apparent + lnbErr.
#pragma once
#include "dect2/common/ring.h"
#include <complex>
#include <cstddef>
#include <vector>

namespace dect2 {
namespace qo100 {

constexpr double kLowBeaconHz = 10489.500e6;    // CW beacon, lower edge of the narrowband transponder
constexpr double kPskBeaconHz = 10489.750e6;    // 400 bit/s BPSK beacon, the middle
constexpr double kHighBeaconHz = 10490.000e6;   // CW beacon, upper edge
constexpr double kNbLoHz = 10489.500e6, kNbHiHz = 10490.000e6;    // narrowband transponder (SSB, CW, digital modes)
constexpr double kNbCentreHz = 10489.750e6;     // the centre of the narrowband channel the receiver cuts out
constexpr double kWbLoHz = 10491.000e6, kWbHiHz = 10499.000e6;    // wideband transponder (DATV)
constexpr double kDatvBeaconHz = 10491.500e6, kDatvBeaconSym = 1.5e6;   // DVB-S2 QPSK, 1.5 Msym/s
constexpr double kDownLoHz = 10489.500e6, kDownHiHz = 10500.000e6;   // the whole downlink
constexpr double kWholeCentreHz = 10494.000e6;   // radio centre for the whole downlink at 10 Msps (both CW beacons and the DATV channels)
constexpr double kNarrowCentreHz = 10490.100e6;  // radio centre for the narrowband transponder alone (2 Msps and up; the DC spike above the upper beacon)

struct LoPreset { const char* name; double loHz; };
const LoPreset* loPresets(int& count);   // 9750 (standard Ku LNB), 9739, 10000, 10600 MHz (inverted: the LO is above the downlink)

inline bool invertedLo(double loHz) { return loHz > kDownLoHz; }
inline double ifOf(double rfHz, double loHz) { return invertedLo(loHz) ? loHz - rfHz : rfHz - loHz; }
inline double rfOf(double ifHz, double loHz) { return invertedLo(loHz) ? loHz - ifHz : ifHz + loHz; }

// Phase-continuous mixer in double precision: x * exp(-j 2 pi f n / fs)
class Nco {
public:
    void set(double hz, double fs);
    void reset() { rot_ = std::complex<double>(1, 0); k_ = 0; }
    // The products are written out: std::complex's operator* checks the result for NaN after every product (that check was most of the
    // cost of a per-sample mixer). The same products and sums in the same order, one per statement (nothing fused): the same bits.
    cf32 mix(cf32 x) {
        const double a = x.real(), b = x.imag(), c = rot_.real(), d = rot_.imag();
        const double ac = a * c, bd = b * d, ad = a * d, bc = b * c;
        const double yr = ac - bd, yi = ad + bc;
        const double s = step_.real(), t = step_.imag();
        const double cs = c * s, dt = d * t, ct = c * t, ds = d * s;
        rot_ = std::complex<double>(cs - dt, ct + ds);
        if (++k_ >= 4096) { k_ = 0; rot_ /= std::abs(rot_); }
        return cf32((float)yr, (float)yi);
    }
    double hz() const { return hz_; }
private:
    std::complex<double> rot_{1, 0}, step_{1, 0};
    double hz_ = 0;
    int k_ = 0;
};

// Decimation by 2 per stage (windowed-sinc FIR), as many stages as keep the output at minOutRate or above; passHz either side of 0 is kept.
class Decimator {
public:
    void configure(double inRate, double passHz, double minOutRate);
    void reset();
    void process(const cf32* x, size_t n, std::vector<cf32>& out);   // appends
    double outRate() const { return out_; }
    int factor() const { return 1 << (int)st_.size(); }
private:
    struct Stage { std::vector<float> h; std::vector<cf32> hist; int ph = 0; };   // hist: the last taps - 1 inputs
    std::vector<Stage> st_;
    std::vector<cf32> a_, b_, ext_;
    double out_ = 0;
};

// The strongest carrier of a power spectrum (fft order shifted: bin N/2 = 0 Hz) within +-maxHz of 0: frequency by a parabola through
// the log powers around the peak, and how far it stands over the median bin.
struct Peak { bool ok = false; double hz = 0; double snrDb = 0; double levelDb = -200; };
Peak strongest(const std::vector<double>& pow, double rate, double maxHz);

// The CW beacon pair in an averaged power spectrum of the narrowband channel (bin k at f0Hz + k * binHz, apparent RF): the shift e
// (|e| <= maxShiftHz) where both kLowBeaconHz + e and kHighBeaconHz + e hold a carrier at least minDb over the median. ok false: none.
struct BeaconFit { bool ok = false; double shiftHz = 0; double lowDb = -200, highDb = -200, floorDb = -200; };
BeaconFit findBeacons(const std::vector<double>& pow, double f0Hz, double binHz, double maxShiftHz, double minDb = 12);

// The beacon tracker: turns the raw fine estimates of the LNB error (one per CW beacon, about every 0.6 s, each good for a few
// Hz but sometimes wrong: a carrier crossing the beacon, keying, a dropout) into one steady correction. An alpha-beta filter follows
// the level and the drift; a measurement outside a gate (set by the recent scatter) is ignored; a new level is taken only when
// both beacons agree on it several epochs in a row (one beacon: more epochs), so a crossing carrier cannot drag it. The output is
// slew-rate limited, so the displayed spectrum and every decoder frequency move smoothly, and it coasts on the drift when the
// beacons fade out. Locked after kConfirm agreeing epochs; lost only after a long silence (hysteresis).
struct BeaconMeas { bool ok = false; double errHz = 0; double snrDb = 0; };
class BeaconTracker {
public:
    static constexpr double kSlewHzPerSec = 100;     // the largest speed the output moves at
    static constexpr double kGateMinHz = 8, kGateMaxHz = 40;
    void reset();
    // one epoch at time t (s): the measurements of the lower and the upper beacon. Returns true when an estimate was taken from them.
    bool update(double t, const BeaconMeas m[2]);
    void miss(double t) { (void)t; ++missRun_; }     // an epoch with no usable beacon
    bool locked() const { return locked_; }
    int missRun() const { return missRun_; }
    double estimate() const { return est_; }         // the filtered level at the last accepted epoch
    double rateHzPerSec() const { return rate_; }
    // the smooth correction at time t (advances the slew limiter; call with non-decreasing t)
    double value(double t);
    double gateHz() const;
private:
    bool accept(double t, double z);
    bool locked_ = false, init_ = false;
    double est_ = 0, rate_ = 0, tEst_ = 0;
    double out_ = 0, tOut_ = 0;
    int missRun_ = 0;
    // candidates for a new level (no estimate yet, or a step)
    int candN_ = 0; bool candTwo_ = true; double candSum_ = 0, candLo_ = 0, candHi_ = 0;
    std::vector<double> inno_;   // recent innovations of accepted epochs
    int nAcc_ = 0;
};

// Least-squares slope of (t, v), Hz per minute when v is Hz and t seconds. false with fewer than 3 points or no time spread.
bool slopePerMin(const std::vector<double>& t, const std::vector<double>& v, double& perMin);

} // namespace qo100
} // namespace dect2
