// TETRAPOL data blocks, HDLC, TPDU and the signalling messages OnAir reads (tetrapol_link.h).
#include "dect2/tetrapol/tetrapol_link.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace tetrapol {

namespace {

int fnOf(const DataFrame& f) { return (f.payload[0] & 1) | ((f.payload[1] & 1) << 1); }

// MSB-first field of a byte string (bit 0 = the top bit of t[0])
int field(const uint8_t* t, int bitOff, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        const int b = bitOff + i;
        v = (v << 1) | ((t[b / 8] >> (7 - b % 8)) & 1);
    }
    return v;
}

} // namespace

bool BlockAssembler::finish(std::vector<uint8_t>& bytes, int dataFrames) {
    if (bad_ > 0) {
        // one frame lost: the parity frame (data[2..65] of all frames XOR to zero) gives it back; no parity in 1 or 2 frame blocks
        if (n_ < 3 || bad_ > 1) { reset(); return false; }
        int k = -1;
        for (int i = 0; i < n_; i++) if (!ok_[i]) k = i;
        if (k < 0 || k == n_ - 1) { /* only the parity frame was lost: nothing to repair */ }
        else {
            for (int b = 2; b < kPayloadBits; b++) {
                uint8_t v = 0;
                for (int i = 0; i < n_; i++) if (i != k) v ^= fr_[i].payload[b];
                fr_[k].payload[b] = v;
            }
            repaired_++;
        }
    }
    bytes.assign((size_t)dataFrames * 8, 0);
    for (int i = 0; i < dataFrames; i++)
        for (int b = 0; b < 64; b++)
            if (fr_[i].payload[2 + b] & 1) bytes[(size_t)(i * 8 + b / 8)] |= (uint8_t)(1u << (b % 8));
    reset();
    return true;
}

bool BlockAssembler::push(const DataFrame& f, bool ok, std::vector<uint8_t>& bytes) {
    if (n_ > kBlockFramesMax) reset();
    const int fn = ok ? fnOf(f) : -1;
    if (n_ == 0) {
        if (!ok) return false;
        if (fn == 0) { fr_[0] = f; ok_[0] = true; n_ = 1; return finish(bytes, 1); }
        if (fn != 1) return false;   // a frame from the middle of a block we missed the start of
        fr_[0] = f; ok_[0] = true; fn_[0] = 1; n_ = 1;
        return false;
    }
    fr_[n_] = f; ok_[n_] = ok; fn_[n_] = fn;
    n_++;
    if (!ok) {
        // a lost frame: only a multi-frame block can get it back (by its parity frame); a lost second frame may still be one
        if (++bad_ > 1) reset();
        return false;
    }
    if (n_ == 2) {
        if (fn == 3) return finish(bytes, 2);
        if (fn == 2) return false;
        reset();
        return fn == 0 || fn == 1 ? push(f, true, bytes) : false;
    }
    // multi-frame block: 01, 10, 11 ..., 10, then the parity frame 01 (at least three data frames)
    if (fn == 1) {
        const int prev = fn_[n_ - 2];
        if (n_ - 1 >= 3 && (prev == 2 || prev == -1)) return finish(bytes, n_ - 1);
        reset();
        return push(f, true, bytes);
    }
    if (fn == 0) { reset(); return push(f, true, bytes); }
    return false;
}

std::vector<DataFrame> blockFrames(const std::vector<uint8_t>& bytes, int scramble, int asb0) {
    const int n = (int)(bytes.size() / 8);
    std::vector<DataFrame> out((size_t)(n >= 3 ? n + 1 : n));
    for (int i = 0; i < n; i++) {
        DataFrame& f = out[(size_t)i];
        for (int b = 0; b < 64; b++) f.payload[2 + b] = (uint8_t)((bytes[(size_t)(i * 8 + b / 8)] >> (b % 8)) & 1);
    }
    auto setFn = [](DataFrame& f, int fn) { f.payload[0] = (uint8_t)(fn & 1); f.payload[1] = (uint8_t)((fn >> 1) & 1); };
    if (n == 1) setFn(out[0], 0);
    else if (n == 2) { setFn(out[0], 1); setFn(out[1], 3); }
    else if (n >= 3) {
        setFn(out[0], 1);
        setFn(out[1], 2);
        for (int i = 2; i < n - 1; i++) setFn(out[(size_t)i], 3);
        setFn(out[(size_t)(n - 1)], 2);
        DataFrame& p = out[(size_t)n];
        setFn(p, 1);
        for (int b = 2; b < kPayloadBits; b++) {
            uint8_t v = 0;
            for (int i = 0; i < n; i++) v ^= out[(size_t)i].payload[b];
            p.payload[b] = v;
        }
    }
    for (auto& f : out) { f.scramble = scramble; f.asb[0] = (uint8_t)(asb0 & 1); }
    return out;
}

