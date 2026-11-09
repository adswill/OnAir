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
