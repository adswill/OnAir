// DMR burst layout and message formats, see dmr_proto.h.
#include "dect2/dmr_proto.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace dmr {

// ---------------------------------------------------------------------------------------------------- sync patterns

const uint64_t kSyncWords[kSyncCount] = {
    0x755FD7DF75F7ULL,   // BS sourced voice
    0xDFF57D75DF5DULL,   // BS sourced data
    0x7F7D5DD57DFDULL,   // MS sourced voice
    0xD5D7F77FD757ULL,   // MS sourced data
    0x77D55F7DFD77ULL,   // standalone reverse channel
    0x5D577F7757FFULL,   // TDMA direct mode slot 1 voice
    0xF7FDD5DDFD55ULL,   // slot 1 data
    0x7DFFD5F55D5FULL,   // slot 2 voice
    0xD7557F5FF7F5ULL};  // slot 2 data

const char* syncName(int s) {
    static const char* n[kSyncCount] = {"BS voice", "BS data", "MS voice", "MS data", "RC", "direct 1 voice", "direct 1 data", "direct 2 voice", "direct 2 data"};
    return s >= 0 && s < kSyncCount ? n[s] : "none";
}

int syncDirectSlot(int s) { return s == kSyncDm1Voice || s == kSyncDm1Data ? 1 : s == kSyncDm2Voice || s == kSyncDm2Data ? 2 : 0; }

int syncOf(int family, bool voice) {
    switch (family) {
    case 0: return voice ? kSyncBsVoice : kSyncBsData;
    case 1: return voice ? kSyncMsVoice : kSyncMsData;
    case 2: return voice ? kSyncDm1Voice : kSyncDm1Data;
    default: return voice ? kSyncDm2Voice : kSyncDm2Data;
    }
}

void syncSymbols(int s, int8_t out[24]) {
    const uint64_t w = kSyncWords[s];
    for (int i = 0; i < 24; i++) out[i] = (int8_t)dibitToSymbol((unsigned)((w >> (46 - 2 * i)) & 3));
}

int matchSync(const Bits& centre48, int maxDiff, int* diff) {
    int best = -1, bd = 99;
    for (int s = 0; s < kSyncCount; s++) {
        int d = 0;
        for (int i = 0; i < 24; i++) {
            const unsigned want = (unsigned)((kSyncWords[s] >> (46 - 2 * i)) & 3);
            const unsigned got = (unsigned)((centre48[2 * i] << 1) | centre48[2 * i + 1]);
            if (want != got) d++;
        }
        if (d < bd) { bd = d; best = s; }
    }
    if (diff) *diff = bd;
    return bd <= maxDiff ? best : -1;
}

// ---------------------------------------------------------------------------------------------------- bursts

const char* dataTypeName(int dt) {
    static const char* n[16] = {"PI header", "Voice LC header", "Terminator with LC", "CSBK", "MBC header", "MBC continuation", "Data header",
                                "Rate 1/2 data", "Rate 3/4 data", "Idle", "Rate 1 data", "Unified single block data", "reserved", "reserved", "reserved", "reserved"};
    return n[dt & 15];
}

const char* dataTypeShort(int dt) {
    static const char* n[16] = {"PI", "LC", "TLC", "CSBK", "MBCH", "MBCC", "DH", "R1/2", "R3/4", "idle", "R1", "USBD", "?", "?", "?", "?"};
    return n[dt & 15];
}

static void putSlotType(Bits& b, int cc, int dt, bool second) {
    const unsigned info = (unsigned)(((cc & 15) << 4) | (dt & 15));
    const unsigned par = golay2008Parity(info);
    if (!second) {
        putBits(b, info, 8);
        putBits(b, par >> 10, 2);
    } else {
        putBits(b, par & 0x3FF, 10);
    }
}