// FCS-16 over the bits in the order they are sent (LSB of each byte first): x^16 + x^12 + x^5 + 1, the first 16 bits inverted, and a frame
// whose FCS is right leaves 0xffff
static uint32_t fcsRun(const uint8_t* bytes, int nbits, int zeros) {
    uint32_t r = 0;
    for (int i = 0; i < nbits + zeros; i++) {
        uint32_t bit = i < nbits ? (uint32_t)((bytes[i / 8] >> (i % 8)) & 1) : 0u;
        if (i < 16) bit ^= 1u;
        r = (r << 1) | bit;
        if (r & 0x10000u) r ^= 0x11021u;
    }
    return r & 0xffffu;
}

bool fcsOk(const uint8_t* bytes, int nbytes) {
    if (nbytes < 3) return false;
    return fcsRun(bytes, nbytes * 8, 0) == 0xffffu;
}

void appendFcs(std::vector<uint8_t>& b) {
    const int n = (int)b.size();
    b.push_back(0); b.push_back(0);
    const uint32_t f = fcsRun(b.data(), n * 8, 16) ^ 0xffffu;
    for (int i = 0; i < 16; i++) {
        const int pos = n * 8 + i;
        if ((f >> (15 - i)) & 1) b[(size_t)(pos / 8)] |= (uint8_t)(1u << (pos % 8));
    }
}

bool parseHdlc(const std::vector<uint8_t>& b, Hdlc& h) {
    h = Hdlc();
    if (b.size() < 5) return false;
    h.z = field(b.data(), 0, 1);
    h.y = field(b.data(), 1, 3);
    h.x = field(b.data(), 4, 12);
    h.cmd = b[2];
    h.info.assign(b.begin() + 3, b.end() - 2);
    h.fcsOk = fcsOk(b.data(), (int)b.size());
    return h.fcsOk;
}

std::vector<uint8_t> buildHdlc(int z, int y, int x, uint8_t cmd, const std::vector<uint8_t>& info, int nbytes) {
    std::vector<uint8_t> b((size_t)nbytes, 0);
    b[0] = (uint8_t)(((z & 1) << 7) | ((y & 7) << 4) | ((x >> 8) & 15));
    b[1] = (uint8_t)(x & 255);
    b[2] = cmd;
    for (size_t i = 0; i < info.size() && 3 + i < (size_t)nbytes - 2; i++) b[3 + i] = info[i];
    const uint32_t r = fcsRun(b.data(), (nbytes - 2) * 8, 16);
    const uint32_t f = r ^ 0xffffu;   // sent highest coefficient first
    for (int i = 0; i < 16; i++) {
        const int pos = (nbytes - 2) * 8 + i;
        if ((f >> (15 - i)) & 1) b[(size_t)(pos / 8)] |= (uint8_t)(1u << (pos % 8));
    }
    return b;
}

bool tsduOf(const Hdlc& h, std::vector<uint8_t>& tsdu) {
    tsdu.clear();
    if (!h.fcsOk || h.info.empty()) return false;
    const uint8_t* d = h.info.data();
    const int n = (int)h.info.size();
    const bool ext = (d[0] >> 7) & 1, seg = (d[0] >> 6) & 1;
    if (h.cmd == kCmdUiVch || h.cmd == kCmdUiCd) return false;   // LSDUs, not TPDUs (describeVch / describeCd)
    if ((h.cmd & 0xef) == kCmdUi || (h.cmd & 0xef) == 0x07) {
        // unnumbered information (UI, UI P0): PAS 0001-3-3 9.5.1.2
        if (ext || seg) return false;   // segmented broadcasts are not reassembled
        if (n > 3) {
            const int len = d[1];
            if (len < 1 || len > n - 2) return false;
            tsdu.assign(d + 2, d + 2 + len);
        } else {
            tsdu.assign(d + 1, d + n);
        }
        return !tsdu.empty();
    }
    if ((h.cmd & 1) == 0) {
        // information frame: a connection-oriented TPDU (PAS 0001-3-3 8.3.1: EXT, SEG, D, TPDU code 5 bits; PAR_FIELD / DEST_REF octet;
        // the length octet when SEG = 0). Only DT (11011) and DTE (11100) are read: the CR / CC / FCR layouts are in figures not checked here.
        const bool hasData = (d[0] >> 5) & 1;
        const int code = d[0] & 0x1f;
        if (ext || seg || !hasData || n < 3 || (code != 0x1b && code != 0x1c)) return false;
        const int off = 2;
        const int len = d[off];
        if (len < 1 || len > n - off - 1) return false;
        tsdu.assign(d + off + 1, d + off + 1 + len);
        return true;
    }
    return false;
}

