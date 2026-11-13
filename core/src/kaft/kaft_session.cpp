// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT session layer: hashes, byte ranges, message codec, compression rule, profile stepper, sender and receiver state machines, PHY glue.
// See kaft_session.h.
#include "dect2/kaft/kaft_session.h"
#include "dect2/mesh/mesh_crypto.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace dect2::kaft {

// ================================================================ BLAKE2s-256 (RFC 7693)

namespace {
const uint32_t kB2sIv[8] = {0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u};
const uint8_t kB2sSigma[10][16] = {
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
    {14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3},
    {11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4},
    {7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8},
    {9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13},
    {2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9},
    {12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11},
    {13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10},
    {6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0},
};
inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void b2sCompress(uint32_t h[8], const uint8_t* block, uint64_t t, bool last) {
    uint32_t m[16], v[16];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)block[4 * i] | ((uint32_t)block[4 * i + 1] << 8) | ((uint32_t)block[4 * i + 2] << 16) | ((uint32_t)block[4 * i + 3] << 24);
    for (int i = 0; i < 8; i++) { v[i] = h[i]; v[i + 8] = kB2sIv[i]; }
    v[12] ^= (uint32_t)t;
    v[13] ^= (uint32_t)(t >> 32);
    if (last) v[14] = ~v[14];
    auto G = [&](int a, int b, int c, int d, uint32_t x, uint32_t y) {
        v[a] = v[a] + v[b] + x;
        v[d] = rotr32(v[d] ^ v[a], 16);
        v[c] = v[c] + v[d];
        v[b] = rotr32(v[b] ^ v[c], 12);
        v[a] = v[a] + v[b] + y;
        v[d] = rotr32(v[d] ^ v[a], 8);
        v[c] = v[c] + v[d];
        v[b] = rotr32(v[b] ^ v[c], 7);
    };
    for (int r = 0; r < 10; r++) {
        const uint8_t* s = kB2sSigma[r];
        G(0, 4, 8, 12, m[s[0]], m[s[1]]);
        G(1, 5, 9, 13, m[s[2]], m[s[3]]);
        G(2, 6, 10, 14, m[s[4]], m[s[5]]);
        G(3, 7, 11, 15, m[s[6]], m[s[7]]);
        G(0, 5, 10, 15, m[s[8]], m[s[9]]);
        G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(2, 7, 8, 13, m[s[12]], m[s[13]]);
        G(3, 4, 9, 14, m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; i++) h[i] ^= v[i] ^ v[i + 8];
}
}  // namespace

Hash32 blake2s256(const uint8_t* p, size_t n) {
    uint32_t h[8];
    for (int i = 0; i < 8; i++) h[i] = kB2sIv[i];
    h[0] ^= 0x01010020u;   // digest length 32, no key, fanout 1, depth 1
    size_t pos = 0;
    while (n - pos > 64) {
        pos += 64;
        b2sCompress(h, p + pos - 64, (uint64_t)pos, false);
    }
    uint8_t last[64];
    std::memset(last, 0, sizeof(last));
    if (n - pos) std::memcpy(last, p + pos, n - pos);
    b2sCompress(h, last, (uint64_t)n, true);
    Hash32 out{};
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) out[(size_t)(4 * i + k)] = (uint8_t)(h[i] >> (8 * k));
    return out;
}

Hash32 sha256Hash(const uint8_t* p, size_t n) {
    Hash32 out{};
    meshcrypto::sha256(p, n, out.data());
    return out;
}

Hash32 hashBytes(int code, const uint8_t* p, size_t n) { return code == 1 ? sha256Hash(p, n) : blake2s256(p, n); }

// ================================================================ RangeSet

void RangeSet::add(uint64_t a, uint64_t b) {
    if (a >= b) return;
    std::vector<Range> out;
    out.reserve(r_.size() + 1);
    bool placed = false;
    for (const Range& r : r_) {
        if (r.second < a) out.push_back(r);
        else if (r.first > b) {
            if (!placed) { out.push_back({a, b}); placed = true; }
            out.push_back(r);
        } else {
            a = std::min(a, r.first);
            b = std::max(b, r.second);
        }
    }
    if (!placed) out.push_back({a, b});
    r_.swap(out);
}

void RangeSet::remove(uint64_t a, uint64_t b) {
    if (a >= b) return;
    std::vector<Range> out;
    out.reserve(r_.size() + 1);
    for (const Range& r : r_) {
        if (r.second <= a || r.first >= b) { out.push_back(r); continue; }
        if (r.first < a) out.push_back({r.first, a});
        if (r.second > b) out.push_back({b, r.second});
    }
    r_.swap(out);
}

bool RangeSet::covers(uint64_t a, uint64_t b) const {
    if (a >= b) return true;
    for (const Range& r : r_)
        if (r.first <= a && r.second >= b) return true;
    return false;
}