Bits makeDataBurst(int cc, int dt, const Bits& p, int sync) {
    Bits b;
    b.reserve(264);
    b.insert(b.end(), p.begin(), p.begin() + 98);
    putSlotType(b, cc, dt, false);
    if (sync >= 0) {
        putBits(b, kSyncWords[sync], 48);
    } else {      // embedded signalling in place of the sync pattern: EMB (colour code, no privacy, no LC fragment), 32 bits of nothing
        const unsigned info = (unsigned)((cc & 15) << 3);
        const unsigned qr = qr1676Parity(info);
        putBits(b, info, 7);
        putBits(b, qr >> 8, 1);
        putBits(b, 0, 32);
        putBits(b, qr & 0xFF, 8);
    }
    putSlotType(b, cc, dt, true);
    b.insert(b.end(), p.begin() + 98, p.begin() + 196);
    return b;
}

Bits makeVoiceBurst(const Bits& vs, int sync, int cc, int pi, int lcss, const Bits& emb32) {
    Bits b;
    b.reserve(264);
    b.insert(b.end(), vs.begin(), vs.begin() + 108);
    if (sync >= 0) {
        putBits(b, kSyncWords[sync], 48);
    } else {
        const unsigned info = (unsigned)(((cc & 15) << 3) | ((pi & 1) << 2) | (lcss & 3));
        const unsigned qr = qr1676Parity(info);
        putBits(b, info, 7);
        putBits(b, qr >> 8, 1);
        b.insert(b.end(), emb32.begin(), emb32.begin() + 32);
        putBits(b, qr & 0xFF, 8);
    }
    b.insert(b.end(), vs.begin() + 108, vs.begin() + 216);
    return b;
}

Bits burstCentre(const Bits& burst) { return Bits(burst.begin() + 108, burst.begin() + 156); }

void burstDataPayload(const Bits& burst, Bits& p) {
    p.assign(burst.begin(), burst.begin() + 98);
    p.insert(p.end(), burst.begin() + 166, burst.begin() + 264);
}

void burstVoicePayload(const Bits& burst, Bits& vs) {
    vs.assign(burst.begin(), burst.begin() + 108);
    vs.insert(vs.end(), burst.begin() + 156, burst.begin() + 264);
}

SlotTypeInfo burstSlotType(const Bits& burst) {
    SlotTypeInfo r;
    unsigned w = (unsigned)(getBits(burst, 98, 10) << 10 | getBits(burst, 156, 10)), info = 0;
    const int e = golay2008Decode(w, info);
    if (e < 0) return r;
    r.ok = true; r.errors = e; r.cc = (int)(info >> 4); r.dt = (int)(info & 15);
    return r;
}

EmbInfo burstEmb(const Bits& burst) {
    EmbInfo r;
    const unsigned w = (unsigned)(getBits(burst, 108, 8) << 8 | getBits(burst, 148, 8)), info = 0;
    unsigned out = info;
    const int e = qr1676Decode(w, out);
    r.emb32.assign(burst.begin() + 116, burst.begin() + 148);
    if (e < 0) return r;
    r.ok = true; r.errors = e; r.cc = (int)(out >> 3); r.pi = (int)((out >> 2) & 1); r.lcss = (int)(out & 3);
    return r;
}

void bitsToSymbols(const Bits& bits, std::vector<int8_t>& sym) {
    sym.resize(bits.size() / 2);
    for (size_t i = 0; i < sym.size(); i++) sym[i] = (int8_t)dibitToSymbol((unsigned)((bits[2 * i] << 1) | bits[2 * i + 1]));
}

// ---------------------------------------------------------------------------------------------------- link control

