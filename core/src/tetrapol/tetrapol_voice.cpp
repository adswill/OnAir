// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRAPOL voice gate, plug-in frame and per-call WAV (tetrapol_voice.h).
#include "dect2/tetrapol/tetrapol_voice.h"
#include <cstring>

namespace dect2 {
namespace tetrapol {

void packPluginFrame(const VoiceFrame& v, bool damaged, uint8_t out[kPluginFrameBytes]) {
    memset(out, 0, kPluginFrameBytes);
    for (int k = 0; k < kVoiceBits; k++)
        if (v.codec[k] & 1) out[k / 8] |= (uint8_t)(0x80u >> (k % 8));
    out[15] = damaged ? 1 : 0;
}

const char* verdictText(Verdict v) {
    switch (v) {
    case Verdict::Clear: return "voice (clear)";
    case Verdict::Encrypted: return "encrypted, not decoded";
    default: return "encryption state unknown, not decoded";
    }
}

void CallGate::onSignal(const CallSignal& s, double now) {
    if (s.kind == CallSignal::CallEnd) {
        if (active_) ended_ = true;
        active_ = false;
        verdict_ = Verdict::Unknown;
        pendingKey_ = -2;
        return;
    }
    if (s.kind != CallSignal::CallStart && s.kind != CallSignal::CallConnect) return;   // a group activation on a control channel is for another carrier
    if (active_) {
        // encrypted is sticky within a call; clear needs a message that says so; no key reference changes nothing
        if (s.keyRef >= 0) {
            if (!keyRefClear(s.keyRef)) verdict_ = Verdict::Encrypted;
            else if (verdict_ == Verdict::Unknown) verdict_ = Verdict::Clear;
        }
        return;
    }
    // before the voice: remembered for the call that starts (a key reference wins over a message without one)
    if (s.keyRef >= 0 || pendingKey_ == -2 || now - pendingAt_ > kPendingSec) pendingKey_ = s.keyRef;
    pendingAt_ = now;
}

Verdict CallGate::onVoice(double now, bool& started) {
    started = false;
    if (active_ && now - last_ > kEndSec) active_ = false;
    if (!active_) {
        active_ = true;
        started = true;
        verdict_ = Verdict::Unknown;
        if (pendingKey_ >= 0 && now - pendingAt_ <= kPendingSec)
            verdict_ = keyRefClear(pendingKey_) ? Verdict::Clear : Verdict::Encrypted;
        pendingKey_ = -2;
    }
    if (chanKey_ >= 0) {   // encrypted wins; clear only fills in an unknown state
        if (!keyRefClear(chanKey_)) verdict_ = Verdict::Encrypted;
        else if (verdict_ == Verdict::Unknown) verdict_ = Verdict::Clear;
    }
    last_ = now;
    return verdict_;
}

bool CallGate::tick(double now) {
    if (ended_) { ended_ = false; return true; }
    if (active_ && now - last_ > kEndSec) {
        active_ = false;
        verdict_ = Verdict::Unknown;
        return true;
    }
    return false;
}

static void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void wavHeader(uint8_t h[44], int rate, uint64_t samples) {
    const uint32_t data = (uint32_t)(samples * 2);
    memcpy(h, "RIFF", 4); put32(h + 4, 36 + data); memcpy(h + 8, "WAVEfmt ", 8);
    put32(h + 16, 16); put16(h + 20, 1); put16(h + 22, 1); put32(h + 24, (uint32_t)rate); put32(h + 28, (uint32_t)rate * 2);
    put16(h + 32, 2); put16(h + 34, 16); memcpy(h + 36, "data", 4); put32(h + 40, data);
}

bool WavWriter::open(const std::string& path, int rate) {
    close();
    f_ = std::fopen(path.c_str(), "wb");
    if (!f_) return false;
    path_ = path;
    rate_ = rate;
    samples_ = 0;
    uint8_t h[44];
    wavHeader(h, rate, 0);
    std::fwrite(h, 1, 44, f_);
    return true;
}

void WavWriter::write(const int16_t* pcm, size_t n) {
    if (!f_ || !pcm || !n) return;
    uint8_t b[512];
    size_t i = 0;
    while (i < n) {
        size_t k = 0;
        for (; k < sizeof b / 2 && i < n; k++, i++) put16(b + 2 * k, (uint16_t)pcm[i]);
        std::fwrite(b, 1, 2 * k, f_);
    }
    samples_ += n;
}

void WavWriter::close() {
    if (!f_) return;
    uint8_t h[44];
    wavHeader(h, rate_, samples_);
    std::fseek(f_, 0, SEEK_SET);
    std::fwrite(h, 1, 44, f_);
    std::fclose(f_);
    f_ = nullptr;
}

void knownVoiceBits(uint32_t n, uint8_t out[kVoiceBits]) {
    uint32_t s = n * 2654435761u + 12345u;
    for (int k = 0; k < kVoiceBits; k++) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        out[k] = (uint8_t)(s & 1);
    }
}

} // namespace tetrapol
} // namespace dect2
