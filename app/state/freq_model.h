// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Which frequency is which (SDR++ semantics), as pure functions (tests/app/test_freq_model.cpp):
//  - the LISTENING frequency: what is being demodulated (the VFO) = the radio centre plus the VFO's offset in the captured band;
//  - the radio CENTRE: the middle of the captured band (at the antenna, after any converter is undone);
//  - the HARDWARE frequency: what the radio itself is set to (the centre through the converter, e.g. +125 MHz for a Ham It Up).
// The big readout shows the listening frequency by default; typing or scrolling it moves only the VFO while the new frequency stays inside
// the captured band, and retunes the radio (the centre onto it) only when it would leave it.
#pragma once
#include <cmath>
#include <cstdio>
#include <string>

namespace fmodel {

struct Freqs {
    double listenHz = 0, centreHz = 0, hardwareHz = 0;
    double converterHz = 0;   // hardware - centre (0: no converter)
};

// dialHz: the app's tuned frequency (a.freqMhz); vfoOffsetHz: a VFO that sits off the dial (the general receiver's channel offset);
// centreHz: the radio centre at the antenna; hardwareHz: what the radio is set to (the converter applied; = centre without one)
inline Freqs compute(double dialHz, double vfoOffsetHz, double centreHz, double hardwareHz) {
    Freqs f;
    f.listenHz = dialHz + vfoOffsetHz;
    f.centreHz = centreHz;
    f.hardwareHz = hardwareHz;
    f.converterHz = std::fabs(hardwareHz - centreHz) > 0.5 ? hardwareHz - centreHz : 0;
    return f;
}

// "11.520.000" (Hz in groups of three, as the big readout writes it); negative frequencies keep their sign
inline std::string groupHz(double hz) {
    long long v = std::llround(std::fabs(hz));
    std::string s = std::to_string(v), o;
    for (size_t i = 0; i < s.size(); i++) { if (i && (s.size() - i) % 3 == 0) o += '.'; o += s[i]; }
    return hz < 0 ? "-" + o : o;
}
// a converter offset in plain words: "+125 MHz", "-6 MHz", "+9.75 GHz"
inline std::string offsetText(double hz) {
    char b[40];
    const double a = std::fabs(hz);
    if (a >= 1e9) snprintf(b, sizeof b, "%+.6g GHz", hz / 1e9);
    else if (a >= 1e6) snprintf(b, sizeof b, "%+.6g MHz", hz / 1e6);
    else snprintf(b, sizeof b, "%+.6g kHz", hz / 1e3);
    return b;
}
// the small line beside the big readout: the radio centre when it is not the listening frequency, the hardware frequency when a converter
// moves it. bigShowsCentre: the readout shows the centre (the option), so the small line names the listening frequency instead.
inline std::string secondaryLine(const Freqs& f, bool bigShowsCentre) {
    std::string s;
    if (std::fabs(f.listenHz - f.centreHz) > 0.5) s = bigShowsCentre ? "listening " + groupHz(f.listenHz) : "centre " + groupHz(f.centreHz);
    if (f.converterHz != 0) {
        if (!s.empty()) s += "   ";
        s += "hardware " + groupHz(f.hardwareHz) + " (converter " + offsetText(f.converterHz) + ")";
    }
    return s;
}

// A new listening frequency: inside [centre - edge*fs, centre + edge*fs] (the channel's own edges lo..hi Hz around it included) only the
// VFO moves (no radio call); outside, the radio centre goes to it and the VFO sits on the centre. sticky: the VFO is clamped into the band
// and the radio never moves (the "decode offset only" tuning style).
struct ListenPlan { bool retune; double centreHz, vfoHz; };
inline ListenPlan planListen(double hz, double centreHz, double fsHz, double chanLoHz = 0, double chanHiHz = 0, bool sticky = false, double edge = 0.45) {
    const double bandLo = centreHz - edge * fsHz, bandHi = centreHz + edge * fsHz;
    if (sticky) return {false, centreHz, std::fmin(bandHi, std::fmax(bandLo, hz))};
    if (!(fsHz > 0) || hz + chanLoHz < bandLo || hz + chanHiHz > bandHi) return {true, hz, hz};
    return {false, centreHz, hz};
}

} // namespace fmodel
