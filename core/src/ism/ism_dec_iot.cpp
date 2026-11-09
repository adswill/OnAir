// ISM sensors: smart-home bus protocols, KNX RF and HomeMatic (BidCoS). Receive only, nothing is decrypted.
//
// KNX RF: wireless M-Bus format A framing (EN 13757-4, as rtl_433's m_bus.c mode S reads it) with the KNX values in the first block. KNX RF 1.1
// uses the S-mode physical layer, 32.768 kchip/s Manchester FSK on 868.3 MHz. Block 1 is L, C, 0xFF 0x03, the 6-byte KNX serial number and
// its CRC; the data blocks of up to 16 bytes each end in a CRC (poly 0x3D65, inverted). The first data block holds the KNX control field, source
// and destination address, L/NPCI, TPCI and APCI. rtl_433 (GPL-2.0-or-later) is the source of the field layout. The address type bit and the
// hop count of the L/NPCI byte are the KNX standard's (the same byte as in KNX TP1).
//
// HomeMatic BidCoS (eQ-3): 868.3 MHz, 2-FSK, about 10 kbit/s (9.992 kbit/s), preamble 0xAA..., sync word 0xE9CA, then the length byte, that many
// bytes and a CRC-16 (poly 0x8005, init 0xFFFF, the CC1101 hardware CRC over the length byte and the bytes as sent). The bytes are obfuscated
// (not encrypted): byte 1 = ~x ^ 0x89, bytes 2 .. len-1 = (previous byte as sent + 0xDC) ^ x, the last byte ^= decoded byte 2. Plain frame:
// len, message counter, flags, type, source(3), destination(3), payload. Obfuscation as in culfw's rf_asksin.c (GPL-2.0-or-later, a-culfw);
// the flag bits and the message types are those of FHEM's HMConfig.pm (GPL-2.0-or-later). Frames the devices protect with AES are shown as
// type and header only.
//
// DASH7 Alliance Protocol (433 / 868 / 915 MHz, GFSK, 9.6 and 55.555 kbit/s; the 166.667 kbit/s class is faster than this receiver's channel): preamble,
// a 16-bit sync word (0xE6D0 background frame, 0x0B67 foreground frame, for the PN9 coding; the FEC-coded channel (words 0xF498 / 0x192F) uses a
// convolutional code that is not decoded here), then the bytes whitened with the PN9 sequence (x^9 + x^5 + 1, all ones start, 8 shifts per byte). A foreground
// frame is the length byte (bytes that follow), subnet, control (target ID type in bits 7-6, EIRP index in bits 5-0, EIRP = index - 32 dBm), the target
// ID (UID 8 bytes, VID 2 bytes, none for broadcast), the network layer (control byte, origin access class and ID), the transport and application
// layers and a CRC-16 (CCITT, init 0xFFFF, big endian) over everything before it. A background frame is subnet, control (ID type and 6-bit tag), ETA
// (16 bits) and the CRC. Layout from the Sub-IoT stack (Apache-2.0, University of Antwerp / Aloxy: phy.c, dll.c, packet.c, d7anp.c, pn9.c, crc.c).
// Only up to the origin address is decoded; the payload is shown as bytes, and not at all when the network layer says it is secured (AES).
//
// EnOcean ERP1 (868.3 MHz, ASK, 125 kbit/s): the line layer is rtl_433's enocean_erp1.c (GPL-2.0-or-later): inverted, chips 0x55 0x20 (11 bits) as
// the start, then 12 chips per byte (8 data bits with two inserted inverted bits, and a 2-bit "more bytes follow" marker), the last byte a CRC-8
// (poly 0x07) of all bytes before it. The telegram is RORG, the data, the 4-byte sender ID and a status byte (EnOcean Radio Protocol 1:
// bit 5 T21, bit 4 NU, low nibble the repeater count). Only the RORG-level fields are shown; the equipment profile (EEP) of a sender is not known
// from the air, so 4BS and VLD data is shown as bytes, except the teach-in telegram of a 4BS device, which carries its profile.
#include "ism_internal.h"
#include <cstring>