uint64_t RangeSet::total() const {
    uint64_t n = 0;
    for (const Range& r : r_) n += r.second - r.first;
    return n;
}

// ================================================================ message codec

namespace {
struct Wr {
    std::vector<uint8_t> v;
    void u8(uint32_t x) { v.push_back((uint8_t)x); }
    void u16(uint32_t x) { u8(x & 0xFF); u8((x >> 8) & 0xFF); }
    void u32(uint32_t x) { u16(x & 0xFFFF); u16(x >> 16); }
    void u64(uint64_t x) { u32((uint32_t)x); u32((uint32_t)(x >> 32)); }
    void str(const std::string& s, size_t maxLen) {
        const size_t n = std::min(s.size(), maxLen);
        u8((uint32_t)n);
        v.insert(v.end(), s.begin(), s.begin() + (long)n);
    }
    void hash(const Hash32& h) { v.insert(v.end(), h.begin(), h.end()); }
    void ranges(const std::vector<RangeSet::Range>& r) {
        const size_t n = std::min(r.size(), kMaxMapRanges);
        u16((uint32_t)n);
        for (size_t i = 0; i < n; i++) { u32((uint32_t)r[i].first); u32((uint32_t)r[i].second); }
    }
};
struct Rd {
    const uint8_t* p;
    size_t n, pos = 0;
    bool ok = true;
    Rd(const std::vector<uint8_t>& m) : p(m.data()), n(m.size()) {}
    uint8_t u8() { if (pos >= n) { ok = false; return 0; } return p[pos++]; }
    uint16_t u16() { const uint16_t a = u8(); return (uint16_t)(a | ((uint16_t)u8() << 8)); }
    uint32_t u32() { const uint32_t a = u16(); return a | ((uint32_t)u16() << 16); }
    uint64_t u64() { const uint64_t a = u32(); return a | ((uint64_t)u32() << 32); }
    std::string str() {
        const size_t len = u8();
        if (pos + len > n) { ok = false; return {}; }
        std::string s((const char*)p + pos, len);
        pos += len;
        return s;
    }
    Hash32 hash() {
        Hash32 h{};
        if (pos + 32 > n) { ok = false; return h; }
        std::memcpy(h.data(), p + pos, 32);
        pos += 32;
        return h;
    }
    std::vector<RangeSet::Range> ranges() {
        std::vector<RangeSet::Range> r;
        const size_t cnt = u16();
        if (cnt > kMaxMapRanges) { ok = false; return r; }
        for (size_t i = 0; i < cnt && ok; i++) {
            const uint64_t a = u32();
            const uint64_t b = u32();
            r.push_back({a, b});
        }
        return r;
    }
};
bool expect(Rd& r, MsgType t) { return r.u8() == (uint8_t)t && r.ok; }
std::string baseName(const std::string& s) {
    const size_t k = s.find_last_of("/\\");
    return k == std::string::npos ? s : s.substr(k + 1);
}
}  // namespace

int msgType(const std::vector<uint8_t>& m) { return m.empty() ? -1 : (int)m[0]; }

std::vector<uint8_t> encode(const OpenMsg& m) {
    Wr w;
    w.u8((uint8_t)MsgType::Open);
    w.u16(m.session);
    w.str(m.callTx, 12);
    w.str(m.callRx, 12);
    w.u8(m.width);
    w.str(baseName(m.fileName), 96);
    w.u64(m.payloadSize);
    w.u64(m.origSize);
    w.u8(m.hashCode);
    w.hash(m.hash);
    w.u8(m.compression);
    w.u8(m.startProfile);
    w.u32(m.resumeOffset);
    w.u16(m.blockSize);
    return w.v;
}
bool decode(const std::vector<uint8_t>& b, OpenMsg& m) {
    Rd r(b);
    if (!expect(r, MsgType::Open)) return false;
    m.session = r.u16();
    m.callTx = r.str();
    m.callRx = r.str();
    m.width = r.u8();
    m.fileName = r.str();
    m.payloadSize = r.u64();
    m.origSize = r.u64();
    m.hashCode = r.u8();
    m.hash = r.hash();
    m.compression = r.u8();
    m.startProfile = r.u8();
    m.resumeOffset = r.u32();
    m.blockSize = r.u16();
    return r.ok;
}

std::vector<uint8_t> encode(const AcceptMsg& m) {
    Wr w;
    w.u8((uint8_t)MsgType::Accept);
    w.u16(m.session);
    w.str(m.callRx, 12);
    w.u16((uint16_t)m.esN0x10);
    w.u8(m.profile);
    w.ranges(m.have);
    return w.v;
}
bool decode(const std::vector<uint8_t>& b, AcceptMsg& m) {
    Rd r(b);
    if (!expect(r, MsgType::Accept)) return false;
    m.session = r.u16();
    m.callRx = r.str();
    m.esN0x10 = (int16_t)r.u16();
    m.profile = r.u8();
    m.have = r.ranges();
    return r.ok;
}

