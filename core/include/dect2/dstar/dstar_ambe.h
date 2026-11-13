// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AMBE (D-STAR voice) through the user's own mbelib, loaded at run time: AMBE is patented and OnAir does not ship it (docs/CODECS.md).
// Without the library the D-STAR receiver still shows the calls, the slow data and the positions, only without sound.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {

bool dstarAmbeAvailable(std::string* path = nullptr);   // mbelib was found and has the functions needed
bool dstarAmbeRescan();                                 // look again (the "Look again" button); true when found

// One decoder state: 72 voice bits in, 160 samples of 8 kHz audio out (20 ms)
class DstarAmbe {
public:
    DstarAmbe();
    ~DstarAmbe();
    bool ok() const;
    void decode(const uint8_t bits[72], int16_t pcm[160]);
    // With the reliability (0..255) of each of the 72 bits: soft-decision correction and concealment before the codec (voice/vocoder_fec.h)
    void decodeSoft(const uint8_t bits[72], const uint8_t rel[72], int16_t pcm[160]);
    void fecCounts(int* frames, int* repaired, int* concealed) const;
private:
    void decodeFrame(const char fr[4][24], int16_t pcm[160]);
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
