// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Pure helpers of the SoapySDR source: no SoapySDR types, so they are tested against a fake driver table without hardware.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace dect2 { namespace soapylogic {

struct Span { double lo = 0, hi = 0, step = 0; };   // a driver range: step 0 = continuous

// A value forced into a driver range and onto its step grid (rounded to the nearest step, never past the ends)
inline double clampToSpan(double v, const Span& s) {
    double c = std::min(std::max(v, s.lo), s.hi);
    if (s.step > 0 && c > s.lo) {
        c = s.lo + std::round((c - s.lo) / s.step) * s.step;
        if (c > s.hi) c = s.lo + std::floor((s.hi - s.lo) / s.step + 1e-9) * s.step;
    }
    return c;
}

struct RateChoice { double hz = 0; bool exact = false; std::string note; };

// The rate to ask for: `want` itself when the driver offers it (within 1 Hz), else the slowest offered rate above it, else the fastest.
// Unlike a silent fallback, `note` says in words what was requested and what is used. An empty table gives `want` and no note.
inline RateChoice chooseRate(const std::vector<Span>& table, double want) {
    RateChoice r;
    if (table.empty()) { r.hz = want; r.exact = true; return r; }
    double above = 0, fastest = 0;
    for (const auto& s : table) {
        if (want >= s.lo - 1.0 && want <= s.hi + 1.0) {
            const double c = clampToSpan(want, s);
            if (std::fabs(c - want) <= 1.0) { r.hz = want; r.exact = true; return r; }
        }
        double c = std::min(std::max(want, s.lo), s.hi);
        if (s.step > 0 && c > s.lo) {
            c = s.lo + std::ceil((c - s.lo) / s.step - 1e-9) * s.step;
            if (c > s.hi) c = s.lo + std::floor((s.hi - s.lo) / s.step + 1e-9) * s.step;
        }
        fastest = std::max(fastest, c);
        if (c >= want - 1.0 && (above == 0 || c < above)) above = c;
    }
    r.hz = above > 0 ? above : fastest;
    char b[200];
    if (r.hz >= want - 1.0) snprintf(b, sizeof b, "this radio does not offer %.3f Msps; %.3f Msps is used", want / 1e6, r.hz / 1e6);
    else snprintf(b, sizeof b, "this radio only offers up to %.3f Msps, %.3f Msps were requested", r.hz / 1e6, want / 1e6);
    r.note = b;
    return r;
}

// Analogue bandwidth: the offered value nearest `want` (the filter is meant to be as wide as the signal)
inline double chooseBandwidth(const std::vector<Span>& table, double want) {
    double best = 0, bd = 1e30;
    for (const auto& s : table) { const double c = clampToSpan(want, s); if (std::fabs(c - want) < bd) { bd = std::fabs(c - want); best = c; } }
    return best;
}

// How a driver's gain element is shown: its own name kept in brackets so the user can match it to the driver's documentation
struct StageName { std::string label, unit; };
inline StageName stageName(const std::string& element) {
    std::string u = element;
    for (auto& c : u) c = (char)toupper((unsigned char)c);
    static const std::map<std::string, std::string> known = {
        {"LNA", "Low-noise amplifier"}, {"VGA", "Variable-gain amplifier"}, {"TIA", "Transimpedance amplifier"}, {"PGA", "Programmable-gain amplifier"},
        {"AMP", "RF amplifier"}, {"RF", "RF gain"}, {"IF", "IF gain"}, {"BB", "Baseband gain"}, {"MIX", "Mixer gain"}, {"MIXER", "Mixer gain"},
        {"ATT", "Attenuator"}, {"IFGR", "IF gain reduction"}, {"RFGR", "RF gain reduction (LNA state)"}, {"LNAG", "LNA gain"}, {"TUNER", "Tuner gain"},
        {"PAD", "Output pad"}, {"IAMP", "Intermediate amplifier"}};
    const auto it = known.find(u);
    StageName n;
    n.label = it == known.end() ? "Gain stage " + element : it->second + " (" + element + ")";
    n.unit = "dB";
    return n;
}