std::vector<uint8_t> uiInfo(const std::vector<uint8_t>& tsdu, int nbytes) {
    std::vector<uint8_t> info;
    info.push_back(0x00);   // ext 0, seg 0, prio 0, tsap 0
    if (nbytes > 8) info.push_back((uint8_t)tsdu.size());
    info.insert(info.end(), tsdu.begin(), tsdu.end());
    return info;
}

int uiBlockBytes(size_t len) {
    if (len <= 2) return 8;
    int n = 16;
    while ((size_t)(n - 7) < len) n += 8;
    return n;
}

bool parseSysInfo(const uint8_t* t, int len, SysInfo& s) {
    s = SysInfo();
    if (len < 9 || t[0] != kDSystemInfo) return false;
    // D_SYSTEM_INFO, PAS 0001-3-2 4.4.76 figures 78 / 79; CELL_STATE (5.3.23): MODE in bits 8..6, BCH in bit 5 (checked on a real
    // control channel: 0x00 in frames 0..3, 0x10 in frames 100..103)
    const int mode = (t[1] >> 5) & 7;
    s.mode = mode;
    s.bch = (t[1] >> 4) & 1;
    if (mode == 0) {
        if (len < 17) return false;
        s.cellConfig = t[2];
        s.cellBn = field(t + 8, 4, 12);
        s.systemTime = t[13];
        s.cellAccess = t[14];
        s.superframe = field(t + 15, 4, 12);
        s.country = t[3];
        s.systemId = t[4];
        s.locArea = t[5];
        s.bnId = t[6];
        const int type = field(t + 7, 0, 2);   // CELL_ID 5.3.21 (12 bits), then CELL_BN 12 bits
        if (type == 0) { s.bsId = field(t + 7, 2, 6); s.rswId = field(t + 7, 8, 4); }
        else if (type == 1) { s.rswId = field(t + 7, 2, 6); s.bsId = field(t + 7, 8, 4); }
        s.uScramble = t[10];
        return true;
    }
    if (mode > 4) return false;
    s.disconnected = true;
    {
        // BSC-disconnected form (PAS 0001-3-2 4.4.76 figure 79): CELL_STATE in the high quartet of octet 2, CELL_ID (5.3.21) in the next 12 bits
        const int type = field(t + 1, 4, 2);
        if (type == 0) { s.bsId = field(t + 1, 6, 6); s.rswId = field(t + 1, 12, 4); }
        else if (type == 1) { s.rswId = field(t + 1, 6, 6); s.bsId = field(t + 1, 12, 4); }
    }
    s.bnId = t[3];
    s.uScramble = t[4];
    s.band = field(t + 7, 0, 4);
    s.channelId = field(t + 7, 4, 12);
    return true;
}

std::vector<uint8_t> buildSysInfo(const SysInfo& s) {
    std::vector<uint8_t> t(17, 0);
    t[0] = kDSystemInfo;
    t[1] = (uint8_t)((s.bch & 1) << 4);   // cell state: normal mode, the bch bit
    t[2] = (uint8_t)(s.cellConfig < 0 ? 0 : s.cellConfig);
    t[3] = (uint8_t)s.country;
    t[4] = (uint8_t)s.systemId;
    t[5] = (uint8_t)s.locArea;
    t[6] = (uint8_t)s.bnId;
    // cell id, format 0: type 2 bits, BS 6 bits, RSW 4 bits; then 4 bits of the cell BN
    t[7] = (uint8_t)(s.bsId & 63);
    t[8] = (uint8_t)((s.rswId & 15) << 4);
    t[10] = (uint8_t)s.uScramble;
    return t;
}

bool parseCallSignal(const uint8_t* t, int len, CallSignal& c) {
    c = CallSignal();
    if (len < 1) return false;
    switch (t[0]) {
    case kDGroupActivation:
        if (len < 9) return false;
        c.kind = CallSignal::GroupActivation;
        c.groupId = field(t + 1, 4, 12);
        c.channelId = field(t + 4, 4, 12);
        c.dScramble = t[7];
        c.keyRef = t[8];
        return true;
    case kDCallConnect:
        if (len < 15) return false;
        c.kind = CallSignal::CallConnect;
        c.channelId = field(t + 2, 4, 12);
        c.dScramble = t[5];
        c.keyRef = t[6];
        return true;
    case kDCallStart: {
        c.kind = CallSignal::CallStart;
        int i = 1;
        while (i < len) {
            const int iei = t[i++];
            if (iei == kIeiKeyReference) {
                if (i >= len) return false;
                c.keyRef = t[i++];
            } else if (iei == kIeiKeyOfCall) {
                if (i >= len) return false;
                i += 1 + t[i];   // skipped, never read
            } else {
                return false;   // an element OnAir does not know: the message is not used
            }
        }
        return i == len;
    }
    case kDCallEnd: case kDGroupEnd: case kDRelease: case kDBackCch: case kDGroupIdle:
        c.kind = CallSignal::CallEnd;
        return true;
    default:
        return false;
    }
}

