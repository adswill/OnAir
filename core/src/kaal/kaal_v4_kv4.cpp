// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The KAAL voice codec 4 (core/src/kaalvoice, docs/KAAL-CODEC.md) as the slot codec of the frame version 4 (kaal_slots.h): 16 kHz,
// 40 ms frames, the type (speech / SID) in band, exactly payloadBits(profile) bits per payload. A silent talker (DTX) gives NoData:
// the slot then carries data and the receiver's decoder continues with comfort noise.
#include "dect2/kaal/kaal_slots.h"
#include "dect2/kaalvoice/kaal_voice.h"
#include <cstring>
#include <vector>

namespace dect2::kaal::v4 {
namespace {

kaalvoice::Profile kvProfile(Profile p) {
    switch (p) {
    case kV80: return kaalvoice::Profile::K8_0;
    case kV48: return kaalvoice::Profile::K4_8;
    case kV32R: return kaalvoice::Profile::K3_2;
    default: return kaalvoice::Profile::K6_0;
    }
}
kaalvoice::Config kvConfig(Profile p) {
    kaalvoice::Config c;
    c.sampleRate = 16000;
    c.frameMs = 40;
    c.profile = kvProfile(p);
    c.inBandType = true;
    c.dtx = true;
    return c;
}

// one codec instance per profile, made when first needed (a slot may change profile from superframe to superframe)
struct Enc : SlotVoiceEncoder {
    std::unique_ptr<kaalvoice::Encoder> e[kProfiles];
    std::vector<uint8_t> f;
    bool encode(const int16_t* pcm, Profile p, uint8_t* packed) override {
        if (!profileVoice(p)) return false;
        if (!e[p]) e[p] = std::make_unique<kaalvoice::Encoder>(kvConfig(p));
        const kaalvoice::FrameType t = e[p]->encode(pcm, f);
        if (t == kaalvoice::FrameType::NoData || f.empty()) return false;
        std::memset(packed, 0, 40);
        std::memcpy(packed, f.data(), std::min<size_t>(40, f.size()));
        return true;
    }
};
struct Dec : SlotVoiceDecoder {
    std::unique_ptr<kaalvoice::Decoder> d[kProfiles];
    Profile last = kV60;
    kaalvoice::Decoder& get(Profile p) {
        if (!d[p]) d[p] = std::make_unique<kaalvoice::Decoder>(kvConfig(p));
        return *d[p];
    }
    void decode(const uint8_t* packed, Profile p, int16_t* pcm) override {
        if (!profileVoice(p)) p = last;
        last = p;
        kaalvoice::Decoder& k = get(p);
        if (!packed) { k.erasure(pcm); return; }
        k.decode(kaalvoice::FrameType::Speech, packed, (size_t)k.info().payloadBytes, pcm);
    }
    void silence(int16_t* pcm) override { get(last).decode(kaalvoice::FrameType::NoData, nullptr, 0, pcm); }
};
SlotVoiceEncoder* mkEnc() { return new Enc; }
SlotVoiceDecoder* mkDec() { return new Dec; }
bool supports(Profile p) { return profileVoice(p); }

}  // namespace

const SlotCodec& kv4SlotCodec() {
    static const SlotCodec c{"KAAL voice 4", mkEnc, mkDec, supports};
    return c;
}

}  // namespace dect2::kaal::v4