// The key a stage is remembered under: per device serial and per mode, so two radios (or two modes) keep their own stage values
inline std::string stageMemoryKey(const std::string& serial, const std::string& mode, const std::string& element) {
    return "soapy/" + (serial.empty() ? std::string("noserial") : serial) + "/" + mode + "/gain." + element;
}

// Device arguments: "driver=remote,remote=tcp://host:55132,remote:driver=rtlsdr,serial=ABC" -> key/value pairs (values may hold '=' after the first)
inline std::map<std::string, std::string> parseArgs(const std::string& a) {
    std::map<std::string, std::string> m;
    size_t i = 0;
    while (i <= a.size()) {
        size_t j = a.find(',', i);
        if (j == std::string::npos) j = a.size();
        const std::string kv = a.substr(i, j - i);
        const size_t e = kv.find('=');
        if (e != std::string::npos && e > 0) m[kv.substr(0, e)] = kv.substr(e + 1);
        i = j + 1;
    }
    return m;
}
inline bool isRemote(const std::string& args) { const auto m = parseArgs(args); const auto d = m.find("driver"); return d != m.end() && d->second == "remote"; }
// The radio's driver behind a SoapyRemote link ("remote:driver"), or its own driver
inline std::string effectiveDriver(const std::string& args) {
    const auto m = parseArgs(args);
    const auto r = m.find("remote:driver");
    if (r != m.end()) return r->second;
    const auto d = m.find("driver");
    return d == m.end() ? std::string() : d->second;
}
// Arguments that pick one radio by its serial (also behind a remote link), or empty when there is none
inline std::string selectBySerial(const std::string& driver, const std::string& serial, const std::string& remoteUrl = std::string()) {
    if (serial.empty()) return {};
    if (!remoteUrl.empty()) return "driver=remote,remote=" + remoteUrl + ",remote:driver=" + driver + ",serial=" + serial;
    return "driver=" + driver + ",serial=" + serial;
}

// Names for a device list where two radios may have the same label: later ones get " (2)", " (3)"
inline std::vector<std::string> uniqueLabels(const std::vector<std::string>& labels) {
    std::map<std::string, int> seen;
    std::vector<std::string> out;
    for (const auto& l : labels) { const int n = ++seen[l]; out.push_back(n == 1 ? l : l + " (" + std::to_string(n) + ")"); }
    return out;
}

// Which radios appeared or went away between two listings (by key), for a refresh that needs no restart
struct Hotplug { std::vector<std::string> added, removed; };
inline Hotplug diffKeys(const std::vector<std::string>& before, const std::vector<std::string>& after) {
    Hotplug h;
    for (const auto& k : after) if (std::find(before.begin(), before.end(), k) == before.end()) h.added.push_back(k);
    for (const auto& k : before) if (std::find(after.begin(), after.end(), k) == after.end()) h.removed.push_back(k);
    return h;
}

// The status line: empty while the stream is clean
inline std::string streamStatus(uint64_t overflows, uint64_t underflows, int reconnects) {
    std::string s;
    auto add = [&](const std::string& t) { s += (s.empty() ? "" : ", ") + t; };
    if (overflows) add(std::to_string(overflows) + " overflow" + (overflows == 1 ? "" : "s") + " (host too slow)");
    if (underflows) add(std::to_string(underflows) + " underflow" + (underflows == 1 ? "" : "s"));
    if (reconnects) add(std::to_string(reconnects) + " reconnect" + (reconnects == 1 ? "" : "s"));
    return s;
}

// A plain explanation for the errors a driver gives when the radio is held by another program
inline std::string busyHint(const std::string& what) {
    std::string l = what;
    for (auto& c : l) c = (char)tolower((unsigned char)c);
    for (const char* k : {"busy", "in use", "claim", "resource temporarily", "already open", "access denied"})
        if (l.find(k) != std::string::npos) return "the radio is in use by another program (or another OnAir window): close it and start again";
    return {};
}

}}  // namespace dect2::soapylogic