std::vector<uint8_t> buildGroupActivation(int groupId, int channelId, int dScramble, int keyRef) {
    std::vector<uint8_t> t(9, 0);
    t[0] = kDGroupActivation;
    t[1] = (uint8_t)((groupId >> 8) & 15);
    t[2] = (uint8_t)(groupId & 255);
    t[4] = (uint8_t)((channelId >> 8) & 15);
    t[5] = (uint8_t)(channelId & 255);
    t[6] = (uint8_t)dScramble;
    t[7] = (uint8_t)dScramble;
    t[8] = (uint8_t)keyRef;
    return t;
}

std::vector<uint8_t> buildCallStart(int keyRef) {
    std::vector<uint8_t> t{kDCallStart};
    if (keyRef >= 0) { t.push_back(kIeiKeyReference); t.push_back((uint8_t)keyRef); }
    return t;
}

std::vector<uint8_t> buildCallEnd() { return {kDCallEnd, 0}; }


// ---- messages as text

// PAS 0001-3-2 v2.3.9 clause 4.5 (CODOP values of the downlink TSDUs)
const char* codopName(int c) {
    switch (c) {
    case 0x08: return "D_REJECT"; case 0x09: return "D_REFUSAL"; case 0x0a: return "D_END";
    case 0x0b: return "D_BACK_CCH"; case 0x0c: return "D_RELEASE"; case 0x0f: return "D_HOOK_ON_INVITATION";
    case 0x10: return "D_RETURN"; case 0x12: return "D_CALL_WAITING"; case 0x13: return "D_AUTHENTICATION";
    case 0x16: return "D_AUTHORISATION"; case 0x18: return "D_CHANNEL_INIT"; case 0x21: return "D_REGISTRATION_NAK";
    case 0x22: return "D_REGISTRATION_ACK"; case 0x23: return "D_FORCED_REGISTRATION"; case 0x25: return "D_LOCATION_ACTIVITY_ACK";
    case 0x31: return "D_CALL_ALERT"; case 0x32: return "D_CALL_SETUP"; case 0x34: return "D_CALL_CONNECT";
    case 0x35: return "D_CALL_SWITCH"; case 0x39: return "D_TRANSFER_NAK"; case 0x3e: return "D_CALL_START";
    case 0x42: return "D_FUNCTIONAL_SHORT_DATA"; case 0x45: return "D_DATA_MSG_DOWN"; case 0x46: return "D_EXPLICIT_SHORT_DATA";
    case 0x48: return "D_DATA_END"; case 0x49: return "D_DATAGRAM_NOTIFY"; case 0x4a: return "D_DATAGRAM";
    case 0x4b: return "D_BROADCAST"; case 0x4c: return "D_DATA_SERV"; case 0x4e: return "D_DATA_DOWN_STATUS";
    case 0x53: return "D_EMERGENCY_NOTIFICATION"; case 0x55: return "D_GROUP_ACTIVATION"; case 0x56: return "D_ECH_ACTIVATION";
    case 0x57: return "D_GROUP_END"; case 0x58: return "D_GROUP_IDLE"; case 0x59: return "D_GROUP_REJECT";
    case 0x5a: return "D_ECH_REJECT"; case 0x5b: return "D_GROUP_PAGING"; case 0x5c: return "D_BROADCAST_NOTIFICATION";
    case 0x5f: return "D_EMERGENCY_ACK"; case 0x60: return "D_CONNECT_DCH"; case 0x62: return "D_CONNECT_CCH";
    case 0x63: return "D_DATA_AUTHENTICATION"; case 0x64: return "D_DATA_REQUEST"; case 0x65: return "D_DCH_OPEN";
    case 0x67: return "D_EXTENDED_STATUS"; case 0x68: return "D_CCH_OPEN"; case 0x69: return "D_BROADCAST_WAITING";
    case 0x70: return "D_ACCESS_DISABLED"; case 0x71: return "D_TRAFFIC_ENABLED"; case 0x72: return "D_TRAFFIC_DISABLED";
    case 0x76: return "D_DEVIATION_ON"; case 0x77: return "D_ABILITY_MNGT"; case 0x78: return "D_SERVICE_DISABLED";
    case 0x80: return "D_EMERGENCY_NAK"; case 0x82: return "D_GROUP_OVERLOAD_ID"; case 0x83: return "D_ECH_OVERLOAD_ID";
    case 0x84: return "D_PRIORITY_GRP_WAITING"; case 0x86: return "D_OC_ACTIVATION"; case 0x87: return "D_OC_REJECT";
    case 0x88: return "D_OC_PAGING"; case 0x90: return "D_SYSTEM_INFO"; case 0x92: return "D_GROUP_LIST";
    case 0x93: return "D_GROUP_COMPOSITION"; case 0x94: return "D_NEIGHBOURING_CELL"; case 0x95: return "D_ECCH_DESCRIPTION";
    case 0x96: return "D_ADDITIONAL_PARTICIPANTS"; case 0x97: return "D_DDCH_DESCRIPTION"; case 0x98: return "D_TKG_PRIO_LIST";
    case 0x99: return "D_GROUP_MASTER"; case 0xc5: return "D_INFORMATION_DELIVERY"; case 0xe0: return "D_CALL_ACTIVATION";
    case 0xe1: return "D_CALL_COMPOSITION"; case 0xe2: return "D_CALL_END"; case 0xe3: return "D_CALL_OVERLOAD_ID";
    default: return nullptr;
    }
}

