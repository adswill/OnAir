// Hytera DMR privacy keystreams, applied to the 49 vocoder bits of one AMBE+2 frame. Receive-side only and only with a key the user typed in.
//
// Source and licence: Hytera Basic Privacy (HBP) and Hytera Enhanced Privacy are NOT published by ETSI or Hytera. The structure here (the
// repeating key bit stream for HBP, RC4 with the key xor the message indicator for Enhanced) follows the open-source decoder DSD-FME
// (github.com/lwvmobile/dsd-fme, src/dsd_mbe.c and src/crypt-rc4.c, ISC licence "Copyright (C) 2010 DSD Author" plus the DSD-FME authors),
// which found it by analysing Hytera radios. ISC is GPL-compatible; the credit is in THIRD_PARTY.md and in the in-app credits.
// It has been checked here only against the DSD-FME algorithm (known-answer vectors in tests/voicekeys), NOT against a live Hytera
// recording: the DMR receiver does not yet hand this code the de-FEC'd 49 bit frames (see docs/KEYS.md).
// No key is ever searched for or recovered.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2 {
namespace voicekeys {

constexpr int kHyteraFramesPerSuperframe = 18;   // 6 voice bursts of 3 frames; the key stream restarts at the voice superframe start
constexpr int kAmbeInfoBits = 49;

// True when the 49 bits are the AMBE+2 "silence" frame (never encrypted by the radio: skipped, as the keystream is not applied to it).
bool hyteraIsSilence(const uint8_t bits49[49]);
// Same for the all-zero style frames some radios send (20 bits from bit 24 are zero).
bool hyteraIsZeroFrame(const uint8_t bits49[49]);

// Hytera Basic Privacy: the key is 40, 128 or 256 bits (5, 16 or 32 bytes). The key stream is the key bits repeated; frame k of the
// superframe (0..17) uses bits [49k, 49k+49) of 882 repeated bits. Returns the 49 key stream bits for that frame.
bool hyteraBpFrameKeystream(const std::vector<uint8_t>& key, int frameIndex, uint8_t out49[49]);

// Hytera Enhanced: RC4 over the 5-byte key (nothing dropped) gives 135 bytes; each byte is xored with key[i % 5] ^ mi[i % 5]. Bits of
// the stream are consumed 49 per frame, silence / zero frames skipping their 49 bits. Fills ks with the 135 bytes.
bool hyteraEnhancedKeystream(const std::vector<uint8_t>& key5, const uint8_t mi5[5], uint8_t ks135[135]);

// Stateful per-call helper: feed the frames of a superframe in order (call startSuperframe() at each superframe start).
class HyteraDecryptor {
public:
    enum Kind { Basic, Enhanced };
    HyteraDecryptor(Kind kind, const std::vector<uint8_t>& key);
    bool ok() const { return ok_; }
    void setMessageIndicator(const uint8_t mi5[5]);   // Enhanced only
    void startSuperframe();
    // xor the key stream into bits49 (one bit per byte) unless the frame is silence / zero; returns true when it was applied
    bool applyFrame(uint8_t bits49[49]);
private:
    Kind kind_;
    std::vector<uint8_t> key_;
    bool ok_ = false;
    uint8_t mi_[5] = {};
    uint8_t ks_[135] = {};
    int frame_ = 0;
    size_t bitPos_ = 0;
};

} // namespace voicekeys
} // namespace dect2
