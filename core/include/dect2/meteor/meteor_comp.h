// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Meteor-M LRPT colour composites: which MSU-MR channels go to red, green and blue for each view of the app, and the row of pixels made from them.
// Channel numbers here are 0 .. 5 = MSU-MR channel 1 .. 6 = APID 64 .. 69 (meteor_proto.h). Meteor-M N2-3 and N2-4 send APID 64, 65 and 67 (and 68 at
// night); channels 1 and 2 (APID 64 0.5 - 0.7 um and 65 0.7 - 1.1 um, the AVHRR-like visible and near-infrared channels) are always there.
// The views are saved in the settings by their number: new ones go at the end.
#pragma once
#include "dect2/meteor/meteor_tel.h"
#include <array>
#include <cstdint>

namespace dect2 {

// 0 automatic, 1 composite 123, 2 composite 125, 3 .. 8 channel 1 .. 6, 9 composite 124, 10 vegetation (ch2, ch1, ch1),
// 11 AVHRR 221 false colour (ch2, ch2, ch1: green land, blue sea; the same as SatDump's 221)
constexpr int kMeteorViewCount = 12;
constexpr int kMeteorViewVegetation = 10, kMeteorViewAvhrr221 = 11;

// the channels (0 .. 5) behind red, green and blue; one channel: the same in all three (grey). `im` (may be null) decides the automatic view.
std::array<int, 3> meteorViewChannels(const MeteorImage* im, int view);

// grey level -> colour for a single infrared channel: cold (bright) white, then red, yellow, green, cyan, blue for the warm ground. 0xAABBGGRR.
uint32_t meteorThermal(uint8_t grey);

// One strip row (8 lines, 8 * kMeteorWidth pixels, 0xAABBGGRR) of the composite `ch` through the per-channel tables lut[channel * 256 + grey] (6 x 256). A channel without
// a strip in this row is black. thermalSingle: a picture of one channel is coloured by meteorThermal.
void meteorComposeRow(const MeteorImage& im, const std::array<int, 3>& ch, int row, const uint8_t* lut, bool thermalSingle, uint32_t* px);

} // namespace dect2