std::string rtiText(int z, int y, int x) {
    char b[24];
    snprintf(b, sizeof b, "%d.%d.0x%03x", z & 1, y & 7, x & 0xfff);
    return b;
}

namespace {
std::string hexOf(const uint8_t* p, int n) {
    std::string o;
    char b[4];
    for (int i = 0; i < n; i++) { snprintf(b, sizeof b, i ? " %02x" : "%02x", p[i]); o += b; }
    return o;
}
// KEY_REFERENCE (PAS 0001-3-2 5.3.41): key type in bits 5..8, key index in bits 1..4
std::string keyText(int k) {
    if (k < 0) return "";
    char b[64];
    if (keyRefClear(k)) snprintf(b, sizeof b, "key ref 0x%02x (clear)", k);
    else snprintf(b, sizeof b, "key ref 0x%02x (encrypted, key type %d index %d)", k, k >> 4, k & 15);
    return b;
}
// CELL_ID (PAS 0001-3-2 5.3.21): FORMAT 2 bits; 00: BS_ID 6 bits then RSW_ID 4 bits, 01: RSW_ID 6 bits then BS_ID 4 bits
std::string cellIdText(const uint8_t* d, int bitOff = 0) {
    const int type = field(d, bitOff, 2);
    char b[32];
    if (type == 0) snprintf(b, sizeof b, "%d/%d", field(d, bitOff + 2, 6), field(d, bitOff + 8, 4));
    else if (type == 1) snprintf(b, sizeof b, "%d/%d", field(d, bitOff + 8, 4), field(d, bitOff + 2, 6));
    else snprintf(b, sizeof b, "?");
    return b;   // BS id / RSW id
}
// ACTIVATION_MODE (PAS 0001-3-2 5.3.3): the high quartet of its octet, TYPE 2 bits then HOOK 2 bits
std::string modeText(int octet) {
    static const char* const type[4] = {"open channel", "internal group", "broadcast", "external group"};
    static const char* const hook[4] = {"automatic", "automatic with tone", "ring", "hook 3"};
    return std::string(type[(octet >> 6) & 3]) + " (" + hook[(octet >> 4) & 3] + ") ";
}
} // namespace

std::string addressText(const uint8_t* p, int len, int& used) {
    used = 0;
    if (len < 1) return "?";
    const int cna = (p[0] >> 4) & 7;
    std::string o;
    if (cna == 0) { used = 1; return "-"; }
    if (cna == 1) {   // RFSI: 9 digits, R R R F S S I I I
        if (len < 5) return "?";
        used = 5;
        int dig[9];
        for (int i = 0; i < 9; i++) dig[i] = field(p, 4 + 4 * i, 4);
        char b[24];
        snprintf(b, sizeof b, "%d%d%d-%d-%d%d-%d%d%d", dig[0], dig[1], dig[2], dig[3], dig[4], dig[5], dig[6], dig[7], dig[8]);
        return b;
    }
    if (cna == 3) {   // PABX: a digit count then that many digits
        const int n = p[0] & 15;
        used = 1 + (n + 1) / 2;
        if (used > len) { used = len; return "?"; }
        for (int i = 0; i < n; i++) o += (char)('0' + std::min(9, field(p, 8 + 4 * i, 4)));
        return "PABX " + o;
    }
    static const char* const kinds[8] = {"", "", "X.400", "", "functional", "long", "binary", "escape"};
    used = len;
    return std::string(kinds[cna]) + " address";
}

