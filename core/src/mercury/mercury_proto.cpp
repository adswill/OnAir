// Mercury frames above the modem (see mercury_proto.h).
#include "dect2/mercury/mercury_proto.h"
#include "dect2/mercury/mercury_phy.h"
#include "dect2/common/crc.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>

namespace dect2 {
namespace mercury {

namespace {

std::string upper(const std::string& s) {
    std::string u;
    for (char c : s) u += (char)std::toupper((unsigned char)c);
    return u;
}

// ---------------------------------------------------------------- the arithmetic code of callsigns (arith.c)
// 38 symbols, each of probability 1/38: A-Z, 0-9, '-', then the end of the callsign
constexpr int kSyms = 38;
constexpr uint64_t kMax = (1ull << 32) - 1, kHalf = 1ull << 31, kQuarter = 1ull << 30, kThreeQ = kHalf + kQuarter;
const char kAlpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";

int symOf(char c) {
    for (int i = 0; i < kSyms - 1; i++) if (kAlpha[i] == c) return i;
    return -1;
}

struct BitOut {
    std::vector<uint8_t> b;
    int bits = 0;
    void put(int v) {
        if ((bits & 7) == 0) b.push_back(0);
        if (v) b.back() |= (uint8_t)(0x80 >> (bits & 7));
        bits++;
    }
};

// ---------------------------------------------------------------- GF(256), polynomial 0x11D (rs_erasure.c)
struct Gf {
    uint8_t exp[512], log[256];
    Gf() {
        int x = 1;
        for (int i = 0; i < 255; i++) {
            exp[i] = (uint8_t)x; log[x] = (uint8_t)i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; i++) exp[i] = exp[i - 255];
        log[0] = 0;
    }
    uint8_t mul(uint8_t a, uint8_t b) const { return (a && b) ? exp[log[a] + log[b]] : 0; }
    uint8_t inv(uint8_t a) const { return exp[255 - log[a]]; }
};
const Gf& gf() { static const Gf g; return g; }

constexpr int kRsMax = 256;
uint8_t genCoef(int K, int idx, int i) {
    if (idx < K) return (uint8_t)(idx == i);
    const int j = idx - K, y = (kRsMax - K) + i;
    return gf().inv((uint8_t)(j ^ y));
}

} // namespace

const char* packetTypeName(int type) {
    static const char* n[8] = {"ARQ control", "ARQ data (1.9.x)", "CALL (1.9.x)", "broadcast file", "broadcast data", "CQ", "CALL / ACCEPT", "type 7"};
    return n[type & 7];
}

int bwHzFromToken(int t) { return t == 1 ? 500 : t == 2 ? 2300 : t == 3 ? 2750 : 0; }
int bwTokenFromHz(int hz) { return hz == 500 ? 1 : hz == 2300 ? 2 : hz == 2750 ? 3 : 0; }

uint16_t callsignCrc(const std::string& call) {
    const std::string u = upper(call).substr(0, 15);
    return crc::crc16CcittFalse((const uint8_t*)u.data(), u.size());
}

std::vector<uint8_t> encodeCallsign(const std::string& callIn) {
    const std::string call = upper(callIn);
    BitOut bw;
    uint64_t low = 0, high = kMax;
    int pending = 0;
    for (size_t i = 0;; i++) {
        int sym;
        if (i >= call.size()) sym = kSyms - 1;
        else { sym = symOf(call[i]); if (sym < 0) return {}; }
        const uint64_t range = high - low + 1;
        high = low + (range * (uint64_t)(sym + 1)) / kSyms - 1;
        low = low + (range * (uint64_t)sym) / kSyms;
        for (;;) {
            if (high < kHalf) { bw.put(0); while (pending > 0) { bw.put(1); pending--; } }
            else if (low >= kHalf) { bw.put(1); while (pending > 0) { bw.put(0); pending--; } low -= kHalf; high -= kHalf; }
            else if (low >= kQuarter && high < kThreeQ) { pending++; low -= kQuarter; high -= kQuarter; }
            else break;
            low <<= 1;
            high = (high << 1) | 1;
        }
        if (sym == kSyms - 1) break;
    }
    pending++;
    if (low < kQuarter) { bw.put(0); while (pending-- > 0) bw.put(1); }
    else { bw.put(1); while (pending-- > 0) bw.put(0); }
    return bw.b;
}

std::string decodeCallsign(const uint8_t* p, size_t n) {
    size_t bytePos = 0;
    int bitPos = 0, readAhead = 24;
    auto bit = [&]() -> int {
        if (bytePos >= n) { if (--readAhead == 0) return -1; return 0; }
        const int b = (p[bytePos] >> (7 - bitPos)) & 1;
        if (++bitPos == 8) { bitPos = 0; bytePos++; }
        return b;
    };
    uint64_t low = 0, high = kMax, value = 0;
    for (int i = 0; i < 32; i++) { const int b = bit(); if (b < 0) return ""; value = (value << 1) | (uint64_t)b; }
    std::string out;
    for (;;) {
        const uint64_t range = high - low + 1;
        const uint64_t scaled = ((value - low + 1) * kSyms - 1) / range;
        if (scaled >= (uint64_t)kSyms) return "";
        const int sym = (int)scaled;
        if (sym == kSyms - 1) break;
        if (out.size() >= 15) break;
        out += kAlpha[sym];
        high = low + (range * (uint64_t)(sym + 1)) / kSyms - 1;
        low = low + (range * (uint64_t)sym) / kSyms;
        bool end = false;
        for (;;) {
            if (high < kHalf) {
            } else if (low >= kHalf) { value -= kHalf; low -= kHalf; high -= kHalf; }
            else if (low >= kQuarter && high < kThreeQ) { value -= kQuarter; low -= kQuarter; high -= kQuarter; }
            else break;
            low <<= 1;
            high = (high << 1) | 1;
            const int b = bit();
            if (b < 0) { end = true; break; }
            value = (value << 1) | (uint64_t)b;
        }
        if (end) break;
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ connect, CQ, disconnect

bool parseCall(const uint8_t* p, size_t n, CallFrame& o) {
    if (n < 14 || packetType(p[0]) != kPtCall2 || (p[1] & 0x80)) return false;
    o.bwHz = bwHzFromToken(packetExt(p[0]) & 3);
    if (!o.bwHz) return false;
    o.minor = (packetExt(p[0]) >> 2) & 7;
    o.sessionId = p[1] & 0x7F;
    o.calledCrc = (uint16_t)(p[2] | p[3] << 8);
    o.caller = decodeCallsign(p + 4, 10);
    return !o.caller.empty();
}

bool parseAccept(const uint8_t* p, size_t n, AcceptFrame& o) {
    if (n < 14 || packetType(p[0]) != kPtCall2 || !(p[1] & 0x80)) return false;
    o.bwHz = bwHzFromToken(packetExt(p[0]) & 3);
    if (!o.bwHz) return false;
    o.minor = (packetExt(p[0]) >> 2) & 7;
    o.sessionId = p[1] & 0x7F;
    o.callerCrc = (uint16_t)(p[2] | p[3] << 8);
    o.calledCrc = (uint16_t)(p[4] | p[5] << 8);
    o.nonce = (uint16_t)(p[6] | p[7] << 8);
    o.startLevel = p[8] & 7;
    o.ctlDeaf = (p[8] & 8) != 0;
    o.callSnrDb = p[9] ? (float)p[9] - 128.f : -1000.f;
    o.reservedSet = (p[8] & 0xF0) != 0 || o.startLevel == 7;
    for (int i = 10; i < 14; i++) if (p[i]) o.reservedSet = true;
    return true;
}

bool parseCq(const uint8_t* p, size_t n, CqFrame& o) {
    if (n < 14 || packetType(p[0]) != kPtCq) return false;
    o.bwHz = bwHzFromToken(packetExt(p[0]));
    if (!o.bwHz) return false;
    o.caller = decodeCallsign(p + 1, 13);
    return !o.caller.empty();
}

bool parseDisconnect(const uint8_t* p, size_t n, DisconnectFrame& o) {
    if (n < 8 || packetType(p[0]) != kPtArqControl || p[1] != 4) return false;
    o.sessionId = p[3];
    o.snrDb = p[6] ? (float)p[6] - 128.f : -1000.f;
    return true;
}

std::vector<uint8_t> buildCall(int sid, const std::string& caller, const std::string& called, int bwHz) {
    std::vector<uint8_t> b(14, 0);
    const std::vector<uint8_t> enc = encodeCallsign(caller);
    if (enc.empty() || enc.size() > 10 || !bwTokenFromHz(bwHz)) return {};
    b[0] = framerByte(kPtCall2, bwTokenFromHz(bwHz));
    b[1] = (uint8_t)(sid & 0x7F);
    const uint16_t c = callsignCrc(called);
    b[2] = (uint8_t)c; b[3] = (uint8_t)(c >> 8);
    std::copy(enc.begin(), enc.end(), b.begin() + 4);
    return b;
}

std::vector<uint8_t> buildAccept(int sid, const std::string& called, const std::string& caller, int bwHz, int level, bool deaf, float snr, uint16_t nonce) {
    std::vector<uint8_t> b(14, 0);
    if (!bwTokenFromHz(bwHz)) return {};
    b[0] = framerByte(kPtCall2, bwTokenFromHz(bwHz));
    b[1] = (uint8_t)((sid & 0x7F) | 0x80);
    const uint16_t cr = callsignCrc(caller), cd = callsignCrc(called);
    b[2] = (uint8_t)cr; b[3] = (uint8_t)(cr >> 8);
    b[4] = (uint8_t)cd; b[5] = (uint8_t)(cd >> 8);
    b[6] = (uint8_t)nonce; b[7] = (uint8_t)(nonce >> 8);
    b[8] = (uint8_t)((level & 7) | (deaf ? 8 : 0));
    if (snr > -999) { const long v = std::lrint(snr) + 128; b[9] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v); }
    return b;
}

std::vector<uint8_t> buildCq(const std::string& caller, int bwHz) {
    std::vector<uint8_t> b(14, 0);
    const std::vector<uint8_t> enc = encodeCallsign(caller);
    if (enc.empty() || enc.size() > 13 || !bwTokenFromHz(bwHz)) return {};
    b[0] = framerByte(kPtCq, bwTokenFromHz(bwHz));
    std::copy(enc.begin(), enc.end(), b.begin() + 1);
    return b;
}

std::vector<uint8_t> buildDisconnect(int sid, float snr) {
    std::vector<uint8_t> b(14, 0);
    b[0] = framerByte(kPtArqControl, 0);
    b[1] = 4;
    b[3] = (uint8_t)sid;
    if (snr > -999) { const int v = (int)std::floor(snr + 0.5f) + 128; b[6] = (uint8_t)std::max(1, std::min(255, v)); }
    return b;
}

uint16_t sessionSeed(int sid, uint16_t nonce, int startLevel, const std::string& caller, const std::string& callee) {
    std::vector<uint8_t> b;
    b.push_back((uint8_t)sid);
    b.push_back((uint8_t)nonce);
    b.push_back((uint8_t)(nonce >> 8));
    b.push_back((uint8_t)pieceSize(startLevel));
    const std::string a = upper(caller), c = upper(callee);
    for (char ch : a) if (b.size() < 35) b.push_back((uint8_t)ch);
    b.push_back('/');
    for (char ch : c) if (b.size() < 36) b.push_back((uint8_t)ch);
    const uint16_t s = crc::crc16CcittFalse(b.data(), b.size());
    return s ? s : 0x5A5A;
}

// ------------------------------------------------------------------------------------------------ the carousel

const char* rungName(int level) {
    static const char* n[7] = {"MFSK", "DATAC15", "DATAC4", "DATAC3", "DATAC1", "DATAC17", "QAM16C2"};
    return level >= 0 && level < 7 ? n[level] : "?";
}

int rungOfMode(int id) {
    switch (id) {
    case kMfsk: case kMfsk500: return 0;
    case kDatac15: return 1; case kDatac4: return 2; case kDatac3: return 3; case kDatac1: return 4; case kDatac17: return 5; case kQam16c2: return 6;
    default: return -1;
    }
}

bool parseCarCtl(const uint8_t* b, size_t n, CarCtl& m) {
    if (n < 14) return false;
    m = CarCtl();
    m.type = b[0] >> 6;
    m.hasData = (b[0] >> 5) & 1;
    m.pollId = (b[0] >> 1) & 0x0F;
    m.ctlDeaf = b[0] & 1;
    m.level = b[1] >> 5;
    m.frames = (b[1] >> 1) & 0x0F;
    m.loss16 = b[2] >> 4;
    m.base = b[2] & 0x0F;
    uint64_t nb = 0;
    for (int i = 0; i < 7; i++) nb = nb << 8 | b[3 + i];
    for (int o = 0; o < 8; o++) {
        const int v = (int)((nb >> (7 * (7 - o))) & 0x7F);
        m.need[o] = v == 127 ? -1 : v;
    }
    m.gap = b[10] == 255 ? -1 : b[10];
    const int snr = (b[11] >> 5) << 3 | (b[11] & 3) << 1 | (b[1] & 1);
    m.snrDb = snr == 63 ? -1000.f : (float)(snr - 24);
    m.hLevel = (b[11] >> 2) & 7;
    m.hFrames = b[12] >> 4;
    m.hId = b[12] & 0x0F;
    m.highBlock = b[13] >> 4;
    m.unopened = (b[13] >> 3) & 1;
    m.shortRung = (b[13] >> 2) & 1;
    m.backlog = b[13] & 3;
    if (m.level >= 7 || m.hLevel >= 7) return false;
    for (int o = 0; o < 8; o++) if (m.need[o] > 96) return false;
    if (m.gap >= 96) return false;
    return true;
}

std::vector<uint8_t> buildCarCtl(const CarCtl& m) {
    std::vector<uint8_t> b(14, 0);
    int snr = 63;
    if (m.snrDb > -999) { const long v = std::lrint(m.snrDb) + 24; snr = v < 0 ? 0 : v > 62 ? 62 : (int)v; }
    b[0] = (uint8_t)((m.type & 3) << 6 | (m.hasData ? 1 : 0) << 5 | (m.pollId & 0x0F) << 1 | (m.ctlDeaf ? 1 : 0));
    b[1] = (uint8_t)((m.level & 7) << 5 | (m.frames & 0x0F) << 1 | (snr & 1));
    b[2] = (uint8_t)((m.loss16 & 0x0F) << 4 | (m.base & 0x0F));
    uint64_t nb = 0;
    for (int o = 0; o < 8; o++) nb = nb << 7 | (uint64_t)(m.need[o] < 0 ? 127 : m.need[o] & 0x7F);
    for (int i = 0; i < 7; i++) b[(size_t)(3 + i)] = (uint8_t)(nb >> (8 * (6 - i)));
    b[10] = (uint8_t)(m.gap < 0 ? 255 : m.gap);
    b[11] = (uint8_t)((snr >> 3) << 5 | (m.hLevel & 7) << 2 | ((snr >> 1) & 3));
    b[12] = (uint8_t)((m.hFrames & 0x0F) << 4 | (m.hId & 0x0F));
    b[13] = (uint8_t)((m.highBlock & 0x0F) << 4 | (m.unopened ? 1 : 0) << 3 | (m.shortRung ? 1 : 0) << 2 | (m.backlog & 3));
    return b;
}

bool parseCarData(const uint8_t* b, size_t len, int P, CarData& m) {
    constexpr size_t kHdr = 2, kSeg = 4, kInline = kSeg + 14;
    if (len < kHdr || P <= 0) return false;
    m = CarData();
    m.left = b[0] >> 4;
    m.unopened = (b[0] >> 3) & 1;
    m.slot = b[0] & 7;
    m.pollId = b[1] >> 4;
    m.highBlock = b[1] & 0x0F;
    if (len >= kHdr + kSeg) { m.ctlDeaf = b[kHdr + kSeg - 1] & 1; m.patAnchor = (b[kHdr + kSeg - 1] >> 1) & 1; }
    size_t pos = kHdr;
    if (len >= kHdr + kInline && (((b[pos] << 8 | b[pos + 1]) >> 5) & 0x7F) == 127) {
        m.hasInlineCtl = parseCarCtl(b + pos + kSeg, 14, m.inlineCtl);
        pos += kInline;
    }
    while (pos + kSeg <= len && m.segs.size() < 8) {
        const uint32_t h = (uint32_t)b[pos] << 24 | (uint32_t)b[pos + 1] << 16 | (uint32_t)b[pos + 2] << 8 | b[pos + 3];
        CarSegment g;
        g.block = (int)(h >> 28);
        g.K = (int)((h >> 21) & 0x7F) + 1;
        g.count = (int)((h >> 15) & 0x3F);
        g.first = (int)((h >> 7) & 0xFF);
        g.pad = (int)((h >> 2) & 0x1F);
        if (!g.count) break;
        pos += kSeg;
        if (g.K > 96 || g.pad >= P || pos + (size_t)g.count * (size_t)P > len) return false;
        g.offset = pos;
        pos += (size_t)g.count * (size_t)P;
        m.segs.push_back(g);
    }
    return true;
}

std::vector<uint8_t> buildCarData(size_t frameLen, int left, int pollId, int highBlock, int block, int K, int first, int count, int pad,
                                  const uint8_t* pieces, int P) {
    std::vector<uint8_t> b(frameLen, 0);
    b[0] = (uint8_t)((left & 0x0F) << 4);
    b[1] = (uint8_t)((pollId & 0x0F) << 4 | (highBlock & 0x0F));
    const uint32_t h = (uint32_t)(block & 0x0F) << 28 | (uint32_t)(K - 1) << 21 | (uint32_t)count << 15 | (uint32_t)(first & 0xFF) << 7 | (uint32_t)pad << 2;
    b[2] = (uint8_t)(h >> 24); b[3] = (uint8_t)(h >> 16); b[4] = (uint8_t)(h >> 8); b[5] = (uint8_t)h;
    if (6 + (size_t)count * (size_t)P <= frameLen) std::memcpy(b.data() + 6, pieces, (size_t)count * (size_t)P);
    return b;
}

bool rsDecode(int K, int P, const std::vector<int>& idx, const std::vector<std::vector<uint8_t>>& pieces, std::vector<std::vector<uint8_t>>& out) {
    if (K < 1 || K >= kRsMax || (int)idx.size() < K || (int)pieces.size() < K) return false;
    const Gf& g = gf();
    std::vector<std::vector<uint8_t>> M((size_t)K, std::vector<uint8_t>((size_t)K));
    out.assign((size_t)K, std::vector<uint8_t>((size_t)P, 0));
    std::vector<int> seen(kRsMax, 0);
    for (int r = 0; r < K; r++) {
        if (idx[(size_t)r] < 0 || idx[(size_t)r] >= kRsMax || seen[(size_t)idx[(size_t)r]]) return false;
        seen[(size_t)idx[(size_t)r]] = 1;
        for (int i = 0; i < K; i++) M[(size_t)r][(size_t)i] = genCoef(K, idx[(size_t)r], i);
        out[(size_t)r] = pieces[(size_t)r];
        out[(size_t)r].resize((size_t)P, 0);
    }
    for (int col = 0; col < K; col++) {
        int piv = -1;
        for (int r = col; r < K; r++) if (M[(size_t)r][(size_t)col]) { piv = r; break; }
        if (piv < 0) return false;
        if (piv != col) { std::swap(M[(size_t)col], M[(size_t)piv]); std::swap(out[(size_t)col], out[(size_t)piv]); }
        const uint8_t inv = g.inv(M[(size_t)col][(size_t)col]);
        if (inv != 1) {
            for (int i = 0; i < K; i++) M[(size_t)col][(size_t)i] = g.mul(M[(size_t)col][(size_t)i], inv);
            for (int k = 0; k < P; k++) out[(size_t)col][(size_t)k] = g.mul(out[(size_t)col][(size_t)k], inv);
        }
        for (int r = 0; r < K; r++) {
            if (r == col || !M[(size_t)r][(size_t)col]) continue;
            const uint8_t f = M[(size_t)r][(size_t)col];
            for (int i = 0; i < K; i++) M[(size_t)r][(size_t)i] ^= g.mul(f, M[(size_t)col][(size_t)i]);
            for (int k = 0; k < P; k++) out[(size_t)r][(size_t)k] ^= g.mul(f, out[(size_t)col][(size_t)k]);
        }
    }
    return true;
}

void rsRepair(int K, int P, const std::vector<std::vector<uint8_t>>& data, int j, std::vector<uint8_t>& out) {
    const Gf& g = gf();
    out.assign((size_t)P, 0);
    for (int i = 0; i < K; i++) {
        const uint8_t c = genCoef(K, K + j, i);
        for (int k = 0; k < P; k++) out[(size_t)k] ^= g.mul(c, data[(size_t)i][(size_t)k]);
    }
}

uint32_t blockCheck(uint32_t key, int id, int K, const uint8_t* bytes, size_t n) {
    const uint8_t h[6] = {(uint8_t)key, (uint8_t)(key >> 8), (uint8_t)(key >> 16), (uint8_t)(key >> 24), (uint8_t)id, (uint8_t)K};
    const uint32_t c = crc::crc32(h, 6);
    return crc::crc32Update(c, bytes, n);
}

// ------------------------------------------------------------------------------------------------ broadcast

bool parseBcastMessage(const uint8_t* p, size_t n, BcastMessage& o) {
    if (n < 1 || packetType(p[0]) != kPtBcastData) return false;
    o.ext = packetExt(p[0]);
    o.lenPrefix = o.ext & 1;
    if (o.lenPrefix) {
        if (n < 3) return false;
        const size_t len = std::min<size_t>((size_t)(p[1] << 8 | p[2]), n - 3);
        o.bytes.assign(p + 3, p + 3 + len);
    } else {
        o.bytes.assign(p + 1, p + n);
        while (!o.bytes.empty() && o.bytes.back() == 0) o.bytes.pop_back();
    }
    return true;
}

std::vector<uint8_t> buildBcastMessage(const uint8_t* msg, size_t n, size_t frameLen, int kissExt) {
    std::vector<uint8_t> b(frameLen, 0);
    if (frameLen < 4) return b;
    n = std::min(n, frameLen - 3);
    b[0] = framerByte(kPtBcastData, 1 | (kissExt & 6));
    b[1] = (uint8_t)(n >> 8); b[2] = (uint8_t)n;
    std::memcpy(b.data() + 3, msg, n);
    return b;
}

bool parseBcastFile(const uint8_t* p, size_t n, BcastFileFrame& o) {
    if (n < 13 || packetType(p[0]) != kPtBcastControl) return false;
    o.session = packetExt(p[0]);
    if (o.session == 0) return false;
    o.F = (uint32_t)p[1] | (uint32_t)p[2] << 8 | (uint32_t)p[3] << 16;
    o.T = (int)(p[4] | p[5] << 8) + 1;
    o.Z = p[6] + 1;
    o.N = (int)(p[7] | p[8] << 8) + 1;
    o.sbn = p[9];
    o.esi = (uint32_t)(p[10] | p[11] << 8);
    o.offset = 12;
    if (o.F == 0 || o.T < 1 || (size_t)o.T > n - 12) return false;
    o.symbols = (int)((n - 12) / (size_t)o.T);
    return o.symbols >= 1 && o.sbn < o.Z;
}

bool parseBundle(const std::vector<uint8_t>& obj, std::string& name, size_t& offset, size_t& len) {
    if (obj.size() < 6) return false;
    const uint32_t total = (uint32_t)obj[0] | (uint32_t)obj[1] << 8 | (uint32_t)obj[2] << 16 | (uint32_t)obj[3] << 24;
    if ((size_t)total + 4 != obj.size()) return false;
    size_t i = 4;
    std::string nm;
    while (i < obj.size() && obj[i] != '\n' && nm.size() <= 255) {
        const char c = (char)obj[i++];
        if (c == '/' || c == '\\' || c == 0) return false;
        nm += c;
    }
    if (i >= obj.size() || obj[i] != '\n' || nm.empty() || nm == "." || nm == "..") return false;
    name = nm;
    offset = i + 1;
    len = obj.size() - offset;
    return true;
}

} // namespace mercury
} // namespace dect2
