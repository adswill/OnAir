// ADC level assessment, automatic gain control and a gain sweep ("gain helper") for the HackRF's three gain stages.
#pragma once
#include "spectrum.h"
#include <string>
#include <vector>

namespace dect2 {

struct GainSetting {
    int lna = 16;      // 0..40 dB, 8 dB steps
    int vga = 20;      // 0..62 dB, 2 dB steps
    bool amp = false;  // +14 dB
    int total() const { return lna + vga + (amp ? 14 : 0); }
    bool operator==(const GainSetting& o) const { return lna == o.lna && vga == o.vga && amp == o.amp; }
};

// A sensible split of a total gain over the stages: RF amp only for high totals, LNA before VGA.
GainSetting gainForTotal(int totalDb);
// Generic radios have one overall gain: it travels in `vga` (lna = 0, amp off), limited to maxDb.
GainSetting genericGain(int totalDb, int maxDb);

enum class AdcStatus { NoSignal, Low, Good, High, Overload };
// The bits of the radio's converter at its current rate (8 = HackRF, RTL-SDR; the default): sets what counts as "low" for classifyAdc()
// and the level AutoGain aims for. The app sets it for the radio in use (adcBitsFor in app/toolbar.cpp).
void setAdcBits(int bits);
int adcBits();
double adcLowDbfs();
AdcStatus classifyAdc(double rmsDbfs, double peak, double clipFraction);
const char* adcStatusName(AdcStatus s);
// One-line explanation / advice for the status, e.g. "ADC clipping: reduce the gain".
std::string adcAdvice(AdcStatus s);

// Keeps the ADC level inside a healthy window: ~-16 dBFS rms for OFDM (peak/rms is ~12 dB) and no clipping.
class AutoGain {
public:
    struct Config {
        double targetDbfs = -16, lowDbfs = -22, highDbfs = -11;
        double settleSec = 1.2;     // wait after every change before judging again
        double maxStepDb = 8;
        int genericMaxDb = 0;       // > 0: a radio with a single overall gain of 0..genericMaxDb dB instead of the HackRF stages
    };
    AutoGain() = default;
    explicit AutoGain(const Config& c) : cfg_(c) {}
    void setGenericMax(int db) { cfg_.genericMaxDb = db; }   // 0 = HackRF stages
    void reset();
    // Feed the newest ADC statistics; returns true when `g` was changed and must be applied to the device.
    bool update(double nowSec, const SignalStats& st, GainSetting& g);
    AdcStatus status() const { return classifyAdc(rms_, peak_, clip_); }
    double rmsDbfs() const { return rms_; }

private:
    Config cfg_;
    double rms_ = -120, peak_ = 0, clip_ = 0, last_ = -1, changedAt_ = -1e9;
    bool init_ = false;
};

// Tries a set of gain combinations and keeps the one with the best SNR (or, without a lock, the best ADC level).
class GainSweep {
public:
    struct Sample { double snrDb = 0; bool locked = false; float clip = 0, rms = -120, peak = 0; };
    struct Entry { GainSetting g; double snrDb = -100; float rms = -120, clip = 0; int n = 0; bool done = false; };
    void start(double nowSec, int genericMaxDb = 0);   // genericMaxDb > 0: sweep one overall gain over 0..genericMaxDb
    bool active() const { return active_; }
    // Returns true when `g` changed (apply it). When the sweep ends, `g` is the winner and active() is false.
    bool update(double nowSec, const Sample& s, GainSetting& g);
    const std::vector<Entry>& entries() const { return e_; }
    int current() const { return idx_; }
    const GainSetting& best() const { return best_; }
    std::string summary() const;

private:
    void finish(GainSetting& g);
    std::vector<Entry> e_;
    int idx_ = -1;
    double t0_ = 0;
    bool active_ = false;
    GainSetting best_;
    double snrSum_ = 0, rmsSum_ = 0, clipMax_ = 0; int nSnr_ = 0, nAll_ = 0;
};

} // namespace dect2
