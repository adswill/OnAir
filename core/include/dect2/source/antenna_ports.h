// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
// The antenna inputs of one radio: listed as one device entry per port (SDRplay "<serial>#<n>", SoapySDR "<args>#antenna=<name>").
// Header only and free of the GUI, so the grouping can be tested.
#include "source.h"
#include <cstdlib>
#include <string>
#include <vector>

namespace dect2 {

struct AntennaPort { int devIdx; std::string label; };

// The entries of the same radio as devs[idx] (itself included, in list order); empty when it has a single port.
inline std::vector<AntennaPort> antennaSiblings(const std::vector<DeviceInfo>& devs, int idx) {
    std::vector<AntennaPort> out;
    if (idx < 0 || idx >= (int)devs.size()) return out;
    const DeviceInfo& d = devs[(size_t)idx];
    const std::string tag = "#antenna=";
    auto soapyBase = [&](const DeviceInfo& x, std::string* ant) {
        const size_t p = x.soapyArgs.rfind(tag);
        if (p == std::string::npos) return std::string();
        if (ant) *ant = x.soapyArgs.substr(p + tag.size());
        return x.soapyArgs.substr(0, p);
    };
    if (d.kind == DeviceInfo::Soapy) {
        const std::string base = soapyBase(d, nullptr);
        if (base.empty()) return out;
        for (int i = 0; i < (int)devs.size(); i++) {
            std::string ant;
            if (devs[(size_t)i].kind == DeviceInfo::Soapy && soapyBase(devs[(size_t)i], &ant) == base) out.push_back({i, ant});
        }
    } else if (d.kind == DeviceInfo::Native && d.nativeArgs.find('#') != std::string::npos) {
        const std::string base = d.nativeArgs.substr(0, d.nativeArgs.find('#'));
        for (int i = 0; i < (int)devs.size(); i++) {
            const DeviceInfo& x = devs[(size_t)i];
            const size_t h = x.nativeArgs.find('#');
            if (x.kind != DeviceInfo::Native || x.board != d.board || h == std::string::npos || x.nativeArgs.substr(0, h) != base) continue;
            // the name is "<model> <serial> <port> (native, ...)": the port is what follows the serial
            const std::string n = x.name.substr(0, x.name.find(" ("));
            std::string lab;
            const size_t sp = x.serial.empty() ? std::string::npos : n.find(x.serial);
            if (sp != std::string::npos) lab = n.substr(sp + x.serial.size());
            while (!lab.empty() && lab[0] == ' ') lab.erase(0, 1);
            if (lab.rfind("antenna ", 0) == 0) lab.erase(0, 8);
            if (lab.empty()) lab = "Port " + std::to_string(atoi(x.nativeArgs.c_str() + h + 1) + 1);
            out.push_back({i, lab});
        }
    }
    if (out.size() < 2) out.clear();
    return out;
}

// Which band a port suits, where known ("" = nothing to say).
inline std::string antennaPortHint(const DeviceInfo& d, const std::string& label) {
    if (label.find("Hi-Z") != std::string::npos) return "High-impedance HF input: 1 kHz - 30 MHz (long wires, loops).";
    if (d.kind == DeviceInfo::Native && d.name.find("RSPdx") != std::string::npos) {
        if (label == "A") return "Antenna A: the full range, 1 kHz - 2 GHz.";
        if (label == "B") return "Antenna B: 1 kHz - 2 GHz (a second input with its own filter).";
        if (label == "C") return "Antenna C: up to 200 MHz (HF, VHF band II and below).";
    }
    if (d.kind == DeviceInfo::Native && d.name.find("RSP2") != std::string::npos) {
        if (label == "A") return "Antenna A: 1 kHz - 2 GHz (SMA).";
        if (label == "B") return "Antenna B: 1 kHz - 2 GHz (second SMA, no antenna power on it).";
    }
    if (d.kind == DeviceInfo::Native && d.name.find("RSPduo") != std::string::npos) {
        if (label == "Tuner 1") return "Tuner 1: 1 kHz - 2 GHz.";
        if (label == "Tuner 2") return "Tuner 2: the second tuner, 1 kHz - 2 GHz.";
    }
    return "";
}

}  // namespace dect2
