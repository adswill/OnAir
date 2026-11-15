// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hand-off for a platform or plug-in xHE-AAC (USAC) decoder that the core does not contain: compressed frames go out, 16-bit PCM comes back.
// Android registers its own MediaCodec decoder here (android/native/src/jni_xhe.cpp); a desktop plug-in can register one the same way later.
// OnAir ships no codec: whatever is registered is the phone's or the user's own. The DRM audio decoder asks libfdk-aac first (drm_usac_lib.h),
// then this, then FFmpeg.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 { namespace drm {

class UsacExternalDecoder {
public:
    virtual ~UsacExternalDecoder() = default;
    enum class Result { kPcm, kNoOutput, kError };   // kNoOutput: accepted, the decoder is still priming (not an error)
    virtual const char* name() const = 0;            // for the status line, e.g. "Android MediaCodec"
    // "" when this device can decode xHE-AAC, else the plain-words reason (shown as the audio status).
    virtual std::string unavailableReason() = 0;
    // `asc`: the AudioSpecificConfig (audio object type 42) built by xheAscFromSdc(). rateHz/channels: from the SDC. false + why: refused.
    virtual bool open(const std::vector<uint8_t>& asc, int rateHz, int channels, std::string& why) = 0;
    // One raw access unit (usacIndependencyFlag first frame after open() or flush()). Interleaved PCM and its rate / channels on kPcm.
    virtual Result decode(const std::vector<uint8_t>& au, std::vector<int16_t>& pcm, int& rateHz, int& channels) = 0;
    virtual void flush() = 0;                        // frames were lost: drop what is in the decoder, the next frame is independent
    virtual void close() = 0;
};

namespace usac_ext {
void setShared(std::shared_ptr<UsacExternalDecoder> d);   // nullptr removes it. Bumps generation().
std::shared_ptr<UsacExternalDecoder> shared();
uint64_t generation();
}

// The status line when a phone has no xHE-AAC decoder (API < 28 or no codec); the Android bridge uses it too.
inline const char* usacNoDecoderText() { return "This phone has no xHE-AAC decoder; DRM audio of this service cannot play"; }

}} // namespace dect2::drm