uint32_t be24(const uint8_t* p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
void putBe24(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }

void packFullLc(const FullLc& lc, uint8_t out[9]) {
    out[0] = (uint8_t)((lc.pf ? 0x80 : 0) | (lc.flco & 0x3F));
    out[1] = (uint8_t)lc.fid;
    out[2] = lc.svc;
    putBe24(out + 3, lc.dst);
    putBe24(out + 6, lc.src);
}

void parseFullLc(const uint8_t in[9], FullLc& lc) {
    memcpy(lc.raw, in, 9);
    lc.pf = (in[0] >> 7) & 1;
    lc.flco = in[0] & 0x3F;
    lc.fid = in[1];
    lc.svc = in[2];
    lc.dst = be24(in + 3);
    lc.src = be24(in + 6);
}

const char* flcoName(int f) {
    switch (f) {
    case kFlcoGroupVoice: return "group voice";
    case kFlcoUnitVoice: return "unit to unit voice";
    case kFlcoTalkerAliasHeader: return "talker alias header";
    case kFlcoTalkerAliasBlock1: case kFlcoTalkerAliasBlock2: case kFlcoTalkerAliasBlock3: return "talker alias block";
    case kFlcoGpsInfo: return "GPS info";
    default: return "link control";
    }
}

std::string serviceOptionsText(uint8_t svc) {
    std::string s;
    if (svc & 0x80) s += "emergency ";
    if (svc & 0x40) s += "privacy ";
    if (svc & 0x08) s += "broadcast ";
    if (svc & 0x04) s += "OVCM ";
    if (svc & 3) { s += "priority "; s += (char)('0' + (svc & 3)); s += ' '; }
    if (!s.empty()) s.pop_back();
    return s;
}

void lcBurstInfo(const uint8_t lc[9], bool terminator, Bits& info96) {
    uint8_t w[12];
    memcpy(w, lc, 9);
    rs129Parity(lc, w + 9);
    const uint8_t mask = terminator ? 0x99 : 0x96;
    for (int i = 9; i < 12; i++) w[i] ^= mask;
    info96.clear();
    bytesToBits(w, 12, info96);
}

int lcBurstDecode(const Bits& info96, bool terminator, uint8_t lc[9]) {
    uint8_t w[12];
    bitsToBytes(info96, 0, 96, w);
    const uint8_t mask = terminator ? 0x99 : 0x96;
    for (int i = 9; i < 12; i++) w[i] ^= mask;
    const int e = rs129Correct(w);
    memcpy(lc, w, 9);
    return e;
}

void crcBlockInfo(const uint8_t data10[10], uint16_t mask, Bits& info96) {
    const uint16_t crc = (uint16_t)(crcCcitt(data10, 10) ^ mask);
    info96.clear();
    bytesToBits(data10, 10, info96);
    putBits(info96, crc, 16);
}

bool crcBlockCheck(const Bits& info96, uint16_t mask, uint8_t data10[10]) {
    uint8_t b[12];
    bitsToBytes(info96, 0, 96, b);
    memcpy(data10, b, 10);
    const uint16_t crc = (uint16_t)(crcCcitt(b, 10) ^ mask);
    return crc == (uint16_t)((b[10] << 8) | b[11]);
}

void csbkInfo(const Csbk& c, Bits& info96) {
    uint8_t d[10];
    d[0] = (uint8_t)((c.lb ? 0x80 : 0) | (c.pf ? 0x40 : 0) | (c.opcode & 0x3F));
    d[1] = (uint8_t)c.fid;
    memcpy(d + 2, c.data, 8);
    crcBlockInfo(d, kMaskCsbk, info96);
}

bool csbkParse(const Bits& info96, Csbk& c) {
    uint8_t d[10];
    if (!crcBlockCheck(info96, kMaskCsbk, d)) return false;
    c.lb = d[0] >> 7; c.pf = (d[0] >> 6) & 1; c.opcode = d[0] & 0x3F; c.fid = d[1];
    memcpy(c.data, d + 2, 8);
    return true;
}

const char* csbkName(int op, int fid) {
    if (fid != 0) return "manufacturer CSBK";
    switch (op) {
    case 0x04: return "unit to unit voice request";
    case 0x05: return "unit to unit answer";
    case 0x07: return "channel timing";
    case 0x26: return "negative acknowledge";
    case 0x38: return "BS outbound activation";
    case 0x3D: return "preamble";
    default: return "CSBK";
    }
}

// ---------------------------------------------------------------------------------------------------- data packets

const char* dpfName(int d) {
    switch (d) {
    case kDpfUdt: return "UDT";
    case kDpfResponse: return "response";
    case kDpfUnconfirmed: return "unconfirmed data";
    case kDpfConfirmed: return "confirmed data";
    case kDpfShortDefined: return "short data (defined)";
    case kDpfShortRaw: return "short data (raw/status)";
    case kDpfProprietary: return "proprietary data";
    default: return "data";
    }
}

const char* sapName(int s) {
    switch (s) {
    case 0: return "UDT";
    case 2: return "TCP/IP header compression";
    case 3: return "UDP/IP header compression";
    case 4: return "IP";
    case 5: return "ARP";
    case 9: return "proprietary";
    case 10: return "short data";
    default: return "SAP";
    }
}

void packDataHeader(const DataHeader& h, uint8_t o[10]) {
    memset(o, 0, 10);
    const int b = h.blocks;
    switch (h.dpf) {
    case kDpfUnconfirmed:
    case kDpfConfirmed:
        o[0] = (uint8_t)((h.group ? 0x80 : 0) | (h.resp ? 0x40 : 0) | ((h.pad >> 4) & 1) << 4 | h.dpf);
        o[1] = (uint8_t)((h.sap << 4) | (h.pad & 15));
        putBe24(o + 2, h.dst); putBe24(o + 5, h.src);
        o[8] = (uint8_t)((h.fmf ? 0x80 : 0) | (b & 0x7F));
        o[9] = h.dpf == kDpfConfirmed ? (uint8_t)((h.resync ? 0x80 : 0) | ((h.ns & 7) << 4) | (h.fsn & 15)) : (uint8_t)(h.fsn & 15);
        break;
    case kDpfShortDefined:
        o[0] = (uint8_t)((h.group ? 0x80 : 0) | (h.resp ? 0x40 : 0) | (((b >> 4) & 3) << 4) | h.dpf);
        o[1] = (uint8_t)((h.sap << 4) | (b & 15));
        putBe24(o + 2, h.dst); putBe24(o + 5, h.src);
        o[8] = (uint8_t)(((h.dd & 63) << 2) | (h.sarq ? 2 : 0) | (h.fmf ? 1 : 0));
        o[9] = (uint8_t)h.pad;
        break;
    case kDpfShortRaw:
        o[0] = (uint8_t)((h.group ? 0x80 : 0) | (h.resp ? 0x40 : 0) | (((b >> 4) & 3) << 4) | h.dpf);
        o[1] = (uint8_t)((h.sap << 4) | (b & 15));
        putBe24(o + 2, h.dst); putBe24(o + 5, h.src);
        if (b == 0) {      // status / precoded
            o[8] = (uint8_t)(((h.sp & 7) << 5) | ((h.dp & 7) << 2) | ((h.status >> 8) & 3));
            o[9] = (uint8_t)(h.status & 0xFF);
        } else {
            o[8] = (uint8_t)(((h.sp & 7) << 5) | ((h.dp & 7) << 2) | (h.sarq ? 2 : 0) | (h.fmf ? 1 : 0));
            o[9] = (uint8_t)h.pad;
        }
        break;
    default:
        o[0] = (uint8_t)h.dpf;
        break;
    }
}

bool parseDataHeader(const uint8_t in[10], DataHeader& h) {
    h = DataHeader();
    h.dpf = in[0] & 15;
    switch (h.dpf) {
    case kDpfUnconfirmed:
    case kDpfConfirmed:
        h.group = in[0] >> 7; h.resp = (in[0] >> 6) & 1;
        h.pad = ((in[0] >> 4) & 1) << 4 | (in[1] & 15);
        h.sap = in[1] >> 4;
        h.dst = be24(in + 2); h.src = be24(in + 5);
        h.fmf = in[8] >> 7; h.blocks = in[8] & 0x7F;
        if (h.dpf == kDpfConfirmed) { h.resync = in[9] >> 7; h.ns = (in[9] >> 4) & 7; }
        h.fsn = in[9] & 15;
        return true;
    case kDpfResponse:
        h.resp = (in[0] >> 6) & 1;
        h.sap = in[1] >> 4;
        h.dst = be24(in + 2); h.src = be24(in + 5);
        h.fmf = in[8] >> 7; h.blocks = in[8] & 0x7F;
        h.status = in[9];
        return true;
    case kDpfShortDefined:
        h.group = in[0] >> 7; h.resp = (in[0] >> 6) & 1;
        h.blocks = ((in[0] >> 4) & 3) << 4 | (in[1] & 15);
        h.sap = in[1] >> 4;
        h.dst = be24(in + 2); h.src = be24(in + 5);
        h.dd = in[8] >> 2; h.sarq = (in[8] >> 1) & 1; h.fmf = in[8] & 1;
        h.pad = in[9];
        return true;
    case kDpfShortRaw:
        h.group = in[0] >> 7; h.resp = (in[0] >> 6) & 1;
        h.blocks = ((in[0] >> 4) & 3) << 4 | (in[1] & 15);
        h.sap = in[1] >> 4;
        h.dst = be24(in + 2); h.src = be24(in + 5);
        h.sp = in[8] >> 5; h.dp = (in[8] >> 2) & 7;
        if (h.blocks == 0) h.status = (in[8] & 3) << 8 | in[9];
        else { h.sarq = (in[8] >> 1) & 1; h.fmf = in[8] & 1; h.pad = in[9]; }
        return true;
    case kDpfUdt:
        h.group = in[0] >> 7; h.resp = (in[0] >> 6) & 1;
        h.sap = in[1] >> 4;
        h.dst = be24(in + 2); h.src = be24(in + 5);
        h.blocks = in[8] & 3;
        return true;
    case kDpfProprietary:
        h.sap = in[0] >> 4;
        return true;
    default:
        return false;
    }
}

int dataBlockBytes(int dt, bool confirmed) {
    switch (dt) {
    case kDtRate12: return confirmed ? 10 : 12;
    case kDtRate34: return confirmed ? 16 : 18;
    case kDtRate1: return confirmed ? 22 : 24;
    default: return 0;
    }
}

static uint16_t confirmedMask(int dt) { return dt == kDtRate34 ? kMaskRate34 : dt == kDtRate1 ? kMaskRate1 : kMaskRate12; }

void dataBlockEncode(int dt, const uint8_t* user, size_t n, bool confirmed, unsigned dbsn, Bits& payload196) {
    const int total = dt == kDtRate12 ? 12 : dt == kDtRate34 ? 18 : 24;
    uint8_t oct[24] = {};
    if (confirmed) {
        const int ub = total - 2;
        uint8_t u[22] = {};
        memcpy(u, user, std::min<size_t>(n, (size_t)ub));
        const unsigned crc = (crc9(u, (size_t)ub, dbsn) ^ confirmedMask(dt)) & 0x1FF;
        oct[0] = (uint8_t)(((dbsn & 0x7F) << 1) | ((crc >> 8) & 1));
        oct[1] = (uint8_t)(crc & 0xFF);
        memcpy(oct + 2, u, (size_t)ub);
    } else {
        memcpy(oct, user, std::min<size_t>(n, (size_t)total));
    }
    if (dt == kDtRate34) {
        trellis34Encode(oct, payload196);
    } else if (dt == kDtRate1) {
        rate1Encode(oct, payload196);
    } else {
        Bits info;
        bytesToBits(oct, 12, info);
        bptc196Encode(info, payload196);
    }
}

bool dataBlockParse(int dt, const uint8_t* oct, bool confirmed, DataBlock& out) {
    const int total = dt == kDtRate12 ? 12 : dt == kDtRate34 ? 18 : dt == kDtRate1 ? 24 : 0;
    if (!total) return false;
    out.confirmed = confirmed;
    out.crcOk = true;
    if (confirmed) {
        out.dbsn = oct[0] >> 1;
        const unsigned rx = (unsigned)(((oct[0] & 1) << 8) | oct[1]);
        const unsigned calc = (crc9(oct + 2, (size_t)(total - 2), out.dbsn) ^ confirmedMask(dt)) & 0x1FF;
        out.crcOk = rx == calc;
        out.bytes.assign(oct + 2, oct + total);
    } else {
        out.bytes.assign(oct, oct + total);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------- text

static void appendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) s += (char)cp;
    else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63)); }
    else if (cp < 0x110000) { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 63)); s += (char)(0x80 | ((cp >> 6) & 63)); s += (char)(0x80 | (cp & 63)); }
    else s += '?';
}

