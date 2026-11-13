// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Frequency converter (up-converter or LNB) in front of the radio. The app shows and takes the frequency at the antenna (the RF); the
// radio is tuned to what comes out of the converter (the IF). This is the one place the two are converted, in both directions.
//   Add     IF = RF + LO       up-converter (Ham-it-up, SpyVerter): HF at the antenna, the radio tunes LO above it
//   Sub     IF = RF - LO       down-converter, LO below the signal (LNBs, Ham It Down 1500 MHz): the spectrum keeps its direction
//   HighLo  IF = LO - RF       down-converter, LO above the signal: the spectrum comes out mirrored (swap I and Q to restore it)
// ppm: the converter's reference error, LO(actual) = LO * (1 + ppm / 1e6). invert: the spectrum is mirrored by something else as well.
// The setting lives in TuneSettings::radio (per radio, saved with it) under the keys below.
#pragma once
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct Converter {
    enum Mode { Add = 0, Sub = 1, HighLo = 2 };
    bool active = false;
    double loHz = 0;
    Mode mode = Add;
    double ppm = 0;
    bool invert = false;   // an extra mirror on top of what the mode does

    double effLoHz() const { return loHz * (1.0 + ppm * 1e-6); }
    // does the converter mirror the spectrum (so I and Q must be swapped for the signal to read the right way round)
    bool inverts() const { return active && ((mode == HighLo) != invert); }
    double rfToIf(double rf) const {
        if (!active) return rf;
        switch (mode) { case Add: return rf + effLoHz(); case Sub: return rf - effLoHz(); default: return effLoHz() - rf; }
    }
    double ifToRf(double f) const {
        if (!active) return f;
        switch (mode) { case Add: return f - effLoHz(); case Sub: return f + effLoHz(); default: return effLoHz() - f; }
    }
    // the radio's frequency would not be a real one (zero or negative)
    bool reachable(double rf) const { return rfToIf(rf) > 0; }
};

struct ConverterPreset {
    const char* id;
    const char* name;
    double loMhz;
    Converter::Mode mode;
    bool hasPpm;   // shows the ppm field (a TCXO or a reference that can be calibrated)
    bool custom;
};

inline const std::vector<ConverterPreset>& converterPresets() {
    static const std::vector<ConverterPreset> p = {
        {"none",      "None",                                      0,       Converter::Add,    false, false},
        {"hamitup",   "Ham It Up / Ham It Up Plus (125 MHz up)",   125,     Converter::Add,    true,  false},
        {"hamitdown", "Ham It Down (1500 MHz down)",               1500,    Converter::Sub,    true,  false},
        {"spyverter", "SpyVerter (120 MHz up)",                    120,     Converter::Add,    false, false},
        {"qo100",     "QO-100 LNB (9750 MHz)",                     9750,    Converter::Sub,    false, false},
        {"bullseye",  "Bullseye TCXO LNB (9750 MHz, ppm)",         9750,    Converter::Sub,    true,  false},
        {"ku-low",    "Ku universal LNB, low band (9750 MHz)",     9750,    Converter::Sub,    true,  false},
        {"ku-high",   "Ku universal LNB, high band (10600 MHz)",   10600,   Converter::Sub,    true,  false},
        {"custom",    "Custom",                                    0,       Converter::Add,    true,  true},
    };
    return p;
}

inline const ConverterPreset* findConverterPreset(const std::string& id) {
    for (const auto& p : converterPresets()) if (id == p.id) return &p;
    return nullptr;
}

// the keys in TuneSettings::radio
inline const char* convKeyPreset() { return "conv"; }
inline const char* convKeyLo()     { return "conv.lo"; }     // MHz, custom only
inline const char* convKeyMode()   { return "conv.mode"; }   // add / sub / high, custom only
inline const char* convKeyPpm()    { return "conv.ppm"; }
inline const char* convKeyInvert() { return "conv.inv"; }    // 1

inline Converter converterFromRadio(const std::map<std::string, std::string>& r) {
    Converter c;
    auto get = [&](const char* k) -> const std::string* { auto it = r.find(k); return it != r.end() ? &it->second : nullptr; };
    const std::string* id = get(convKeyPreset());
    if (!id) return c;
    const ConverterPreset* p = findConverterPreset(*id);
    if (!p || std::string(p->id) == "none") return c;
    c.active = true;
    c.loHz = p->loMhz * 1e6;
    c.mode = p->mode;
    if (p->custom) {
        if (const std::string* v = get(convKeyLo())) c.loHz = atof(v->c_str()) * 1e6;
        if (const std::string* v = get(convKeyMode())) c.mode = *v == "sub" ? Converter::Sub : *v == "high" ? Converter::HighLo : Converter::Add;
    }
    if (p->hasPpm) if (const std::string* v = get(convKeyPpm())) c.ppm = atof(v->c_str());
    if (const std::string* v = get(convKeyInvert())) c.invert = *v == "1";
    if (!(c.loHz > 0)) c.active = false;
    return c;
}

// swap I and Q in place: mirrors the spectrum
inline void swapIq(std::complex<float>* x, size_t n) {
    for (size_t i = 0; i < n; i++) x[i] = std::complex<float>(x[i].imag(), x[i].real());
}

}   // namespace dect2
