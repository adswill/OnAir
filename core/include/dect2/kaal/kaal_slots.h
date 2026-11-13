// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAAL frame version 4: four independent voice slots (TDMA) in the 20 kHz channel of the 16 ksym/s profile. The slot interface for the
// voice codec (docs/KAAL-SLOTS.md): per 40 ms superframe every slot carries one payload of a fixed size per profile; the receiver hands
// it out bit-exact or as an erasure. Header only, no PHY internals (those are in kaal_v4.h).
#pragma once
#include <cstddef>
#include <cstdint>

namespace dect2::kaal::v4 {

constexpr int kSlots = 4;
constexpr double kSuperframeS = 0.040;
constexpr int kPcmPerPayload = 640;   // 16 kHz speech samples per 40 ms payload

enum Profile : uint8_t { kIdle = 0, kV80 = 1, kV60 = 2, kV48 = 3, kV32R = 4, kD68 = 5, kD26R = 6 };
constexpr int kProfiles = 7;

// LDPC information bits k of the profile's slot word (all different, so the receiver can tell the profile from the decode).
constexpr int profileK(Profile p) {
    switch (p) {
    case kV80: return 320; case kV60: return 240; case kV48: return 192; case kV32R: return 128;
    case kD68: case kIdle: return 304; case kD26R: return 136;
    }
    return 0;
}
constexpr bool profileQpsk(Profile p) { return p == kV32R || p == kD26R; }
constexpr bool profileVoice(Profile p) { return p >= kV80 && p <= kV32R; }
// Payload bits per 40 ms: the codec bits for voice, the data bytes * 8 for data (header and CRC-16 not counted).
constexpr int payloadBits(Profile p) {
    switch (p) {
    case kV80: case kV60: case kV48: case kV32R: return profileK(p);
    case kD68: case kIdle: return 34 * 8;
    case kD26R: return 13 * 8;
    }
    return 0;
}
constexpr int dataBytes(Profile p) { return (p == kD68 || p == kIdle) ? 34 : p == kD26R ? 13 : 0; }
constexpr const char* profileName(Profile p) {
    switch (p) {
    case kIdle: return "idle"; case kV80: return "V80 8.0k"; case kV60: return "V60 6.0k"; case kV48: return "V48 4.8k";
    case kV32R: return "V32R 3.2k"; case kD68: return "D68 data"; case kD26R: return "D26R data";
    }
    return "?";
}

struct SlotPayload {   // one slot of one superframe
    Profile profile = kIdle;
    bool ok = false;               // RX: the slot word decoded and (data) its CRC passed; TX: ignored
    uint16_t nbits = 0;            // payloadBits(profile)
    uint8_t bits[40] = {};         // payload MSB first, 8 bits per byte
    // data slots only (D68, D26R, idle fill): the 16-bit data header
    uint8_t dataType = 0;          // 0 fill, 1 text (UTF-8), 2 beacon frame, 3 file / generic bytes, 4..15 reserved
    uint8_t stream = 0;            // 0..15
    uint8_t seq = 0;               // per stream, wraps
};

// The voice codec plugs in here. One encoder / decoder per slot, 640 samples of 16 kHz speech per 40 ms payload. The built-in one is
// KAAL voice codec 4 (kaal_v4_kv4.cpp); registerSlotCodec() replaces it (tests).
class SlotVoiceEncoder {
public:
    virtual ~SlotVoiceEncoder() = default;
    // pcm: kPcmPerPayload samples. packed: payloadBits(p) bits MSB first (40 bytes room). False: nothing to send (e.g. silence / DTX).
    virtual bool encode(const int16_t* pcm, Profile p, uint8_t* packed) = 0;
};
class SlotVoiceDecoder {
public:
    virtual ~SlotVoiceDecoder() = default;
    // packed: the payload, or nullptr for an erasure (conceal). pcm: kPcmPerPayload samples out.
    virtual void decode(const uint8_t* packed, Profile p, int16_t* pcm) = 0;
    // The talker's slot carried no voice this superframe (DTX: the transmitter sent data instead): comfort noise / silence.
    virtual void silence(int16_t* pcm) { for (int i = 0; i < kPcmPerPayload; i++) pcm[i] = 0; }
};
struct SlotCodec {
    const char* name = nullptr;
    SlotVoiceEncoder* (*makeEncoder)() = nullptr;   // new'ed, the caller deletes it
    SlotVoiceDecoder* (*makeDecoder)() = nullptr;
    bool (*supports)(Profile) = nullptr;            // the voice profiles the codec offers
};
void registerSlotCodec(const SlotCodec& c);         // kaal_v4.cpp; the last registration wins (tests register stubs)
const SlotCodec* slotCodec();                       // the registered codec, else the built-in KAAL voice codec 4 (kv4SlotCodec)
const SlotCodec& kv4SlotCodec();                    // KAAL voice codec 4 (core/src/kaalvoice) in 40 ms in-band-typed frames

}  // namespace dect2::kaal::v4
