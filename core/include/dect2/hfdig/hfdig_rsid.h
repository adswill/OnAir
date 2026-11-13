// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// fldigi's RSID (Reed-Solomon identifier): 15 symbols of 1024 samples at 11025 Hz, tones spaced 11025/1024 Hz, a 12-bit mode number coded RS(15,3) over GF(16) (the code of fldigi's rsid.cxx,
// GPL-3.0). It names the digital mode and its audio frequency exactly. Detection is matched against the listed modes; up to 2 of the 15 symbols may disagree.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct RsidMode {
    int code;
    const char* name;
    int kbdFamily;      // KbdFamily when the keyboard decoder handles it, -1 otherwise
    const char* kbdVariant;   // the variant name of kbdVariantName(); nullptr for Olivia / Contestia (which use tones and bandwidth)
    int tones, bandwidth;
};
const std::vector<RsidMode>& rsidModes();

// 15 symbols (values 0..15) for a code.
void rsidEncode(int code, uint8_t out[15]);

struct RsidHit { int code = 0; std::string name; double audioHz = 0; int distance = 0; };

class RsidDetector {
public:
    RsidDetector();
    ~RsidDetector();
    void reset();
    void feedAudio(const float* x, size_t n);     // 8000 Hz
    bool poll(RsidHit& out);                      // a mode that appeared since the last poll
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