static std::string cleanText(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        if (c == '\r' || c == '\n' || c == '\t') out += ' ';
        else if (c < 0x20 || c == 0x7F) { /* control characters and padding are dropped */ }
        else out += (char)c;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

const char* ddName(int dd) {
    switch (dd) {
    case kDdBinary: return "binary";
    case kDdBcd: return "BCD";
    case kDd7Bit: return "7 bit";
    case kDdIso8859_1: return "ISO 8859-1";
    case kDdUtf8: return "UTF-8";
    case kDdUtf16: return "UTF-16";
    case kDdUtf16Be: return "UTF-16BE";
    case kDdUtf16Le: return "UTF-16LE";
    case kDdUtf32: return "UTF-32";
    case kDdUtf32Be: return "UTF-32BE";
    case kDdUtf32Le: return "UTF-32LE";
    default: return dd > kDdIso8859_1 && dd <= kDdIso8859_16 ? "ISO 8859" : "reserved";
    }
}

std::string textFromBytes(const uint8_t* p, size_t n, int format, bool ta) {
    // short data formats (table 9.50); a talker alias uses its own two bit code (0 7 bit, 1 8 bit, 2 UTF-8, 3 UTF-16BE)
    int f = format;
    if (ta) f = format == 0 ? kDd7Bit : format == 1 ? kDdIso8859_1 : format == 2 ? kDdUtf8 : kDdUtf16Be;
    std::string s;
    if (f == 0) {
        bool printable = n > 0;
        for (size_t i = 0; i < n; i++) if (p[i] < 0x20 || p[i] > 0x7E) printable = false;
        if (printable) return std::string((const char*)p, n);
        char b[8];
        for (size_t i = 0; i < n && i < 24; i++) { snprintf(b, sizeof b, "%02X ", p[i]); s += b; }
        if (n > 24) s += "...";
        if (!s.empty() && s.back() == ' ') s.pop_back();
        return "[" + s + "]";
    }
    if (f == 1) {
        for (size_t i = 0; i < n; i++) { s += (char)('0' + (p[i] >> 4 & 15) % 10); s += (char)('0' + (p[i] & 15) % 10); }
        return s;
    }
    if (f == 2) {                       // 7 bit characters packed one after the other (the packing is not given in TS 102 361-1: the talker alias rule is used)
        size_t bits = n * 8;
        for (size_t i = 0; i + 7 <= bits; i += 7) {
            unsigned v = 0;
            for (int k = 0; k < 7; k++) { const size_t q = i + (size_t)k; v = (v << 1) | ((p[q / 8] >> (7 - q % 8)) & 1); }
            s += (char)v;
        }
        return cleanText(s);
    }
    if (f >= kDdIso8859_1 && f <= kDdIso8859_16) {
        for (size_t i = 0; i < n; i++) {
            if (p[i] < 0x80) s += (char)p[i];
            else if (f == kDdIso8859_1) appendUtf8(s, p[i]);
            else s += '?';
        }
        return cleanText(s);
    }
    if (f == kDdUtf8) {
        for (size_t i = 0; i < n; i++) {
            const unsigned char c = p[i];
            if (c < 0x80) { s += (char)c; continue; }
            int len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 0;
            bool ok = len > 0 && i + (size_t)len <= n;
            for (int k = 1; ok && k < len; k++) if ((p[i + (size_t)k] & 0xC0) != 0x80) ok = false;
            if (ok) { s.append((const char*)p + i, (size_t)len); i += (size_t)len - 1; }
            else s += '?';
        }
        return cleanText(s);
    }
    if (f >= kDdUtf16 && f <= kDdUtf16Le) {
        bool le = f == kDdUtf16Le;
        size_t i = 0;
        if (f == kDdUtf16 && n >= 2) {
            if (p[0] == 0xFF && p[1] == 0xFE) { le = true; i = 2; }
            else if (p[0] == 0xFE && p[1] == 0xFF) { le = false; i = 2; }
        }
        for (; i + 1 < n; i += 2) {
            uint32_t cp = le ? (uint32_t)(p[i] | p[i + 1] << 8) : (uint32_t)(p[i] << 8 | p[i + 1]);
            if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < n) {
                const uint32_t lo = le ? (uint32_t)(p[i + 2] | p[i + 3] << 8) : (uint32_t)(p[i + 2] << 8 | p[i + 3]);
                if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
            }
            appendUtf8(s, cp);
        }
        return cleanText(s);
    }
    if (f >= kDdUtf32 && f <= kDdUtf32Le) {
        bool le = f == kDdUtf32Le;
        for (size_t i = 0; i + 3 < n; i += 4) {
            const uint32_t cp = le ? (uint32_t)(p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (uint32_t)p[i + 3] << 24)
                                   : (uint32_t)((uint32_t)p[i] << 24 | p[i + 1] << 16 | p[i + 2] << 8 | p[i + 3]);
            appendUtf8(s, cp);
        }
        return cleanText(s);
    }
    return "";
}

