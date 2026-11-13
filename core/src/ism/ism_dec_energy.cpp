// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors: device decoders for energy meters, wireless M-Bus and Z-Wave (passive). Written from rtl_433 src/devices/.
#include "ism_internal.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace dect2 {

namespace ism_detail {

void decEfergy(Bits& B, Out& out) {
    if (B.rows() != 1) return;
    const unsigned nb = B.n[0];
    if (nb < 64 || nb > 65) return;
    uint8_t by[8];
    for (int i = 0; i < 8; i++) by[i] = B.byteAt(0, i);
    if ((by[0] & 0xf0) != 0xf0 && (by[0] & 0xf0) != 0x00) return;
    if (by[0] & 0xf0) for (auto& v : by) v = (uint8_t)~v;
    int zeros = 0;
    for (auto v : by) zeros += v == 0;
    if (zeros > 5) return;
    const unsigned sum = (unsigned)addBytes(by, 7);
    if (sum == 0 || (sum & 0xff) != by[7]) return;
    const uint8_t fact = (uint8_t)(-(int8_t)by[6] + 15);
    if (fact < 7 || fact > 23) return;
    IsmReading R = mk("Efergy-e2CT", "Energy", decs(by[2] << 8 | by[1]), "CHECKSUM");
    R.battery = (by[3] & 0x40) ? 1 : 0;
    R.add("Current", (double)(by[4] << 8 | by[5]) / (double)(1 << fact), "A", 2);
    R.add("Interval", (((by[3] & 0x30) >> 4) + 1) * 6, "s", 0);
    if (by[3] & 0x80) R.add("Learning", 1, "", 0);
    out.push_back(R);
}

// current_cost.c: CurrentCost TX and EnviR (FSK NRZ, Manchester; no checksum: the Manchester code must hold for all 64 bits)
void decCurrentCost(Bits& B, Out& out) {
    B.invert();
    static const uint8_t classic[6] = {0xcc, 0xcc, 0xcc, 0xce, 0x91, 0x5d};
    static const uint8_t envir[6] = {0x55, 0x55, 0x55, 0x55, 0xa4, 0x57};
    bool isEnvir = false;
    unsigned sp = B.search(0, 0, envir, 48);
    if (sp + 47 + 112 <= B.n[0]) { isEnvir = true; sp += 47; }
    else {
        sp = B.search(0, 0, classic, 45);
        if (sp + 45 + 112 > B.n[0]) return;
        sp += 45;
    }
    Bits pk;
    manchester(B, 0, sp, pk, 0);
    if (pk.n[0] < 64) return;
    const uint8_t* b = pk.row(0);
    const int dev = (b[0] & 0x0f) << 8 | b[1];
    if ((b[0] & 0xf0) == 0) {
        IsmReading R = mk(isEnvir ? "CurrentCost-EnviR" : "CurrentCost-TX", "Energy", decs(dev), "MANCHESTER");
        R.add("Power 0", (b[2] & 0x80) ? ((b[2] & 0x7F) << 8 | b[3]) : 0, "W", 0);
        R.add("Power 1", (b[4] & 0x80) ? ((b[4] & 0x7F) << 8 | b[5]) : 0, "W", 0);
        R.add("Power 2", (b[6] & 0x80) ? ((b[6] & 0x7F) << 8 | b[7]) : 0, "W", 0);
        out.push_back(R);
    } else if ((b[0] & 0xf0) == 64) {
        IsmReading R = mk(isEnvir ? "CurrentCost-EnviRCounter" : "CurrentCost-Counter", "Energy", decs(dev), "MANCHESTER");
        R.add("Sensor type", b[3], "", 0);
        R.add("Counter", (double)((uint32_t)b[4] << 24 | b[5] << 16 | b[6] << 8 | b[7]), "", 0);
        out.push_back(R);
    }
}

// m_bus.c: wireless M-Bus (EN 13757-4) mode T (3-of-6) and C (NRZ), the link-layer header only: manufacturer, address, version, type.
// The header block's CRC (poly 0x3D65) is checked; the application data is neither decoded nor decrypted.
uint8_t dec3of6(uint8_t v) {
    switch (v) {
    case 22: return 0x0; case 13: return 0x1; case 14: return 0x2; case 11: return 0x3;
    case 28: return 0x4; case 25: return 0x5; case 26: return 0x6; case 19: return 0x7;
    case 44: return 0x8; case 37: return 0x9; case 38: return 0xA; case 35: return 0xB;
    case 52: return 0xC; case 49: return 0xD; case 50: return 0xE; case 41: return 0xF;
    default: return 0xF0;
    }
}
bool mbusCrc(const uint8_t* b, unsigned off) {
    const uint16_t c = (uint16_t)~ismCrc16(b, off, 0x3D65, 0);
    return c == (uint16_t)((b[off] << 8) | b[off + 1]);
}
const char* mbusType(uint8_t t) {
    switch (t) {
    case 0x00: return "Other"; case 0x01: return "Oil"; case 0x02: return "Electricity"; case 0x03: return "Gas"; case 0x04: return "Heat";
    case 0x05: return "Steam"; case 0x06: return "Warm Water"; case 0x07: return "Water"; case 0x08: return "Heat Cost Allocator";
    case 0x09: return "Compressed Air"; case 0x0A: case 0x0B: return "Cooling load meter"; case 0x0C: return "Heat";
    case 0x0D: return "Heat/Cooling load meter"; case 0x0E: return "Bus/System component"; case 0x0F: return "Unknown";
    case 0x15: return "Hot Water"; case 0x16: return "Cold Water"; case 0x17: return "Hot/Cold Water meter"; case 0x18: return "Pressure";
    case 0x19: return "A/D Converter"; case 0x1A: return "Smoke detector"; case 0x1B: return "Room sensor"; case 0x1C: return "Gas detector";
    case 0x20: return "Breaker (electricity)"; case 0x21: return "Valve (gas or water)"; case 0x25: return "Customer unit (display device)";
    case 0x28: return "Waste water meter"; case 0x29: return "Garbage"; case 0x2A: return "Carbon dioxide";
    case 0x31: return "Communication controller"; case 0x32: return "Unidirectional repeater"; case 0x33: return "Bidirectional repeater";
    case 0x36: return "Radio converter (system side)"; case 0x37: return "Radio converter (meter side)";
    default: return "";
    }
}
void decMbus(Bits& B, Out& out) {
    static const uint8_t preT[2] = {0x54, 0x3D};
    const unsigned nb = B.n[0];
    if (nb < 32 + 13 * 8 || nb > 64 + 256 * 12) return;
    unsigned off = B.search(0, 0, preT, 16);
    if (off + 13 * 8 >= nb) return;
    off += 16;
    uint8_t nx;
    B.extract(0, off, &nx, 8);
    off += 8;
    std::vector<uint8_t> d;
    const char* mode;
    bool formatB = false;
    if (nx == 0x54) {
        mode = "C";
        B.extract(0, off, &nx, 8);
        off += 8;
        if (nx != 0xCD && nx != 0x3D) return;
        formatB = nx == 0x3D;
        d.resize((nb - off) / 8);
        if (d.empty()) return;
        B.extract(0, off, d.data(), (unsigned)d.size() * 8);
    } else {
        mode = "T";
        off -= 8;
        const unsigned n = (nb - off) / 12;
        d.resize(n);
        for (unsigned i = 0; i < n; i++) {
            uint8_t h, l;
            B.extract(0, off + i * 12, &h, 6);
            B.extract(0, off + i * 12 + 6, &l, 6);
            h = dec3of6(h >> 2); l = dec3of6(l >> 2);
            if (h > 0xf) { d.resize(i); break; }
            if (l > 0xf) { d.resize(i); break; }   // the frame ends at the first code that is not 3-of-6
            d[i] = (uint8_t)(h << 4 | l);
        }
    }
    if (d.size() < 12) return;
    if (!formatB) {
        if (!mbusCrc(d.data(), 10)) return;
    } else {
        const unsigned L = d[0];
        if (L < 12 || L + 1 > d.size()) return;
        if (!mbusCrc(d.data(), std::min<unsigned>(L - 1, 126))) return;
    }
    const uint16_t mf = (uint16_t)(d[3] << 8 | d[2]);
    if (d[2] == 0xFF && d[3] == 0x03) return;   // KNX RF has its own S-mode physical layer: decKnxRf (ism_dec_iot.cpp)
    const std::string man{(char)(((mf >> 10) & 0x1F) + 0x40), (char)(((mf >> 5) & 0x1F) + 0x40), (char)((mf & 0x1F) + 0x40)};
    const unsigned id = (unsigned)(bcd(d[7]) * 1000000 + bcd(d[6]) * 10000 + bcd(d[5]) * 100 + bcd(d[4]));
    char ids[16];
    snprintf(ids, sizeof ids, "%08u", id);
    IsmReading R = mk("Wireless-MBus", "Meter", ids, "CRC");
    R.addText("Mode", std::string(mode) + (formatB ? " (format B)" : ""));
    R.addText("Manufacturer", man);
    const char* ty = mbusType(d[9]);
    R.addText("Type", *ty ? std::string(ty) : hexs(d[9], 2));
    R.add("Version", d[8], "", 0);
    R.addText("C field", hexs(d[1], 2));
    R.add("Length", d[0], "bytes", 0);
    out.push_back(R);
}

// ---------------------------------------------------------------- Z-Wave (ITU-T G.9959), passive and never decrypted
// Not an rtl_433 decoder. R1: 9.6 kbit/s Manchester FSK, R2: 40 kbit/s NRZ FSK, R3: 100 kbit/s GFSK (NRZ). A frame is a 0x55 preamble, the
// start of frame 0xF0, then HomeID(4) Src(1) FC(2) Len(1) Dst(1) payload and the check: 0xFF ^ XOR of the bytes (R1, R2) or CRC-16
// CCITT, init 0x1D0F (R3). Len counts the whole MPDU. Both line polarities and (R1) both Manchester phases are tried; the check decides.

const char* zwClassName(uint8_t c) {
    switch (c) {
    case 0x00: return "No Operation"; case 0x01: return "Z-Wave Protocol"; case 0x20: return "Basic"; case 0x25: return "Switch Binary";
    case 0x26: return "Switch Multilevel"; case 0x30: return "Sensor Binary"; case 0x31: return "Sensor Multilevel"; case 0x32: return "Meter";
    case 0x56: return "CRC-16 Encap"; case 0x59: return "Association Group Info"; case 0x5E: return "Z-Wave Plus Info"; case 0x60: return "Multi Channel";
    case 0x70: return "Configuration"; case 0x71: return "Notification"; case 0x72: return "Manufacturer Specific"; case 0x73: return "Powerlevel";
    case 0x7A: return "Firmware Update"; case 0x80: return "Battery"; case 0x84: return "Wake Up"; case 0x85: return "Association";
    case 0x86: return "Version"; case 0x8E: return "Multi Channel Association"; case 0x98: return "Security S0"; case 0x9F: return "Security S2";
    default: return "";
    }
}
std::string zwCommand(uint8_t c, uint8_t k) {
    auto pick = [&](std::initializer_list<std::pair<int, const char*>> l) -> std::string { for (auto& e : l) if (e.first == k) return e.second; return ""; };
    switch (c) {
    case 0x20: case 0x25: return pick({{1, "Set"}, {2, "Get"}, {3, "Report"}});
    case 0x26: return pick({{1, "Set"}, {2, "Get"}, {3, "Report"}, {4, "Start Level Change"}, {5, "Stop Level Change"}});
    case 0x30: return pick({{2, "Get"}, {3, "Report"}});
    case 0x31: return pick({{1, "Supported Get"}, {2, "Supported Report"}, {4, "Get"}, {5, "Report"}});
    case 0x32: return pick({{1, "Get"}, {2, "Report"}, {3, "Supported Get"}, {4, "Supported Report"}, {5, "Reset"}});
    case 0x71: return pick({{4, "Get"}, {5, "Report"}, {7, "Supported Get"}, {8, "Supported Report"}});
    case 0x80: return pick({{2, "Get"}, {3, "Report"}});
    case 0x84: return pick({{4, "Interval Set"}, {5, "Notification"}, {6, "Interval Get"}, {7, "Interval Report"}, {8, "No More Information"}});
    case 0x00: return "NOP";
    case 0x01: return pick({{1, "Node Info"}, {2, "Request Node Info"}, {4, "Assign IDs"}, {0x0C, "Find Nodes In Range"}, {0x0D, "Get Nodes In Range"}});
    default: return "";
    }
}
double zwValue(const uint8_t* p, int size, bool& ok) {
    ok = size == 1 || size == 2 || size == 4;
    int32_t v = 0;
    if (!ok) return 0;
    for (int i = 0; i < size; i++) v = (int32_t)(((uint32_t)v << 8) | p[i]);
    if (size == 1) v = (int8_t)v; else if (size == 2) v = (int16_t)v;
    return v;
}
void zwField(IsmReading& R, const char* name, int value, const std::string& text) {
    IsmField f; f.name = name; f.value = value; f.decimals = 0; f.text = text; R.fields.push_back(f);
}
void zwPayload(IsmReading& R, const uint8_t* p, unsigned n) {
    if (n == 0) return;
    const uint8_t cc = p[0], cmd = n > 1 ? p[1] : 0;
    const char* cn = zwClassName(cc);
    zwField(R, "Command Class", cc, *cn ? std::string(cn) : "0x" + hexU(cc, 2));
    if (cc == 0x98 || cc == 0x9F) { R.addText("Command", "encrypted"); return; }   // S0 / S2: the payload is not shown or decoded
    if (n < 2) return;
    const std::string cs = zwCommand(cc, cmd);
    R.addText("Command", cs.empty() ? "0x" + hexU(cmd, 2) : cs);
    bool ok = false;
    if ((cc == 0x20 || cc == 0x25 || cc == 0x26) && (cmd == 1 || cmd == 3) && n >= 3) {
        if (cc == 0x25) R.addText("Value", p[2] == 0 ? "off" : p[2] == 0xFF ? "on" : decs(p[2]));
        else R.add("Level", p[2], p[2] <= 99 ? "%" : "", 0);
    } else if (cc == 0x30 && cmd == 3 && n >= 3) {
        R.addText("State", p[2] ? "triggered" : "idle");
    } else if (cc == 0x80 && cmd == 3 && n >= 3) {
        if (p[2] == 0xFF) R.addText("Battery level", "low"); else R.add("Battery level", p[2], "%", 0);
    } else if (cc == 0x84 && cmd == 7 && n >= 5) {
        R.add("Wake up interval", (p[2] << 16) | (p[3] << 8) | p[4], "s", 0);
    } else if (cc == 0x71 && cmd == 5 && n >= 4) {
        R.add("Alarm type", p[2], "", 0); R.add("Alarm level", p[3], "", 0);
    } else if ((cc == 0x31 && cmd == 5 && n >= 4) || (cc == 0x32 && cmd == 2 && n >= 4)) {
        const bool sensor = cc == 0x31;
        const uint8_t type = p[2], fmt = p[3];
        const int size = fmt & 7, scale = (fmt >> 3) & 3, prec = fmt >> 5;
        const unsigned off = 4;
        if (n < off + size) return;
        const double v = zwValue(p + off, size, ok) / std::pow(10.0, prec);
        if (!ok) return;
        const char* name = ""; const char* unit = "";
        if (sensor) {
            switch (type) {
            case 1: name = "Temperature"; unit = scale ? "F" : "C"; break;
            case 2: name = "General"; unit = scale ? "" : "%"; break;
            case 3: name = "Luminance"; unit = scale ? "lux" : "%"; break;
            case 4: name = "Power"; unit = scale ? "BTU/h" : "W"; break;
            case 5: name = "Humidity"; unit = scale ? "g/m3" : "%"; break;
            case 8: name = "Pressure"; unit = scale ? "inHg" : "kPa"; break;
            case 0x0F: name = "Voltage"; unit = scale ? "mV" : "V"; break;
            case 0x10: name = "Current"; unit = scale ? "mA" : "A"; break;
            case 0x11: name = "CO2"; unit = "ppm"; break;
            case 0x1B: name = "UV index"; unit = ""; break;
            default: name = "Sensor"; break;
            }
        } else {
            const int mt = type & 0x1F;
            static const char* const el[4] = {"kWh", "kVAh", "W", "pulses"};
            static const char* const gas[4] = {"m3", "ft3", "", "pulses"};
            static const char* const wat[4] = {"m3", "ft3", "gal", "pulses"};
            switch (mt) {
            case 1: name = scale == 2 ? "Power" : "Energy"; unit = el[scale]; break;
            case 2: name = "Gas"; unit = gas[scale]; break;
            case 3: name = "Water"; unit = wat[scale]; break;
            default: name = "Meter"; break;
            }
        }
        R.add(name, v, unit, prec);
    }
}

void decZwave(Bits& B, Out& out, int rate) {
    const bool mc = rate == 1, crc = rate == 3;
    const size_t n0 = out.size();
    static const uint8_t pre[2] = {0x55, 0xF0};
    for (int row = 0; row < B.rows(); row++) {
        for (int inv = 0; inv < 2; inv++) {
            for (int ph = 0; ph < (mc ? 2 : 1); ph++) {
                Bits S;
                if (mc) {
                    Bits src = B;
                    if (inv) src.invert();
                    manchester(src, row, (unsigned)ph, S, 0);
                } else {
                    for (unsigned i = 0; i < B.n[row]; i++) S.addBit(B.bit(row, i) ^ inv);
                }
                unsigned pos = 0;
                while (pos < S.n[0]) {
                    pos = S.search(0, pos, pre, 16);
                    if (pos >= S.n[0]) break;
                    const unsigned start = pos + 16;
                    pos++;
                    if (start + 9 * 8 > S.n[0]) continue;
                    uint8_t h[9];
                    S.extract(0, start, h, 72);
                    const unsigned L = h[7];
                    if (L < (crc ? 11u : 10u) || L > (crc ? 170u : 64u) || start + L * 8 > S.n[0]) continue;
                    std::vector<uint8_t> m(L);
                    S.extract(0, start, m.data(), L * 8);
                    if (crc) {
                        if (ismCrc16(m.data(), L - 2, 0x1021, 0x1D0F) != (uint16_t)((m[L - 2] << 8) | m[L - 1])) continue;
                    } else if ((uint8_t)(0xFF ^ xorBytes(m.data(), L - 1)) != m[L - 1]) continue;
                    const unsigned home = (unsigned)m[0] << 24 | (unsigned)m[1] << 16 | (unsigned)m[2] << 8 | m[3];
                    const unsigned src = m[4], dst = m[8];
                    char id[24];
                    snprintf(id, sizeof id, "%08X:%u", home, src);
                    IsmReading R = mk(rate == 1 ? "Z-Wave-R1" : rate == 2 ? "Z-Wave-R2" : "Z-Wave-R3", "Z-Wave", id, crc ? "CRC" : "CHECKSUM");
                    R.addText("Home ID", hexU(home, 8));
                    zwField(R, "Source", (int)src, "");
                    zwField(R, "Destination", (int)dst, dst == 0xFF ? "broadcast" : "");
                    const int ht = m[5] & 0x0F;
                    R.addText("Frame", ht == 1 ? "Singlecast" : ht == 2 ? "Multicast" : ht == 3 ? "Ack" : ht == 8 ? "Routed" : "Type " + decs(ht));
                    R.add("Sequence", m[6] & 0x0F, "", 0);
                    if (m[5] & 0x40) R.addText("Ack requested", "yes");
                    zwPayload(R, m.data() + 9, L - 9 - (crc ? 2 : 1));
                    out.push_back(R);
                    break;
                }
                if (out.size() > n0) return;
            }
        }
    }
}


} // namespace ism_detail

} // namespace dect2
