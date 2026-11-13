// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Motorola Type II test signal: a control channel of OSWs and analog voice channels with a tone, in one capture. For the tests and the mode's built-in generator; no real system.
#pragma once
#include "dect2/common/ring.h"
#include "dect2/smartnet/smartnet_proto.h"
#include <cstdint>
#include <vector>

namespace dect2 {

struct SmartnetGenCall {
    int chan = 0x010;
    int tg = 0x1230, src = 12345;          // tg: with the option bits cleared
    int options = 0;
    bool digital = false, group = true;
    double startSec = 1.0, endSec = 3.0;
    double toneHz = 1000, devHz = 3000;
    float amp = 0.4f;
    bool voiceCarrier = true;              // false: the grant only (an encrypted or digital call we only check the logging of)
};

struct SmartnetGenConfig {
    double rate = 480000;
    double centreHz = 851500000;
    double controlHz = 851512500;
    int controlChan = 0x014;
    int system = 0x4A2C, site = 3;
    int siteBand = 3;                      // the band field of the site broadcast (3: 800 domestic)
    int features = 0x1F;                   // wide area, digital, analog, CVSD, active: a SmartZone site
    bool wideArea = true;
    double durSec = 6;
    double noise = 0.01;
    double cfoHz = 0;
    double deviationHz = 1200;
    float controlAmp = 0.5f;
    bool invert = false;                   // the data polarity of the receiver chain
    double clockPpm = 0;
    uint32_t seed = 1;
    smartnet::Band band = smartnet::Band::Domestic800;
    smartnet::CustomPlan custom;
    double oswErrorRate = 0;               // the share of OSWs corrupted on the air (the CRC must fail them)
    bool idleInsideMessages = false;       // a lone idle sometimes sits in the middle of a two-word message
    std::vector<SmartnetGenCall> calls;
};

std::vector<smartnet::Osw> smartnetGrantOsws(const SmartnetGenCall& c);   // the two OSWs of the grant
std::vector<cf32> smartnetSystemSignal(const SmartnetGenConfig& c);

} // namespace dect2
