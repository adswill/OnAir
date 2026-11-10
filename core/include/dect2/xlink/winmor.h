// WINMOR (Rick Muething KN6KB, protocol specification Rev 1.0.6, public domain; Winlink dropped WINMOR in 2020) on HF: frame pilot detection only. Single 1500 Hz carrier, 93.75 baud DBPSK with the phase
// alternating every symbol for at least 24 symbols, then one same-phase symbol (frame sync) and four 46.875 baud 4FSK symbols (frame type). The specification does not define the 4FSK symbol-to-bits mapping,
// the Reed-Solomon generator or the callsign byte packing, so the frames cannot be read from it; the tuning error and the raw frame-type tones are reported. See docs/LEFTOVERS-3.8.md.
#pragma once
#include "dect2/common/ring.h"
#include <array>
#include <complex>
#include <functional>
#include <vector>

namespace dect2::xlink::winmor {

constexpr double kRate = 12000.0;           // complex samples, the dial frequency at 0 Hz, upper sideband: audio f is +f here

struct Pilot { double offsetHz = 0; std::array<int, 4> tones{{-1, -1, -1, -1}}; int symbols = 0; float metric = 0; };

class PilotFinder {
public:
    using Cb = std::function<void(const Pilot&)>;
    void setCallback(Cb cb) { cb_ = std::move(cb); }
    void reset();
    void feed(const cf32* x, size_t n);          // complex 12 kHz, usb
private:
    Cb cb_;
    std::vector<cf32> b_;
    int64_t base_ = 0, n_ = 0, hold_ = 0, trig_ = -1;
    int run_ = 0;
    std::complex<double> C_{0, 0};
    double P_ = 0;
    void analyse(int64_t endAt);
};

// test signal
std::vector<cf32> txPilotFrame(int pilotSymbols, const std::array<int, 4>& tones, double offsetHz, int extraSymbols);

} // namespace dect2::xlink::winmor
