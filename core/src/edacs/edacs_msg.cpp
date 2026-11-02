// EDACS message layer (see edacs_msg.h for the sources).
#include "dect2/edacs/edacs_msg.h"
#include <cstdio>

namespace dect2::edacs {

namespace {
inline uint32_t bits(uint32_t w, int shift, int n) { return (w >> shift) & ((1u << n) - 1u); }
inline void put(uint32_t& w, int shift, int n, uint32_t v) { w &= ~(((1u << n) - 1u) << shift); w |= (v & ((1u << n) - 1u)) << shift; }
}

Layout defaultLayout() { return Layout(); }

Msg parse(uint32_t w1, uint32_t w2, const Layout& l) {
    Msg m;
    w1 &= 0xFFFFFFF; w2 &= 0xFFFFFFF;
    if (l.esk) { w1 ^= ((uint32_t)l.esk & 0xFF) << 20; w2 ^= ((uint32_t)l.esk & 0xFF) << 20; w1 &= 0xFFFFFFF; w2 &= 0xFFFFFFF; }
    m.raw = w1;
    if (l.extended) {
        const uint32_t mt1 = bits(w1, 23, 5), mt2 = bits(w1, 19, 4);
        if (mt1 == 0x1F) {
            if (mt2 == 0x1) { m.type = MsgType::AdjacentSite; m.lcn = (int)bits(w1, 12, 5); m.index = (int)bits(w1, 8, 4); m.site = (int)bits(w1, 0, 8); }
            else if (mt2 == 0xA) { m.type = MsgType::SiteId; m.site = (int)(((w1 & 0x7000) >> 7) | (w1 & 0x1F)); m.area = (int)bits(w1, 5, 7); }
            else if (mt2 == 0x0) { m.type = MsgType::Other; m.other = "initiate test call"; }
            else if (mt2 == 0xB) { m.type = MsgType::Other; m.other = "dynamic regroup plan bitmap"; }
            else if (mt2 == 0xC) { m.type = MsgType::Other; m.other = "supergroup patch"; }
            else if (mt2 == 0x4 || mt2 == 0x7 || mt2 == 0x8 || mt2 == 0xD) { m.type = MsgType::Other; m.other = "system command"; }
        } else if (mt1 == 0x3 || mt1 == 0x6) {
            m.type = MsgType::GroupAssign; m.digital = mt1 == 0x3;
            m.lcn = (int)bits(w1, 17, 5); m.update = bits(w1, 16, 1) != 0; m.gid = (int)bits(w1, 0, 16);
            m.txTrunk = bits(w2, 21, 1) != 0; m.emergency = bits(w2, 20, 1) != 0; m.source = m.lid = (int)bits(w2, 0, 20);
        } else if (mt1 == 0x10) {
            m.type = MsgType::IndividualAssign; m.individual = true;
            m.lcn = (int)bits(w2, 20, 5); m.digital = bits(w1, 21, 1) != 0; m.update = bits(w1, 20, 1) != 0;
            m.gid = (int)bits(w1, 0, 20); m.source = m.lid = (int)bits(w2, 0, 20);
        } else if (mt1 == 0x2) { m.type = MsgType::DataAssign; m.gid = (int)bits(w1, 0, 16); m.source = (int)bits(w2, 0, 20); }
        else if (mt1 == 0x1) { m.type = MsgType::Other; m.other = "TDMA group grant"; }
        else if (mt1 == 0x12) { m.type = MsgType::Other; m.other = "call end / unkey"; }
        else if (mt1 == 0x16) { m.type = MsgType::AllCall; m.digital = bits(w1, 16, 1) != 0; m.update = bits(w1, 15, 1) != 0; m.source = (int)bits(w2, 0, 20); }
        else if (mt1 == 0x19) { m.type = MsgType::Other; m.other = "group message"; }
    } else {
        const uint32_t a = bits(w1, 25, 3), b = bits(w1, 22, 3), d = bits(w1, 17, 5);
        if (a <= 3) {
            m.type = MsgType::GroupAssign; m.digital = (a & 2) != 0; m.emergency = (a & 1) != 0;
            m.lid = (int)((bits(w1, 18, 7) << 7) | bits(w2, 17, 7));
            m.lcn = (int)bits(w1, 12, 5); m.txTrunk = bits(w1, 11, 1) != 0; m.gid = (int)bits(w1, 0, 11);
            m.source = m.lid;
        } else if (a == 5) {
            m.type = MsgType::DataAssign; m.individual = bits(w1, 24, 1) != 0;
            m.port = (int)((bits(w1, 20, 3) << 3) | bits(w2, 20, 3));
            m.lcn = (int)bits(w1, 15, 5);
            if (bits(w1, 14, 1)) m.lid = (int)bits(w1, 0, 14); else m.gid = (int)bits(w1, 0, 11);
        } else if (a == 6) {
            m.type = MsgType::LoginAck; m.gid = (int)bits(w1, 14, 11); m.lid = (int)bits(w1, 0, 14);
        } else if (a == 7) {
            if (b == 0) { m.type = MsgType::StatusRequest; m.status = (int)bits(w1, 14, 8); m.lid = (int)bits(w1, 0, 14); }
            else if (b == 1) {
                m.type = MsgType::InterconnectAssign; m.lcn = (int)bits(w1, 15, 5);
                if (bits(w1, 14, 1)) { m.individual = true; m.lid = (int)bits(w1, 0, 14); } else m.gid = (int)bits(w1, 0, 11);
                const uint32_t c = bits(w1, 20, 2); m.digital = (c & 1) != 0; m.txTrunk = (c & 2) != 0;
            } else if (b == 3) {   // channel update (group call, or individual call with the calling unit in word 2)
                m.type = MsgType::GroupAssign; m.update = true; m.lcn = (int)bits(w1, 15, 5);
                const uint32_t c = bits(w1, 20, 2); m.digital = (c & 1) != 0; m.txTrunk = (c & 2) != 0;
                if (bits(w1, 14, 1)) { m.type = MsgType::IndividualAssign; m.individual = true; m.gid = m.lid = (int)bits(w1, 0, 14); m.source = (int)bits(w2, 0, 14); }
                else { m.gid = (int)bits(w1, 0, 11); m.emergency = bits(w1, 13, 1) != 0; }
            } else if (b == 4) { m.type = MsgType::SystemAssignedId; m.gid = (int)bits(w1, 0, 11); m.status = (int)bits(w1, 11, 11); }
            else if (b == 5) {
                m.type = MsgType::IndividualAssign; m.individual = true; m.txTrunk = bits(w1, 21, 1) != 0;
                m.lcn = (int)bits(w1, 15, 5); m.digital = bits(w1, 14, 1) != 0;
                m.gid = (int)bits(w1, 0, 14); m.source = (int)bits(w2, 0, 14); m.lid = m.source;
            } else if (b == 6) {
                m.lcn = (int)bits(w1, 14, 5); m.lid = (int)bits(w1, 0, 14);
                if (bits(w1, 19, 1)) m.type = MsgType::CallEnd;   // console drop
                else { m.type = MsgType::Other; m.other = "console unkey"; }
            } else if (b == 7) {
                if (d == 0x01) { m.type = MsgType::AdjacentSite; m.lcn = (int)bits(w1, 12, 5); m.index = (int)bits(w1, 9, 3); m.site = (int)bits(w1, 4, 5); }
                else if (d >= 0x08 && d <= 0x0B) {
                    m.type = MsgType::SiteId; m.lcn = (int)bits(w1, 12, 5); m.priority = (int)bits(w1, 9, 3);
                    m.scat = bits(w1, 7, 1) != 0; m.failsoft = bits(w1, 6, 1) != 0; m.aux = bits(w1, 5, 1) != 0; m.site = (int)bits(w1, 0, 5);
                } else if (d == 0x0F) {
                    m.type = MsgType::AllCall; m.lcn = (int)bits(w1, 12, 5); m.digital = bits(w1, 11, 1) != 0; m.update = bits(w1, 10, 1) != 0;
                    m.txTrunk = bits(w1, 9, 1) != 0; m.lid = (int)(bits(w1, 0, 7) | (bits(w2, 1, 7) << 6));
                } else if (d == 0x00) { m.type = MsgType::Other; m.other = "cancel dynamic regroup"; }
                else if (d == 0x02) { m.type = MsgType::Other; m.other = "extended site options"; }
                else if (d == 0x04) { m.type = MsgType::Other; m.other = "regroup plan bitmap"; }
                else if (d == 0x05) { m.type = MsgType::Other; m.other = "auxiliary control channel"; }
                else if (d == 0x06) { m.type = MsgType::Other; m.other = "initiate test call"; }
                else if (d == 0x07) { m.type = MsgType::Other; m.other = "unit enable / disable"; }
                else if (d == 0x10) { m.type = MsgType::Other; m.other = "dynamic regrouping"; }
            }
        }
    }
    if ((m.type == MsgType::GroupAssign || m.type == MsgType::IndividualAssign || m.type == MsgType::CallEnd) && (m.lcn < 1 || m.lcn > 25))
        m.type = MsgType::Unknown;   // there is no LCN 0, and LCNs from 26 up are status codes (busy, queued, denied)
    return m;
}

void build(const Msg& m, const Layout& l, uint32_t& w1, uint32_t& w2) {
    w1 = w2 = 0;
    if (l.extended) {
        switch (m.type) {
        case MsgType::GroupAssign:
            put(w1, 23, 5, m.digital ? 0x3 : 0x6); put(w1, 17, 5, (uint32_t)m.lcn); put(w1, 16, 1, m.update); put(w1, 0, 16, (uint32_t)m.gid);
            put(w2, 21, 1, m.txTrunk); put(w2, 20, 1, m.emergency); put(w2, 0, 20, (uint32_t)(m.source < 0 ? 0 : m.source));
            break;
        case MsgType::IndividualAssign:
            put(w1, 23, 5, 0x10); put(w1, 21, 1, m.digital); put(w1, 20, 1, m.update); put(w1, 0, 20, (uint32_t)m.gid);
            put(w2, 20, 5, (uint32_t)m.lcn); put(w2, 0, 20, (uint32_t)(m.source < 0 ? 0 : m.source));
            break;
        case MsgType::SiteId:
            put(w1, 23, 5, 0x1F); put(w1, 19, 4, 0xA); put(w1, 5, 7, (uint32_t)(m.area < 0 ? 0 : m.area));
            put(w1, 12, 3, (uint32_t)m.site >> 5); put(w1, 0, 5, (uint32_t)m.site);
            break;
        case MsgType::AdjacentSite:
            put(w1, 23, 5, 0x1F); put(w1, 19, 4, 0x1); put(w1, 12, 5, (uint32_t)m.lcn); put(w1, 8, 4, (uint32_t)m.index); put(w1, 0, 8, (uint32_t)m.site);
            break;
        case MsgType::AllCall:
            put(w1, 23, 5, 0x16); put(w1, 16, 1, m.digital); put(w1, 15, 1, m.update); put(w2, 0, 20, (uint32_t)(m.source < 0 ? 0 : m.source));
            break;
        default: break;
        }
    } else {
        switch (m.type) {
        case MsgType::GroupAssign:
            if (m.update) {   // channel update (MT-A 7, MT-B 3)
                put(w1, 25, 3, 7); put(w1, 22, 3, 3);
                put(w1, 20, 2, (m.digital ? 1u : 0u) | (m.txTrunk ? 2u : 0u)); put(w1, 15, 5, (uint32_t)m.lcn); put(w1, 14, 1, 0);
                put(w1, 13, 1, m.emergency); put(w1, 0, 11, (uint32_t)m.gid);
            } else {
                put(w1, 25, 3, (m.digital ? 2u : 0u) | (m.emergency ? 1u : 0u));
                put(w1, 18, 7, (uint32_t)(m.lid < 0 ? 0 : m.lid) >> 7); put(w2, 17, 7, (uint32_t)(m.lid < 0 ? 0 : m.lid));
                put(w1, 12, 5, (uint32_t)m.lcn); put(w1, 11, 1, m.txTrunk); put(w1, 0, 11, (uint32_t)m.gid);
            }
            break;
        case MsgType::IndividualAssign:   // MT-A 7, MT-B 5
            put(w1, 25, 3, 7); put(w1, 22, 3, 5); put(w1, 21, 1, m.txTrunk); put(w1, 15, 5, (uint32_t)m.lcn); put(w1, 14, 1, m.digital);
            put(w1, 0, 14, (uint32_t)m.gid); put(w2, 0, 14, (uint32_t)(m.source < 0 ? 0 : m.source));
            break;
        case MsgType::DataAssign:
            put(w1, 25, 3, 5); put(w1, 24, 1, m.individual); put(w1, 20, 3, (uint32_t)m.port >> 3); put(w2, 20, 3, (uint32_t)m.port);
            put(w1, 15, 5, (uint32_t)m.lcn);
            if (m.lid >= 0) { put(w1, 14, 1, 1); put(w1, 0, 14, (uint32_t)m.lid); } else put(w1, 0, 11, (uint32_t)m.gid);
            break;
        case MsgType::LoginAck: put(w1, 25, 3, 6); put(w1, 14, 11, (uint32_t)m.gid); put(w1, 0, 14, (uint32_t)m.lid); break;
        case MsgType::StatusRequest: put(w1, 25, 3, 7); put(w1, 22, 3, 0); put(w1, 14, 8, (uint32_t)m.status); put(w1, 0, 14, (uint32_t)m.lid); break;
        case MsgType::CallEnd: put(w1, 25, 3, 7); put(w1, 22, 3, 6); put(w1, 19, 1, 1); put(w1, 14, 5, (uint32_t)m.lcn); put(w1, 0, 14, (uint32_t)(m.lid < 0 ? 0 : m.lid)); break;
        case MsgType::SiteId:
            put(w1, 25, 3, 7); put(w1, 22, 3, 7); put(w1, 17, 5, 0x08); put(w1, 12, 5, (uint32_t)m.lcn);
            put(w1, 9, 3, (uint32_t)(m.priority < 0 ? 0 : m.priority)); put(w1, 7, 1, m.scat); put(w1, 6, 1, m.failsoft); put(w1, 5, 1, m.aux); put(w1, 0, 5, (uint32_t)m.site);
            break;
        case MsgType::AdjacentSite:
            put(w1, 25, 3, 7); put(w1, 22, 3, 7); put(w1, 17, 5, 0x01); put(w1, 12, 5, (uint32_t)m.lcn); put(w1, 9, 3, (uint32_t)m.index); put(w1, 4, 5, (uint32_t)m.site);
            break;
        case MsgType::AllCall:
            put(w1, 25, 3, 7); put(w1, 22, 3, 7); put(w1, 17, 5, 0x0F); put(w1, 12, 5, (uint32_t)m.lcn); put(w1, 11, 1, m.digital); put(w1, 10, 1, m.update);
            put(w1, 9, 1, m.txTrunk); put(w1, 0, 7, (uint32_t)m.lid); put(w2, 1, 7, (uint32_t)m.lid >> 6);
            break;
        default: break;
        }
    }
    if (l.esk) { w1 = (w1 ^ (((uint32_t)l.esk & 0xFF) << 20)) & 0xFFFFFFF; w2 = (w2 ^ (((uint32_t)l.esk & 0xFF) << 20)) & 0xFFFFFFF; }
}

const char* typeName(MsgType t) {
    switch (t) {
    case MsgType::GroupAssign: return "group call assignment";
    case MsgType::IndividualAssign: return "individual call assignment";
    case MsgType::DataAssign: return "data call assignment";
    case MsgType::InterconnectAssign: return "interconnect assignment";
    case MsgType::SiteId: return "site ID";
    case MsgType::AdjacentSite: return "adjacent site";
    case MsgType::CallEnd: return "call end";
    case MsgType::LoginAck: return "login acknowledge";
    case MsgType::StatusRequest: return "status";
    case MsgType::AllCall: return "system all-call";
    case MsgType::SystemAssignedId: return "system assigned ID";
    case MsgType::Other: return "other command";
    default: return "unknown";
    }
}

std::string describe(const Msg& m) {
    char b[160];
    switch (m.type) {
    case MsgType::GroupAssign:
        if (m.lid >= 0) snprintf(b, sizeof b, "group %s, LCN %d, group %d, unit %d%s%s", m.update ? "update" : "call", m.lcn, m.gid, m.lid, m.emergency ? ", emergency" : "", m.digital ? ", digital" : "");
        else snprintf(b, sizeof b, "group %s, LCN %d, group %d%s%s", m.update ? "update" : "call", m.lcn, m.gid, m.emergency ? ", emergency" : "", m.digital ? ", digital" : "");
        break;
    case MsgType::IndividualAssign: snprintf(b, sizeof b, "individual call, LCN %d, unit %d, from %d%s", m.lcn, m.gid, m.source, m.digital ? ", digital" : ""); break;
    case MsgType::DataAssign: snprintf(b, sizeof b, "data call, LCN %d, %s %d, port %d", m.lcn, m.lid >= 0 ? "unit" : "group", m.lid >= 0 ? m.lid : m.gid, m.port); break;
    case MsgType::InterconnectAssign: snprintf(b, sizeof b, "interconnect, LCN %d, %s %d", m.lcn, m.individual ? "unit" : "group", m.individual ? m.lid : m.gid); break;
    case MsgType::SiteId:
        if (m.area >= 0) snprintf(b, sizeof b, "site %d, area %d", m.site, m.area);
        else snprintf(b, sizeof b, "site %d, control channel LCN %d, priority %d%s%s%s", m.site, m.lcn, m.priority, m.failsoft ? ", failsoft" : "", m.scat ? ", SCAT" : "", m.aux ? ", auxiliary" : "");
        break;
    case MsgType::AdjacentSite: snprintf(b, sizeof b, "adjacent site %d, control channel LCN %d, index %d", m.site, m.lcn, m.index); break;
    case MsgType::CallEnd: snprintf(b, sizeof b, "call end (drop), LCN %d", m.lcn); break;
    case MsgType::LoginAck: snprintf(b, sizeof b, "login, group %d, unit %d", m.gid, m.lid); break;
    case MsgType::StatusRequest:
        if (m.status == 248) snprintf(b, sizeof b, "status request, unit %d", m.lid);
        else snprintf(b, sizeof b, "status %03d, unit %d", m.status, m.lid);
        break;
    case MsgType::AllCall: snprintf(b, sizeof b, "system all-call, LCN %d", m.lcn); break;
    case MsgType::SystemAssignedId: snprintf(b, sizeof b, "system assigned ID %d, group %d", m.status, m.gid); break;
    case MsgType::Other: snprintf(b, sizeof b, "%s", m.other); break;
    default: snprintf(b, sizeof b, "not interpreted"); break;
    }
    return b;
}

} // namespace dect2::edacs