std::vector<uint8_t> encode(const StartMsg& m) {
    Wr w;
    w.u8((uint8_t)MsgType::Start);
    w.u16(m.session);
    w.u8(m.profile);
    return w.v;
}
bool decode(const std::vector<uint8_t>& b, StartMsg& m) {
    Rd r(b);
    if (!expect(r, MsgType::Start)) return false;
    m.session = r.u16();
    m.profile = r.u8();
    return r.ok;
}

std::vector<uint8_t> encode(const FinishMsg& m) {
    Wr w;
    w.u8((uint8_t)MsgType::Finish);
    w.u16(m.session);
    w.u64(m.payloadSize);
    w.u8(m.hashCode);
    w.hash(m.hash);
    w.hash(m.origHash);
    return w.v;
}
bool decode(const std::vector<uint8_t>& b, FinishMsg& m) {
    Rd r(b);
    if (!expect(r, MsgType::Finish)) return false;
    m.session = r.u16();
    m.payloadSize = r.u64();
    m.hashCode = r.u8();
    m.hash = r.hash();
    m.origHash = r.hash();
    return r.ok;
}

std::vector<uint8_t> encode(const VerifiedMsg& m) {
    Wr w;
    w.u8((uint8_t)MsgType::Verified);
    w.u16(m.session);
    w.u8(m.ok ? 1 : 0);
    return w.v;
}
bool decode(const std::vector<uint8_t>& b, VerifiedMsg& m) {
    Rd r(b);
    if (!expect(r, MsgType::Verified)) return false;
    m.session = r.u16();
    m.ok = r.u8() != 0;
    return r.ok;
}

std::vector<uint8_t> encode(const RejectMsg& m) {
    Wr w;
    w.u8((uint8_t)MsgType::Reject);
    w.u16(m.session);
    w.u8(m.reason);
    w.ranges(m.have);
    return w.v;
}
bool decode(const std::vector<uint8_t>& b, RejectMsg& m) {
    Rd r(b);
    if (!expect(r, MsgType::Reject)) return false;
    m.session = r.u16();
    m.reason = r.u8();
    m.have = r.ranges();
    return r.ok;
}

std::vector<uint8_t> encode(const AbortMsg& m) {
    Wr w;
    w.u8((uint8_t)MsgType::Abort);
    w.u16(m.session);
    w.u8(m.reason);
    return w.v;
}
bool decode(const std::vector<uint8_t>& b, AbortMsg& m) {
    Rd r(b);
    if (!expect(r, MsgType::Abort)) return false;
    m.session = r.u16();
    m.reason = r.u8();
    return r.ok;
}

std::vector<uint8_t> encode(const PollMsg& m) {
    Wr w;
    w.u8((uint8_t)MsgType::Poll);
    w.u16(m.session);
    w.u16(m.burstNo);
    return w.v;
}
bool decode(const std::vector<uint8_t>& b, PollMsg& m) {
    Rd r(b);
    if (!expect(r, MsgType::Poll)) return false;
    m.session = r.u16();
    m.burstNo = r.u16();
    return r.ok;
}

std::vector<uint8_t> encode(const AckMsg& m) {
    Wr w;
    const size_t nf = std::min<size_t>(m.seen.size(), (size_t)kFramesPerBurst);
    w.u8((uint8_t)MsgType::Ack);
    w.u16(m.session);
    w.u16(m.burstNo);
    w.u8((uint32_t)nf);
    w.u16((uint16_t)m.esN0x10);
    w.u8(m.lossPct);
    w.u8(m.requestedProfile);
    uint32_t mask = 0;
    for (size_t i = 0; i < nf; i++)
        if (m.seen[i]) mask |= 1u << i;
    w.u16(mask);
    for (size_t i = 0; i < nf; i++) {
        if (!m.seen[i]) continue;
        const std::vector<uint8_t> empty;
        const std::vector<uint8_t>& ok = i < m.blockOk.size() ? m.blockOk[i] : empty;
        const size_t nb = std::min<size_t>(ok.size(), 255);
        w.u8((uint32_t)nb);
        for (size_t j = 0; j < nb; j += 8) {
            uint32_t by = 0;
            for (size_t k = 0; k < 8 && j + k < nb; k++)
                if (ok[j + k]) by |= 1u << k;
            w.u8(by);
        }
    }
    return w.v;
}
bool decode(const std::vector<uint8_t>& b, AckMsg& m) {
    Rd r(b);
    if (!expect(r, MsgType::Ack)) return false;
    m.session = r.u16();
    m.burstNo = r.u16();
    const size_t nf = r.u8();
    if (nf > (size_t)kFramesPerBurst) return false;
    m.esN0x10 = (int16_t)r.u16();
    m.lossPct = r.u8();
    m.requestedProfile = r.u8();
    const uint32_t mask = r.u16();
    m.seen.assign(nf, 0);
    m.blockOk.assign(nf, {});
    for (size_t i = 0; i < nf && r.ok; i++) {
        if (!((mask >> i) & 1u)) continue;
        m.seen[i] = 1;
        const size_t nb = r.u8();
        m.blockOk[i].assign(nb, 0);
        for (size_t j = 0; j < nb; j += 8) {
            const uint32_t by = r.u8();
            for (size_t k = 0; k < 8 && j + k < nb; k++) m.blockOk[i][j + k] = (uint8_t)((by >> k) & 1u);
        }
    }
    return r.ok;
}

