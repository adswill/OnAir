// VDL Mode 3 bit layer: see vdl3_proto.h.
#include "dect2/vdl3/vdl3_proto.h"
#include "dect2/modebuilder/modebuilder.h"
#include <array>
#include <cstdio>

namespace dect2 {

namespace {

std::vector<uint8_t> bitsOf(const char* s) {
    std::vector<uint8_t> b;
    for (; *s; s++) if (*s == '0' || *s == '1') b.push_back((uint8_t)(*s - '0'));
    return b;
}

// Annex 10 Vol III, Part I: 6.4.4.2.1.2 (S1, S1*), 6.4.4.3.1.2 (S2), 6.4.4.1.1.2 (S2*), each transmitted from left to right
const std::vector<uint8_t>& syncTable(int i) {
    static const std::vector<uint8_t> t[4] = {
        bitsOf("000 111 001 001 010 110 000 011 100 110 011 111 010 101 100 101"),
        bitsOf("000 001 111 111 100 000 110 101 010 000 101 001 100 011 010 011"),
        bitsOf("000 111 011 010 000 100 001 010 100 101 011 110 001 110 101 111"),
        bitsOf("000 001 101 100 110 010 111 100 010 011 101 000 111 000 011 001"),
    };
    return t[i & 3];
}

// 6.4.4.1.2: the parity part of G (the first 12 columns of each row; the last 12 are the identity)
constexpr const char* kGolayP[12] = {
    "110101110001", "011111001001", "111010010101", "011000111011", "111001101100", "101100110110",
    "100110011011", "010110111100", "001011011110", "000101101111", "110111000110", "101011100011",
};

uint32_t parityOf(uint32_t data12) {   // data bit 11 = x1 (row 1 of G)
    static const std::array<uint32_t, 12> rows = [] {
        std::array<uint32_t, 12> r{};
        for (int i = 0; i < 12; i++) {
            uint32_t v = 0;
            for (int j = 0; j < 12; j++) v = (v << 1) | (uint32_t)(kGolayP[i][j] - '0');
            r[(size_t)i] = v;
        }
        return r;
    }();
    uint32_t p = 0;
    for (int i = 0; i < 12; i++) if (data12 & (1u << (11 - i))) p ^= rows[(size_t)i];
    return p;
}

// syndrome (12 bits) -> error pattern (24 bits) + 1, for every pattern of 3 or fewer errors; 0 = no pattern of that weight
const std::vector<uint32_t>& syndromeTable() {
    static const std::vector<uint32_t> t = [] {
        std::vector<uint32_t> tab(4096, 0);
        auto add = [&](uint32_t e) {
            const uint32_t s = ((e >> 12) & 0xFFF) ^ parityOf(e & 0xFFF);
            if (!tab[s]) tab[s] = e + 1;
        };
        add(0);
        for (int a = 0; a < 24; a++) {
            add(1u << a);
            for (int b = a + 1; b < 24; b++) {
                add((1u << a) | (1u << b));
                for (int c = b + 1; c < 24; c++) add((1u << a) | (1u << b) | (1u << c));
            }
        }
        return tab;
    }();
    return t;
}

const modebuilder::ReedSolomon& rs() {
    static const modebuilder::ReedSolomon r(8, 0x187, 255, 245, 120, 1);   // roots alpha^120 .. alpha^129, shortened to (72,62)
    return r;
}

int popcount(uint32_t v) { int n = 0; while (v) { v &= v - 1; n++; } return n; }

} // namespace

const uint8_t kVdl3GrayBits[8] = {0, 1, 3, 2, 6, 7, 5, 4};
int vdl3StepForBits(int b) {
    for (int i = 0; i < 8; i++) if (kVdl3GrayBits[i] == (b & 7)) return i;
    return 0;
}

const char* vdl3SyncName(Vdl3Sync s) {
    switch (s) {
    case Vdl3Sync::S1: return "S1";
    case Vdl3Sync::S1Star: return "S1*";
    case Vdl3Sync::S2: return "S2";
    case Vdl3Sync::S2Star: return "S2*";
    }
    return "?";
}

const std::vector<uint8_t>& vdl3SyncBits(Vdl3Sync s) { return syncTable((int)s); }

const std::vector<int>& vdl3SyncPhases(Vdl3Sync s) {
    static const std::array<std::vector<int>, 4> t = [] {
        std::array<std::vector<int>, 4> a;
        for (int i = 0; i < 4; i++) {
            const auto& b = syncTable(i);
            int cur = 0;
            for (size_t k = 0; k + 2 < b.size(); k += 3) {
                cur = (cur + vdl3StepForBits((b[k] << 2) | (b[k + 1] << 1) | b[k + 2])) % 8;
                a[(size_t)i].push_back(cur);
            }
        }
        return a;
    }();
    return t[(size_t)((int)s & 3)];
}

const char* vdl3KindName(Vdl3Kind k) {
    switch (k) {
    case Vdl3Kind::MUplink: return "M uplink";
    case Vdl3Kind::MUplink3T: return "M uplink (3T)";
    case Vdl3Kind::Handoff: return "H handoff check";
    case Vdl3Kind::PollResponse: return "Poll response";
    case Vdl3Kind::MDownlink: return "M downlink";
    case Vdl3Kind::NetEntry: return "Net entry request";
    case Vdl3Kind::VdData: return "V/D data";
    case Vdl3Kind::VdVoice: return "V/D voice";
    default: return "unknown";
    }
}

int vdl3PayloadSymbols(Vdl3Kind k) {
    switch (k) {
    case Vdl3Kind::MUplink: return 32;
    case Vdl3Kind::MUplink3T: return 128;
    case Vdl3Kind::Handoff: return 40;
    case Vdl3Kind::PollResponse: case Vdl3Kind::MDownlink: case Vdl3Kind::NetEntry: return 16;
    case Vdl3Kind::VdData: case Vdl3Kind::VdVoice: return 200;
    default: return 0;
    }
}

int vdl3GolayWords(Vdl3Kind k) {
    if (k == Vdl3Kind::VdData || k == Vdl3Kind::VdVoice) return 1;
    return vdl3PayloadSymbols(k) / 8;
}

Vdl3Sync vdl3SyncFor(Vdl3Kind k) {
    switch (k) {
    case Vdl3Kind::MDownlink: return Vdl3Sync::S1;
    case Vdl3Kind::NetEntry: return Vdl3Sync::S1Star;
    case Vdl3Kind::VdData: case Vdl3Kind::VdVoice: return Vdl3Sync::S2;
    default: return Vdl3Sync::S2Star;
    }
}

Vdl3Kind vdl3KindFromSync(Vdl3Sync s, int symbols) {
    switch (s) {
    case Vdl3Sync::S1: return symbols == 16 ? Vdl3Kind::MDownlink : Vdl3Kind::Unknown;
    case Vdl3Sync::S1Star: return symbols == 16 ? Vdl3Kind::NetEntry : Vdl3Kind::Unknown;
    case Vdl3Sync::S2: return symbols == 200 ? Vdl3Kind::VdData : Vdl3Kind::Unknown;
    case Vdl3Sync::S2Star:
        if (symbols == 16) return Vdl3Kind::PollResponse;
        if (symbols == 32) return Vdl3Kind::MUplink;
        if (symbols == 40) return Vdl3Kind::Handoff;
        if (symbols == 128) return Vdl3Kind::MUplink3T;
        return Vdl3Kind::Unknown;
    }
    return Vdl3Kind::Unknown;
}

uint32_t vdl3GolayEncode(uint32_t d) {
    d &= 0xFFF;
    return (parityOf(d) << 12) | d;
}

int vdl3GolayDecode(uint32_t w, uint32_t& d) {
    w &= 0xFFFFFF;
    const uint32_t s = ((w >> 12) & 0xFFF) ^ parityOf(w & 0xFFF);
    const uint32_t e1 = syndromeTable()[s];
    if (!e1) return -1;
    const uint32_t e = e1 - 1;
    d = (w ^ e) & 0xFFF;
    return popcount(e);
}

void vdl3Scramble(std::vector<uint8_t>& bits, size_t from) {
    uint16_t s = 0x6959;   // 6.4.3.1.4: 1101 0010 1011 001, the left-most bit in the first stage (as the Mode 2 scrambler)
    for (size_t i = from; i < bits.size(); i++) {
        const uint8_t x = (uint8_t)((s ^ (s >> 14)) & 1);
        s = (uint16_t)((s >> 1) | ((uint16_t)x << 14));
        bits[i] ^= x;
    }
}

std::vector<uint8_t> vdl3RsEncode(const std::vector<uint8_t>& data) {
    std::vector<int> d(kVdl3RsK, 0);
    for (size_t i = 0; i < data.size() && i < (size_t)kVdl3RsK; i++) d[i] = data[i];
    const std::vector<int> cw = rs().encode(d);
    std::vector<uint8_t> out(cw.size());
    for (size_t i = 0; i < cw.size(); i++) out[i] = (uint8_t)cw[i];
    return out;
}

int vdl3RsDecode(std::vector<uint8_t>& cw) {
    if (cw.size() != (size_t)kVdl3RsN) return -1;
    std::vector<int> v(cw.begin(), cw.end());
    const int r = rs().decode(v);
    if (r < 0) return -1;
    for (size_t i = 0; i < v.size(); i++) cw[i] = (uint8_t)v[i];
    return r;
}

std::vector<uint8_t> vdl3PayloadBits(Vdl3Kind k, const std::vector<uint32_t>& info12, const std::vector<uint8_t>& data, const std::vector<uint8_t>& voiceBits) {
    std::vector<uint8_t> b;
    const int words = vdl3GolayWords(k);
    for (int i = 0; i < words; i++) {
        const uint32_t w = vdl3GolayEncode(i < (int)info12.size() ? info12[(size_t)i] : 0);
        for (int j = 23; j >= 0; j--) b.push_back((uint8_t)((w >> j) & 1));
    }
    if (k == Vdl3Kind::VdData) {
        for (uint8_t o : vdl3RsEncode(data)) for (int j = 0; j < 8; j++) b.push_back((uint8_t)((o >> j) & 1));
    } else if (k == Vdl3Kind::VdVoice) {
        for (size_t i = 0; i < 576; i++) b.push_back(i < voiceBits.size() ? (uint8_t)(voiceBits[i] & 1) : 0);
    }
    return b;
}

std::vector<uint8_t> vdl3BurstBits(Vdl3Kind k, const std::vector<uint32_t>& info12, const std::vector<uint8_t>& data, const std::vector<uint8_t>& voiceBits) {
    std::vector<uint8_t> b = vdl3SyncBits(vdl3SyncFor(k));
    std::vector<uint8_t> p = vdl3PayloadBits(k, info12, data, voiceBits);
    vdl3Scramble(p);
    b.insert(b.end(), p.begin(), p.end());
    return b;
}

bool vdl3DecodePayload(Vdl3Sync sync, int symbols, const std::vector<uint8_t>& bits, Vdl3Decoded& out) {
    out = Vdl3Decoded();
    const Vdl3Kind k = vdl3KindFromSync(sync, symbols);
    if (k == Vdl3Kind::Unknown || bits.size() < (size_t)symbols * 3) return false;
    out.kind = k;
    const int words = vdl3GolayWords(k);
    out.golayOk = true;
    for (int i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int j = 0; j < 24; j++) w = (w << 1) | (bits[(size_t)(24 * i + j)] & 1u);
        uint32_t d = 0;
        const int r = vdl3GolayDecode(w, d);
        if (r < 0) { out.golayOk = false; d = w & 0xFFF; }
        else out.golayFixed += r;
        out.info12.push_back(d);
    }
    if (k == Vdl3Kind::VdData) {
        std::vector<uint8_t> cw(kVdl3RsN, 0);
        for (int i = 0; i < kVdl3RsN; i++) {
            uint8_t o = 0;
            for (int j = 0; j < 8; j++) o |= (uint8_t)((bits[(size_t)(24 + 8 * i + j)] & 1) << j);
            cw[(size_t)i] = o;
        }
        std::vector<uint8_t> fixedCw = cw;
        const int r = vdl3RsDecode(fixedCw);
        if (r >= 0) {
            out.rsOk = true;
            out.rsFixed = r;
            out.data.assign(fixedCw.begin(), fixedCw.begin() + kVdl3RsK);
        } else {
            out.kind = Vdl3Kind::VdVoice;
            out.raw = cw;
        }
    }
    return true;
}

std::string vdl3Hex(const std::vector<uint8_t>& b, size_t max) {
    std::string s;
    char t[4];
    for (size_t i = 0; i < b.size() && i < max; i++) { snprintf(t, sizeof t, "%02X", b[i]); s += t; }
    if (b.size() > max) s += "..";
    return s;
}

std::string vdl3InfoHex(const std::vector<uint32_t>& v) {
    std::string s;
    char t[8];
    for (size_t i = 0; i < v.size(); i++) { snprintf(t, sizeof t, i ? " %03X" : "%03X", (unsigned)(v[i] & 0xFFF)); s += t; }
    return s;
}

std::string vdl3PrintableText(const std::vector<uint8_t>& b) {
    size_t n = b.size();
    while (n > 0 && b[n - 1] == 0) n--;   // the zero padding of a short message
    if (n < 4) return std::string();
    size_t good = 0;
    std::string s;
    for (size_t i = 0; i < n; i++) {
        const uint8_t c = b[i];
        if (c >= 32 && c < 127) { good++; s += (char)c; }
        else s += '.';
    }
    return good * 10 >= n * 9 ? s : std::string();
}

} // namespace dect2
