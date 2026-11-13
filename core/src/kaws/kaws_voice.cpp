// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS voice: superframe assembly and the test stand-in codec. See kaws_voice.h.
#include "dect2/kaws/kaws_voice.h"
#include <cmath>

namespace dect2::kaws {

namespace {
class TestCodec : public VoiceCodec {
public:
    const char* name() const override { return "kaws-test-pattern (not a speech codec)"; }
    int bitsPerFrame(int profile) const override { return voiceCodecBits(profile); }
    Bits encode(const int16_t* pcm, int profile) override {
        int nb = voiceCodecBits(profile);
        double e = 0;
        int zc = 0;
        uint32_t h = 2166136261u;
        for (int i = 0; i < kVoiceFrameSamples; i++) {
            e += (double)pcm[i] * pcm[i];
            if (i && ((pcm[i] >= 0) != (pcm[i - 1] >= 0))) zc++;
            h = (h ^ (uint16_t)pcm[i]) * 16777619u;
        }
        int rms = (int)std::lround(std::log2(std::sqrt(e / kVoiceFrameSamples) + 1.0) * 4.0);   // 0..63
        BitWriter w;
        w.put((uint64_t)std::min(rms, 63), 6);
        w.put((uint64_t)std::min(zc, 255), 8);
        while ((int)w.bits.size() < nb) {
            h = h * 1664525u + 1013904223u;
            w.put((h >> 16) & 1u, 1);
        }
        w.bits.resize((size_t)nb);
        return w.bits;
    }
    void decode(const Bits& bits, int profile, int16_t* pcm) override {
        (void)profile;
        BitReader r(bits);
        int rms = (int)r.get(6), zc = (int)r.get(8);
        double amp = std::pow(2.0, rms / 4.0) - 1.0;
        double f = zc * 0.5 / 0.04;   // Hz from the zero crossings of 40 ms
        for (int i = 0; i < kVoiceFrameSamples; i++) pcm[i] = (int16_t)std::lround(std::max(-32000.0, std::min(32000.0, amp * std::sin(2.0 * M_PI * f * i / 8000.0))));
    }
};
}  // namespace

std::unique_ptr<VoiceCodec> makeTestCodec() { return std::make_unique<TestCodec>(); }

std::vector<Bits> VoiceTx::push(const int16_t* pcm, size_t n) {
    std::vector<Bits> out;
    pending_.insert(pending_.end(), pcm, pcm + n);
    while (pending_.size() >= (size_t)kVoiceFrameSamples) {
        frames_.push_back(codec_.encode(pending_.data(), profile_));
        pending_.erase(pending_.begin(), pending_.begin() + kVoiceFrameSamples);
        if ((int)frames_.size() == kVoiceFrames) {
            VoiceSide s;
            s.call = call_;
            s.callNumber = callNo_;
            s.slowData = slow_;
            Bits blk;
            if (voiceBuild(profile_, frames_, voiceSideEncode(s), blk)) out.push_back(blk);
            frames_.clear();
        }
    }
    return out;
}

VoiceRxResult voiceReceive(VoiceCodec& codec, int profile, const Bits& block) {
    VoiceRxResult r;
    std::vector<Bits> frames;
    Bits side;
    if (!voiceParse(profile, block, frames, side)) return r;
    r.sideOk = voiceSideDecode(side, r.side);
    r.pcm.resize((size_t)kVoiceFrames * kVoiceFrameSamples);
    for (int i = 0; i < kVoiceFrames; i++) codec.decode(frames[(size_t)i], profile, r.pcm.data() + (size_t)i * kVoiceFrameSamples);
    return r;
}

}  // namespace dect2::kaws
