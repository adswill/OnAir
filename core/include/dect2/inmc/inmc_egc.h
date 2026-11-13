// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What a SafetyNET / FleetNET message is about, read from its service code, its priority and its text: the NAVAREA or METAREA, the kind
// (navigational warning, weather forecast, distress relay, search and rescue, ...) and the warning number ("123/26").
//
// The packet's own address bytes are NOT decoded here (their layout is not in a public specification; inmc_pkt.h shows them as hex). Mariners
// read the area from the text, where every NAVAREA / METAREA message names it ("NAVAREA IV 123/26", "METAREA 9"), and so does this.
// Facts used: the 21 NAVAREAs / METAREAs of the IMO / IHO World-Wide Navigational Warning Service (Roman numerals I to XXI), the SafetyNET
// service and priority codes of IMO's SafetyNET Manual. Nothing is taken from Scytale-C or any other decoder's source.
#pragma once
#include <string>

namespace dect2 {
namespace inmc {

struct EgcInfo {
    std::string area;      // "NAVAREA IV" / "METAREA 9" ("" when the text names none)
    int areaNumber = 0;    // 1..21, 0 = none
    std::string subject;   // "Navigational warning", "Weather forecast", "Distress relay", ... ("" for a system message)
    std::string ident;     // "123/26", "" when none
};

// roman numeral I..XXI (also 1..21 as digits) to its number, 0 when it is neither
int areaNumberFromText(const std::string& s);
std::string romanNumeral(int n);
// service: the EGC service code of the packet (inmc::serviceName), priority 0..3 (3 = distress), text: the assembled text
EgcInfo classifyEgc(int service, int priority, const std::string& text);

}  // namespace inmc
}  // namespace dect2
