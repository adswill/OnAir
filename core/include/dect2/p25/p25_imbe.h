// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// IMBE (P25 Phase 1 voice) through the user's own mbelib, loaded at run time: IMBE is patented and OnAir does not ship it (docs/CODECS.md).
// Without the library the P25 receiver still shows the calls, unit IDs, talkgroups and the trunking system, only without sound.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

bool p25ImbeAvailable(std::string* path = nullptr);   // mbelib was found and has the IMBE functions
bool p25ImbeRescan();                                 // look again (the "Look again" button); true when found

// One decoder state: a deinterleaved IMBE 7200x4400 frame in (c0..c7, mbelib does the FEC), 160 samples of 8 kHz audio out (20 ms)
class P25Imbe {
public:
    P25Imbe();
    ~P25Imbe();
    bool ok() const;
    void decode(const char fr[8][23], int16_t pcm[160]);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
