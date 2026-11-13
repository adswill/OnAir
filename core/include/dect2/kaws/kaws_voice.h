// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS voice (profiles P6, P7): the pluggable speech codec interface and the superframe assembly. KAWS does not ship a speech codec of its own: the spec's
// own-codec design (section 6) is open, and Codec2 700C is the reference. A codec is loaded by whoever integrates the mode (user-installed codecs, docs/CODECS.md);
// this file only fixes the frame contract: 40 ms of 8 kHz speech (320 samples) <-> 28 bits (P6) or 48 bits (P7).
#pragma once
#include "dect2/kaws/kaws_frame.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2::kaws {

constexpr int kVoiceFrameSamples = 320;   // 40 ms at 8 kHz

class VoiceCodec {
public:
    virtual ~VoiceCodec() = default;
    virtual const char* name() const = 0;
    // Bits per 40 ms frame this codec produces for the profile (28 for P6, 48 for P7), 0 when the profile is not supported.
    virtual int bitsPerFrame(int profile) const = 0;
    virtual Bits encode(const int16_t* pcm320, int profile) = 0;                           // bitsPerFrame(profile) bits
    virtual void decode(const Bits& bits, int profile, int16_t* pcm320) = 0;              // 320 samples
};

// A deterministic stand-in used by the tests: it quantises the frame energy and dominant frequency and fills the rest of the bits from a hash of the samples.
// It is NOT a speech codec and says so in its name; it exists to exercise the framing and the radio path with changing bit patterns.
std::unique_ptr<VoiceCodec> makeTestCodec();

// Collects PCM into 15-frame superframes (600 ms). One call to push() with any number of samples returns the finished blocks (K bits for the profile).
class VoiceTx {
public:
    VoiceTx(VoiceCodec& codec, int profile, std::string call, uint8_t callNumber) : codec_(codec), profile_(profile), call_(std::move(call)), callNo_(callNumber) {}
    std::vector<Bits> push(const int16_t* pcm, size_t n);
    void setSlowData(const Bits& b108) { slow_ = b108; slow_.resize(108, 0); }   // the slice sent with the next superframe
private:
    VoiceCodec& codec_;
    int profile_;
    std::string call_;
    uint8_t callNo_;
    Bits slow_ = Bits(108, 0);
    std::vector<int16_t> pending_;
    std::vector<Bits> frames_;
};

struct VoiceRxResult {
    bool sideOk = false;
    VoiceSide side;
    std::vector<int16_t> pcm;   // 15 x 320 samples
};
VoiceRxResult voiceReceive(VoiceCodec& codec, int profile, const Bits& block);

}  // namespace dect2::kaws