// ================================================================ compression rule

bool isCompressedExtension(const std::string& fileName) {
    const size_t k = fileName.find_last_of('.');
    if (k == std::string::npos) return false;
    std::string e = fileName.substr(k + 1);
    for (char& c : e) c = (char)std::tolower((unsigned char)c);
    static const char* const kExt[] = {"jpg", "jpeg", "png", "gif", "webp", "mp4", "mkv", "avi", "mov", "mp3", "aac", "m4a", "ogg", "opus", "flac", "zip", "7z",
                                       "gz", "xz", "bz2", "zst", "rar", "docx", "xlsx", "pptx", "apk", "jar"};
    for (const char* x : kExt)
        if (e == x) return true;
    return false;
}

bool compressionWorthwhile(const uint8_t* data, size_t n, const std::string& fileName, const CompressFn& compress) {
    if (!compress || n == 0 || isCompressedExtension(fileName)) return false;
    const size_t sample = std::min<size_t>(n, 65536);
    const std::vector<uint8_t> c = compress(data, sample);
    if (c.empty()) return false;
    return c.size() * 100 <= sample * 95;   // it must shrink by at least 5 percent
}

// ================================================================ stepper

int ProfileStepper::update(int cur, double esN0Db, double blockLoss) {
    const int lo = std::max(0, c_.minProfile), hi = std::min(kNumProfiles - 1, c_.maxProfile);
    cur = std::max(lo, std::min(hi, cur));
    hist_.push_back(esN0Db);
    while ((int)hist_.size() > std::max(1, c_.averageBursts)) hist_.erase(hist_.begin());
    double avg = 0;
    for (double v : hist_) avg += v;
    avg /= (double)hist_.size();
    if (hold_ > 0) hold_--;
    const bool lowSnr = avg < profileInfo(cur).reqEsN0Db + c_.downMarginDb;
    if (lowSnr || blockLoss > c_.downLoss) {
        const int steps = blockLoss > c_.downLoss2 ? 2 : 1;
        const int next = std::max(lo, cur - steps);
        good_ = 0;
        if (next < cur) hold_ = c_.hysteresis;
        return next;
    }
    if (cur < hi && avg >= profileInfo(cur + 1).reqEsN0Db + c_.upMarginDb && blockLoss < c_.upLoss) good_++;
    else good_ = 0;
    if (good_ >= c_.upBursts && hold_ == 0) {
        good_ = 0;
        return cur + 1;
    }
    return cur;
}

int startingProfile(double esN0Db, double marginDb, int maxProfile) {
    for (int p = std::min(maxProfile, kNumProfiles - 1); p > 0; p--)
        if (profileInfo(p).reqEsN0Db + marginDb <= esN0Db) return p;
    return 0;
}

// ================================================================ timing helpers

double TxBurst::seconds() const {
    double s = 0;
    for (const auto& f : frames)
        if (!f.empty()) s += frameSeconds(width, profile, (int)f.size());
    return s;
}

double ctrlFrameSeconds(Width w, size_t msgBytes) {
    const size_t per = (size_t)payloadBytes(kCtrlProfile);
    const int blocks = (int)std::max<size_t>(1, (msgBytes + 2 + per - 1) / per);
    return frameSeconds(w, kCtrlProfile, blocks);
}

// ================================================================ sender

Sender::Sender(const SenderConfig& cfg, const std::vector<uint8_t>& file) : cfg_(cfg), payload_(file) {
    origSize_ = file.size();
    origHash_ = hashBytes(cfg_.hashCode, file.data(), file.size());
    if (cfg_.compress && compressionWorthwhile(file.data(), file.size(), cfg_.fileName, cfg_.compress)) {
        std::vector<uint8_t> c = cfg_.compress(file.data(), file.size());
        if (!c.empty() && c.size() < file.size()) {
            payload_ = std::move(c);
            compression_ = cfg_.compressionId;
        }
    }
    hash_ = hashBytes(cfg_.hashCode, payload_.data(), payload_.size());
    todo_.add(0, payload_.size());
    profile_ = std::max(cfg_.minProfile, std::min(cfg_.maxProfile, cfg_.startProfile));
}

