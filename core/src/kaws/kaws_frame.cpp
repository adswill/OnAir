// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAWS framing. See kaws_frame.h for the decisions where the spec is open.
#include "dect2/kaws/kaws_frame.h"
#include "dect2/kris/kris_common.h"
#include "dect2/rs/kaws.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2::kaws {

// ---------------------------------------------------------------- helpers

static uint16_t crcBits(const Bits& b, size_t from, size_t n) {
    std::vector<uint8_t> bytes((n + 7) / 8, 0);
    for (size_t i = 0; i < n; i++)
        if (b[from + i] & 1) bytes[i >> 3] = (uint8_t)(bytes[i >> 3] | (0x80u >> (i & 7)));
    return kris::crc16(bytes.data(), bytes.size());
}

static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

// ---------------------------------------------------------------- callsign, grid, hash, text

static int callIndex(char c) {
    c = upper(c);
    for (int i = 0; i < 40; i++)
        if (kCallAlphabet[i] == c) return i;
    return -1;
}

bool callsignValid(const std::string& cs) {
    if (cs.empty() || cs.size() > (size_t)kCallChars) return false;
    bool anyNonBlank = false;
    for (char c : cs) {
        int i = callIndex(c);
        if (i < 0) return false;
        if (i != 0) anyNonBlank = true;
    }
    return anyNonBlank;
}

uint64_t callsignEncode(const std::string& cs) {
    if (!callsignValid(cs)) return ~0ull;
    uint64_t v = 0;
    for (int i = 0; i < kCallChars; i++) {
        int d = (i < (int)cs.size()) ? callIndex(cs[(size_t)i]) : 0;
        v = v * 40 + (uint64_t)d;
    }
    return v;
}

std::string callsignDecode(uint64_t v) {
    char buf[kCallChars + 1];
    for (int i = kCallChars - 1; i >= 0; i--) {
        buf[i] = kCallAlphabet[v % 40];
        v /= 40;
    }
    buf[kCallChars] = 0;
    std::string s(buf);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

uint32_t recipientHash(const std::string& cs) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < kCallChars; i++) {
        char c = (i < (int)cs.size()) ? upper(cs[(size_t)i]) : ' ';
        h ^= (uint8_t)c;
        h *= 16777619u;
    }
    return (h ^ (h >> 22)) & 0x3FFFFFu;
}

bool gridEncode(const std::string& g, uint32_t& v) {
    if (g.size() != 6) return false;
    int a = upper(g[0]) - 'A', b = upper(g[1]) - 'A', c = g[2] - '0', d = g[3] - '0', e = upper(g[4]) - 'A', f = upper(g[5]) - 'A';
    if (a < 0 || a > 17 || b < 0 || b > 17 || c < 0 || c > 9 || d < 0 || d > 9 || e < 0 || e > 23 || f < 0 || f > 23) return false;
    v = (uint32_t)((((a * 18 + b) * 10 + c) * 10 + d) * 24 + e) * 24 + (uint32_t)f;
    return true;
}

std::string gridDecode(uint32_t v) {
    if (v >= 18662400u) return std::string();
    int f = (int)(v % 24); v /= 24;
    int e = (int)(v % 24); v /= 24;
    int d = (int)(v % 10); v /= 10;
    int c = (int)(v % 10); v /= 10;
    int b = (int)(v % 18); v /= 18;
    int a = (int)v;
    std::string s;
    s.push_back((char)('A' + a)); s.push_back((char)('A' + b)); s.push_back((char)('0' + c)); s.push_back((char)('0' + d)); s.push_back((char)('A' + e)); s.push_back((char)('A' + f));
    return s;
}

