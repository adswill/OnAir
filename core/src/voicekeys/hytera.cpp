// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hytera Basic / Enhanced Privacy key streams: see hytera.h for the source, licence and what is verified.
#include "dect2/voicekeys/hytera.h"
#include "dect2/voicekeys/voice_crypto.h"
#include <cstring>

namespace dect2 {
namespace voicekeys {

namespace {
constexpr uint64_t kSilence56 = 0xF801A99F8CE080ULL;   // AMBE+2 default silence vector as a 56-bit value (49 bits used)
}

bool hyteraIsSilence(const uint8_t b[49]) {
    for (int i = 0; i < 49; i++) if (b[i] != (uint8_t)((kSilence56 >> (55 - i)) & 1)) return false;
    return true;
}
bool hyteraIsZeroFrame(const uint8_t b[49]) {
    for (int i = 24; i < 44; i++) if (b[i]) return false;
    return true;
}

bool hyteraBpFrameKeystream(const std::vector<uint8_t>& key, int frameIndex, uint8_t out[49]) {
    const size_t nbits = key.size() * 8;
    if (!(key.size() == 5 || key.size() == 16 || key.size() == 32)) return false;
    if (frameIndex < 0 || frameIndex >= kHyteraFramesPerSuperframe) return false;
    for (int i = 0; i < 49; i++) {
        const size_t p = ((size_t)frameIndex * 49 + (size_t)i) % nbits;
        out[i] = (key[p >> 3] >> (7 - (p & 7))) & 1;
    }
    return true;
}

bool hyteraEnhancedKeystream(const std::vector<uint8_t>& key5, const uint8_t mi[5], uint8_t ks[135]) {
    if (key5.size() != 5) return false;
    Rc4 rc;
    rc.setKey(key5.data(), 5);
    uint8_t r[135];
    rc.keystream(r, 135);
    for (int i = 0; i < 135; i++) ks[i] = (uint8_t)(key5[i % 5] ^ mi[i % 5] ^ r[i]);
    return true;
}

HyteraDecryptor::HyteraDecryptor(Kind kind, const std::vector<uint8_t>& key) : kind_(kind), key_(key) {
    if (kind_ == Basic) { uint8_t t[49]; ok_ = hyteraBpFrameKeystream(key_, 0, t); }
    else ok_ = key_.size() == 5 && hyteraEnhancedKeystream(key_, mi_, ks_);
}

void HyteraDecryptor::setMessageIndicator(const uint8_t mi5[5]) {
    std::memcpy(mi_, mi5, 5);
    if (kind_ == Enhanced && ok_) hyteraEnhancedKeystream(key_, mi_, ks_);
    startSuperframe();
}

void HyteraDecryptor::startSuperframe() { frame_ = 0; bitPos_ = 0; }

bool HyteraDecryptor::applyFrame(uint8_t bits[49]) {
    if (!ok_) return false;
    const int f = frame_ < kHyteraFramesPerSuperframe ? frame_ : kHyteraFramesPerSuperframe - 1;
    frame_++;
    if (kind_ == Basic) {
        if (hyteraIsSilence(bits)) return false;
        uint8_t k[49];
        if (!hyteraBpFrameKeystream(key_, f, k)) return false;
        for (int i = 0; i < 49; i++) bits[i] ^= k[i];
        return true;
    }
    const size_t start = bitPos_;
    bitPos_ += 49;   // silence and zero frames still use up their 49 bits
    if (hyteraIsSilence(bits) || hyteraIsZeroFrame(bits)) return false;
    if (start + 49 > sizeof ks_ * 8) return false;
    for (int i = 0; i < 49; i++) { const size_t p = start + (size_t)i; bits[i] ^= (ks_[p >> 3] >> (7 - (p & 7))) & 1; }
    return true;
}

} // namespace voicekeys
} // namespace dect2
