// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Thread frame layers above 802.15.4 (see thread_proto.h).
#include "dect2/thread/thread_proto.h"
#include <cstdio>
#include <cstring>

namespace dect2 {
namespace thread {

const char* mleCommandName(int c) {
    switch (c) {
        case 0: return "Link Request"; case 1: return "Link Accept"; case 2: return "Link Accept and Request"; case 3: return "Link Reject";
        case 4: return "Advertisement"; case 7: return "Data Request"; case 8: return "Data Response"; case 9: return "Parent Request";
        case 10: return "Parent Response"; case 11: return "Child ID Request"; case 12: return "Child ID Response";
        case 13: return "Child Update Request"; case 14: return "Child Update Response"; case 15: return "Announce";
        case 16: return "Discovery Request"; case 17: return "Discovery Response";
        default: return "";
    }
}

const char* serviceOfPort(int p) {
    switch (p) {
        case kMlePort: return "MLE";
        case kCoapPort: return "CoAP";
        case kTmfPort: return "Thread Management (CoAP)";
        case kJoinerPort: return "Joiner";
        default: return "";
    }
}

namespace {
std::string hexBytes(const uint8_t* p, size_t n) {
    std::string s; char b[4];
    for (size_t i = 0; i < n; i++) { snprintf(b, sizeof b, "%02X", p[i]); if (i) s += ':'; s += b; }
    return s;
}
const char* coapType(int t) { static const char* n[] = {"CON", "NON", "ACK", "RST"}; return n[t & 3]; }
std::string coapCode(int c) {
    char b[24]; const int cl = c >> 5, d = c & 31;
    snprintf(b, sizeof b, "%d.%02d", cl, d);
    std::string s = b;
    if (c == 0x01) s += " GET"; else if (c == 0x02) s += " POST"; else if (c == 0x03) s += " PUT"; else if (c == 0x04) s += " DELETE";
    else if (c == 0x41) s += " Created"; else if (c == 0x44) s += " Changed"; else if (c == 0x45) s += " Content";
    return s;
}
}

// IPHC: 011 TF(2) NH HLIM(2) | CID SAC SAM(2) M DAC DAM(2), RFC 6282 clause 3.1; the inline fields in order, then the UDP next header compression
Frame analyse(const zigbee::Frame& zf, const uint8_t* psdu, size_t n) {
    Frame t;
    char b[240];
    t.encrypted = zf.encrypted;
    if (!zf.ok) return t;
    const uint8_t* p = psdu + zf.payloadAt;
    const size_t pl = zf.payloadLen;
    if (zf.type == zigbee::Beacon && !zf.encrypted) {
        if (pl >= 26 && p[pl - 26] == 3) {   // protocol id 3 (Thread), flags (version in the upper nibble), network name (16), extended PAN ID (8)
            const uint8_t* q = p + pl - 26;
            t.beacon = true;
            t.version = q[1] >> 4;
            char nm[17] = {};
            for (int i = 0; i < 16 && q[2 + i]; i++) nm[i] = (q[2 + i] >= 32 && q[2 + i] < 127) ? (char)q[2 + i] : '?';
            t.networkName = nm;
            for (int i = 0; i < 8; i++) t.extPan = t.extPan << 8 | q[18 + i];
            snprintf(b, sizeof b, "Thread beacon, network \"%s\", ext PAN %s, version %d%s", nm, hexBytes(q + 18, 8).c_str(), t.version,
                     zf.assocPermit ? ", joining open" : "");
            t.info = b;
            return t;
        }
    }
    if (zf.type != zigbee::Data) {
        t.info = zf.info;
        return t;
    }
    if (zf.encrypted) {
        snprintf(b, sizeof b, "Thread data, link-layer AES-CCM encrypted (level %d, key index %d, counter %u), %zu bytes: content not readable without the network key",
                 zf.secLevel, zf.secKeyIndex, (unsigned)zf.frameCounter, pl);
        t.info = b;
        return t;
    }
    size_t i = 0;
    std::string what;
    // 6LoWPAN dispatches in order: mesh header, fragment header, then IPHC or an uncompressed IPv6 header
    while (i < pl) {
        const uint8_t d0 = p[i];
        if ((d0 >> 6) == 2) {   // 10 V F HopsLeft: mesh header
            t.lowpan = t.mesh = true;
            const bool v = d0 >> 5 & 1, f = d0 >> 4 & 1;
            int hops = d0 & 15;
            size_t k = 1;
            if (hops == 15) { if (pl < i + 2) break; hops = p[i + 1]; k = 2; }
            t.meshHops = hops;
            const size_t a1 = v ? 2 : 8, a2 = f ? 2 : 8;
            if (pl < i + k + a1 + a2) break;
            auto addr = [&](size_t at, size_t len) {
                char ab[24];
                if (len == 2) snprintf(ab, sizeof ab, "0x%02X%02X", p[at], p[at + 1]);
                else { int o = 0; for (size_t q = 0; q < len; q++) o += snprintf(ab + o, sizeof ab - (size_t)o, "%02X", p[at + q]); }
                return std::string(ab);
            };
            t.meshOrig = addr(i + k, a1);
            t.meshFinal = addr(i + k + a1, a2);
            i += k + a1 + a2;
            what += "mesh, ";
            continue;
        }
        if ((d0 >> 3) == 0x18 || (d0 >> 3) == 0x1C) {   // 11000 / 11100: FRAG1 / FRAGN
            const bool first = (d0 >> 3) == 0x18;
            if (pl < i + (first ? 4u : 5u)) break;
            t.lowpan = t.frag = true;
            t.fragSize = (d0 & 7) << 8 | p[i + 1];
            t.fragTag = p[i + 2] << 8 | p[i + 3];
            t.fragOffset = first ? 0 : p[i + 4] * 8;
            i += first ? 4 : 5;
            snprintf(b, sizeof b, "fragment tag %d (%d bytes%s), ", t.fragTag, t.fragSize, first ? ", first" : "");
            what += b;
            if (!first) { i = pl; break; }   // later fragments carry no header of their own
            continue;
        }
        break;
    }
    if (i < pl && (p[i] >> 5) == 3 && pl >= i + 2) {   // 011: IPHC
        t.lowpan = t.iphc = true;
        const int b0 = p[i], b1 = p[i + 1];
        i += 2;
        const int tf = b0 >> 3 & 3, nh = b0 >> 2 & 1, hlim = b0 & 3;
        const int cid = b1 >> 7 & 1, sac = b1 >> 6 & 1, sam = b1 >> 4 & 3, m = b1 >> 3 & 1, dac = b1 >> 2 & 1, dam = b1 & 3;
        if (cid) i += 1;
        i += tf == 0 ? 4 : tf == 1 ? 3 : tf == 2 ? 1 : 0;
        if (!nh) { if (i < pl) t.nextHeader = p[i]; i += 1; }
        if (hlim == 0) i += 1;
        auto addr = [&](size_t len, const char* tag, std::string& out) {
            if (i + len > pl) { i = pl + 1; return; }
            if (len == 16) out = std::string(tag) + " " + hexBytes(p + i, 16);
            else if (len) out = std::string(tag) + " (inline " + std::to_string(len) + " bytes " + hexBytes(p + i, len) + ")";
            i += len;
        };
        std::string src, dst;
        const size_t sl = sac ? (sam == 0 ? 0 : sam == 1 ? 8 : sam == 2 ? 2 : 0) : (sam == 0 ? 16 : sam == 1 ? 8 : sam == 2 ? 2 : 0);
        addr(sl, "src", src);
        size_t dl;
        if (!m) dl = dac ? (dam == 1 ? 8 : dam == 2 ? 2 : 0) : (dam == 0 ? 16 : dam == 1 ? 8 : dam == 2 ? 2 : 0);
        else dl = dac ? (dam == 0 ? 6 : 0) : (dam == 0 ? 16 : dam == 1 ? 6 : dam == 2 ? 4 : 1);
        addr(dl, m ? "dst multicast" : "dst", dst);
        if (i > pl) { t.info = what + "6LoWPAN IPHC (truncated)"; return t; }
        if (nh && i < pl && (p[i] & 0xF8) == 0xF0) {   // UDP header compression: 11110 C PP
            const int c = p[i] >> 2 & 1, pp = p[i] & 3;
            i++;
            t.udp = true;
            if (pp == 0 && i + 4 <= pl) { t.srcPort = p[i] << 8 | p[i + 1]; t.dstPort = p[i + 2] << 8 | p[i + 3]; i += 4; }
            else if (pp == 1 && i + 3 <= pl) { t.srcPort = p[i] << 8 | p[i + 1]; t.dstPort = 0xF000 | p[i + 2]; i += 3; }
            else if (pp == 2 && i + 3 <= pl) { t.srcPort = 0xF000 | p[i]; t.dstPort = p[i + 1] << 8 | p[i + 2]; i += 3; }
            else if (pp == 3 && i + 1 <= pl) { t.srcPort = 0xF0B0 | (p[i] >> 4); t.dstPort = 0xF0B0 | (p[i] & 15); i += 1; }
            else { t.udp = false; }
            if (!c) i += 2;   // checksum inline
        } else if (!nh && t.nextHeader == 17 && i + 8 <= pl) {   // uncompressed UDP header
            t.udp = true; t.srcPort = p[i] << 8 | p[i + 1]; t.dstPort = p[i + 2] << 8 | p[i + 3]; i += 8;
        }
        snprintf(b, sizeof b, "6LoWPAN IPHC%s%s%s", src.empty() ? "" : ", ", src.c_str(), dst.empty() ? "" : (std::string(", ") + dst).c_str());
        what += b;
        if (t.udp) {
            t.service = serviceOfPort(t.dstPort)[0] ? serviceOfPort(t.dstPort) : serviceOfPort(t.srcPort);
            snprintf(b, sizeof b, ", UDP %d > %d%s%s", t.srcPort, t.dstPort, t.service.empty() ? "" : " ", t.service.c_str());
            what += b;
            if (i < pl && (t.srcPort == kMlePort || t.dstPort == kMlePort)) {   // MLE: security suite 0xFF = none, 0 = AES-CCM (the rest is encrypted)
                t.mle = true;
                if (p[i] == 0xFF && i + 1 < pl) {
                    t.mleSecured = false; t.mleCommand = p[i + 1];
                    const char* nm = mleCommandName(t.mleCommand);
                    snprintf(b, sizeof b, ", MLE %s (not secured)", nm[0] ? nm : ("command " + std::to_string(t.mleCommand)).c_str());
                    what += b;
                } else if (p[i] == 0) {
                    t.mleSecured = true;
                    what += ", MLE secured with AES-CCM (command encrypted)";
                } else what += ", MLE";
            } else if (i + 4 <= pl && (t.dstPort == kCoapPort || t.srcPort == kCoapPort || t.dstPort == kTmfPort || t.srcPort == kTmfPort) && (p[i] >> 6) == 1) {
                t.coap = true;
                snprintf(b, sizeof b, ", CoAP %s %s", coapType(p[i] >> 4 & 3), coapCode(p[i + 1]).c_str());
                what += b;
            }
        } else if (t.nextHeader == 58) what += ", ICMPv6";
        else if (nh && !t.udp) what += ", compressed next header";
        t.info = what;
        return t;
    }
    if (t.lowpan) { t.info = what.empty() ? "6LoWPAN" : what.substr(0, what.size() >= 2 ? what.size() - 2 : 0); return t; }
    if (pl >= 1 && p[0] == 0x41) { t.info = "6LoWPAN uncompressed IPv6 header"; t.lowpan = true; return t; }
    snprintf(b, sizeof b, "Data, %zu bytes (not 6LoWPAN)", pl);
    t.info = b;
    return t;
}

} // namespace thread
} // namespace dect2
