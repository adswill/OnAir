// Reception Doctor: from a short IQ capture and the receiver state, says in plain sentences why a mode is not decoding
// (overload, too little signal, off-centre signal, rate too narrow, adjacent interferer, drops, DC spike, I/Q imbalance, wrong mode),
// each with a one-click fix where that is safe. Pure analysis, no ImGui, no engine: tested on synthetic IQ (docs/RECEPTION-DOCTOR.md).
#pragma once
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace dect2::doctor {

using cf32 = std::complex<float>;

// What the app knows besides the samples
struct Context {
    double rateHz = 0;            // sample rate of the capture
    double modeBwHz = 0;          // width of the wanted channel (0 = unknown: no width, offset or interferer checks)
    double modeOffsetHz = 0;      // where the wanted channel should sit from the capture centre
    double gainDb = 0, gainMaxDb = 0;   // the radio's gain (gainMaxDb 0 = unknown)
    bool live = false;            // a live radio (a file has no gain, drops or DC spike to fix)
    bool locked = false;          // the decoder is locked: the doctor stays quiet about everything but real hazards
    double deliveredPct = -1;     // samples the radio handed over, percent of its rate (-1 unknown)
    bool radioDrops = false;      // the radio side lost samples recently (USB)
    bool cpuDrops = false;        // the OnAir side lost samples recently (too slow for this computer)
    std::string modeId;           // the current mode (ModeTuning::id), to tell "wrong mode" from "right mode"
    bool identify = true;         // run the signal identifier when nothing else explains it
    double rfHz = 0;              // centre frequency, for the identifier's band prior
};

enum class Code { Clipping, WeakSignal, NoSignal, OffCentre, RateTooNarrow, Interferer, UsbDrops, CpuDrops, DcSpike, IqImbalance, WrongMode, LooksFine };
enum class Fix { None, LowerGain, RaiseGain, ApplyOffset, WidenRate, SwitchMode };
enum Severity { SevInfo = 0, SevWarn = 1, SevBad = 2 };

struct Finding {
    Code code = Code::LooksFine;
    int severity = SevInfo;
    std::string text;        // one plain English sentence (the app translates by `code` and builds its own text from the numbers)
    Fix fix = Fix::None;
    double value = 0;        // LowerGain/RaiseGain: dB to change; ApplyOffset: Hz to move the capture (add to the tune offset); WidenRate: the rate in Hz
    std::string modeId, subId;   // SwitchMode: the mode (and sub-mode) to open
    double a = 0, b = 0;     // the numbers behind the sentence (clip %, offset Hz, dB ...), for the app's own text
};

struct Measure {              // the numbers behind the findings, shown in the panel's detail line
    double rmsDbfs = -120, peak = 0, clipPct = 0;
    double floorDbfs = -120;      // median noise bin, dBFS per bin
    double strongestOverFloorDb = 0;
    double strongestHz = 0;       // the strongest signal's centre from the capture centre
    double strongestBwHz = 0;
    double dcOverDb = 0, imageRejDb = 99;
};

struct Report {
    std::vector<Finding> findings;   // worst first; a single LooksFine when nothing is wrong
    Measure m;
    bool valid = false;
};

Report diagnose(const cf32* x, size_t n, const Context& c);

} // namespace dect2::doctor