void Sender::retry(State back) {
    if (++retries_ > cfg_.maxRetries) state_ = State::Failed;
    else state_ = back;
}

TxBurst Sender::buildBurst() {
    TxBurst b;
    b.session = cfg_.session;
    b.burstNo = burstNo_;
    b.profile = profile_;
    b.width = cfg_.width;
    const size_t cap = (size_t)dataBytesPerBlock(profile_);
    const int nb = maxBlocksPerFrame(profile_, cfg_.width);
    for (int f = 0; f < cfg_.framesPerBurst && !todo_.empty(); f++) {
        std::vector<DataBlock> frame;
        for (int k = 0; k < nb && !todo_.empty(); k++) {
            const RangeSet::Range r = todo_.ranges().front();
            const uint64_t take = std::min<uint64_t>(cap, r.second - r.first);
            DataBlock blk;
            blk.offset = (uint32_t)r.first;
            blk.data.assign(payload_.begin() + (long)r.first, payload_.begin() + (long)(r.first + take));
            todo_.remove(r.first, r.first + take);
            frame.push_back(std::move(blk));
        }
        b.frames.push_back(std::move(frame));
    }
    stats_.bursts++;
    stats_.blocksSent += b.blockCount();
    stats_.dataBytesSent += b.dataBytes();
    stats_.airtime += b.seconds();
    burstNo_ = (uint16_t)((burstNo_ + 1) & 0x0FFF);
    return b;
}

SendAction Sender::next() {
    SendAction a;
    switch (state_) {
    case State::Open: {
        OpenMsg o;
        o.session = cfg_.session;
        o.callTx = cfg_.callsign;
        o.callRx = cfg_.peer;
        o.width = (uint8_t)cfg_.width;
        o.fileName = cfg_.fileName;
        o.payloadSize = payload_.size();
        o.origSize = origSize_;
        o.hashCode = (uint8_t)cfg_.hashCode;
        o.hash = hash_;
        o.compression = (uint8_t)compression_;
        o.startProfile = (uint8_t)profile_;
        o.blockSize = (uint16_t)dataBytesPerBlock(profile_);
        a.kind = SendAction::Ctrl;
        a.ctrl = encode(o);
        a.expectsReply = true;
        stats_.ctrlSent++;
        stats_.airtime += ctrlFrameSeconds(cfg_.width, a.ctrl.size());
        state_ = State::WaitAccept;
        return a;
    }
    case State::Start: {
        StartMsg s;
        s.session = cfg_.session;
        s.profile = (uint8_t)profile_;
        a.kind = SendAction::Ctrl;
        a.ctrl = encode(s);
        a.expectsReply = false;
        stats_.ctrlSent++;
        stats_.airtime += ctrlFrameSeconds(cfg_.width, a.ctrl.size());
        state_ = State::Data;
        return a;
    }
    case State::Data:
        if (todo_.empty()) {
            state_ = State::Finish;
            return next();
        }
        a.kind = SendAction::Burst;
        a.burst = buildBurst();
        inflight_ = a.burst;
        state_ = State::WaitAck;
        return a;
    case State::Poll: {
        PollMsg p;
        p.session = cfg_.session;
        p.burstNo = inflight_.burstNo;
        a.kind = SendAction::Ctrl;
        a.ctrl = encode(p);
        a.expectsReply = true;
        stats_.polls++;
        stats_.ctrlSent++;
        stats_.airtime += ctrlFrameSeconds(cfg_.width, a.ctrl.size());
        state_ = State::WaitAck;
        return a;
    }
    case State::Finish: {
        FinishMsg f;
        f.session = cfg_.session;
        f.payloadSize = payload_.size();
        f.hashCode = (uint8_t)cfg_.hashCode;
        f.hash = hash_;
        f.origHash = origHash_;
        a.kind = SendAction::Ctrl;
        a.ctrl = encode(f);
        a.expectsReply = true;
        stats_.ctrlSent++;
        stats_.airtime += ctrlFrameSeconds(cfg_.width, a.ctrl.size());
        state_ = State::WaitVerified;
        return a;
    }
    case State::Done: a.kind = SendAction::Done; return a;
    case State::Failed: a.kind = SendAction::Failed; return a;
    default: return a;   // waiting for a reply: nothing to send
    }
}

void Sender::onTimeout() {
    stats_.timeouts++;
    stats_.airtime += kTimeoutSeconds;
    switch (state_) {
    case State::WaitAccept: retry(State::Open); break;
    case State::WaitAck: retry(State::Poll); break;
    case State::WaitVerified: retry(State::Finish); break;
    default: break;
    }
}

