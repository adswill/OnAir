// Clean-up of the samples of a zero-IF radio before any receiver sees them: the DC spike (the LO leaking into the ADC, a spike in the
// middle of the spectrum on the HackRF, RTL-SDR and the like) and IQ imbalance (I and Q with slightly different gains or not quite 90
// degrees apart, which puts a mirror image of every signal on the other side of the centre). Both are measured from the signal itself,
// so they need no calibration; both are off unless the user turns them on. tests/test_iq_modes.cpp checks them against every mode.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstddef>

namespace dect2 {

using cf32 = std::complex<float>;

class IqCorrector {
public:
    std::atomic<bool> dc{false}, iq{false};

    // keepDc: the wanted signal can have a carrier on the centre (an FM station tuned exactly), which any DC removal takes with the spike:
    // FM audio fell from 54 to 28 dB S/N with the station on the centre (a quiet one from 42 to 6 dB).
    // keepIq: the wanted signal is itself not circular around the centre (DTMB's frame header with the carrier offset near zero, the vision
    // and sound carriers of analog TV), which the blind estimate below cannot tell from an imbalance. The engine decides both per mode (engine.cpp).
    // forceDc: remove the DC spike although the user did not ask for it (offset tuning that the radio cannot do falls back to this)
    void process(cf32* x, size_t n, bool keepDc = false, bool keepIq = false, bool forceDc = false) {
        const bool doDc = (dc.load(std::memory_order_relaxed) || forceDc) && !keepDc, doIq = iq.load(std::memory_order_relaxed) && !keepIq;
        if (!doDc && !doIq) { mi_ = mq_ = 0; nDc_ = 0; }
        if (!doIq) { fresh_ = true; a_ = 0; g_ = 1; s_ = 1; gs_ = 1; i1_ = i2_ = q1_ = q2_ = 0; }
        if (!doDc && !doIq) return;
        // DC: a running mean with a time constant of 2^20 samples (0.1 s at 10 Msps, 0.5 s at 2 Msps). It starts from the mean of the first
        // block with 2^17 samples and narrows to 2^20 as a plain average, so that it settles at once (a plain average from the first sample
        // is a very wide notch for a moment: ATSC lost fields while it locked). The notch is a fraction of a hertz wide at the rates of the narrow modes
        // (2^17 samples took 2 dB of DMR's SNR, and a DRM carrier 3 Hz from the centre); it follows the LO leak as the radio warms up. Without the DC
        // removal the IQ correction works on the signal around that mean and leaves the DC as it is (with keepDc, the carrier on the centre
        // and the spike: turned by the correction they did more harm to FM than the imbalance).
        double sII = 0, sQQ = 0, sIQ = 0;
        double mi = mi_, mq = mq_, nDc = nDc_;
        if (nDc == 0 && n) {   // a fresh start: the mean of this block
            for (size_t i = 0; i < n; i++) { mi += x[i].real(); mq += x[i].imag(); }
            mi /= (double)n; mq /= (double)n;
        }
        float i1 = i1_, i2 = i2_, q1 = q1_, q2 = q2_;
        for (size_t i = 0; i < n; i++) {
            float I = x[i].real(), Q = x[i].imag();
            nDc = std::min(nDc + 1, kDcSamples);
            const double k = 1.0 / std::max(nDc, kDcStart);
            mi += k * (I - mi); mq += k * (Q - mq);
            const float ci = (float)mi, cq = (float)mq;
            I -= ci; Q -= cq;
            if (doIq) {
                // the statistics are taken of the second difference of the signal, a high-pass that weights the band by f^4 around the centre:
                // the imbalance is the same at every frequency, while what is not circular in a proper signal sits near the centre (the
                // carrier and the sidebands of an FM station or an AM channel there, a BPSK or FSK carrier on the centre) and so does the
                // DC that keepDc leaves in. Without it a quiet FM station on the centre went from 42 to 20 dB S/N with the correction on.
                const float dI = I - 2 * i1 + i2, dQ = Q - 2 * q1 + q2;
                i2 = i1; i1 = I; q2 = q1; q1 = Q;
                sII += (double)dI * dI; sQQ += (double)dQ * dQ; sIQ += (double)dI * dQ;
                Q = gs_ * (Q - a_ * I);   // Q made orthogonal to I and as strong as it,
                I = s_ * I;               // both scaled so that the signal keeps its power
            }
            if (!doDc) { I += ci; Q += cq; }
            x[i] = cf32(I, Q);
        }
        mi_ = mi; mq_ = mq; nDc_ = nDc;
        i1_ = i1; i2_ = i2; q1_ = q1; q2_ = q2;
        if (doIq && n) {
            // the powers and the I-Q correlation of the uncorrected signal, averaged over about 2^20 samples
            const double w = fresh_ ? 1.0 : std::min(1.0, (double)n / 1048576.0);   // the first block starts the averages
            fresh_ = false;
            pII_ += w * (sII / n - pII_); pQQ_ += w * (sQQ / n - pQQ_); pIQ_ += w * (sIQ / n - pIQ_);
            if (pII_ > 1e-15) {
                const double a = pIQ_ / pII_;                 // how much of I leaks into Q (the phase error)
                const double rest = pQQ_ - a * pIQ_;          // the power of Q once that leak is gone
                // a real radio is off by a few percent at most; a signal that is not circular at all (a strong carrier on the centre with no noise
                // around it) would push the estimate anywhere, so it is held to +-3 dB and +-11 degrees
                if (rest > 1e-15) {
                    a_ = (float)std::clamp(a, -0.2, 0.2); g_ = (float)std::clamp(std::sqrt(pII_ / rest), 0.708, 1.413);
                    // neither I nor Q is known to be the right one, so the corrected signal is given the power the radio delivered: matching Q
                    // to I alone made the signal 0.5 dB weaker against the DC that FM keeps (and its audio worse with the correction than without)
                    const double qPow = pQQ_ - 2 * a_ * pIQ_ + (double)a_ * a_ * pII_, out = pII_ + (double)g_ * g_ * qPow;
                    s_ = out > 1e-15 ? (float)std::sqrt((pII_ + pQQ_) / out) : 1.f;
                    gs_ = g_ * s_;
                }
            }
        }
    }
    // the measured imbalance, for display: gain ratio in dB and phase error in degrees
    double gainDb() const { return g_ > 0 ? -20 * std::log10(g_) : 0; }
    double phaseDeg() const { return std::asin(std::fmax(-1.0, std::fmin(1.0, a_ * g_))) * 57.29578; }

private:
    static constexpr double kDcSamples = 1048576.0, kDcStart = 131072.0;   // time constant of the DC estimate, and the one it starts with
    double mi_ = 0, mq_ = 0, nDc_ = 0;
    double pII_ = 0, pQQ_ = 0, pIQ_ = 0;
    bool fresh_ = true;
    float a_ = 0, g_ = 1, s_ = 1, gs_ = 1;           // Q -= a I, Q *= g; both times s (gs = g s)
    float i1_ = 0, i2_ = 0, q1_ = 0, q2_ = 0;          // the last two samples, for the second difference
};

} // namespace dect2