bool parseNeighbours(const uint8_t* t, int len, std::vector<Neighbour>& out) {
    out.clear();
    if (len < 2 || t[0] != 0x94) return false;
    const int n = t[1] & 15;
    if (!n) return true;
    if (len < 3 + 3 * n) return false;
    const uint8_t* d = t + 3;
    for (int i = 0; i < n; i++, d += 3) {
        Neighbour nb;
        nb.bnNb = field(d, 0, 4);
        nb.channelId = field(d, 4, 12);
        nb.param = d[2];
        out.push_back(nb);
    }
    int rest = len - 3 - 3 * n;
    size_t ci = 0;
    while (rest >= 2) {
        const int iei = d[0], il = d[1];
        d += 2; rest -= 2;
        if (il > rest) break;
        if (iei == 0x02)   // IEI_CELL_ID_LIST: one cell id per adjacent cell, in order
            for (int k = 0; k + 1 < il && ci < out.size(); k += 2, ci++) {
                const int type = field(d + k, 0, 2);
                if (type == 0) { out[ci].bsId = field(d + k, 2, 6); out[ci].rswId = field(d + k, 8, 4); }
                else if (type == 1) { out[ci].bsId = field(d + k, 8, 4); out[ci].rswId = field(d + k, 2, 6); }
            }
        d += il; rest -= il;
    }
    return true;
}