void Sender::onReply(const std::vector<uint8_t>& msg) {
    stats_.airtime += ctrlFrameSeconds(cfg_.width, msg.size()) + 2 * kTurnaroundSeconds;
    const int t = msgType(msg);
    if (t == (int)MsgType::Abort) {
        AbortMsg a;
        if (decode(msg, a) && a.session == cfg_.session) state_ = State::Failed;
        return;
    }
    switch (state_) {
    case State::WaitAccept: {
        AcceptMsg a;
        if (t == (int)MsgType::Reject) { state_ = State::Failed; return; }
        if (t != (int)MsgType::Accept || !decode(msg, a) || a.session != cfg_.session) return;
        for (const auto& r : a.have) {
            const uint64_t e = std::min<uint64_t>(r.second, payload_.size());
            acked_.add(r.first, e);
            todo_.remove(r.first, e);
        }
        if (!cfg_.fixedProfile) profile_ = std::max(cfg_.minProfile, std::min(cfg_.maxProfile, (int)a.profile));
        retries_ = 0;
        state_ = State::Start;
        return;
    }
    case State::WaitAck: {
        AckMsg a;
        if (t != (int)MsgType::Ack || !decode(msg, a) || a.session != cfg_.session || a.burstNo != inflight_.burstNo) return;
        for (size_t i = 0; i < inflight_.frames.size(); i++) {
            for (size_t j = 0; j < inflight_.frames[i].size(); j++) {
                const DataBlock& blk = inflight_.frames[i][j];
                const bool ok = i < a.seen.size() && a.seen[i] && j < a.blockOk[i].size() && a.blockOk[i][j];
                if (ok) acked_.add(blk.offset, blk.offset + blk.data.size());
                else {
                    todo_.add(blk.offset, blk.offset + blk.data.size());
                    stats_.blocksLost++;
                }
            }
        }
        if (!cfg_.fixedProfile) profile_ = std::max(cfg_.minProfile, std::min(cfg_.maxProfile, (int)a.requestedProfile));
        retries_ = 0;
        inflight_ = TxBurst();
        state_ = State::Data;
        return;
    }
    case State::WaitVerified: {
        if (t == (int)MsgType::Verified) {
            VerifiedMsg v;
            if (!decode(msg, v) || v.session != cfg_.session) return;
            state_ = v.ok ? State::Done : State::Failed;
            return;
        }
        if (t == (int)MsgType::Reject) {
            RejectMsg r;
            if (!decode(msg, r) || r.session != cfg_.session) return;
            todo_.clear();
            todo_.add(0, payload_.size());
            acked_.clear();
            if (r.reason == 0) {
                for (const auto& x : r.have) {
                    const uint64_t e = std::min<uint64_t>(x.second, payload_.size());
                    todo_.remove(x.first, e);
                    acked_.add(x.first, e);
                }
            }
            retries_ = 0;
            state_ = State::Data;
        }
        return;
    }
    default: return;
    }
}

// ================================================================ receiver

namespace {
std::vector<RangeSet::Range> cappedRanges(const RangeSet& s) {
    std::vector<RangeSet::Range> r = s.ranges();
    if (r.size() > kMaxMapRanges) {
        std::sort(r.begin(), r.end(), [](const RangeSet::Range& a, const RangeSet::Range& b) { return (a.second - a.first) > (b.second - b.first); });
        r.resize(kMaxMapRanges);   // the smallest ranges are left out: the sender sends them again, which costs little
        std::sort(r.begin(), r.end());
    }
    return r;
}
bool sameCall(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i])) return false;
    return true;
}
}  // namespace

Receiver::Receiver(const ReceiverConfig& cfg) : cfg_(cfg), stepper_(cfg.stepper) {}

std::vector<uint8_t> Receiver::reject(uint8_t reason) {
    RejectMsg r;
    r.session = session_;
    r.reason = reason;
    if (reason == 0) r.have = cappedRanges(have_);
    else have_.clear();
    return encode(r);
}

