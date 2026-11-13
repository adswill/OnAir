// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA unencrypted voice path (receive only). Frame and burst structure follow the public ETSI EN 300 392-2 descriptions
// (full slot traffic channel: two 216 bit blocks, scrambled and block interleaved over 432 bits).
//
// Rules, all mandatory:
//  1. Encrypted calls are never decoded. The encryption state is checked FIRST, before any bit of the burst is touched.
//     Unknown encryption state counts as encrypted. No key work, no cipher work of any kind.
//  2. No speech codec is part of this program. The ACELP codec is a user-installed add-on loaded at run time. This code
//     only hands the descrambled coded burst (432 type-4 bits, still interleaved, channel coding NOT removed) to a SpeechFrameSink;
//     the channel decoding of the traffic channel (tetra_traffic.h) turns that into the speech frame bits the plug-in takes.
//  3. A speech frame that is passed on is never stored by this code.
#pragma once
#include "dect2/tetra/tetra_pdu.h"
#include "dect2/tetra/tetra_phy.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dect2::tetra {

// Where coded speech frames go: the codec loader (tetra_voice_codec.h). bits: one bit per byte, the 432 descrambled type-4 bits of a burst.
// The followed timeslot comes frame by frame, in the order of the air: onCodedFrame() for a TDMA frame with a clear traffic burst,
// onMissingBurst() for one without (a pause, a stolen half slot, a burst too weak or not decoded, a slot lost where two captures meet), so that
// the sink can play the air as it was, pauses and all. The frames of a capture come in one run, once per capture; onCaptureEnd() then says that
// nothing more comes from that capture.
class SpeechFrameSink {
public:
    virtual ~SpeechFrameSink() = default;
    virtual void onCodedFrame(const uint8_t* bits, size_t n) = 0;
    virtual void onMissingBurst() {}
    virtual void onCaptureEnd() {}
};

// Collects what decodeCarrier gives a sink, to hand it on (flush) only if the carrier turns out to be the one that is played: several carriers
// of a capture can show the same call, and a carrier that gave only silence must not be heard in front of one that gave speech.
class FrameBuffer final : public SpeechFrameSink {
public:
    void onCodedFrame(const uint8_t* bits, size_t n) override { ev_.push_back({true, std::vector<uint8_t>(bits, bits + n)}); }
    void onMissingBurst() override { ev_.push_back({false, {}}); }
    size_t size() const { return ev_.size(); }
    void flush(SpeechFrameSink& to) const { for (const auto& e : ev_) { if (e.coded) to.onCodedFrame(e.bits.data(), e.bits.size()); else to.onMissingBurst(); } }
private:
    struct Ev { bool coded; std::vector<uint8_t> bits; };
    std::vector<Ev> ev_;
};

// The followed timeslot as one sequence of TDMA frames across captures: next is the speech frame index, (mn - 1) * 17 + fn - 1 in the hyperframe
// (frame 18 of a multiframe carries no speech), of the frame expected after the last one given to the sink; -1: nothing followed yet. Kept by the
// caller of decodeCarrier for the carrier it follows (key is the caller's name for it), so that the frames of the slot lost where two captures
// meet are given to the sink as silence and the pauses keep their length.
struct VoiceTimeline {
    static constexpr long kSpeechFrames = 60 * 17;   // per hyperframe
    long key = -1;
    int tn = 0;
    long next = -1;
};

enum class VoiceVerdict { Passed, EncryptedNotDecoded, StateUnknownNotDecoded };

// What the signalling of the call and the cell says about air interface encryption: encMode is the MAC-RESOURCE encryption mode
// (0 clear, anything else encrypted), known is false when no clear set-up signalling of this call was seen.
struct CallEncryption {
    bool known = false;
    int encMode = 0;
    bool cellOffersEncryption = false;   // the cell advertises the air interface encryption service (information only: a clear call on such a cell is decoded)
};
VoiceVerdict voiceVerdict(const CallEncryption& e);
const char* voiceVerdictText(VoiceVerdict v);   // "voice (clear)", "encrypted, not decoded", "encryption state unknown, not decoded"