// A-Z (0..25), 0-9 (26..35), then the spec list: space . , ? ! / - : @ + ' " ( ) % # = (36..52), then my completion ; * & $ < > [ ] _ | ~ (53..63)
static const char kText[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .,?!/-:@+'\"()%#=;*&$<>[]_|~";

int textCode(char c) {
    c = upper(c);
    for (int i = 0; i < 64; i++)
        if (kText[i] == c) return i;
    return textCode('?');
}
char textChar(int code) { return (code >= 0 && code < 64) ? kText[code] : '?'; }

// ---------------------------------------------------------------- blocks

bool makeBlock(int K, uint8_t msgId, uint8_t index, const Bits& payload, Bits& block) {
    if (K < kBlockOverhead || (int)payload.size() > K - kBlockOverhead || msgId > 15 || index > 15) return false;
    BitWriter w;
    w.put(msgId, 4);
    w.put(index, 4);
    w.bits.insert(w.bits.end(), payload.begin(), payload.end());
    w.pad((size_t)(K - kBlockCrcBits));
    uint16_t c = crcBits(w.bits, 0, w.bits.size());
    w.put(c, 16);
    block = w.bits;
    return true;
}

bool parseBlock(int K, const Bits& block, uint8_t& msgId, uint8_t& index, Bits& payload) {
    if ((int)block.size() != K || K < kBlockOverhead) return false;
    uint16_t c = crcBits(block, 0, (size_t)(K - kBlockCrcBits));
    uint16_t got = 0;
    for (int i = 0; i < 16; i++) got = (uint16_t)((got << 1) | (block[(size_t)(K - 16 + i)] & 1));
    if (c != got) return false;
    BitReader r(block);
    msgId = (uint8_t)r.get(4);
    index = (uint8_t)r.get(4);
    payload.assign(block.begin() + kBlockHeaderBits, block.begin() + (K - kBlockCrcBits));
    return true;
}

std::vector<Bits> splitMessage(int K, uint8_t msgId, const Bits& msg) {
    std::vector<Bits> out;
    int usable = K - kBlockOverhead;
    if (usable <= 0) return out;
    size_t nb = std::max<size_t>(1, (msg.size() + (size_t)usable - 1) / (size_t)usable);
    if (nb > (size_t)kMaxBlocks) return out;
    for (size_t b = 0; b < nb; b++) {
        Bits pl(msg.begin() + (long)(b * (size_t)usable), msg.begin() + (long)std::min(msg.size(), (b + 1) * (size_t)usable));
        Bits blk;
        if (!makeBlock(K, msgId, (uint8_t)b, pl, blk)) return {};
        out.push_back(blk);
    }
    return out;
}

// ---------------------------------------------------------------- frames

static void putHeader(BitWriter& w, FrameType t) {
    w.put(kVersion, 2);
    w.put((uint64_t)t, 3);
}

FrameType frameTypeOf(const Bits& payload) {
    BitReader r(payload);
    r.get(2);
    return (FrameType)r.get(3);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

size_t expectedMessageBits(const Bits& p) {
    if (p.size() < 5) return 0;
    BitReader r(p);
    if (r.get(2) != (uint64_t)kVersion) return 0;
    switch ((FrameType)r.get(3)) {
        case FrameType::BeaconShort: return 104;
        case FrameType::BeaconFull: {
            if (p.size() < 147) return 0;
            BitReader q(p, 143);
            return 147 + 6 * (size_t)q.get(4);
        }
        case FrameType::Chat: {
            if (p.size() < 8) return 0;
            uint64_t fl = r.get(3);
            size_t hdr = 8 + 48 + (((fl >> 1) & 1) ? 48 : 22) + 8 + 8 + 7;
            if (p.size() < hdr) return 0;
            BitReader q(p, hdr - 7);
            size_t n = (size_t)q.get(7);
            return hdr + n * (((fl >> 2) & 1) ? 8 : 6);
        }
        case FrameType::Data: {
            if (p.size() < 21) return 0;
            BitReader q(p, 5);
            return 5 + 16 + (size_t)q.get(16) * 8 + 24;
        }
    }
    return 0;
}

Bits encodeBeaconShort(const BeaconShort& b) {
    BitWriter w;
    putHeader(w, FrameType::BeaconShort);
    w.put(callsignEncode(b.call) & ((1ull << 48) - 1), 48);
    uint32_t g = 0;
    gridEncode(b.grid, g);
    w.put(g, 25);
    w.put((uint64_t)clampi(b.powerStep, 0, 63), 6);
    w.put((uint64_t)clampi(b.fineLat, 0, 63), 6);
    w.put((uint64_t)clampi(b.fineLon, 0, 63), 6);
    w.put((uint64_t)clampi(b.linkQuality, 0, 255), 8);
    return w.bits;   // 104 bits
}

bool decodeBeaconShort(const Bits& p, BeaconShort& b) {
    if (p.size() < 104) return false;
    BitReader r(p);
    if (r.get(2) != (uint64_t)kVersion || r.get(3) != (uint64_t)FrameType::BeaconShort) return false;
    uint64_t cs = r.get(48);
    uint32_t g = (uint32_t)r.get(25);
    b.call = callsignDecode(cs);
    b.grid = gridDecode(g);
    b.powerStep = (int)r.get(6);
    b.fineLat = (int)r.get(6);
    b.fineLon = (int)r.get(6);
    b.linkQuality = (int)r.get(8);
    return callsignValid(b.call) && !b.grid.empty();
}

Bits encodeBeaconFull(const BeaconFull& b) {
    BitWriter w;
    putHeader(w, FrameType::BeaconFull);
    w.put(callsignEncode(b.call) & ((1ull << 48) - 1), 48);
    int32_t la = (int32_t)std::lround(std::max(-90.0, std::min(90.0, b.lat)) * 8388608.0 / 90.0);
    la = std::max(-8388608, std::min(8388607, la));
    int32_t lo = (int32_t)std::lround(std::max(-180.0, std::min(180.0, b.lon)) * 16777216.0 / 180.0);
    lo = std::max(-16777216, std::min(16777215, lo));
    w.put((uint64_t)(uint32_t)la & 0xFFFFFFu, 24);
    w.put((uint64_t)(uint32_t)lo & 0x1FFFFFFu, 25);
    w.put((uint64_t)clampi(b.powerStep, 0, 255), 8);
    w.put((uint64_t)clampi(b.linkQuality, 0, 255), 8);
    uint32_t g = 0;
    gridEncode(b.grid, g);
    w.put(g, 25);
    size_t n = std::min<size_t>(b.text.size(), 10);
    w.put(n, 4);
    for (size_t i = 0; i < n; i++) w.put((uint64_t)textCode(b.text[i]), 6);
    return w.bits;
}

bool decodeBeaconFull(const Bits& p, BeaconFull& b) {
    if (p.size() < 147) return false;
    BitReader r(p);
    if (r.get(2) != (uint64_t)kVersion || r.get(3) != (uint64_t)FrameType::BeaconFull) return false;
    b.call = callsignDecode(r.get(48));
    int32_t la = (int32_t)r.get(24);
    if (la & 0x800000) la -= 0x1000000;
    int32_t lo = (int32_t)r.get(25);
    if (lo & 0x1000000) lo -= 0x2000000;
    b.lat = la * 90.0 / 8388608.0;
    b.lon = lo * 180.0 / 16777216.0;
    b.powerStep = (int)r.get(8);
    b.linkQuality = (int)r.get(8);
    b.grid = gridDecode((uint32_t)r.get(25));
    size_t n = (size_t)r.get(4);
    if (n > 10 || p.size() < 147 + n * 6) return false;
    b.text.clear();
    for (size_t i = 0; i < n; i++) b.text.push_back(textChar((int)r.get(6)));
    return callsignValid(b.call) && !b.grid.empty();
}

Bits encodeChat(const Chat& c) {
    BitWriter w;
    putHeader(w, FrameType::Chat);
    w.put(((c.utf8 ? 1u : 0u) << 2) | ((c.firstMessage ? 1u : 0u) << 1) | (c.more ? 1u : 0u), 3);
    w.put(callsignEncode(c.from) & ((1ull << 48) - 1), 48);
    if (c.firstMessage) w.put(callsignEncode(c.to) & ((1ull << 48) - 1), 48);
    else w.put(recipientHash(c.to), 22);
    w.put(c.seq, 8);
    w.put(c.ackRef, 8);
    size_t n = std::min<size_t>(c.text.size(), 100);
    w.put(n, 7);
    for (size_t i = 0; i < n; i++) {
        if (c.utf8) w.put((uint8_t)c.text[i], 8);
        else w.put((uint64_t)textCode(c.text[i]), 6);
    }
    return w.bits;
}

bool decodeChat(const Bits& m, Chat& c) {
    BitReader r(m);
    if (m.size() < 5 + 3 + 48 + 22 + 16 + 7) return false;
    if (r.get(2) != (uint64_t)kVersion || r.get(3) != (uint64_t)FrameType::Chat) return false;
    uint64_t fl = r.get(3);
    c.utf8 = (fl >> 2) & 1;
    c.firstMessage = (fl >> 1) & 1;
    c.more = fl & 1;
    c.from = callsignDecode(r.get(48));
    if (c.firstMessage) { c.to = callsignDecode(r.get(48)); c.toHash = recipientHash(c.to); }
    else { c.toHash = (uint32_t)r.get(22); c.to.clear(); }
    c.seq = (uint8_t)r.get(8);
    c.ackRef = (uint8_t)r.get(8);
    size_t n = (size_t)r.get(7);
    if (n > 100) return false;
    int cb = c.utf8 ? 8 : 6;
    if (r.pos + n * (size_t)cb > m.size()) return false;
    c.text.clear();
    for (size_t i = 0; i < n; i++) c.text.push_back(c.utf8 ? (char)r.get(8) : textChar((int)r.get(6)));
    return callsignValid(c.from);
}

Bits encodeAck(const Ack& a, int K) {
    BitWriter w;
    w.put(callsignEncode(a.from) & ((1ull << 48) - 1), 48);
    w.put(a.msgId & 15u, 4);
    w.put(a.bitmap, 32);
    w.put((uint64_t)clampi(a.snrDb + 35, 0, 63), 6);
    w.put((uint64_t)clampi(a.suggestedProfile, 0, 15), 4);
    uint16_t c = crcBits(w.bits, 0, w.bits.size());
    w.put(c, 16);
    w.pad((size_t)K);
    return w.bits;
}

bool decodeAck(const Bits& f, Ack& a) {
    if (f.size() < 110) return false;
    if (crcBits(f, 0, 94) != (uint16_t)BitReader(f, 94).get(16)) return false;
    for (size_t i = 110; i < f.size(); i++)
        if (f[i]) return false;
    BitReader r(f);
    a.from = callsignDecode(r.get(48));
    a.msgId = (uint8_t)r.get(4);
    a.bitmap = (uint32_t)r.get(32);
    a.snrDb = (int)r.get(6) - 35;
    a.suggestedProfile = (int)r.get(4);
    return callsignValid(a.from);
}

BlockKind classifyBlock(int K, const Bits& block) {
    uint8_t m, i;
    Bits p;
    if (parseBlock(K, block, m, i, p)) return BlockKind::Structured;
    Ack a;
    if (decodeAck(block, a)) return BlockKind::Ack;
    return BlockKind::Invalid;
}

Bits encodeData(const std::vector<uint8_t>& bytes) {
    BitWriter w;
    putHeader(w, FrameType::Data);
    w.put(bytes.size() & 0xFFFFu, 16);
    for (uint8_t b : bytes) w.put(b, 8);
    w.put(onair_kaws_crc24(bytes.data(), (int32_t)bytes.size()), 24);
    return w.bits;
}

bool decodeData(const Bits& m, std::vector<uint8_t>& bytes) {
    BitReader r(m);
    if (m.size() < 5 + 16 + 24) return false;
    if (r.get(2) != (uint64_t)kVersion || r.get(3) != (uint64_t)FrameType::Data) return false;
    size_t n = (size_t)r.get(16);
    if (r.pos + n * 8 + 24 > m.size()) return false;
    bytes.resize(n);
    for (size_t i = 0; i < n; i++) bytes[i] = (uint8_t)r.get(8);
    uint32_t c = (uint32_t)r.get(24);
    return c == onair_kaws_crc24(bytes.data(), (int32_t)bytes.size());
}

// ---------------------------------------------------------------- voice

int voiceCodecBits(int profile) { return profile == 6 ? 28 : (profile == 7 ? 48 : 0); }

Bits voiceSideEncode(const VoiceSide& s) {
    BitWriter w;
    w.put(callsignEncode(s.call) & ((1ull << 48) - 1), 48);
    w.put(kVersion, 2);
    w.put(2, 2);   // type: voice
    w.put(s.callNumber & 15u, 4);
    Bits sd = s.slowData;
    sd.resize(108, 0);
    w.bits.insert(w.bits.end(), sd.begin(), sd.end());
    w.put(crcBits(w.bits, 0, w.bits.size()), 16);
    return w.bits;   // 180
}

bool voiceSideDecode(const Bits& side, VoiceSide& s) {
    if (side.size() != (size_t)kVoiceSideBits) return false;
    if (crcBits(side, 0, 164) != (uint16_t)BitReader(side, 164).get(16)) return false;
    BitReader r(side);
    s.call = callsignDecode(r.get(48));
    if (r.get(2) != (uint64_t)kVersion || r.get(2) != 2) return false;
    s.callNumber = (uint8_t)r.get(4);
    s.slowData.assign(side.begin() + 56, side.begin() + 164);
    return callsignValid(s.call);
}

bool voiceBuild(int profile, const std::vector<Bits>& frames, const Bits& side, Bits& block) {
    int cb = voiceCodecBits(profile);
    if (!cb || frames.size() != (size_t)kVoiceFrames || side.size() != (size_t)kVoiceSideBits) return false;
    block.clear();
    for (int i = 0; i < kVoiceFrames; i++) {
        if ((int)frames[(size_t)i].size() != cb) return false;
        block.insert(block.end(), frames[(size_t)i].begin(), frames[(size_t)i].end());
        block.insert(block.end(), side.begin() + i * kVoiceSideBitsPerFrame, side.begin() + (i + 1) * kVoiceSideBitsPerFrame);
    }
    return true;
}

bool voiceParse(int profile, const Bits& block, std::vector<Bits>& frames, Bits& side) {
    int cb = voiceCodecBits(profile);
    if (!cb || block.size() != (size_t)(kVoiceFrames * (cb + kVoiceSideBitsPerFrame))) return false;
    frames.clear();
    side.clear();
    size_t at = 0;
    for (int i = 0; i < kVoiceFrames; i++) {
        frames.emplace_back(block.begin() + (long)at, block.begin() + (long)at + cb);
        at += (size_t)cb;
        side.insert(side.end(), block.begin() + (long)at, block.begin() + (long)at + kVoiceSideBitsPerFrame);
        at += kVoiceSideBitsPerFrame;
    }
    return true;
}

}  // namespace dect2::kaws