std::vector<uint8_t> Receiver::onCtrl(const std::vector<uint8_t>& msg, double esN0Db) {
    switch (msgType(msg)) {
    case (int)MsgType::Open: {
        OpenMsg o;
        if (!decode(msg, o)) return {};
        if (!cfg_.callsign.empty() && !sameCall(o.callRx, "CQ") && !sameCall(o.callRx, cfg_.callsign)) return {};
        if (o.payloadSize > cfg_.maxPayload) return encode(AbortMsg{o.session, 3});
        if (o.compression != 0 && !cfg_.decompressors.count(o.compression)) return encode(AbortMsg{o.session, 2});
        Wr w;
        w.str(baseName(o.fileName), 96);
        w.u64(o.payloadSize);
        w.u64(o.origSize);
        w.u8(o.hashCode);
        w.hash(o.hash);
        w.u8(o.compression);
        const Hash32 kh = blake2s256(w.v.data(), w.v.size());
        const std::string key((const char*)kh.data(), kh.size());
        const bool again = (state_ == State::Accepted || state_ == State::Started) && key == key_ && o.session == session_;
        if (!again) {
            if ((state_ == State::Accepted || state_ == State::Started) && !complete_) {
                Held h;
                h.buf = std::move(buf_);
                h.have = have_;
                held_[key_] = std::move(h);
            }
            auto it = held_.find(key);
            if (it != held_.end()) {
                buf_ = std::move(it->second.buf);
                have_ = it->second.have;
                held_.erase(it);
            } else {
                buf_.assign((size_t)o.payloadSize, 0);
                have_.clear();
            }
            key_ = key;
            session_ = o.session;
            open_ = o;
            name_ = baseName(o.fileName);
            peer_ = o.callTx;
            complete_ = false;
            file_.clear();
            state_ = State::Accepted;
            stepper_.reset();
            lastAck_.clear();
            lastBurst_ = 0xFFFF;
        }
        AcceptMsg a;
        a.session = o.session;
        a.callRx = cfg_.callsign;
        const bool known = std::isfinite(esN0Db);
        a.esN0x10 = known ? (int16_t)std::lround(std::max(-300.0, std::min(600.0, esN0Db * 10.0))) : 0;
        a.profile = (uint8_t)(known ? startingProfile(esN0Db, 2.0, cfg_.stepper.maxProfile) : (int)o.startProfile);
        a.have = cappedRanges(have_);
        return encode(a);
    }
    case (int)MsgType::Start: {
        StartMsg s;
        if (!decode(msg, s) || s.session != session_ || state_ != State::Accepted) return {};
        state_ = State::Started;
        return {};
    }
    case (int)MsgType::Poll: {
        PollMsg p;
        if (!decode(msg, p) || p.session != session_) return {};
        return p.burstNo == lastBurst_ ? lastAck_ : std::vector<uint8_t>();
    }
    case (int)MsgType::Abort: {
        AbortMsg a;
        if (!decode(msg, a) || a.session != session_) return {};
        if ((state_ == State::Accepted || state_ == State::Started) && !complete_) {
            Held h;
            h.buf = std::move(buf_);
            h.have = have_;
            held_[key_] = std::move(h);
            buf_.clear();
            have_.clear();
        }
        state_ = State::Idle;
        return {};
    }
    case (int)MsgType::Finish: {
        FinishMsg f;
        if (!decode(msg, f) || f.session != session_) return {};
        if (state_ == State::Done && complete_) return encode(VerifiedMsg{session_, true});
        if (state_ != State::Accepted && state_ != State::Started) return {};
        const uint64_t size = open_.payloadSize;
        if (!have_.covers(0, size)) return reject(0);
        const Hash32 hp = hashBytes(f.hashCode, buf_.data(), (size_t)size);
        if (hp != f.hash || (f.hashCode == open_.hashCode && hp != open_.hash)) return reject(1);
        if (open_.compression == 0) {
            file_ = buf_;
        } else {
            const auto it = cfg_.decompressors.find(open_.compression);
            std::vector<uint8_t> out;
            if (it == cfg_.decompressors.end() || !it->second(buf_.data(), (size_t)size, out) || out.size() != open_.origSize ||
                hashBytes(f.hashCode, out.data(), out.size()) != f.origHash)
                return reject(2);
            file_ = std::move(out);
        }
        complete_ = true;
        state_ = State::Done;
        held_.erase(key_);
        return encode(VerifiedMsg{session_, true});
    }
    default: return {};
    }
}

std::vector<uint8_t> Receiver::onBurst(const RxBurst& b) {
    if ((state_ != State::Accepted && state_ != State::Started) || b.session != session_ || b.frames.empty()) return {};
    state_ = State::Started;
    AckMsg a;
    a.session = session_;
    a.burstNo = b.burstNo;
    const size_t nf = std::min<size_t>(b.frames.size(), (size_t)kFramesPerBurst);
    a.seen.assign(nf, 0);
    a.blockOk.assign(nf, {});
    size_t total = 0, failed = 0, seenFrames = 0, lostFrames = 0;
    for (size_t i = 0; i < nf; i++) {
        const RxBurstFrame& fr = b.frames[i];
        if (!fr.seen) { lostFrames++; continue; }
        a.seen[i] = 1;
        seenFrames++;
        for (const RxDataBlock& blk : fr.blocks) {
            const uint64_t end = (uint64_t)blk.blk.offset + blk.blk.data.size();
            const bool ok = blk.ok && end <= buf_.size();
            if (ok) {
                if (!blk.blk.data.empty()) std::memcpy(buf_.data() + blk.blk.offset, blk.blk.data.data(), blk.blk.data.size());
                have_.add(blk.blk.offset, end);
            }
            a.blockOk[i].push_back(ok ? 1 : 0);
            total++;
            if (!ok) failed++;
        }
    }
    // a frame whose header was lost counts with the average block number of the others
    const size_t avgBlocks = seenFrames ? (total + seenFrames / 2) / seenFrames : 0;
    const double expected = (double)total + (double)(lostFrames * avgBlocks);
    const double loss = (seenFrames == 0) ? 1.0 : (expected > 0 ? ((double)failed + (double)(lostFrames * avgBlocks)) / expected : 0.0);
    a.esN0x10 = (int16_t)std::lround(std::max(-300.0, std::min(600.0, (double)b.esN0Db * 10.0)));
    a.lossPct = (uint8_t)std::lround(std::max(0.0, std::min(100.0, loss * 100.0)));
    a.requestedProfile = (uint8_t)stepper_.update(b.profile, (double)b.esN0Db, loss);
    lastAck_ = encode(a);
    lastBurst_ = b.burstNo;
    return lastAck_;
}

