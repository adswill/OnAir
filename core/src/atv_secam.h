// Analog TV receiver, SECAM colour: the chrominance is an FM subcarrier (4.40625 MHz on D'R lines, 4.25 MHz on D'B lines, line by line). In the
// complex baseband of the vision channel the subcarrier is a single-sideband signal, so it can be taken from there directly: mix 4.286 MHz to
// zero, low-pass, decimate to about 2.5 Msps, undo the high-frequency pre-emphasis (the "bell", an FIR made from the response of BT.470-6 item
// 2.13), frequency discriminator, undo the low-frequency pre-emphasis (item 2.7). One line at a time; the video decoder does the sequencing.
#pragma once
#include "dect2/atv_std.h"
#include <complex>
#include <vector>

namespace dect2 {

class AtvSecam {
public:
    using cx = std::complex<float>;
    void configure(double videoRate);
    int decim() const { return dec_; }
    double rate() const { return fd_; }
    int half() const { return hlf_; }                         // input samples either side of an output that its filter needs

    // Baseband of a stretch of the vision channel: zi, zq are n samples of the complex baseband (carrier at 0 Hz). Output m is at input
    // sample half() + m * decim(); it holds the stretch of the spectrum around 4.286 MHz, shifted to zero. Returns the number of outputs.
    size_t baseband(const float* zi, const float* zq, size_t n, std::vector<cx>& out);

    // Mean frequency (Hz, absolute: 4.286 MHz plus the offset) and amplitude of the subcarrier over outputs [a, b) of a baseband stretch.
    // The frequency is the phase advance from one output to the next, weighted by the power; amp is the size of the tone itself.
    void tone(const std::vector<cx>& bb, size_t a, size_t b, double& fHz, double& amp) const;

    // Picture line: from the baseband to D' (the colour difference signal of the line, in units of 1 = the nominal deviation). Value m is the
    // frequency between outputs m-1 and m, so half an output before output m. `isR` picks the rest frequency and the deviation. Where the
    // subcarrier is weaker than `gate` (amplitude of a baseband sample after the bell) D' is 0. Writes bb.size() values. amp(): the mean
    // amplitude after the bell over outputs [a, b) (the picture part of the line).
    void demod(const std::vector<cx>& bb, bool isR, std::vector<float>& d, float gate, size_t a, size_t b);
    float amp() const { return amp_; }

private:
    double fv_ = 0, fd_ = 0;
    int dec_ = 4, hlf_ = 19;
    std::vector<float> lp_;                                   // decimating low-pass at the video rate
    std::vector<cx> bell_;                                    // inverse of the high-frequency pre-emphasis at fd
    std::vector<float> post_;                                 // low-pass on D' at fd
    std::vector<cx> belled_;
    std::vector<float> mi_, mq_, ti_, tq_, disc_;
    float amp_ = 0;
    double a1_ = 0, b0_ = 1, b1_ = 0;                         // low-frequency de-emphasis: y = (b0 x + b1 x1 - a1 y1) / a0 with a0 = 1 + a
    double a0_ = 1;
};

} // namespace dect2
