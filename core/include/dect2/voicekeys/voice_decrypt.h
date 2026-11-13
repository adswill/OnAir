// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Per-call voice decryption with a user-supplied key. A receiver that has detected encryption (the DMR privacy flag, the P25/NXDN cipher
// fields) builds a Decryptor for the call from the key the store matched, then hands each voice frame's bits through decryptFrame() before
// the vocoder. When no key matches, nothing is built and nothing is decrypted — the call is still only reported, exactly as before.
//
// What is and is not public (read voice_keys.h for the full contract):
//   - The cipher primitives and the OFB / RC4 / scrambler keystream generation are from public specifications.
//   - P25 (TIA-102.AAAD) carries the message indicator (MI), algorithm id and key id in the HDU / LDU2, which the receiver passes in here;
//     the keystream runs continuously across the voice-frame bits and is reseeded from a fresh MI when one arrives.
//   - DMR enhanced/basic privacy as deployed by Motorola / Hytera uses proprietary key/IV handling that is NOT publicly published; OnAir
//     therefore applies only the generic ETSI-documented keystream over the voice bits and does not reverse-engineer the vendor framing.
//     setUnsupportedNote() carries that caveat to the UI.
//   - Hytera Basic / Enhanced Privacy: from DSD-FME (ISC, credited), hytera.h; applied to 49-bit frames, never searched or recovered.
//   - NXDN / dPMR scramblers (15-bit LFSR) are applied as publicly described; the DES / AES options use the OFB keystream.
// Nothing here tries keys against the traffic or recovers a key. Wrong-key detection is advisory only (frame sanity), never a search.
#pragma once
#include "dect2/voicekeys/voice_keys.h"
#include "dect2/voicekeys/voice_crypto.h"
#include "dect2/voicekeys/hytera.h"
#include <cstdint>
#include <memory>
#include <string>

namespace dect2 {
namespace voicekeys {

enum class DecryptStatus {
    NotEncrypted,   // the call is in the clear
    NoKey,          // encrypted, the user has no matching key: "encrypted: add your key"
    Decrypted,      // a key matched and was applied: "decrypted with key <name>"
    WrongKey,       // a key was applied but the vocoder frames look like noise: "key mismatch"
    Unsupported,    // encrypted with an algorithm/framing OnAir cannot apply (e.g. a restricted algorithm, or an unpublished vendor framing)
};
const char* decryptStatusText(DecryptStatus s);

// One decryptor per active encrypted call. Not thread-safe: it lives on the receiver's feed thread with the call.
class Decryptor {
public:
    // Build from a chosen key. frameBits is how many bits each voice frame hands in (DMR burst 216, P25 IMBE 144, NXDN/dPMR VCH 72).
    Decryptor(const VoiceKey& key, int frameBits);

    // Load / reload the message indicator or IV for the keystream. P25: the 72-bit MI (9 bytes). DMR/NXDN: whatever the standard carries,
    // left-justified into the block. Call it when the call starts and whenever a fresh MI arrives; the keystream restarts from it.
    void setIv(const uint8_t* iv, size_t n);

    // Decrypt one voice frame in place (bits are one-per-byte, 0/1). Advances the keystream. Returns false only if unsupported.
    bool decryptFrame(uint8_t* bits);

    // The receiver feeds back the vocoder's error metric for the frame it just decoded (e.g. mbelib errs/errs2, or a FEC/CRC fail count).
    // After a few frames this decides WrongKey vs Decrypted. This is display only: it never changes the key or tries another.
    void reportFrameErrors(int errs);

    // Hytera only: call at each DMR voice superframe start (the first of the 18 frames); the IV (setIv) is the 5-byte message indicator
    void startSuperframe() { if (hyt_) hyt_->startSuperframe(); }

    DecryptStatus status() const { return status_; }
    const std::string& keyName() const { return name_; }
    const std::string& unsupportedNote() const { return note_; }

    // Build the status for the no-key / not-encrypted cases without a Decryptor.
    static std::string statusLine(DecryptStatus s, const std::string& keyName = std::string());

private:
    void keystreamBytes(uint8_t* out, size_t n);
    VoiceKey key_;
    int frameBits_ = 0;
    Algorithm alg_ = Algorithm::None;
    DecryptStatus status_ = DecryptStatus::Decrypted;
    std::string name_, note_;
    // keystream state
    Rc4 rc4_;
    std::unique_ptr<HyteraDecryptor> hyt_;   // Hytera BP / Enhanced: frameBits is 49 (the vocoder bits after FEC), see hytera.h
    Des des_;
    Lfsr15 lfsr_;
    uint8_t iv_[16] = {};
    bool haveIv_ = false;
    // OFB runs block by block; keep the residue of the current block so frames of odd bit lengths stay continuous.
    uint8_t ofbFb_[16] = {};
    uint8_t ofbBuf_[16] = {};
    size_t ofbPos_ = 16;   // 16 = empty, force a fresh block
    // wrong-key heuristic
    int frames_ = 0, badFrames_ = 0;
};

// Convenience: look up the store for this call, build a Decryptor if a key matches, else return the no-key / not-encrypted status.
// `encrypted` is what the receiver already decided from the protocol fields. Returns nullptr in `out` when there is nothing to decrypt.
struct Lookup {
    DecryptStatus status = DecryptStatus::NotEncrypted;
    std::unique_ptr<Decryptor> decryptor;   // non-null only when a key matched
    std::string keyName;
};
Lookup lookupForCall(const CallKeyInfo& info, bool encrypted, int frameBits, const uint8_t* iv = nullptr, size_t ivLen = 0);

} // namespace voicekeys
} // namespace dect2