// ================================================================ glue to the physical layer

std::vector<FrameSpec> framesForBurst(const TxBurst& b, const std::string& callsign) {
    std::vector<FrameSpec> out;
    for (size_t i = 0; i < b.frames.size(); i++) {
        FrameSpec fs;
        fs.header.width = b.width;
        fs.header.profile = b.profile;
        fs.header.type = FrameType::Data;
        fs.header.session = b.session;
        fs.header.frameNo = (uint16_t)(((b.burstNo & 0x0FFF) << 4) | (i & 15));
        fs.header.frameInBurst = (uint8_t)(i & 15);
        fs.header.lastInBurst = i + 1 == b.frames.size();
        fs.header.callsign = callsign;
        for (const DataBlock& blk : b.frames[i]) fs.blocks.push_back(packDataBlock(blk.offset, blk.data.data(), blk.data.size()));
        out.push_back(std::move(fs));
    }
    return out;
}

bool burstFromFrames(const std::vector<RxFrame>& frames, RxBurst& out) {
    out = RxBurst();
    bool any = false;
    double snr = 0;
    int nSnr = 0;
    for (const RxFrame& f : frames) {
        if (f.header.type != FrameType::Data) continue;
        if (!any) {
            out.session = f.header.session;
            out.burstNo = (uint16_t)((f.header.frameNo >> 4) & 0x0FFF);
            out.profile = f.header.profile;
            any = true;
        }
        if (f.header.session != out.session || ((f.header.frameNo >> 4) & 0x0FFF) != out.burstNo) continue;
        const size_t idx = f.header.frameInBurst;
        if (out.frames.size() <= idx) out.frames.resize(idx + 1);
        RxBurstFrame& fr = out.frames[idx];
        fr.seen = true;
        fr.blocks.clear();
        for (const RxBlock& rb : f.blocks) {
            RxDataBlock d;
            d.ok = rb.ok && unpackDataBlock(rb.payload, d.blk.offset, d.blk.data);
            fr.blocks.push_back(std::move(d));
        }
        snr += f.esN0Db;
        nSnr++;
    }
    out.esN0Db = nSnr ? (float)(snr / nSnr) : 0.0f;
    return any;
}

FrameSpec frameForCtrl(const std::vector<uint8_t>& msg, bool isAck, uint16_t session, uint16_t frameNo, Width w, const std::string& callsign) {
    FrameSpec fs;
    fs.header.width = w;
    fs.header.profile = kCtrlProfile;
    fs.header.type = isAck ? FrameType::Ack : FrameType::Ctrl;
    fs.header.session = session;
    fs.header.frameNo = frameNo;
    fs.header.callsign = callsign;
    fs.header.lastInBurst = true;
    std::vector<uint8_t> data;
    data.push_back((uint8_t)(msg.size() & 0xFF));
    data.push_back((uint8_t)((msg.size() >> 8) & 0xFF));
    data.insert(data.end(), msg.begin(), msg.end());
    const size_t per = (size_t)payloadBytes(kCtrlProfile);
    if ((data.size() + per - 1) / per > (size_t)kCtrlMaxBlocks) return fs;   // too long: no blocks, the builder refuses it
    for (size_t pos = 0; pos < data.size(); pos += per) {
        const size_t take = std::min(per, data.size() - pos);
        fs.blocks.emplace_back(data.begin() + (long)pos, data.begin() + (long)(pos + take));
    }
    return fs;
}

bool msgFromFrame(const RxFrame& f, std::vector<uint8_t>& msg) {
    if (f.header.type == FrameType::Data || f.blocks.empty()) return false;
    std::vector<uint8_t> data;
    for (const RxBlock& b : f.blocks) {
        if (!b.ok) return false;
        data.insert(data.end(), b.payload.begin(), b.payload.end());
    }
    if (data.size() < 2) return false;
    const size_t len = (size_t)data[0] | ((size_t)data[1] << 8);
    if (len > data.size() - 2) return false;
    msg.assign(data.begin() + 2, data.begin() + 2 + (long)len);
    return true;
}

}  // namespace dect2::kaft
