// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Choosing a radio from the list by what the user types on the command line: --list-devices prints the list, --device <index | serial |
// name part> picks one, --antenna <name> picks the antenna socket of a radio that has several (an SDRplay RSP2 lists one entry per socket,
// a SoapySDR radio one per input). Used by onair-cli (tools/dect2cli.cpp) and the app's command line (app/main.cpp). Header only, no I/O.
#pragma once
#include "dect2/source/source.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

namespace devpick_detail {
inline std::string low(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
inline bool allDigits(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; }); }
inline std::string trim(std::string s) { while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back(); size_t i = 0; while (i < s.size() && std::isspace((unsigned char)s[i])) i++; return s.substr(i); }
}

// The antenna socket of an entry: "A", "B", "Hi-Z", "Tuner 1", the SoapySDR input name; "" when the radio has no choice
inline std::string deviceAntenna(const DeviceInfo& d) {
    using namespace devpick_detail;
    const size_t t = d.soapyArgs.rfind("#antenna=");
    if (t != std::string::npos) return d.soapyArgs.substr(t + 9);
    const std::string n = d.name;
    const std::string ln = low(n);
    auto tail = [&](size_t from) {   // up to the bracket that follows ("(native, experimental)")
        const size_t e = n.find(" (", from);
        return trim(n.substr(from, e == std::string::npos ? std::string::npos : e - from));
    };
    size_t p = ln.rfind(" antenna ");
    if (p != std::string::npos) return tail(p + 9);
    p = ln.rfind(" tuner ");
    if (p != std::string::npos) return tail(p + 1);   // "Tuner 1", "Tuner 2", "Tuner 1 Hi-Z"
    p = ln.rfind(" hi-z");
    if (p != std::string::npos) return "Hi-Z";
    return "";
}

// The driver name for the list: "hackrf", the native or SoapySDR board name, "file", "synthetic", ...
inline std::string deviceDriver(const DeviceInfo& d) {
    switch (d.kind) {
    case DeviceInfo::HackRF: return "hackrf";
    case DeviceInfo::Synthetic: return "synthetic";
    case DeviceInfo::File: return "file";
    case DeviceInfo::Audio: return "audio";
    case DeviceInfo::Hamlib: return "hamlib";
    case DeviceInfo::Group: return "group";
    default: return d.board.empty() ? (d.kind == DeviceInfo::Soapy ? "soapy" : "native") : d.board;
    }
}

// One line per radio: index, driver, name, serial, antenna (tab separated, "-" for an empty field), the header line first
inline std::string deviceListText(const std::vector<DeviceInfo>& list) {
    std::string o = "index\tdriver\tname\tserial\tantenna\n";
    for (size_t i = 0; i < list.size(); i++) {
        const DeviceInfo& d = list[i];
        const std::string a = deviceAntenna(d);
        o += std::to_string(i) + "\t" + deviceDriver(d) + "\t" + d.name + "\t" + (d.serial.empty() ? "-" : d.serial) + "\t" + (a.empty() ? "-" : a) + "\n";
    }
    return o;
}

// Which entry `sel` means: an exact name, an exact serial, an index (digits, when no serial is that number), else every entry whose name,
// serial or driver holds the text (or all of its words), narrowed by `antenna` (exact, else a part of the entry's antenna). Returns the
// index, or -1 with `err` saying why (nothing found, or the candidates when it is not unique).
inline int pickDevice(const std::vector<DeviceInfo>& list, const std::string& selIn, const std::string& antennaIn, std::string& err) {
    using namespace devpick_detail;
    err.clear();
    const std::string sel = trim(selIn), ant = low(trim(antennaIn));
    auto antOk = [&](const DeviceInfo& d) {
        if (ant.empty()) return true;
        const std::string a = low(deviceAntenna(d));
        return a == ant || (!a.empty() && a.find(ant) != std::string::npos);
    };
    std::vector<int> c;
    auto describe = [&](const std::vector<int>& v) { std::string s; for (int i : v) s += "\n  " + std::to_string(i) + ": " + list[(size_t)i].name; return s; };
    if (sel.empty()) {   // only an antenna: the entries that have it
        for (size_t i = 0; i < list.size(); i++) if (list[i].isRadio() && antOk(list[i])) c.push_back((int)i);
    } else {
        for (size_t i = 0; i < list.size(); i++) if (list[i].name == sel && antOk(list[i])) c.push_back((int)i);
        if (c.empty()) for (size_t i = 0; i < list.size(); i++) if (!list[i].serial.empty() && low(list[i].serial) == low(sel) && antOk(list[i])) c.push_back((int)i);
        if (c.empty() && allDigits(sel) && sel.size() < 6) {
            const size_t k = (size_t)std::stoul(sel);
            if (k < list.size()) { if (!antOk(list[k])) { err = "device " + sel + " has no antenna \"" + antennaIn + "\""; return -1; } return (int)k; }
        }
        if (c.empty()) for (size_t i = 0; i < list.size(); i++) {
            const DeviceInfo& d = list[i];
            const std::string hay = low(d.name + " " + d.serial + " " + deviceDriver(d));
            if (hay.find(low(sel)) != std::string::npos && antOk(d)) c.push_back((int)i);
        }
        if (c.empty()) {   // all the words, in any order ("rsp2 antenna B 171007A210")
            std::vector<std::string> words; std::string w;
            for (char ch : low(sel) + " ") { if (ch == ' ' || ch == ',') { if (!w.empty()) words.push_back(w); w.clear(); } else w += ch; }
            for (size_t i = 0; i < list.size(); i++) {
                const DeviceInfo& d = list[i];
                const std::string hay = low(d.name + " " + d.serial + " " + deviceDriver(d));
                bool all = !words.empty() && antOk(d);
                for (const auto& x : words) if (hay.find(x) == std::string::npos) all = false;
                if (all) c.push_back((int)i);
            }
        }
    }
    if (c.size() == 1) return c[0];
    if (c.empty()) { err = "no radio matches \"" + selIn + "\"" + (ant.empty() ? "" : " with antenna \"" + antennaIn + "\"") + " (see --list-devices)"; return -1; }
    if (ant.empty() && !c.empty() && !list[(size_t)c[0]].serial.empty()) {   // several entries of one radio that differ in the antenna only: the first (the driver's default) is what OnAir always used
        bool same = true;
        for (int i : c) if (list[(size_t)i].serial != list[(size_t)c[0]].serial || list[(size_t)i].board != list[(size_t)c[0]].board) same = false;
        if (same) return c[0];
    }
    err = "\"" + selIn + "\" matches " + std::to_string(c.size()) + " radios, be more exact (the index from --list-devices, the serial, or --antenna):" + describe(c);
    return -1;
}

}  // namespace dect2
