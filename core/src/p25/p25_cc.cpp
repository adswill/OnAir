// See p25_cc.h.
#include "dect2/p25/p25_cc.h"
#include <cstdio>

namespace dect2 {
namespace p25 {

const char* denyReasonText(int r) {
    switch (r) {
    case 0x10: return "requesting unit not valid";
    case 0x11: return "requesting unit not authorised for this service";
    case 0x20: return "target unit not valid";
    case 0x21: return "target unit not authorised for this service";
    case 0x2F: return "target unit refused the call";
    case 0x30: return "target group not valid";
    case 0x31: return "target group not authorised for this service";
    case 0x40: return "invalid dialing";
    case 0x41: return "telephone number not authorised";
    case 0x42: return "PSTN not valid";
    case 0x50: return "call timeout";
    case 0x51: return "landline terminated the call";
    case 0x52: return "subscriber unit terminated the call";
    case 0x5F: return "call preempted";
    case 0x60: return "site access denial";
    case 0x67: return "PTT collide";
    case 0x77: return "PTT bonk";
    case 0xF0: return "call options not valid for service";
    case 0xF1: return "protection option not valid";
    case 0xF2: return "duplex option not valid";
    case 0xF3: return "circuit or packet mode not valid";
    case 0xFF: return "system does not support this service";
    default: return r >= 0x61 && r <= 0xEF ? "user or system defined" : "reserved";
    }
}

const char* queueReasonText(int r) {
    switch (r) {
    case 0x10: return "requesting unit active in another service";
    case 0x20: return "target unit active in another service";
    case 0x2F: return "target unit queued this call";
    case 0x30: return "target group active in another service";
    case 0x40: return "channel resources not available";
    case 0x41: return "telephone resources not available";
    case 0x42: return "data resources not available";
    case 0x43: return "superframe resources not available";
    case 0x50: return "service in use";
    default: return r >= 0x61 ? "user or system defined" : "reserved";
    }
}

const char* serviceText(int s) {
    switch (s & 0x3F) {
    case 0x00: return "group voice"; case 0x04: return "unit to unit voice"; case 0x05: return "unit to unit answer";
    case 0x08: return "telephone"; case 0x14: return "data channel"; case 0x15: return "data page"; case 0x18: return "status update";
    case 0x1A: return "status query"; case 0x1C: return "message update"; case 0x1F: return "call alert"; case 0x20: return "extended function";
    case 0x28: return "group affiliation"; case 0x2C: return "registration"; default: return "service";
    }
}

static std::string line(const char* fmt, int a = 0, int b = 0, int c = 0, int d = 0, const char* s = "", const char* s2 = "") {
    char x[220];
    snprintf(x, sizeof x, fmt, a, b, c, d, s, s2);
    return x;
}

bool tsbkDescribe(int mfid, int op, uint64_t a, CcItem& o) {
    o = CcItem();
    if (mfid == 0x90) {
        const int f0 = (int)((a >> 48) & 0xFFFF), f1 = (int)((a >> 32) & 0xFFFF), f2 = (int)((a >> 16) & 0xFFFF), f3 = (int)(a & 0xFFFF);
        switch (op) {
        case 0x00: case 0x01:
            o.kind = op == 0 ? kCcPatchAdd : kCcPatchDel;
            o.supergroup = f0;
            for (int g : {f1, f2, f3}) if (g && g != f0) o.members.push_back(g);
            o.text = line(op == 0 ? "patch: supergroup %d adds talkgroups %d %d %d" : "patch: supergroup %d drops talkgroups %d %d %d", f0, f1, f2, f3);
            return true;
        case 0x02:
            o.kind = kCcPatchGrant; o.channel = (int)((a >> 40) & 0xFFFF); o.supergroup = (int)((a >> 24) & 0xFFFF); o.unit = (int)(a & 0xFFFFFF);
            o.text = line("patch call: supergroup %d from %d on channel %d", o.supergroup, o.unit, o.channel);
            return true;
        case 0x03:
            o.kind = kCcPatchGrant; o.channel = f0; o.supergroup = f1; o.channel2 = f2;
            o.text = line("patch call update: supergroup %d on channel %d, supergroup %d on channel %d", f1, f0, f3, f2);
            return true;
        default: return false;
        }
    }
    if (mfid == 0xA4 && op == 0x30) {           // Harris regroup / encryption command
        const bool g = (a >> 62) & 1, act = (a >> 61) & 1;
        if (!g) return false;
        o.kind = act ? kCcPatchAdd : kCcPatchDel;
        o.supergroup = (int)((a >> 40) & 0xFFFF);
        const int ga = (int)(a & 0xFFFF);
        if (ga && ga != o.supergroup) o.members.push_back(ga);
        o.text = line(act ? "patch: supergroup %d adds talkgroup %d (Harris)" : "patch: supergroup %d drops talkgroup %d (Harris)", o.supergroup, ga);
        return true;
    }
    if (mfid != 0 && mfid != 1) return false;
    switch (op) {
    case 0x14: {
        o.kind = kCcDataGrant; o.channel = (int)((a >> 40) & 0xFFFF); o.channel2 = (int)((a >> 24) & 0xFFFF); o.unit = (int)(a & 0xFFFFFF);
        o.text = line("data channel grant to unit %d on channel %d", o.unit, o.channel);
        return true; }
    case 0x16: o.kind = kCcDataGrant; o.channel = (int)((a >> 32) & 0xFFFF); o.channel2 = (int)((a >> 16) & 0xFFFF); o.text = line("SNDCP data channel %d announced", o.channel); return true;
    case 0x18: o.kind = kCcStatus; o.unit = (int)(a & 0xFFFFFF); o.tg = (int)((a >> 24) & 0xFFFFFF); o.reason = (int)((a >> 48) & 0xFFFF); o.text = line("status 0x%04X from unit %d to unit %d", o.reason, o.unit, o.tg); return true;
    case 0x20: {
        const int svc = (int)((a >> 56) & 0x3F); o.kind = kCcAck; o.service = svc; o.unit = (int)(a & 0xFFFFFF);
        o.text = "acknowledged: " + std::string(serviceText(svc)) + " for unit " + std::to_string(o.unit);
        return true; }
    case 0x21: {
        const int svc = (int)((a >> 56) & 0x3F), r = (int)((a >> 48) & 0xFF); o.kind = kCcQueue; o.reason = r; o.service = svc; o.unit = (int)(a & 0xFFFFFF);
        o.text = "queued: " + std::string(serviceText(svc)) + " for unit " + std::to_string(o.unit) + ": " + queueReasonText(r);
        return true; }
    case 0x27: {
        const int svc = (int)((a >> 62) & 3) << 4 | (int)((a >> 56) & 0x3F);
        const int r = (int)((a >> 48) & 0xFF); o.kind = kCcDeny; o.reason = r; o.service = svc & 0x3F; o.unit = (int)(a & 0xFFFFFF);
        o.text = "denied: " + std::string(serviceText(o.service)) + " for unit " + std::to_string(o.unit) + ": " + denyReasonText(r);
        return true; }
    case 0x28: {
        const int gav = (int)((a >> 56) & 3); o.kind = kCcAffiliation; o.accepted = gav == 0; o.tg = (int)((a >> 24) & 0xFFFF); o.unit = (int)(a & 0xFFFFFF);
        o.text = line(gav == 0 ? "unit %d affiliated to talkgroup %d" : gav == 1 ? "unit %d refused talkgroup %d" : gav == 2 ? "unit %d denied talkgroup %d" : "unit %d affiliation to talkgroup %d failed", o.unit, o.tg);
        return true; }
    case 0x2A: o.kind = kCcOther; o.unit = (int)(a & 0xFFFFFF); o.text = line("group affiliation query to unit %d", o.unit); return true;
    case 0x2B: {
        const int rv = (int)((a >> 56) & 3); o.kind = kCcRegistration; o.accepted = rv == 0; o.tg = (int)((a >> 40) & 0xFFFF); o.unit = (int)(a & 0xFFFFFF);
        o.text = line(rv == 0 ? "unit %d location registered, talkgroup %d" : "unit %d location registration failed, talkgroup %d", o.unit, o.tg);
        return true; }
    case 0x2C: {
        const int rv = (int)((a >> 60) & 3); o.kind = kCcRegistration; o.accepted = rv == 0; o.unit = (int)((a >> 24) & 0xFFFFFF);
        o.text = line(rv == 0 ? "unit %d registered" : "unit %d registration refused", o.unit);
        return true; }
    case 0x2D: o.kind = kCcOther; o.unit = (int)(a & 0xFFFFFF); o.text = line("registration command to unit %d", o.unit); return true;
    case 0x2F: o.kind = kCcDeregistration; o.unit = (int)(a & 0xFFFFFF); o.text = line("unit %d deregistered", o.unit); return true;
    case 0x35: {
        const bool vl = (a >> 61) & 1;
        if (!vl) { o.kind = kCcOther; o.text = "time and date announcement"; return true; }
        const int raw = (int)((a >> 48) & 0xFFF);
        const int mins = (raw & 0x7FF) * ((raw >> 11) & 1 ? -1 : 1);
        o.kind = kCcOther; o.text = line("time announcement, local offset %+d min", mins);
        return true; }
    case 0x38: o.kind = kCcOther; { char b[100]; snprintf(b, sizeof b, "system services: available 0x%06X, supported 0x%06X", (unsigned)((a >> 32) & 0xFFFFFF), (unsigned)((a >> 8) & 0xFFFFFF)); o.text = b; } return true;
    default: return false;
    }
}

uint32_t crc32Pdu(const uint8_t* p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = crc & 1 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

// the payload after the header, with a check of its CRC-32 (the last four bytes of the last block)
static bool payloadCheck(const std::vector<std::vector<uint8_t>>& blocks, std::vector<uint8_t>& d) {
    if (blocks.size() < 2 || blocks[0].size() != 12) return false;
    const int blks = blocks[0][6] & 0x7F;
    if (blks < 1 || (int)blocks.size() < blks + 1) return false;
    d.clear();
    for (int k = 1; k <= blks; k++) { if (blocks[(size_t)k].size() != 12) return false; d.insert(d.end(), blocks[(size_t)k].begin(), blocks[(size_t)k].end()); }
    const uint32_t have = (uint32_t)d[d.size() - 4] << 24 | (uint32_t)d[d.size() - 3] << 16 | (uint32_t)d[d.size() - 2] << 8 | d[d.size() - 1];
    return have == crc32Pdu(d.data(), d.size() - 4);
}

bool mbtToTsbks(const std::vector<std::vector<uint8_t>>& blocks, std::vector<Tsbk>& out) {
    out.clear();
    if (blocks.empty() || blocks[0].size() != 12) return false;
    const std::vector<uint8_t>& h = blocks[0];
    const int fmt = h[0] & 0x1F, sap = h[1] & 0x3F;
    if (sap != 61 || (fmt != 0x15 && fmt != 0x17)) return false;
    std::vector<uint8_t> d;
    if (!payloadCheck(blocks, d)) return false;
    // bytes from the end, as OP25 counts with mbt_data << 32 (the CRC replaced by four zero bytes)
    for (int i = 0; i < 4; i++) d[d.size() - 1 - (size_t)i] = 0;
    const size_t N = d.size();
    auto bits = [&](int low, int width) -> uint64_t {      // bits low .. low + width - 1 of the big-endian number d (LSB = 0)
        uint64_t v = 0;
        for (int i = low + width - 1; i >= low; i--) {
            const size_t byte = N - 1 - (size_t)(i / 8);
            if (byte >= N) return 0;
            v = v << 1 | ((d[byte] >> (i % 8)) & 1);
        }
        return v;
    };
    const int src = h[3] << 16 | h[4] << 8 | h[5];
    const int op = h[7] & 0x3F;
    Tsbk t;
    t.opcode = op;
    switch (op) {
    case 0x00: {
        if (N < 12) return false;
        t.args = (bits(64, 16) << 40) | (bits(32, 16) << 24) | (uint64_t)src;
        out.push_back(t);
        return true; }
    case 0x02: {
        if (N < 24 || bits(168, 8) != 0x90) return false;
        t.mfid = 0x90; t.args = (bits(80, 16) << 40) | (bits(48, 16) << 24) | (uint64_t)src;
        out.push_back(t);
        return true; }
    case 0x28: {
        if (N < 24) return false;
        t.args = (bits(127, 1) << 63) | (bits(120, 2) << 56) | (bits(144, 16) << 40) | (bits(128, 16) << 24) | (uint64_t)src;
        out.push_back(t);
        return true; }
    case 0x2C: {
        if (N < 12) return false;
        t.args = (bits(48, 2) << 60) | (bits(80, 12) << 48) | (bits(56, 24) << 24) | (uint64_t)src;
        out.push_back(t);
        return true; }
    case 0x3A: {
        const uint64_t sys = (uint64_t)(h[4] << 8 | h[5]) & 0xFFF;
        t.opcode = kRfssSts;
        t.args = (sys << 40) | (bits(88, 8) << 32) | (bits(80, 8) << 24) | (bits(64, 16) << 8);
        out.push_back(t);
        return true; }
    case 0x3B: {
        const uint64_t sys = (uint64_t)(h[4] << 8 | h[5]) & 0xFFF;
        t.opcode = kNetSts;
        t.args = (bits(76, 20) << 36) | (sys << 24) | (bits(56, 16) << 8);
        out.push_back(t);
        return true; }
    case 0x3C: {
        const uint64_t sys = (uint64_t)(h[4] << 8 | h[5]) & 0xFFF;
        t.opcode = kAdjSts;
        t.args = (sys << 40) | ((uint64_t)h[8] << 32) | ((uint64_t)h[9] << 24) | (bits(80, 16) << 8);
        out.push_back(t);
        return true; }
    default: return false;
    }
}

bool pduAssemble(const std::vector<std::vector<uint8_t>>& blocks, PduData& o) {
    o = PduData();
    if (blocks.empty() || blocks[0].size() != 12) return false;
    const auto& h = blocks[0];
    if ((h[0] & 0x1F) != 0x15) return false;      // unconfirmed delivery only
    o.outbound = (h[0] >> 5) & 1;
    o.sap = h[1] & 0x3F;
    o.llid = h[3] << 16 | h[4] << 8 | h[5];
    std::vector<uint8_t> d;
    if (!payloadCheck(blocks, d)) return false;
    o.crcOk = true;
    const int pad = h[7] & 0x1F, off = h[9] & 0x3F;
    int len = (int)d.size() - 4 - pad;
    if (len < 0 || off > len) return false;
    o.payload.assign(d.begin() + off, d.begin() + len);
    char b[220];
    const auto& p = o.payload;
    if ((o.sap == 4 || o.sap == 0) && p.size() >= 20 && (p[0] >> 4) == 4) {
        const int ihl = (p[0] & 15) * 4;
        snprintf(b, sizeof b, "IPv4 %d.%d.%d.%d to %d.%d.%d.%d, protocol %d, %zu bytes", p[12], p[13], p[14], p[15], p[16], p[17], p[18], p[19], p[9], p.size());
        o.summary = b;
        if ((p[9] == 17 || p[9] == 6) && (int)p.size() >= ihl + 4) {
            snprintf(b, sizeof b, ", ports %d to %d", p[(size_t)ihl] << 8 | p[(size_t)ihl + 1], p[(size_t)ihl + 2] << 8 | p[(size_t)ihl + 3]);
            o.summary += b;
        }
    } else {
        snprintf(b, sizeof b, "SAP %d, %zu bytes", o.sap, p.size());
        o.summary = b;
        std::string asc;
        for (uint8_t c : p) if (c >= 32 && c < 127) asc += (char)c;
        if (asc.size() >= 4 && asc.size() * 2 >= p.size()) o.summary += ": " + asc.substr(0, 80);
    }
    return true;
}

} // namespace p25
} // namespace dect2
