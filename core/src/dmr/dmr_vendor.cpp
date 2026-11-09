// See dmr_vendor.h.
#include "dect2/dmr/dmr_vendor.h"
#include <cstring>

namespace dect2 {
namespace dmr {

const char* vendorCsbkName(int fid, int op) {
    if (fid == 0x10) {
        switch (op) {
        case 0x3A: return "Capacity Plus: control";
        case 0x3B: return "Capacity Plus: adjacent sites";
        case 0x3E: return "Capacity Plus: channel status";
        default: return "Capacity Plus";
        }
    }
    if (fid == 0x06) {
        switch (op) {
        case 0x01: return "Connect Plus: adjacent sites";
        case 0x03: return "Connect Plus: voice grant";
        case 0x06: return "Connect Plus: data grant";
        case 0x0C: return "Connect Plus: slot termination";
        default: return "Connect Plus";
        }
    }
    return "";
}

bool CapPlus::feed(const uint8_t d[8], std::vector<CapPlusGrant>& out, int& rest) {
    out.clear();
    const int fl = d[0] >> 6, ts = (d[0] >> 5) & 1;
    rest = d[0] & 0x0F;
    if (fl == 2 || fl == 3) {       // first or only block: cs_pdu bytes 0 .. 9 kept from byte 2 of the PDU
        std::memset(bits_[ts], 0, sizeof bits_[ts]);
        std::memcpy(bits_[ts], d, 8);
        blocks_[ts] = 0;
    } else {                        // a further block: seven more bytes
        if (blocks_[ts] >= 6) return false;
        std::memcpy(bits_[ts] + 8 + 7 * blocks_[ts], d + 1, 7);
        blocks_[ts]++;
    }
    if (fl != 1 && fl != 3) return false;
    const uint8_t* b = bits_[ts];
    const uint8_t bank1 = b[1];
    int tally = 0;
    for (int i = 0; i < 8; i++) if (bank1 & (0x80 >> i)) tally++;
    uint8_t bank2 = (size_t)(2 + tally) < sizeof bits_[ts] ? b[2 + tally] : 0;
    int k = 0;
    int offset = 2;              // the first talkgroup byte
    for (int i = 0; i < 16; i++) {
        const bool on = i < 8 ? (bank1 >> (7 - i)) & 1 : (bank2 >> (15 - i)) & 1;
        if (i == 8) k++;         // the second bank's byte sits between the two lists
        if (!on) continue;
        const int at = offset + k;
        if ((size_t)at >= sizeof bits_[ts]) break;
        const int tg = b[at];
        k++;
        if (tg) { CapPlusGrant g; g.lsn = i + 1; g.tg = tg; out.push_back(g); }
    }
    return true;
}

bool conPlusParse(int op, const uint8_t d[8], ConPlus& o) {
    o = ConPlus();
    o.opcode = op;
    switch (op) {
    case 0x01: for (int i = 0; i < 5; i++) o.neighbours[i] = d[i] & 0x3F; return true;
    case 0x03: o.src = (uint32_t)d[0] << 16 | (uint32_t)d[1] << 8 | d[2]; o.dst = (uint32_t)d[3] << 16 | (uint32_t)d[4] << 8 | d[5]; o.lcn = d[6] >> 4; o.slot = (d[6] >> 3) & 1; return true;
    case 0x06: o.dst = (uint32_t)d[0] << 16 | (uint32_t)d[1] << 8 | d[2]; o.lcn = d[3] >> 4; o.slot = (d[3] >> 3) & 1; return true;
    case 0x0C: o.dst = (uint32_t)d[0] << 16 | (uint32_t)d[1] << 8 | d[2]; return true;
    default: return false;
    }
}

bool ipService(const uint8_t* p, size_t n, IpService& o) {
    o = IpService();
    if (n < 28 || (p[0] >> 4) != 4) return false;
    const size_t ihl = (size_t)(p[0] & 15) * 4;
    if (ihl < 20 || n < ihl + 8 || p[9] != 17) return false;
    const int src = p[ihl] << 8 | p[ihl + 1], dst = p[ihl + 2] << 8 | p[ihl + 3];
    o.port = dst;
    if (src == 4005 || dst == 4005) {
        o.port = 4005; o.kind = "ARS";
        size_t at = ihl + 8;
        std::string name;
        for (size_t i = at; i < n && i < at + 40; i++) if (p[i] >= 0x20 && p[i] < 0x7F) name += (char)p[i];
        o.text = name;
        return true;
    }
    if (src == 4007 || dst == 4007) {
        o.port = 4007; o.kind = "TMS";
        size_t ptr = ihl + 8;
        if (n < ptr + 4) return false;
        int len = p[ptr] << 8 | p[ptr + 1];
        ptr += 2;
        const uint8_t hdr = p[ptr++];
        const uint8_t adl = p[ptr++];
        if (adl) ptr += adl;
        if (ptr < n) { uint8_t more = p[ptr] >> 7; while (more && ptr + 1 < n) { const uint8_t b1 = p[ptr++]; more = b1 >> 7; if (more) ptr++; } }
        if ((hdr & 0x0F) != 0) { o.ack = true; o.text = "acknowledgment"; return true; }
        if (ptr < 2 || ptr > n) return false;
        int lenText = len > 31 ? len - (int)(ptr - 31) : len;
        size_t q = ptr - 2;
        std::string t;
        bool first = true;
        for (int i = 0; i + 1 < lenText && q + 1 < n; i += 2, q += 2) {
            const uint16_t u = first ? (uint16_t)p[q + 1] : (uint16_t)(p[q] << 8 | p[q + 1]);
            first = false;
            if (u == 0) continue;
            if (u < 0x80) { if (u >= 0x20) t += (char)u; }
            else if (u < 0x800) { t += (char)(0xC0 | u >> 6); t += (char)(0x80 | (u & 63)); }
            else if (u != 0x040D) { t += (char)(0xE0 | u >> 12); t += (char)(0x80 | ((u >> 6) & 63)); t += (char)(0x80 | (u & 63)); }
        }
        o.text = t;
        return true;
    }
    return false;
}

} // namespace dmr
} // namespace dect2
