// TETRA DMO bursts and PDUs (see tetradmo_proto.h).
#include "dect2/tetradmo/tetradmo_proto.h"
#include <cstdio>

namespace dect2::tetradmo {

using namespace dect2::tetra;

const uint8_t kPreambleP1[12] = {0,0,1,1,0,0,1,0,0,0,1,1};
const uint8_t kPreambleP2[12] = {1,0,0,1,1,0,1,0,1,0,0,1};
const uint8_t kPreambleP3[12] = {0,0,0,1,0,1,0,0,0,1,1,1};

namespace {
struct W {
    Bits& b; int p = 0;
    void u(uint32_t v, int n) { for (int i = 0; i < n; i++) b.push_back((uint8_t)((v >> (n - 1 - i)) & 1)); p += n; }
};
struct R {
    const Bits& b; int p = 0;
    uint32_t u(int n) { uint32_t v = 0; for (int i = 0; i < n && p < (int)b.size(); i++, p++) v = (v << 1) | (b[(size_t)p] & 1); return v; }
};
int phaseOf(const uint8_t* bits) { return dibitPhase(bits[0], bits[1]); }
// the phase step of the phase adjustment bits so that the phase between symbol n1 and n2 is zero overall (9.4.3.3.5)
void adjust(Bits& b, int at, int n1, int n2) {
    int sum = 0;
    for (int n = n1; n <= n2; n++) { if (2 * n - 2 == at) continue; sum += phaseOf(b.data() + 2 * n - 2); }
    const int want = (-sum % 8 + 8) % 8;
    for (int a = 0; a < 2; a++) for (int c = 0; c < 2; c++) if ((dibitPhase(a, c) + 8) % 8 == want) { b[(size_t)at] = (uint8_t)a; b[(size_t)at + 1] = (uint8_t)c; return; }
}
}

uint32_t dccInit(uint32_t mni24, uint32_t src24) {
    const uint32_t e = ((mni24 & 0x3F) << 24) | (src24 & 0xFFFFFF);
    return (e << 2) | 3u;
}

Bits dnb(const Bits& bkn1, const Bits& bkn2, bool two) {
    Bits b(kBurstBits, 0);
    const uint8_t* pre = two ? kPreambleP2 : kPreambleP1;
    for (int i = 0; i < 12; i++) b[(size_t)i] = pre[i];
    std::copy(bkn1.begin(), bkn1.end(), b.begin() + kBkn1At);
    const uint8_t* tr = two ? kTrainP : kTrainN;
    for (int i = 0; i < 22; i++) b[(size_t)(kTrainAt + i)] = tr[i];
    std::copy(bkn2.begin(), bkn2.end(), b.begin() + kBkn2At);
    adjust(b, 12, 8, 126);
    return b;
}

Bits dsb(const Bits& sb, const Bits& bkn2) {
    Bits b(kBurstBits, 0);
    for (int i = 0; i < 12; i++) b[(size_t)i] = kPreambleP3[i];
    for (int i = 0; i < 80; i++) b[(size_t)(kFcAt + i)] = kFreqCorr[i];
    std::copy(sb.begin(), sb.end(), b.begin() + kSbAt);
    for (int i = 0; i < 38; i++) b[(size_t)(kYAt + i)] = kTrainY[i];
    std::copy(bkn2.begin(), bkn2.end(), b.begin() + kBkn2At);
    adjust(b, 12, 8, 126);
    return b;
}

const char* dmName(int t) {
    switch (t) {
    case 0: return "DM-RESERVED";
    case 1: return "DM-SDS OCCUPIED";
    case 2: return "DM-TIMING REQUEST";
    case 3: return "DM-TIMING ACK";
    case 8: return "DM-SETUP";
    case 9: return "DM-SETUP PRES";
    case 10: return "DM-CONNECT";
    case 11: return "DM-DISCONNECT";
    case 12: return "DM-CONNECT ACK";
    case 13: return "DM-OCCUPIED";
    case 14: return "DM-RELEASE";
    case 15: return "DM-TX CEASED";
    case 16: return "DM-TX REQUEST";
    case 17: return "DM-TX ACCEPT";
    case 18: return "DM-PREEMPT";
    case 19: return "DM-PRE ACCEPT";
    case 20: return "DM-REJECT";
    case 21: return "DM-INFO";
    case 22: return "DM-SDS UDATA";
    case 23: return "DM-SDS DATA";
    case 24: return "DM-SDS ACK";
    case 25: return "gateway message";
    case 30: case 31: return "proprietary";
    default: return "reserved";
    }
}

Bits encodeSyncS(const SyncS& s) {
    Bits b;
    W w{b};
    w.u((uint32_t)s.systemCode, 4); w.u(0, 2); w.u((uint32_t)s.commType, 2);
    w.u(s.commType == 1 || s.commType == 3 ? (s.msFlag ? 1 : 0) : 0, 1);
    w.u(s.commType >= 2 ? (s.gatewayGenerated ? 1 : 0) : 0, 1);
    w.u((uint32_t)s.abUsage, 2);
    w.u((uint32_t)(s.slot - 1) & 3, 2);
    w.u((uint32_t)s.frame, 5);
    w.u((uint32_t)s.aiEncState, 2);
    if (s.aiEncState != 0) { w.u(s.tvp & 0x1FFFFFFF, 29); w.u(0, 1); w.u((uint32_t)s.ksg, 4); w.u((uint32_t)s.keyNumber, 5); }
    else w.u(0, 39);
    return b;
}

bool parseSyncS(const Bits& b, SyncS& o) {
    if (b.size() < 60) return false;
    R r{b};
    o.systemCode = (int)r.u(4); o.syncPduType = (int)r.u(2); o.commType = (int)r.u(2);
    const int a = (int)r.u(1), g = (int)r.u(1);
    o.msFlag = (o.commType == 1 || o.commType == 3) && a; o.gatewayGenerated = o.commType >= 2 && g;
    o.abUsage = (int)r.u(2); o.slot = (int)r.u(2) + 1; o.frame = (int)r.u(5); o.aiEncState = (int)r.u(2);
    if (o.aiEncState != 0) { o.tvp = r.u(29); r.u(1); o.ksg = (int)r.u(4); o.keyNumber = (int)r.u(5); }
    return o.syncPduType == 0 && o.frame >= 1 && o.frame <= 18;
}

namespace {
void encodeBody(W& w, const DmMessage& m, int commType, int capacity) {
    w.u((uint32_t)(commType == 1 ? m.repeaterAddress : 0), 10);
    w.u(0, 1);                                  // fill bit indication
    w.u(m.fragmented ? 1 : 0, 1);
    if (m.fragmented) w.u((uint32_t)m.schFSlots, 4);
    w.u((uint32_t)m.frameCountdown, 2);
    w.u((uint32_t)m.destType, 2); w.u(m.dest, 24);
    w.u((uint32_t)m.srcType, 2); w.u(m.src, 24);
    w.u(m.mni, 24);
    w.u((uint32_t)m.messageType, 5);
    if (m.messageType == (int)Dm::Setup || m.messageType == (int)Dm::SetupPres) {
        w.u(m.timingFlag ? 1 : 0, 1); w.u(m.lchFlag ? 1 : 0, 1); w.u(m.preemption ? 1 : 0, 1); w.u((uint32_t)m.powerClass, 3);
        w.u(0, 1); w.u(0, 2); w.u(0, 1); w.u(m.twoFrequency ? 1 : 0, 1);
        w.u((uint32_t)m.circuitMode, 4); w.u(0, 4); w.u((uint32_t)m.priority, 2);
        w.u(m.e2eEncrypted ? 1 : 0, 1); w.u(m.groupCall ? 1 : 0, 1); w.u(0, 1); w.u(0, 2);
    }
    while ((int)w.b.size() < capacity) w.b.push_back(0);
}
void parseBody(R& r, int commType, DmMessage& o) {
    (void)commType;
    o.repeaterAddress = (int)r.u(10);
    o.fillBit = r.u(1); o.fragmented = r.u(1);
    if (o.fragmented) o.schFSlots = (int)r.u(4);
    o.frameCountdown = (int)r.u(2);
    o.destType = (int)r.u(2);
    if (o.destType != 2) o.dest = r.u(24);
    o.srcType = (int)r.u(2);
    if (o.srcType != 2) o.src = r.u(24);
    o.mni = r.u(24);
    o.messageType = (int)r.u(5);
    if (o.messageType == (int)Dm::Setup || o.messageType == (int)Dm::SetupPres) {
        o.setupKnown = true;
        o.timingFlag = r.u(1); o.lchFlag = r.u(1); o.preemption = r.u(1); o.powerClass = (int)r.u(3);
        r.u(1); r.u(2); r.u(1); o.twoFrequency = r.u(1);
        o.circuitMode = (int)r.u(4); r.u(4); o.priority = (int)r.u(2);
        o.e2eEncrypted = r.u(1); o.groupCall = r.u(1);
    }
}
}

Bits encodeSyncH(const DmMessage& m, int commType) {
    Bits b;
    W w{b};
    encodeBody(w, m, commType, 124);
    b.resize(124);
    return b;
}

bool parseSyncH(const Bits& b, int commType, DmMessage& o) {
    if (b.size() < 124) return false;
    R r{b};
    parseBody(r, commType, o);
    o.valid = true;
    return true;
}

Bits encodeDmacData(const DmacData& d, int commType, int len) {
    Bits b;
    W w{b};
    w.u(0, 2);                                  // MAC PDU type 00: DMAC-DATA
    w.u(0, 1);                                  // fill
    w.u(d.secondHalfStolen ? 1 : 0, 1);
    w.u(0, 1);                                  // fragmentation
    w.u(d.nullPdu ? 1 : 0, 1);
    if (!d.nullPdu) {
        w.u((uint32_t)d.msg.frameCountdown, 2);
        w.u((uint32_t)d.aiEncState, 2);
        w.u((uint32_t)d.msg.destType, 2); w.u(d.msg.dest, 24);
        w.u((uint32_t)d.msg.srcType, 2); w.u(d.msg.src, 24);
        w.u(commType < 2 ? d.msg.mni : 0, 24);
        w.u((uint32_t)d.msg.messageType, 5);
    }
    b.resize((size_t)len);
    return b;
}

bool parseDmacData(const Bits& b, int commType, DmacData& o) {
    if (b.size() < 8) return false;
    R r{b};
    const int type = (int)r.u(2);
    if (type != 0) return false;
    r.u(1);
    o.secondHalfStolen = r.u(1);
    r.u(1);
    o.nullPdu = r.u(1);
    o.valid = true;
    if (o.nullPdu) return true;
    o.msg.frameCountdown = (int)r.u(2);
    o.aiEncState = (int)r.u(2);
    o.msg.destType = (int)r.u(2);
    if (o.msg.destType != 2) o.msg.dest = r.u(24);
    o.msg.srcType = (int)r.u(2);
    if (o.msg.srcType != 2) o.msg.src = r.u(24);
    if (commType < 2) o.msg.mni = r.u(24);
    o.msg.messageType = (int)r.u(5);
    o.msg.valid = true;
    return true;
}

std::string describeMessage(const DmMessage& m) {
    char b[160];
    snprintf(b, sizeof b, "%s: from %u to %u, MNI %u/%u", dmName(m.messageType), m.src, m.dest, m.mni >> 14, m.mni & 0x3FFF);
    return b;
}

} // namespace dect2::tetradmo
