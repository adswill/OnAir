// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// MPT 1327 codewords: check bits and the address codeword fields of the control channel messages.
#include "dect2/mpt1327/mpt1327_proto.h"
#include <cstdio>
#include <cstring>

namespace dect2::mpt1327 {

namespace {

// Generator polynomial X15 + X14 + X13 + X11 + X4 + X2 + 1 without the X15 term (section 3.2.3)
constexpr uint32_t kPoly = 0x6815;

uint32_t crc15(uint64_t info48) {
    // codeword bits 1 .. 48 are the coefficients of X62 .. X15: divide info * X15 by the generator
    uint32_t reg = 0;
    for (int i = 47; i >= 0; i--) {
        const uint32_t in = (uint32_t)((info48 >> i) & 1);
        const uint32_t fb = ((reg >> 14) & 1) ^ in;
        reg = (reg << 1) & 0x7FFF;
        if (fb) reg ^= kPoly;
    }
    return reg;
}

int parity64(uint64_t v) {
    v ^= v >> 32; v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (int)(v & 1);
}

const char* const kAloha[8] = {"ALH", "ALHS", "ALHD", "ALHE", "ALHR", "ALHX", "ALHF", "reserved"};
const char* const kAlohaText[8] = {"any single codeword message invited", "invited, except RQD", "invited, except RQS", "emergency requests only",
                                   "registration or emergency requests", "invited, except RQR", "fall-back mode", "reserved"};
const char* const kAck[8] = {"ACK", "ACKI", "ACKQ", "ACKX", "ACKV", "ACKE", "ACKT", "ACKB"};
const char* const kAckText[8] = {"acknowledgement", "intermediate acknowledgement", "call queued", "message rejected", "called unit unavailable",
                                 "emergency call acknowledged", "try on the given address", "call-back or negative acknowledgement"};
const char* const kAhoy[8] = {"AHY", "spare", "AHYX", "reserved", "reserved", "AHYP", "AHYQ", "AHYC"};
const char* const kAhoyText[8] = {"availability check", "spare (customisation)", "cancel alert / waiting state", "reserved", "reserved",
                                  "called unit presence check", "status message", "short data invitation"};
const char* const kMisc[8] = {"MARK", "MAINT", "CLEAR", "MOVE", "BCAST", "reserved", "reserved", "reserved"};
const char* const kMiscText[8] = {"control channel marker", "call maintenance", "clear down from allocated channel", "move to control channel",
                                  "system parameter broadcast", "reserved", "reserved", "reserved"};

}  // namespace

uint16_t checkBits(uint64_t info48) {
    const uint32_t c = crc15(info48) ^ 1u;                       // the final check bit of the (63,48) code is inverted
    const uint64_t w63 = (info48 << 15) | c;
    return (uint16_t)((c << 1) | (uint32_t)parity64(w63));       // even parity over the whole 64-bit codeword
}

uint64_t encode(uint64_t info48) {
    info48 &= 0xFFFFFFFFFFFFull;
    return (info48 << 16) | checkBits(info48);
}

bool valid(uint64_t cw) { return checkBits(cw >> 16) == (uint16_t)(cw & 0xFFFF); }

bool correct(uint64_t& cw) {
    if (valid(cw)) return false;
    // Appendix 2: the code corrects any 1 error and detects any 4 (distance 6), so a single flipped bit that makes the word valid is the answer
    for (int i = 0; i < 64; i++)
        if (valid(cw ^ (1ull << i))) { cw ^= 1ull << i; return true; }
    return false;
}

uint32_t field(uint64_t cw, int firstBit, int count) {
    const int shift = 64 - (firstBit - 1) - count;
    return (uint32_t)((cw >> shift) & ((1ull << count) - 1));
}

const char* specialIdent(int ident) {   // section 4, Table 4-1
    switch (ident) {
    case 8191: return "ALLI";
    case 8190: return "TSCI";
    case 8189: return "IPFIXI";
    case 8188: return "SDMI";
    case 8187: return "DIVERTI";
    case 8186: return "INCI";
    case 8185: return "REGI";
    case 8103: return "DNI";
    case 8102: return "PABXI";
    case 8101: return "PSTNGI";
    case 0: return "DUMMYI";
    default: break;
    }
    return nullptr;
}

std::string addressText(const Address& a) {
    char b[40];
    if (a.ident < 0) return "-";
    if (const char* sp = specialIdent(a.ident)) return sp;
    if (a.ident >= 8121 && a.ident <= 8135) { snprintf(b, sizeof b, "short-form ident %d", a.ident - 8120); return b; }
    if (a.pfix < 0) { snprintf(b, sizeof b, "ident %d", a.ident); return b; }
    snprintf(b, sizeof b, "%d/%d", a.pfix, a.ident);
    return b;
}

bool decodeCcsc(uint64_t cw, int& sys) {
    if (field(cw, 1, 1) != 0 || (uint16_t)(cw & 0xFFFF) != kSync) return false;
    sys = (int)field(cw, 2, 15);
    return true;
}

bool decodeAddress(uint64_t cw, Message& m) {
    m = Message();
    m.cw = cw;
    if (field(cw, 1, 1) != 1) return false;
    char b[200];
    const int pfix = (int)field(cw, 2, 7), ident1 = (int)field(cw, 9, 13);
    if (field(cw, 22, 1) == 0) {   // the go-to-channel structure (section 5.4): bit 22 is 0
        m.type = "GTC";
        m.called = {pfix, ident1};
        m.channel = (int)field(cw, 24, 10);
        m.caller = {-1, (int)field(cw, 34, 13)};
        snprintf(b, sizeof b, "go to channel %d, %s", m.channel, field(cw, 23, 1) ? "data (audio muted)" : "speech");
        m.detail = b;
        return true;
    }
    const uint32_t cat = field(cw, 23, 3);
    if (cat == 0) {
        const uint32_t type = field(cw, 26, 2), func = field(cw, 28, 3);
        if (type == 3) {   // miscellaneous (5.5.4)
            m.type = kMisc[func];
            m.detail = kMiscText[func];
            if (func == 0) {   // MARK: CHAN4 (bits 2-5), A, SYS (7-21)
                m.sys = (int)field(cw, 7, 15);
                snprintf(b, sizeof b, "%s, system %d, control channel (low 4 bits) %u", kMiscText[func], m.sys, (unsigned)field(cw, 2, 4));
                m.detail = b;
            } else if (func == 1) {   // MAINT: PFIX/IDENT1, CHAN (31-40), OPER (41-43)
                static const char* const oper[8] = {"pressel on", "pressel off", "periodic message", "disconnect", "customised", "reserved", "clear down units not labelled by this address", "disable user transmission"};
                m.called = {pfix, ident1};
                m.channel = (int)field(cw, 31, 10);
                snprintf(b, sizeof b, "call maintenance on channel %d: %s", m.channel, oper[field(cw, 41, 3)]);
                m.detail = b;
            } else if (func == 2) {   // CLEAR: CHAN (2-11), CONT (12-21)
                m.channel = (int)field(cw, 2, 10);
                const int cont = (int)field(cw, 12, 10);
                if (cont) snprintf(b, sizeof b, "clear down from channel %d, move to control channel %d", m.channel, cont);
                else snprintf(b, sizeof b, "clear down from channel %d", m.channel);
                m.detail = b;
            } else if (func == 3) {   // MOVE: PFIX/IDENT1, CONT (31-40), (M) (41-45)
                m.called = {pfix, ident1};
                m.channel = (int)field(cw, 31, 10);
                snprintf(b, sizeof b, "move to control channel %d (address qualifier %u)", m.channel, (unsigned)field(cw, 41, 5));
                m.detail = b;
            } else if (func == 4) {   // BCAST: SYSDEF in bits 2 .. 6, SYS in bits 7 .. 21 (section 5.5.4.5)
                const uint32_t sysdef = field(cw, 2, 5);
                m.sys = (int)field(cw, 7, 15);
                if (sysdef == 0 || sysdef == 1 || sysdef == 4 || sysdef == 5) {
                    m.channel = (int)field(cw, 31, 10);
                    snprintf(b, sizeof b, "%s, system %d, control channel %d", sysdef == 0 ? "control channel announced" : sysdef == 1 ? "control channel withdrawn" : sysdef == 4 ? "adjacent site control channel" : "vote now advice", m.sys, m.channel);
                    if (sysdef == 4 || sysdef == 5) { const int adj = (int)field(cw, 45, 4); if (adj) snprintf(b + strlen(b), sizeof b - strlen(b), ", site %d", adj); }
                    m.detail = b;
                } else if (sysdef == 2) {   // PER (31), IVAL (32-36), PON (37), ID (38)
                    const bool per = field(cw, 31, 1) == 0;
                    snprintf(b, sizeof b, "call maintenance parameters, system %d: %s%s, pressel on %s, MAINT address %s", m.sys,
                             per ? "periodic messages every " : "no periodic messages", per ? std::to_string(field(cw, 32, 5)).append(" s").c_str() : "",
                             field(cw, 37, 1) ? "not sent" : "sent", field(cw, 38, 1) ? "individual" : "group");
                    m.detail = b;
                } else if (sysdef == 3) { snprintf(b, sizeof b, "registration parameters, system %d", m.sys); m.detail = b; }
                else { snprintf(b, sizeof b, "broadcast type %u, system %d (reserved or spare)", (unsigned)sysdef, m.sys); m.detail = b; }
            } else {
                m.called = {pfix, ident1};   // reserved functions: shown as sent
            }
            return true;
        }
        m.called = {pfix, ident1};
        if (type == 0) {   // Aloha (5.5.1): CHAN4 31-34, WT 35-37, RSVD 38-39, (M) 40-44, (N) 45-48
            m.type = kAloha[func];
            snprintf(b, sizeof b, "%s, control channel (low 4 bits) %u, wait %u, address bits %u, aloha number %u", kAlohaText[func],
                     (unsigned)field(cw, 31, 4), (unsigned)field(cw, 35, 3), (unsigned)field(cw, 40, 5), (unsigned)field(cw, 45, 4));
            m.detail = b;
        } else if (type == 1) {   // acknowledgements (5.5.2): IDENT2 31-43, QUAL 44, (N) 45-48
            m.type = kAck[func];
            m.caller = {-1, (int)field(cw, 31, 13)};
            snprintf(b, sizeof b, "%s, qualifier %u", kAckText[func], (unsigned)field(cw, 44, 1));
            m.detail = b;
        } else {           // ahoys (5.5.3.2), IDENT2 31-43
            m.type = kAhoy[func];
            m.caller = {-1, (int)field(cw, 31, 13)};
            m.detail = kAhoyText[func];
            if (func == 0) {   // AHY: D 44, POINT 45, CHECK 46, E 47, AD 48
                snprintf(b, sizeof b, "availability check of the %s unit, %s%s%s%s", field(cw, 45, 1) ? "calling" : "called", field(cw, 44, 1) ? "data" : "speech",
                         field(cw, 46, 1) ? ", readiness checked" : "", field(cw, 47, 1) ? ", emergency" : "", field(cw, 48, 1) ? ", data codeword follows" : "");
                m.detail = b;
            } else if (func == 6) {   // AHYQ: STATUS 44-48
                snprintf(b, sizeof b, "status message %u", (unsigned)field(cw, 44, 5));
                m.detail = b;
            } else if (func == 7) {   // AHYC: SLOTS 44-45, DESC 46-48
                snprintf(b, sizeof b, "short data invitation, descriptor %u, %u slots", (unsigned)field(cw, 46, 3), (unsigned)field(cw, 44, 2));
                m.detail = b;
            }
        }
        return true;
    }
    m.called = {pfix, ident1};
    if (cat == 1) {
        if (field(cw, 26, 1)) {   // HEAD (5.6.2): LEN 27-28, PFIX2 29-35, IDENT2 36-48
            m.type = "HEAD";
            m.caller = {(int)field(cw, 29, 7), (int)field(cw, 36, 13)};
            snprintf(b, sizeof b, "short data message header, %u data codeword%s follow", (unsigned)field(cw, 27, 2) + 1, field(cw, 27, 2) ? "s" : "");
            m.detail = b;
        } else {
            m.type = "SAM";
            m.detail = "single address message";
        }
    } else if (cat == 2) {
        m.type = "STDDATA";
        m.detail = "standard data call set-up";
    } else if (cat == 5) {
        m.type = "STDACK";
        m.detail = "standard data acknowledgement or selective acknowledgement";
    } else {
        snprintf(b, sizeof b, "category %u (reserved or spare)", (unsigned)cat);
        m.type = "RSVD";
        m.detail = b;
    }
    return true;
}

}  // namespace dect2::mpt1327