// ---------------------------------------------------------------------------------------------------- talker alias

void TalkerAlias::add(const FullLc& lc) {
    if (lc.flco == kFlcoTalkerAliasHeader) {
        clear();
        format = lc.raw[2] >> 6;
        length = (lc.raw[2] >> 1) & 31;
        memcpy(data, lc.raw + 2, 7);
        have = 1;
    } else if (lc.flco >= kFlcoTalkerAliasBlock1 && lc.flco <= kFlcoTalkerAliasBlock3) {
        const int i = lc.flco - kFlcoTalkerAliasBlock1 + 1;
        memcpy(data + 7 * i, lc.raw + 2, 7);
        have |= (uint8_t)(1 << i);
    }
}

static int aliasPieces(int format, int length) {
    const int perHeader = format == 0 ? 7 : format == 3 ? 3 : 6, perBlock = format == 0 ? 8 : format == 3 ? 3 : 7;
    // 16 bit characters: the header holds 3 characters, the blocks 3.5; the counts of the specification (3, 6, 10, 13) are used
    if (format == 3) return length <= 3 ? 1 : length <= 6 ? 2 : length <= 10 ? 3 : 4;
    if (length <= perHeader) return 1;
    return 1 + std::min(3, (length - perHeader + perBlock - 1) / perBlock);
}

