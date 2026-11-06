#include "dect2/tetra/tetra_voice.h"
#include <algorithm>

namespace dect2::tetra {

VoiceVerdict voiceVerdict(const CallEncryption& e) {
    if (!e.known) return VoiceVerdict::StateUnknownNotDecoded;
    // A cell that only advertises the air interface encryption service (SYSINFO) is not an encrypted call: many networks offer it and still run
    // clear calls. What counts is the evidence of this call: its set-up PDU was read from clear MAC-RESOURCE signalling and did not say
    // encrypted (`known`), and no PDU of the capture was encrypted (encMode). Anything unclear stays "not decoded"; nothing is ever decrypted.
    if (e.encMode != 0) return VoiceVerdict::EncryptedNotDecoded;
    return VoiceVerdict::Passed;
}

const char* voiceVerdictText(VoiceVerdict v) {
    switch (v) {
        case VoiceVerdict::Passed: return "voice (clear)";
        case VoiceVerdict::EncryptedNotDecoded: return "encrypted, not decoded";
        default: return "encryption state unknown, not decoded";
    }
}

void CallTracker::onEvent(const CallEvent& e, double now) {
    if (e.kind == 3) {   // D-DISCONNECT / D-RELEASE: the call is over
        for (auto& s : slot_) if (s.valid && s.callId == e.callId) s.valid = false;
        return;
    }
    if (e.kind == 4) {   // D-TX-GRANTED / CONTINUE / CEASED: a call already known goes on (a long group call outlives kTtlSec); its state is kept as it is
        for (auto& s : slot_) if (s.valid && s.callId == e.callId) s.t = now;
        return;
    }
    if (e.kind != 1 && e.kind != 2) return;
    if (mainCarrier_ < 0 || e.carrier != mainCarrier_ || e.slots <= 0) return;   // not tied to a slot of this carrier: ignored
    for (int tn = 1; tn <= 4; tn++) {
        if (!(e.slots & (1 << (4 - tn)))) continue;
        Entry& s = slot_[tn - 1];
        if (s.valid && s.callId == e.callId) {
            // a cipher flag is believed only after 2 identical consecutive messages of the call (one bit-flipped PDU that passed the CRC must not mute a
            // clear call); clear is accepted at once and wins over an earlier encrypted state; "not stated" (D-CONNECT without the element) changes nothing
            if (e.enc == 1) { if (s.encSeen) s.enc = 1; s.encSeen = true; }
            else if (e.enc == 0) { s.enc = 0; s.encSeen = false; }
            if (s.enc == 1) { s.gssi = s.issi = -1; }
            else if (e.enc == 0) { if (e.gssi >= 0) s.gssi = e.gssi; if (e.issi >= 0) s.issi = e.issi; }
            s.t = now;
        } else {
            s.valid = true; s.callId = e.callId; s.enc = e.enc == 0 ? 0 : -1; s.encSeen = e.enc == 1; s.t = now;
            s.gssi = s.issi = -1;
            if (e.enc == 0) { s.gssi = e.gssi; s.issi = e.issi; }
        }
    }
}

CallEncryption CallTracker::slotState(int tn, double now) const {
    CallEncryption c;
    if (tn < 1 || tn > 4) return c;
    const Entry& s = slot_[tn - 1];
    if (!s.valid || s.enc < 0 || now - s.t > kTtlSec || now < s.t) return c;
    c.known = true;
    c.encMode = s.enc == 1 ? 1 : 0;
    return c;
}

void CallTracker::slotIds(int tn, double now, uint32_t& tg, uint32_t& issi) const {
    tg = issi = 0;
    if (tn < 1 || tn > 4) return;
    const Entry& s = slot_[tn - 1];
    if (!s.valid || s.enc != 0 || now - s.t > kTtlSec || now < s.t) return;
    if (s.gssi > 0) tg = (uint32_t)s.gssi;
    if (s.issi > 0) issi = (uint32_t)s.issi;
}

bool VoiceStreamPicker::offer(long key, double sec, int& tn) const {
    if (fed_) return false;   // this capture already gave its voice
    if (key_ >= 0 && sec - sec_ <= kHoldSec) {
        if (key != key_) return false;
        tn = tn_;
        return true;
    }
    tn = -1;
    return true;
}

void VoiceStreamPicker::heard(long key, int tn, double sec) {
    if (tn > 0) { key_ = key; tn_ = tn; sec_ = sec; fed_ = true; }
    else if (key == key_) key_ = -1;   // the followed stream was decoded and is quiet: the next call may come from anywhere
}

VoiceVerdict processTrafficBurst(const float* bkn1, const float* bkn2, uint32_t scr, const CallEncryption& e, SpeechFrameSink* sink) {
    const VoiceVerdict v = voiceVerdict(e);   // first, before the burst is read
    if (v != VoiceVerdict::Passed) return v;
    if (!sink || !bkn1 || !bkn2) return v;
    float f[432];
    std::copy(bkn1, bkn1 + 216, f);
    std::copy(bkn2, bkn2 + 216, f + 216);
    // EN 300 392-2 clause 8: the type-5 bits of the traffic channel are scrambled with the cell's colour code like the SCH/F; the block
    // interleaver of the speech channel (EN 300 395-2 clause 5.5.3, NOT the a = 103 interleaver of the control channels) is undone by the traffic decoder
    descramble(f, 432, scr);
    uint8_t bits[432];
    for (int i = 0; i < 432; i++) bits[i] = f[i] > 0.f ? 1 : 0;
    sink->onCodedFrame(bits, 432);
    return v;
}

} // namespace dect2::tetra