namespace dect2 {

namespace ism_detail {

namespace {

std::string knxAddr(unsigned a, bool group) {
    char b[24];
    if (group) snprintf(b, sizeof b, "%u/%u/%u", (a >> 11) & 0x1F, (a >> 8) & 7, a & 0xFF);
    else snprintf(b, sizeof b, "%u.%u.%u", (a >> 12) & 0xF, (a >> 8) & 0xF, a & 0xFF);
    return b;
}

std::string bytesHex(const uint8_t* p, unsigned n) {
    std::string s;
    for (unsigned i = 0; i < n; i++) s += hexU(p[i], 2);
    return s;
}

} // namespace

// ---------------------------------------------------------------- KNX RF

void decKnxRf(Bits& B, Out& out) {
    static const uint8_t preS[3] = {0x54, 0x76, 0x96};
    const unsigned nb = B.n[0];
    if (nb < 32 + 13 * 8 || nb > 64 + 256 * 8) return;
    unsigned off = B.search(0, 0, preS, 24);
    if (off >= nb) return;
    off += 24;
    Bits m;
    manchester(B, 0, off, m, 800);
    const unsigned nbytes = m.n[0] / 8;
    if (nbytes < 12 + 8 + 2) return;
    std::vector<uint8_t> in(nbytes);
    m.extract(0, 0, in.data(), nbytes * 8);
    if (in[2] != 0xFF || in[3] != 0x03) return;            // the KNX RF marker in the manufacturer field
    if (!mbusCrc(in.data(), 10)) return;
    const unsigned L = in[0];
    if (L < 9 + 8) return;
    const unsigned ndata = L - 9, blocks = (ndata + 15) / 16;
    if (12 + ndata + 2 * blocks > nbytes) return;
    std::vector<uint8_t> d;
    for (unsigned k = 0; k < blocks; k++) {
        const unsigned len = std::min<unsigned>(16, ndata - k * 16);
        const uint8_t* p = in.data() + 12 + k * 18;
        if (!mbusCrc(p, len)) return;
        d.insert(d.end(), p, p + len);
    }
    const unsigned src = (unsigned)(d[1] << 8 | d[2]), dst = (unsigned)(d[3] << 8 | d[4]);
    const bool group = (d[5] & 0x80) != 0;
    IsmReading R = mk("KNX-RF", "Sensor", bytesHex(&in[4], 6), "CRC");
    R.addText("Source", knxAddr(src, false));
    R.addText(group ? "Group address" : "Destination", knxAddr(dst, group));
    R.add("Hop count", (d[5] >> 4) & 7, "", 0);
    R.add("Length", d[5] & 0x0F, "bytes", 0);
    R.addText("KNX control", hexU(d[0], 2));
    R.addText("TPCI/APCI", hexU(d[6], 2) + hexU(d[7], 2));
    if (d.size() > 8) R.addText("Data", bytesHex(d.data() + 8, (unsigned)d.size() - 8));
    out.push_back(R);
}

// ---------------------------------------------------------------- DASH7

namespace {

std::string d7Id(const uint8_t* p, unsigned n) { return bytesHex(p, n); }

void pn9Dewhiten(std::vector<uint8_t>& v) {
    uint16_t r = 0x1FF;
    for (auto& b : v) {
        b ^= (uint8_t)r;
        for (int i = 0; i < 8; i++) r = (uint16_t)((((((r & 0x20) >> 5) ^ r) << 8) | ((r >> 1) & 0xFF)) & 0x1FF);
    }
}

bool d7Try(Bits& B, Out& out, const char* rateName) {
    struct Sw { uint16_t w; int cls; };
    static const Sw sws[] = {{0xE6D0, 0}, {0x0B67, 1}};
    const unsigned nb = B.n[0];
    for (const auto& sw : sws) {
        const uint8_t pat[2] = {(uint8_t)(sw.w >> 8), (uint8_t)sw.w};
        unsigned off = B.search(0, 0, pat, 16);
        if (off >= nb) continue;
        off += 16;
        const unsigned avail = (nb - off) / 8;
        if (avail < 6) continue;
        std::vector<uint8_t> d(std::min(avail, 262u));
        B.extract(0, off, d.data(), (unsigned)d.size() * 8);
        pn9Dewhiten(d);
        if (sw.cls == 0) {   // background frame: subnet, control (ID type, tag), ETA, CRC
            if (ismCrc16(d.data(), 4, 0x1021, 0xFFFF) != (uint16_t)(d[4] << 8 | d[5])) continue;
            IsmReading R = mk("DASH7-Background", "Sensor", hexU(d[0], 2) + ":" + hexU(d[1] & 0x3F, 2), "CRC");
            R.addText("Rate", rateName);
            R.addText("Subnet", hexU(d[0], 2));
            R.add("Target ID type", d[1] >> 6, "", 0);
            R.add("Identifier tag", d[1] & 0x3F, "", 0);
            R.add("ETA", (unsigned)(d[2] << 8 | d[3]), "ticks", 0);
            out.push_back(R);
            return true;
        }
        const unsigned len = d[0];
        if (len < 6 || len + 1 > d.size()) continue;
        if (ismCrc16(d.data(), len - 1, 0x1021, 0xFFFF) != (uint16_t)(d[len - 1] << 8 | d[len])) continue;
        unsigned i = 1;
        const uint8_t subnet = d[i++], ctl = d[i++];
        const unsigned idType = ctl >> 6;                       // 0 NBID, 1 NOID, 2 UID, 3 VID
        const unsigned tn = idType == 2 ? 8 : idType == 3 ? 2 : 0;
        if (i + tn > len - 1) continue;
        const std::string target = tn ? d7Id(&d[i], tn) : "broadcast";
        i += tn;
        std::string origin;
        unsigned oclass = 0;
        bool haveOrigin = false, secured = false;
        unsigned nls = 0;
        if (i < len - 1) {                                       // network layer control: NLS method (bits 3-0), origin ID type (5-4), hop (6), origin void (7)
            const uint8_t nc = d[i++];
            nls = nc & 0x0F;
            secured = nls != 0;
            if (!(nc & 0x80)) {
                const unsigned ot = (nc >> 4) & 3, on = ot == 2 ? 8 : ot == 3 ? 2 : ot == 0 ? 1 : 0;
                if (i + 1 + on <= len - 1) {
                    oclass = d[i++];
                    if (on) origin = d7Id(&d[i], on);
                    i += on;
                    haveOrigin = true;
                }
            }
        }
        IsmReading R = mk("DASH7", "Sensor", haveOrigin && !origin.empty() ? origin : target, "CRC");
        R.addText("Rate", rateName);
        R.addText("Subnet", hexU(subnet, 2));
        R.addText("Target", target);
        R.add("EIRP", (int)(ctl & 0x3F) - 32, "dBm", 0);
        if (haveOrigin) { R.addText("Origin access class", hexU(oclass, 2)); }
        if (secured) R.add("Security (NLS method)", nls, "", 0);
        else if (i < len - 1) R.addText("Payload", bytesHex(&d[i], std::min(len - 1 - i, 24u)));
        out.push_back(R);
        return true;
    }
    return false;
}

} // namespace

void decDash7(Bits& B, Out& out, bool lo) {
    if (B.n[0] < 16 + 8 * 6 || B.n[0] > 16 + 8 * 270) return;
    const char* rate = lo ? "9.6 kbit/s" : "55.555 kbit/s";
    Bits c = B;
    if (d7Try(c, out, rate)) return;
    c = B;
    c.invert();
    d7Try(c, out, rate);
}

// ---------------------------------------------------------------- EnOcean ERP1

namespace {

const char* eoRorg(uint8_t r) {
    switch (r) {
    case 0xF6: return "RPS"; case 0xD5: return "1BS"; case 0xA5: return "4BS"; case 0xD2: return "VLD"; case 0xD4: return "UTE";
    case 0xC5: return "SYS_EX"; case 0xA6: return "ADT"; case 0x30: return "SEC";
    default: return "";
    }
}

// 12 chips -> one byte: 0 ok, 1 more bytes follow, 2 end, -1 not a valid group
int eoByte(const Bits& b, unsigned pos, std::vector<uint8_t>& out) {
    if (pos + 12 > b.n[0]) return -1;
    auto c = [&](unsigned i) { return b.bit(0, pos + i); };
    if (c(2) == c(3) || c(6) == c(7)) return -1;
    const unsigned v = (unsigned)(c(0) << 7 | c(1) << 6 | c(2) << 5 | c(4) << 4 | c(5) << 3 | c(6) << 2 | c(8) << 1 | c(9));
    out.push_back((uint8_t)v);
    return c(10) == 0 && c(11) == 1 ? 1 : 2;
}

} // namespace

void decEnOcean(Bits& B, Out& out) {
    static const uint8_t pre[2] = {0x55, 0x20};
    if (B.rows() != 1 || B.n[0] < 11 + 12 * 8) return;
    Bits c = B;
    c.invert();
    unsigned pos = c.search(0, 0, pre, 11);
    if (pos >= c.n[0]) return;
    pos += 11;
    std::vector<uint8_t> t;
    for (;;) {
        const int r = eoByte(c, pos, t);
        pos += 12;
        if (r != 1 || t.size() >= 40) break;
    }
    if (t.size() < 8 || ismCrc8(t.data(), (unsigned)t.size() - 1, 0x07, 0) != t.back()) return;
    const unsigned n = (unsigned)t.size();
    const uint8_t rorg = t[0], st = t[n - 2];
    const uint8_t* d = &t[1];
    const unsigned dn = n - 7;
    IsmReading R = mk("EnOcean-ERP1", "Sensor", bytesHex(&t[n - 6], 4), "CRC");
    const char* rn = eoRorg(rorg);
    R.addText("RORG", std::string(rn) + (*rn ? " (" : "") + hexU(rorg, 2) + (*rn ? ")" : ""));
    R.add("Repeaters", st & 0x0F, "", 0);
    R.addText("Status", hexU(st, 2));
    if (rorg == 0xF6 && dn == 1) {
        if (st & 0x10) {                                     // NU: a rocker switch telegram
            R.add("Rocker 1", (d[0] >> 5) & 7, "", 0);
            R.addText("Energy bow", (d[0] & 0x10) ? "pressed" : "released");
            if (d[0] & 1) R.add("Rocker 2", (d[0] >> 1) & 7, "", 0);
        } else {
            R.add("Buttons pressed", (d[0] >> 5) & 7, "", 0);
            R.addText("Energy bow", (d[0] & 0x10) ? "pressed" : "released");
        }
    } else if (rorg == 0xD5 && dn == 1) {
        if (!(d[0] & 8)) R.addText("Teach-in", "1BS");
        else R.addText("Contact", (d[0] & 1) ? "closed" : "open");
    } else if (rorg == 0xA5 && dn == 4) {
        if (!(d[3] & 8)) {                                   // teach-in: with the EEP bit the profile and manufacturer follow
            R.addText("Teach-in", "4BS");
            if (d[3] & 0x80) {
                R.addText("EEP", "A5-" + hexU(d[0] >> 2, 2) + "-" + hexU((unsigned)(((d[0] & 3) << 5) | (d[1] >> 3)), 2));
                R.add("Manufacturer", (double)(((d[1] & 7) << 8) | d[2]), "", 0);
            }
        } else R.addText("Data", bytesHex(d, dn));
    } else if (dn) {
        R.addText("Data", bytesHex(d, std::min(dn, 24u)));
    }
    out.push_back(R);
}

// ---------------------------------------------------------------- HomeMatic BidCoS

namespace {

const char* hmType(uint8_t t) {
    switch (t) {
    case 0x00: return "Device info"; case 0x01: return "Config"; case 0x02: return "Ack"; case 0x03: return "AES reply";
    case 0x04: return "AES key"; case 0x10: return "Info"; case 0x11: return "Set"; case 0x12: return "Have data";
    case 0x3E: return "Switch"; case 0x3F: return "Time stamp"; case 0x40: return "Remote event"; case 0x41: return "Sensor event";
    case 0x42: return "Switch level"; case 0x53: return "Sensor data"; case 0x54: return "Gas event"; case 0x58: return "Climate event";
    case 0x59: return "Set team temperature"; case 0x5A: return "Thermostat control"; case 0x5E: case 0x5F: return "Power event";
    case 0x70: return "Weather event";
    default: return "";
    }
}

std::string hmFlags(uint8_t f) {
    static const char* n[8] = {"WAKEUP", "WAKEMEUP", "BCAST", "", "BURST", "BIDI", "RPTED", "RPTEN"};
    std::string s;
    for (int i = 0; i < 8; i++) if ((f >> i) & 1) { if (!s.empty()) s += ' '; s += n[i][0] ? n[i] : "BIT3"; }
    return s.empty() ? "none" : s;
}

bool hmTry(Bits& B, Out& out) {
    static const uint8_t sync[2] = {0xE9, 0xCA};
    const unsigned nb = B.n[0];
    unsigned off = B.search(0, 0, sync, 16);
    if (off + 16 + 8 >= nb) return false;
    off += 16;
    uint8_t len;
    B.extract(0, off, &len, 8);
    if (len < 9 || len > 64 || off + (unsigned)(1 + len + 2) * 8 > nb) return false;
    std::vector<uint8_t> e(1 + len + 2);
    B.extract(0, off, e.data(), (unsigned)e.size() * 8);
    if (ismCrc16(e.data(), 1 + len, 0x8005, 0xFFFF) != (uint16_t)(e[1 + len] << 8 | e[2 + len])) return false;
    std::vector<uint8_t> m(1 + len);
    m[0] = len;
    m[1] = (uint8_t)(~e[1] ^ 0x89);
    for (unsigned l = 2; l < len; l++) m[l] = (uint8_t)((e[l - 1] + 0xDC) ^ e[l]);
    m[len] = (uint8_t)(e[len] ^ m[2]);
    // m[1] counter, m[2] flags, m[3] type, m[4..6] source, m[7..9] destination, m[10..] payload
    const uint8_t* p = m.data() + 10;
    const unsigned pn = len - 9;
    IsmReading R = mk("HomeMatic-BidCoS", "Sensor", bytesHex(&m[4], 3), "CRC");
    R.addText("Destination", bytesHex(&m[7], 3));
    R.add("Message counter", m[1], "", 0);
    R.addText("Flags", hmFlags(m[2]));
    const char* ty = hmType(m[3]);
    R.addText("Type", *ty ? std::string(ty) : hexU(m[3], 2));
    if (m[3] == 0x70 && pn >= 3) {                                       // weather event: temperature in 0.1 C (sign bit 0x4000), humidity
        const unsigned t = (unsigned)(p[0] << 8 | p[1]);
        R.add("Temperature", ((t & 0x3FFF) / 10.0) * ((t & 0x4000) ? -1 : 1), "C", 1);
        R.add("Humidity", p[2], "%", 0);
    } else if ((m[3] == 0x40 || m[3] == 0x41) && pn >= 2) {                // remote / sensor event
        R.add("Button", p[0] & 0x3F, "", 0);
        R.addText("Press", (p[0] & 0x40) ? "long" : "short");
        R.battery = (p[0] & 0x80) ? 0 : 1;
        R.add(m[3] == 0x40 ? "Counter" : "Number", p[1], "", 0);
        if (m[3] == 0x41 && pn >= 3) R.add("Value", p[2], "", 0);
    } else if (m[3] == 0x02 && pn >= 5 && p[0] == 0x01) {                  // ack with status
        R.add("Channel", p[1], "", 0);
        R.add("Status", p[2], "", 0);
        R.battery = (p[3] & 0x80) ? 0 : 1;
        R.add("RSSI", -(int)p[4], "dBm", 0);
    } else if (m[3] == 0x10 && pn >= 5 && p[0] == 0x06) {                  // actuator status
        R.add("Channel", p[1], "", 0);
        R.add("Status", p[2], "", 0);
        R.add("RSSI", -(int)p[4], "dBm", 0);
    } else if (m[3] == 0x3E && pn >= 6) {                                  // switch: peer, channel, counter
        R.addText("Peer", bytesHex(p, 3));
        R.add("Channel", p[4], "", 0);
        R.add("Counter", p[5], "", 0);
    } else if (pn) {
        R.addText("Payload", bytesHex(p, std::min(pn, 24u)));
    }
    out.push_back(R);
    return true;
}

} // namespace

void decHomeMatic(Bits& B, Out& out) {
    if (B.n[0] < 16 + 8 * 12 || B.n[0] > 8 * 90 + 128) return;
    Bits c = B;
    if (hmTry(c, out)) return;
    c = B;
    c.invert();                                                            // either tone can be the mark
    hmTry(c, out);
}

} // namespace ism_detail

} // namespace dect2
