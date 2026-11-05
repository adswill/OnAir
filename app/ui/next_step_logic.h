// "What do I do next": one plain line about how the reception is going and, when a click can help, the one fix to offer.
// Pure logic (no ImGui, no App): app/ui/ui2_shell.cpp (nextStepLine) fills In from the receiver and draws the result; tested by
// tests/app/test_next_step.cpp. Texts are N_() and go through tr() when shown. House style: plain words, no jargon, no exclamation marks.
// Honest by design: a mode whose lock state the app cannot read (lockKnown false) never gets "no signal"; it only gets the hints that
// rest on measured things (overload, too little level, lost samples, a slow computer).
#pragma once
#include <cmath>
#include <string>

#ifndef N_
#define N_(s) s
#endif

namespace nextstep {

enum class State { Stopped, Starting, Good, Weak, NoSignal, Overload, TooLow, LosingSamples, TooSlow, Unknown };
enum class Fix { None, Start, AutoGain };

struct In {
    bool running = false;
    bool hasRadio = true;        // a radio (or file / demo signal) is chosen
    bool radioHasGain = false;   // the radio has a gain to set
    bool autoGain = false;       // automatic gain is already on
    double runSec = 0;           // seconds since the receiver started
    bool lockKnown = false;      // the mode reports lock / signal quality
    bool locked = false;
    double snrDb = NAN;          // NAN = not measured
    double rmsDbfs = -120;       // level at the radio's ADC
    double clipFraction = 0;     // share of samples at full scale (0..1)
    bool overload = false;       // the gain meter says the radio is overloaded
    bool radioLoss = false;      // a live radio under its sample rate (USB, cable, driver)
    bool cpuLoss = false;        // samples lost because the computer is too slow
};

struct Out {
    State state = State::Unknown;
    Fix fix = Fix::None;
    const char* text = "";       // N_()
    const char* fixLabel = "";   // N_(); empty when fix is None
    bool needsAttention() const { return state != State::Good && state != State::Starting && state != State::Unknown; }
};

constexpr double kStartSec = 3.0;      // before this the receiver is still starting
constexpr double kSearchSec = 8.0;     // before this "no signal" is too early to say
constexpr double kLowDbfs = -70.0;     // below this almost nothing reaches the ADC with the gain where it is
constexpr double kClipFrac = 0.001;    // 0.1 % of the samples at full scale
constexpr double kWeakSnrDb = 6.0;     // locked but under this: weak

inline Out advise(const In& in) {
    Out o;
    if (!in.running) {
        o.state = State::Stopped;
        if (!in.hasRadio) { o.text = N_("No radio chosen. Pick a radio, or the demo signal, then press Start."); return o; }
        o.text = N_("Stopped. Press Start to listen.");
        o.fix = Fix::Start; o.fixLabel = N_("Start");
        return o;
    }
    if (in.runSec < kStartSec) { o.state = State::Starting; o.text = N_("Starting..."); return o; }
    // measured problems first: they explain a bad signal and need no lock to be seen
    if (in.radioLoss) { o.state = State::LosingSamples; o.text = N_("The radio is losing samples. Use a USB 3 port, a short cable, or a lower sample rate."); return o; }
    if (in.cpuLoss) { o.state = State::TooSlow; o.text = N_("This computer is too slow for this setting. Lower the sample rate or the bandwidth."); return o; }
    if (in.overload || in.clipFraction > kClipFrac) {
        o.state = State::Overload;
        if (in.radioHasGain && !in.autoGain) { o.text = N_("The signal is too strong for the radio. Lower the gain, or let Auto gain do it."); o.fix = Fix::AutoGain; o.fixLabel = N_("Auto gain"); }
        else if (in.radioHasGain) o.text = N_("The signal is too strong for the radio. Auto gain is working on it; a strong station nearby may need a filter or a smaller antenna.");
        else o.text = N_("The signal is too strong for the radio. Try a smaller antenna or move the antenna away from the transmitter.");
        return o;
    }
    if (in.lockKnown && in.locked) {
        if (!std::isnan(in.snrDb) && in.snrDb < kWeakSnrDb) {
            o.state = State::Weak;
            o.text = N_("Locked, but the signal is weak. Move the antenna, raise it, or try Auto gain.");
            if (in.radioHasGain && !in.autoGain) { o.fix = Fix::AutoGain; o.fixLabel = N_("Auto gain"); }
            return o;
        }
        o.state = State::Good; o.text = N_("Receiving well."); return o;
    }
    if (in.rmsDbfs < kLowDbfs && in.radioHasGain && !in.autoGain && in.runSec >= kSearchSec) {
        o.state = State::TooLow;
        o.text = N_("Very little reaches the radio. Raise the gain, or let Auto gain find it, and check the antenna.");
        o.fix = Fix::AutoGain; o.fixLabel = N_("Auto gain");
        return o;
    }
    if (in.lockKnown && in.runSec >= kSearchSec) {
        o.state = State::NoSignal;
        o.text = in.autoGain || !in.radioHasGain ? N_("No signal found yet. Check the antenna and the frequency.")
                                                 : N_("No signal found yet. Check the antenna and the frequency, or try Auto gain.");
        if (in.radioHasGain && !in.autoGain) { o.fix = Fix::AutoGain; o.fixLabel = N_("Auto gain"); }
        return o;
    }
    o.state = in.lockKnown ? State::Starting : State::Unknown;
    o.text = in.lockKnown ? N_("Looking for the signal...")
                          : N_("Listening. This mode does not report a lock: watch the list for decoded items.");   // never "no signal": the app cannot tell
    return o;
}

}  // namespace nextstep
