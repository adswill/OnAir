// Analog TV receiver, radio side: carrier search, the vision channel (mixer, vestigial-sideband filter, detectors) and the FM sound channel.
#pragma once
#include "dect2/atv_std.h"
#include "dect2/ring.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dect2 {

// ---- finds the vision carrier (and the sound carrier that belongs to it) in the averaged spectrum
struct AtvCarrier {
    double visionHz = 0;         // from the centre of the input
    double visionDb = 0;         // peak above the neighbouring lines, dB
    double soundHz = 0;          // 0 = none found
    double soundDb = 0;
    double soundRelDb = -99;     // the sound carrier against the vision carrier line, dB (a sound carrier is 10 to 20 dB below it; the colour subcarrier is much lower)
    double spacingMhz = 0;       // 5.5, 6.0, 6.5 or 4.5 when a sound carrier was found
    double dsbDb = 0;            // power 0.3 .. 0.6 MHz below the carrier against the same above it (about 0 for a flat double-sideband, strongly negative for a Nyquist slope)
    double score = 0;
};

class AtvCarrierSearch {
public:
    void configure(double fs);
    void reset();
    void feed(const cf32* x, size_t n);
    int spectra() const { return count_; }
    // the averaged spectrum is judged after enough spectra; returns candidates, best first (empty if none stands out)
    std::vector<AtvCarrier> candidates() const;
    // averaged power spectrum in dB relative to the strongest bin, the bin width in Hz, bin 0 = DC (fftshifted: bin n/2 = DC)
    void spectrumDb(std::vector<float>& db, double& binHz) const;
    // true: keep a running average, one spectrum every 25 ms, for the display (the picture is being decoded) instead of collecting for the search
    void setMonitor(bool on);
    // Monitor spectrum around a carrier: `points` values from loMhz to hiMhz above `centreHz`, dB below the strongest of them (peak of the bins in each step)
    void display(double centreHz, double loMhz, double hiMhz, int points, std::vector<float>& out) const;
    // The strongest line within +-halfWidthHz of centreHz in the monitor spectrum: its frequency (parabolic interpolation) and how far it stands
    // above the rest of that stretch, dB, and its level (dB, arbitrary reference). False when there is no spectrum yet.
    bool peakNear(double centreHz, double halfWidthHz, double& fHz, double& promDb, double& levelDb) const;
private:
    double fs_ = 0;
    bool monitor_ = false;
    size_t nfft_ = 8192, fill_ = 0, skip_ = 0;
    std::vector<cf32> buf_;
    std::vector<float> win_, acc_;
    int count_ = 0;
    struct Fx;
    std::shared_ptr<Fx> fft_;
};

// ---- FM sound channel: mixer, decimation, discriminator, de-emphasis, 48 kHz out (mono)
class AtvSound {
public:
    void configure(double fs);
    void reset();
    void setCarrier(double hz) { carrier_ = hz; }              // from the centre of the input
    void setSystem(double devKhz, double preEmphUs);
    // consumes n samples (DC removed); appends audio at 48 kHz to `out`
    void process(const cf32* x, size_t n, std::vector<float>& out);
    bool present() const { return present_; }
    float devKhz() const { return devKhz_; }
    float levelDb() const { return levelDb_; }
    double trimHz() const { return trim_; }
private:
    struct Impl;
    std::shared_ptr<Impl> p_;
    double fs_ = 0, carrier_ = 0, trim_ = 0;
    bool present_ = false;
    float devKhz_ = 0, levelDb_ = -120;
};

// ---- the vision channel
class AtvFront {
public:
    static constexpr size_t kMaxBlock = 2048;
    void configure(double fs);
    bool ready() const { return ready_; }
    double videoRate() const { return fv_; }
    bool colourCapable() const { return fv_ >= 9.5e6; }
    void reset();
    void setCarrier(double hz);                         // vision carrier, from the centre of the input
    double carrierHz() const { return carrier_ + trim_; }
    // the channel plan: sound carrier spacing (MHz) sets the upper edge of the video band; the vestigial sideband width sets the low-frequency correction
    void setPlan(double soundSpacingMhz);
    void setSyncDetector(bool on) { syncDet_ = on; }
    bool syncDetector() const { return syncDet_; }
    // double sideband correction amount (1 = a flat vestigial region, 0 = the transmitter already has the Nyquist slope)
    void setDsbAmount(float a) { dsbAmt_ = a; }
    // From the video decoder, once per line: the phase of the carrier against the local oscillator (radians) and how much to trust it (0..1)
    void carrierError(double rad, double trust);
    // up to kMaxBlock input samples (DC removed); produces video-rate samples
    size_t process(const cf32* x, size_t n);
    const float* v() const { return v_.data(); }         // detected video, more carrier = larger
    const float* i() const { return i_.data(); }         // complex baseband, carrier at 0 Hz
    const float* q() const { return q_.data(); }
    double lastBlockPhaseDeg() const { return 0; }
    double bandUpMhz() const { return upMhz_; }
    double noiseGain() const { return nGain_; }          // sum of the squared taps of the channel filter: output noise power / input noise power

private:
    struct Dec;
    void design();
    double fs_ = 0, fv_ = 0;
    bool ready_ = false, syncDet_ = false;
    double carrier_ = -2.75e6, trim_ = 0, trimRate_ = 0;
    double spacing_ = 5.5, upMhz_ = 5.0, vsbMhz_ = 0.75, centreHz_ = 0, nGain_ = 1;
    float dsbAmt_ = 1.f;
    int decim_ = 1;
    double ph1_ = 0;                                    // mixer 1 phase (radians)
    double nv_ = 0;                                     // video samples so far
    std::vector<float> m1i_, m1q_, d1i_, d1q_, y_i_, y_q_, v_, i_, q_, corrIn_, corrOut_;
    std::vector<float> corrTaps_, histI_, histQ_;
    std::shared_ptr<Dec> di_, dq_;
    size_t pend_ = 0;
};

} // namespace dect2