std::string describeTsdu(const uint8_t* t, int len, int* keyRef) {
    if (keyRef) *keyRef = -1;
    if (len < 1) return "";
    const int c = t[0];
    const char* nm = codopName(c);
    char b[256];
    std::string o = nm ? nm : "";
    if (!nm) { snprintf(b, sizeof b, "codop 0x%02x (not in the public PAS)", c); o = b; }
    auto add = [&](const char* fmt, auto... a) { snprintf(b, sizeof b, fmt, a...); o += b; };
    auto key = [&](int k) { if (keyRef) *keyRef = k; o += " " + keyText(k); };
    switch (c) {
    case 0x90: {
        SysInfo s;
        if (!parseSysInfo(t, len, s)) break;
        if (!s.disconnected)
            add(": country %d system %d LA %d BN %d cell %d/%d cell BN 0x%03x, mux type %d, U scramble %d", s.country, s.systemId,
                s.locArea, s.bnId, s.bsId, s.rswId, s.cellBn, s.muxType(), s.uScramble);   // the superframe count is left out: one row
        else add(": cell disconnected (mode %d), BN %d cell %d/%d, channel %d", s.mode, s.bnId, s.bsId, s.rswId, s.channelId);
        return o;
    }
    case 0x94: {
        std::vector<Neighbour> v;
        if (!parseNeighbours(t, len, v)) break;
        add(": %zu adjacent cells:", v.size());
        for (const auto& n : v) {
            add(" ch %d BN %d", n.channelId, n.bnNb);
            if (n.bsId >= 0) add(" cell %d/%d", n.bsId, n.rswId);
            o += ";";
        }
        return o;
    }
    case 0x55: case 0x5b: {   // D_GROUP_ACTIVATION (4.4.44, figure 46), D_GROUP_PAGING (4.4.51)
        if (len < 5) break;
        add(": %sgroup %d coverage 0x%02x", modeText(t[1]).c_str(), field(t + 1, 4, 12), t[3]);
        if (c != 0x5b && len >= 9) {
            add(" channel %d U scramble %d D scramble %d", field(t + 4, 4, 12), t[6], t[7]);
            key(t[8]);
            // IEI TTI = 0x05 (5.3), TV 2+1 octets
            if (len >= 12 && t[9] == 0x05) o += " TTI " + rtiText(field(t + 10, 0, 1), field(t + 10, 1, 3), field(t + 10, 4, 12));
        } else if (c == 0x5b) key(t[4]);
        return o;
    }
    case 0x92: {   // D_GROUP_LIST (4.4.48, figure 50; REFERENCE_LIST 5.3.56, TYPE_NB 5.3.89: 01 talk group, 10 emergency, 11 open channel)
        if (len < 2) break;
        add(": revision %d", t[1] >> 5);
        if ((t[1] >> 5) == 0 || len < 3) return o;
        int i = 3;
        std::string tg, og, em;
        while (i < len) {
            const int type = t[i] >> 6, num = t[i] & 63;
            i++;
            if (type == 0) break;
            for (int k = 0; k < num; k++) {
                if (type == 1 && i + 4 <= len) { snprintf(b, sizeof b, " 0x%02x", t[i]); tg += b; i += 4; }
                else if (type == 3 && i + 5 <= len) { snprintf(b, sizeof b, " %d", field(t + i + 1, 4, 12)); og += b; i += 5; }
                else if (type == 2 && i + 2 <= len) { em += " " + cellIdText(t + i); i += 2; }
                else { i = len; break; }
            }
        }
        if (!tg.empty()) o += ", talk group coverage ids" + tg;
        if (!og.empty()) o += ", open groups" + og;
        if (!em.empty()) o += ", emergency cells" + em;
        return o;
    }
    case 0x93:   // D_GROUP_COMPOSITION: GROUP_ID 12 bits, OG_NB 4 bits
        if (len < 3) break;
        add(": group %d, %d member groups", field(t + 1, 0, 12), t[2] & 15);
        return o;
    case 0x3e: {   // D_CALL_START: KEY_REFERENCE as TV (IEI 0x83); KEY_OF_CALL (IEI 0x03) is skipped, never read
        int i = 1;
        while (i < len) {
            const int iei = t[i++];
            if (iei == kIeiKeyReference && i < len) key(t[i++]);
            else if (iei == kIeiKeyOfCall && i < len) { o += " (a key of the call is present, never read)"; i += 1 + t[i]; }
            else break;
        }
        return o;
    }
    case 0x34:   // D_CALL_CONNECT: CALL_TYPE, CHANNEL_ID 12 bits, U / D_CH_SCRAMBLING, KEY_REFERENCE, VALID_RT
        if (len < 7) break;
        add(": call type 0x%02x channel %d U scramble %d D scramble %d", t[1], field(t + 2, 4, 12), t[4], t[5]);
        key(t[6]);
        return o;
    case 0x60:   // D_CONNECT_DCH: DCH_LOW_LAYER, CHANNEL_ID 12 bits, U / D_CH_SCRAMBLING
        if (len < 6) break;
        add(": channel %d U scramble %d D scramble %d", field(t + 2, 4, 12), t[4], t[5]);
        return o;
    case 0x31:   // D_CALL_ALERT (4.4.11): the code only
        return o;
    case 0x32: case 0x53: case 0x23: {   // D_CALL_SETUP (CALLING_ADR, 4.4.12), D_EMERGENCY_NOTIFICATION (CALLING_ADR, CELL_ID), D_FORCED_REGISTRATION
        if (len < 2) return o;
        int used = 0;
        o += ": from " + addressText(t + 1, len - 1, used);
        if (c == 0x53 && 1 + used + 2 <= len) o += " cell " + cellIdText(t + 1 + used);
        return o;
    }
    case 0x96: {
        if (len < 7) break;
        add(": coverage 0x%02x", t[1]);
        for (int i = 2; i + 5 <= len; i += 5) { int used = 0; o += " " + addressText(t + i, len - i, used); }
        return o;
    }
    case 0x59:   // D_GROUP_REJECT: ACTIVATION_MODE, GROUP_ID, COVERAGE_ID, CAUSE
        if (len < 5) break;
        add(": %sgroup %d coverage 0x%02x cause %d", modeText(t[1]).c_str(), field(t + 1, 4, 12), t[3], t[4]);
        return o;
    case 0x83:   // D_ECH_OVERLOAD_ID: ACTIVATION_MODE, GROUP_ID, CELL_ID, ORGANISATION
        if (len < 6) break;
        add(": %sgroup %d cell %s organisation %d", modeText(t[1]).c_str(), field(t + 1, 4, 12), cellIdText(t + 3).c_str(), t[5]);
        return o;
    case 0x49: case 0x4a:   // D_DATAGRAM_NOTIFY / D_DATAGRAM: CALL_PRIORITY 4 bits, MESSAGE_REFERENCE 2 octets, KEY_REFERENCE
        if (len < 5) break;
        add(": priority %d message ref %d", (t[1] >> 4) & 15, (t[2] << 8) | t[3]);
        key(t[4]);
        if (c == 0x4a) {
            if ((t[4] & 15) == 0) o += ", data " + hexOf(t + 5, len - 5);
            else add(", %d data bytes (encrypted, not shown)", len - 5);
        }
        return o;
    case 0x46: case 0x45: case 0x42: case 0x4b: case 0x77: case 0x67:   // short data, data, broadcast: the bytes as sent
        if (len > 1) o += ": " + hexOf(t + 1, len - 1);
        return o;
    case 0x0c: case 0x08: case 0x09: case 0x10: case 0x0f: case 0x58: case 0x48:
        if (len >= 2) add(": cause %d", t[1]);
        return o;
    case 0x22:   // D_REGISTRATION_ACK: COMPLETE_REG, RT_MIN_ACTIVITY, RT_STATUS, HOST_ADR (5 octets), ...
        if (len < 9) break;
        {
            int used = 0;
            o += ": " + addressText(t + 4, len - 4, used);
        }
        return o;
    case 0x21:   // D_REGISTRATION_NAK: CAUSE, HOST_ADR, BN_ID, CELL_ID
        if (len < 7) break;
        {
            int used = 0;
            add(": cause %d ", t[1]);
            o += addressText(t + 2, len - 2, used);
        }
        return o;
    case 0x13: case 0x16: case 0x63: case 0x64:   // D_AUTHENTICATION, D_AUTHORISATION (TV), D_DATA_AUTHENTICATION, D_DATA_REQUEST
        if (len >= 2) key(c == 0x16 ? (t[1] == kIeiKeyReference && len > 2 ? t[2] : -1) : t[1]);
        return o;
    default:
        break;
    }
    if (len > 1) o += ": " + hexOf(t + 1, std::min(len - 1, 40));
    return o;
}

