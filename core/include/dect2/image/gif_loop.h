// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Animated GIF of grey pictures (GIF89a, own LZW encoder) and the full-disk loop of a geostationary channel: the last N full disks of one
// product as a time-lapse (xrit-rx, goestools users' loops). docs/GEO-LOOP-3.9.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::img {

// frames: each w * h grey bytes; delayCs: hundredths of a second per frame; loops for ever. False + err on a write error or bad sizes.
bool gifWriteGray(const std::string& path, const std::vector<std::vector<uint8_t>>& frames, int w, int h, int delayCs, std::string& err);
// the same into memory (tests)
std::vector<uint8_t> gifEncodeGray(const std::vector<std::vector<uint8_t>>& frames, int w, int h, int delayCs);
// a reader for the encoder's own output (tests): frames back, false when the file is not a GIF it understands
bool gifDecodeGray(const std::vector<uint8_t>& gif, std::vector<std::vector<uint8_t>>& frames, int& w, int& h);

// the product a picture belongs to: its name with the digits (times, sequence numbers) taken out ("IMG_FD_023_IR105_20190907_000000" ->
// "IMG_FD__IR_"), so the frames of one channel and sector group together
std::string loopProductKey(const std::string& name);
// PNG files (oldest first) to a GIF loop: grey (luma), scaled so the longer side is at most maxSide, frames of other sizes than the newest left out
bool gifLoopFromPngs(const std::vector<std::string>& pngs, const std::string& out, int maxSide, int delayCs, std::string& err);

}  // namespace dect2::img