// Per-call encryption state from the call set-up PDUs (EN 300 392-2 clause 14.7: D-SETUP and D-CONNECT carry the end-to-end encryption flag in the basic
// service information). A downlink traffic slot counts as clear ONLY when a set-up PDU for the call in that slot said "clear mode" (flag 0) and the
// state is fresh; encrypted (flag 1), never seen, expired or released all give "not known": the slot is then not decoded.
// The call is tied to the slot through the channel allocation of the MAC-RESOURCE that carried the PDU, and only when that allocation names the cell's
// main (control) carrier; traffic on any other carrier stays unknown (AMBIGUITY, left unresolved: the traffic carrier number cannot be read from the
// capture of another carrier, so those calls are never decoded). The state expires kTtlSec after the last set-up PDU for the slot (the set-up is
// repeated for late entry; a stale entry could otherwise outlive the call): a missed repeat only costs audio, never decodes an unchecked call.
// D-TX-GRANTED / D-TX-CONTINUE / D-TX-CEASED of the same call id (sent at every talk spurt of a group call) restart that time, so a long call
// is not cut off after kTtlSec; they never create an entry or change its encryption state.
class CallTracker {
public:
    static constexpr double kTtlSec = 20.0;
    void setMainCarrier(int carrierNumber) { mainCarrier_ = carrierNumber; }   // -1: unknown (every event is then ignored)
    int mainCarrier() const { return mainCarrier_; }
    void onEvent(const CallEvent& e, double nowSec);
    // known=false: no fresh set-up PDU for this downlink slot (tn 1..4) or it did not say clear; encMode 1: the set-up PDU said encrypted
    CallEncryption slotState(int tn, double nowSec) const;
    // Group and subscriber IDs of the call in this slot (0 unknown). Zero for any call not known to be clear: an encrypted call keeps no IDs.
    void slotIds(int tn, double nowSec, uint32_t& talkgroup, uint32_t& issi) const;
private:
    struct Entry { bool valid = false; int callId = 0, enc = -1; bool encSeen = false; double t = 0; int gssi = -1, issi = -1; };
    Entry slot_[4];
public:
    FragReassembler frag_[5];     // MAC-FRAG / MAC-END of each downlink timeslot (index 1 - 4; 0 for frames not tied to one)
private:
    int mainCarrier_ = -1;
};

// Which voice stream reaches the one speech codec: one carrier and one timeslot at a time. A group call is sent by every site of the network
// that has members of the group, and a capture is decoded carrier by carrier, so without this every copy of the call (and every other busy
// timeslot) went through the codec in turn and each piece of speech was heard once per copy. The first carrier of a capture (the strongest:
// carriers are decoded in level order) with a clear burst wins; that carrier and timeslot are then followed until a capture decodes the
// carrier without clear voice in the slot, or the carrier has not been heard for kHoldSec.
class VoiceStreamPicker {
public:
    static constexpr double kHoldSec = 6.0;
    void newCapture() { fed_ = false; }
    // Before decoding carrier `key` of the capture taken at `sec`: false when it must not reach the sink; else tn is the timeslot to pass
    // (-1: the first one with clear voice)
    bool offer(long key, double sec, int& tn) const;
    // After decoding a carrier that was offered: tn is the timeslot whose clear voice reached the sink (0: none)
    void heard(long key, int tn, double sec);
private:
    long key_ = -1;
    int tn_ = 0;
    double sec_ = 0;
    bool fed_ = false;
};

// One full slot traffic burst: bkn1 and bkn2 are the two 216 soft bit blocks (llr > 0 means a 1). The verdict is taken first;
// only a Passed burst is descrambled (type-5 to type-4 bits) and given to the sink (which may be null: the frame is then dropped).
VoiceVerdict processTrafficBurst(const float* bkn1, const float* bkn2, uint32_t scramblerInit, const CallEncryption& e, SpeechFrameSink* sink);

} // namespace dect2::tetra