bool TalkerAlias::complete() const {
    if (!(have & 1)) return false;
    const int n = aliasPieces(format, length);
    for (int i = 0; i < n; i++) if (!(have & (1 << i))) return false;
    return length > 0;
}

std::string TalkerAlias::text() const {
    if (!complete()) return "";
    uint8_t bytes[28];
    size_t n = 0;
    if (format == 0) {      // 7 bit: 49 bits in the header, then 56 per block
        Bits bits;
        bits.push_back(data[0] & 1);
        for (int i = 1; i < 7; i++) putBits(bits, data[i], 8);
        for (int b = 1; b < 4; b++)
            for (int i = 0; i < 7; i++) putBits(bits, data[7 * b + i], 8);
        std::string s;
        for (int c = 0; c < length && (size_t)(c * 7 + 7) <= bits.size(); c++) s += (char)getBits(bits, (size_t)c * 7, 7);
        return cleanText(s);
    }
    for (int i = 1; i < 7; i++) bytes[n++] = data[i];
    for (int b = 1; b < 4; b++)
        for (int i = 0; i < 7; i++) bytes[n++] = data[7 * b + i];
    size_t len = format == 3 ? (size_t)length * 2 : (size_t)length;
    len = std::min(len, n);
    return textFromBytes(bytes, len, format, true);
}

int talkerAliasPdus(const std::string& alias, int format, FullLc out[4]) {
    std::vector<uint8_t> bytes;
    int length = 0;
    if (format == 3) {
        for (unsigned char c : alias) { bytes.push_back(0); bytes.push_back(c); }
        length = (int)alias.size();
    } else {
        bytes.assign(alias.begin(), alias.end());
        length = (int)alias.size();
    }
    length = std::min(length, format == 3 ? 13 : 27);
    bytes.resize(28, 0);
    for (int i = 0; i < 4; i++) { out[i] = FullLc(); out[i].flco = kFlcoTalkerAliasHeader + i; }
    uint8_t* h = out[0].raw;
    h[0] = (uint8_t)kFlcoTalkerAliasHeader;
    h[2] = (uint8_t)((format << 6) | (length << 1));
    for (int i = 0; i < 6; i++) h[3 + i] = bytes[i];
    for (int b = 1; b < 4; b++) {
        out[b].raw[0] = (uint8_t)(kFlcoTalkerAliasHeader + b);
        for (int i = 0; i < 7; i++) out[b].raw[2 + i] = bytes[6 + 7 * (b - 1) + i];
    }
    return aliasPieces(format, length);
}

} // namespace dmr
} // namespace dect2
