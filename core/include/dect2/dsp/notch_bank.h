// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Manual notch filters set from the spectrum (a birdie the user points at): a short list of narrow complex band-stops in the IQ domain, in front of
// the spectrum, the waterfall and the decoders. Each one is a pair of one-pole / one-zero complex sections with the zero on the unit circle at the
// notch's baseband frequency, (1 - c z^-1) / (1 - r c z^-1) with c = exp(j 2 pi f / fs) and r = 1 - pi W / fs: an exact null at f, a -3 dB width of
// about 0.6 W for the pair, 0.5 dB or less of loss 2 W from the centre and none further out. One complex multiply per section per sample; with an
// empty list (or all disabled) process() does nothing at all.
// Two kinds: "stays at this RF frequency" (rfHz, the baseband place follows the radio's centre: a real carrier of the air) and "follows the tuning"
// (offsetHz from the capture centre: an image or LO artefact of the radio).
#pragma once
#include <atomic>
#include <complex>
#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct Notch {
    bool enabled = true;
    bool follow = false;       // true: offsetHz from the capture centre; false: rfHz, an absolute radio frequency
    double rfHz = 0;
    double offsetHz = 0;
    double widthHz = 200;      // kMinWidthHz .. kMaxWidthHz
};

class NotchBank {
public:
    using cf32 = std::complex<float>;
    static constexpr int kMax = 8;
    static constexpr double kMinWidthHz = 25, kMaxWidthHz = 5000, kDefaultWidthHz = 200;

    // Replaces the list (any thread). A notch that keeps its position in the list keeps its filter state.
    void set(const std::vector<Notch>& v);
    std::vector<Notch> get() const;
    // One block at rate rateHz whose centre is centreHz (the radio's real RF centre). A no-op with nothing enabled.
    void process(cf32* x, size_t n, double rateHz, double centreHz);
    bool active() const { return count_.load(std::memory_order_relaxed) > 0; }
    // baseband frequency of a notch for a centre, and whether it is inside the capture (+-0.48 of the rate)
    static double basebandHz(const Notch& n, double centreHz) { return n.follow ? n.offsetHz : n.rfHz - centreHz; }
    static double clampWidth(double w);

    // Stored text: "1;404007000,200,0,1|..." version, then per notch: frequency Hz (rf, or offset when following), width Hz, follow 0/1, enabled 0/1.
    static std::string toText(const std::vector<Notch>& v);
    static std::vector<Notch> fromText(const std::string& s);

private:
    struct St { double ur[2] = {0, 0}, ui[2] = {0, 0}; };   // per stage: u = x[n-1] - r y[n-1]
    mutable std::mutex mu_;
    std::vector<Notch> pending_;
    std::atomic<bool> dirty_{false};
    std::atomic<int> count_{0};
    std::vector<Notch> act_;
    std::vector<St> st_;
};

} // namespace dect2