std::string describeVch(const uint8_t* d, int n) {
    if (n < 1) return "";
    static const char* const names[] = {"D_CHANNEL_FREE", "D_FORCED_LISTENING", "D_VOICE_STUFFING", "D_START_SPEECH", "D_VOICE_STUFFING_2"};
    const int c = d[0];
    char b[96];
    if (c <= 4) snprintf(b, sizeof b, "%s", names[c]);
    else if (c >= 0x10 && c <= 0x13) snprintf(b, sizeof b, "%s", c == 0x10 ? "U_START_SPEECH" : c == 0x11 ? "U_END_SPEECH_1" : c == 0x12 ? "U_END_SPEECH_2" : "U_END_SPEECH_3");
    else snprintf(b, sizeof b, "VCH codop 0x%02x", c);
    std::string o = b;
    if ((c == 2 || c == 3) && n >= 3) { snprintf(b, sizeof b, ": transmit priority %d", d[2]); o += b; }   // the block number (d[1]) counts up: left out, one row
    return o;
}

std::string describeCd(const uint8_t* d, int n, std::string* talker) {
    if (n < 1) return "";
    char b[96];
    const int c = d[0];
    static const char* const names[] = {"", "TP_ADDRESS", "TP_ALIAS", "AMBIENCE_LISTENING", "TP_ADDRESS_CHIF", "TP_ADDRESS_INHIB", "TP_ADDRESS_CHIF_INHIB"};
    std::string o = c >= 1 && c <= 6 ? names[c] : (snprintf(b, sizeof b, "CD codop 0x%02x", c), std::string(b));
    if ((c == 1 || c == 4 || c == 5 || c == 6) && n >= 7) {
        int used = 0;
        const std::string a = addressText(d + 2, n - 2, used);
        snprintf(b, sizeof b, ": talking party %s (modifier %d)", a.c_str(), d[1] >> 4);
        o += b;
        if (talker) *talker = a;
    } else if (c == 2 && n >= 3) {
        std::string al;
        for (int i = 2; i < n && i < 11; i++) al += (d[i] >= 32 && d[i] < 127) ? (char)d[i] : '.';
        o += ": alias " + al;
        if (talker) *talker = al;
    } else if (n > 1) {
        o += ": " + hexOf(d + 1, n - 1);
    }
    return o;
}

bool isStuffing(const std::vector<uint8_t>& b) {
    return b.size() == 8 && b[0] == 0x70 && b[1] == 0x00 && b[2] == kCmdUi && !fcsOk(b.data(), 8);
}

std::string describePch(const std::vector<uint8_t>& b) {
    if (b.size() != 16) return "";
    std::string o;
    for (int i = 0; i < 4; i++) {
        const uint8_t* a = b.data() + 8 + 2 * i;
        const int y = field(a, 1, 3), x = field(a, 4, 12);
        if (y == 7 && x == 0) continue;   // TTI no station: an empty slot
        o += (o.empty() ? "" : ", ") + rtiText(field(a, 0, 1), y, x);
    }
    bool any = false;
    for (int i = 0; i < 8; i++) any |= b[(size_t)i] != 0;
    if (o.empty() && !any) return "";
    return "PCH: activation bitmap " + hexOf(b.data(), 8) + (o.empty() ? "" : ", paged " + o);
}

bool describeRch(const std::vector<uint8_t>& b, std::string& text) {
    text.clear();
    if (b.size() != 8 || !fcsOk(b.data(), 8)) return false;
    for (int i = 0; i < 3; i++) {
        const uint8_t* a = b.data() + 2 * i;
        const int z = field(a, 0, 1), y = field(a, 1, 3), x = field(a, 4, 12);
        if (z == 0 && y == 7 && x == 0) continue;
        if (z) { text += text.empty() ? "" : ", "; text += y == 4 ? "NACK (noise)" : y == 5 ? "NACK (collision)" : "NACK"; continue; }
        text += (text.empty() ? "ACK " : ", ACK ") + rtiText(z, y, x);
    }
    if (!text.empty()) text = "RCH: " + text;
    return true;
}

} // namespace tetrapol
} // namespace dect2
