// Adaptive notch for the radio's own spurs (clock harmonics, USB and switching-supply lines, LO leakage away from DC): narrow lines that sit at
// the same place of the baseband for seconds. Measured from the signal itself (a 4096-point Hann FFT every 0.2 s, averaged by power, a line is a
// local maximum 14 dB over its surroundings that is narrow and stays), cut with one complex one-pole notch per line (the line is mixed to DC, its mean
// removed, mixed back), at most kMax of them, each about half an FFT bin wide.
//
// What it never takes: a signal. A line is cut only when it is proved instrumental: the same baseband frequency was a line under another radio centre
// frequency (a signal of the air moves with the tuner, a spur of the radio does not), so nothing is cut before the first retune after a line was seen
// for a second or two. Persistence alone proves nothing: a mode that watches the whole capture has wanted lines anywhere (Inmarsat Aero's other P channels
// were cut by an earlier version that cut out-of-channel lines at once). Known weakness: a comb of fixed carriers retuned by exactly its spacing (FM
// broadcast stations on a 100 kHz raster) puts a real carrier at the same baseband place under two centres. Off by default in the engine (Engine::setSpurNotch).
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {

class SpurNotch {
public:
    using cf32 = std::complex<float>;
    static constexpr int kFft = 4096;
    static constexpr int kMax = 6;
    struct Spur {
        double hz = 0;            // baseband frequency
        float dB = 0;             // over the local level
        bool inChannel = false;
        bool confirmed = false;   // proved instrumental by another radio centre
        bool active = false;      // being cut now
    };

    void configure(double rateHz);                 // a new capture rate: forgets everything
    void reset();
    // The wanted channel: centre (offset from the capture centre) and width of up to two windows (a mode that tunes off its channel has one on
    // each side), the capture's RF centre (a change of it makes every line a candidate again: real signals moved, spurs did not), and width 0 = none known
    void setChannel(double offsetHz, double bwHz, double rfCentreHz, double altOffsetHz = 0);
    void process(cf32* x, size_t n);
    std::vector<Spur> spurs() const { return list_; }
    int activeCount() const;
    double binHz() const { return rate_ / kFft; }
    double thresholdDb = 14;                       // a line must stand this far over its local level
    int framesToAct = 8;                           // frames a line must be seen before it is cut
    double frameSec = 0.2;                         // one analysis frame this often (set before configure; the tests shorten it)

private:
    struct Tr { double hz = 0; float dB = 0; int seen = 0, missed = 0; double rf = 0; bool known = false; bool act = false; bool conf = false; };
    struct Osc { double w = 0; double ph = 0; cf32 m{0.f, 0.f}; bool on = false; };
    void frame();
    void detect();
    bool protectedHz(double hz) const;
    double rate_ = 0;
    double chOff_ = 0, chBw_ = 0, altOff_ = 0, rf_ = 0, rfNew_ = 0;
    bool haveRf_ = false;
    std::vector<float> win_, re_, im_, acc_, base_;
    size_t fill_ = 0, gap_ = 0, period_ = 1;
    bool capturing_ = false;
    int frames_ = 0;
    std::vector<Tr> tr_, memory_;                  // memory_: lines of an earlier radio centre (for the instrumental test)
    std::vector<Spur> list_;
    Osc osc_[kMax];
    int nOsc_ = 0;
    float a_ = 0.f;
};

} // namespace dect2
