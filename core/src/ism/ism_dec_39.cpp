// ISM sensors, the 3.9 additions: home automation (X10 RF, Insteon), thermometers (inFactory, Ambient Weather / Ecowitt WH31E),
// tyre pressure (Nissan, GM aftermarket), energy and utility meters (Efergy Optical, ESIC EMT7110, Itron ERT SCM+).
//
// Written from the protocol descriptions in rtl_433 (GPL-2.0-or-later, https://github.com/merbanan/rtl_433) src/devices/: x10_rf.c, insteon.c,
// infactory.c, ambientweather_wh31e.c, tpms_nissan.c, tpms_gm.c, efergy_optical.c, esic_emt7110.c, scmplus.c. Every one of those files says
// "version 2 of the License, or (at your option) any later version", which is GPL-3 compatible. The code is our own; the formats, checks and
// constants are theirs. The same devices are also decoded by the vendored rtl_433 sources of the Sub-GHz sensors mode.
#include "ism_internal.h"

namespace dect2 {

// bit_util.c crc4: the remainder in the high nibble, as rtl_433 computes it
uint8_t ismCrc4(const uint8_t* m, unsigned n, uint8_t poly, uint8_t init) {
    unsigned rem = (unsigned)init << 4;
    const unsigned p = (unsigned)poly << 4;
    for (unsigned i = 0; i < n; i++) {
        rem ^= m[i];
        for (int b = 0; b < 8; b++) rem = (rem & 0x80) ? (rem << 1) ^ p : rem << 1;
    }
    return (uint8_t)((rem >> 4) & 0x0f);
}

namespace ism_detail {

namespace {
uint8_t reverse8(uint8_t x) {
    x = (uint8_t)((x & 0xF0) >> 4 | (x & 0x0F) << 4);
    x = (uint8_t)((x & 0xCC) >> 2 | (x & 0x33) << 2);
    return (uint8_t)((x & 0xAA) >> 1 | (x & 0x55) << 1);
}
} // namespace

// x10_rf.c: X10 RF remotes and sensors (310 / 433 MHz). A 9 ms sync pulse and 4.5 ms gap, then 32 bits PPM (562 / 1687 us gaps); byte 1 is
// the complement of byte 0, byte 3 of byte 2, and some bits are constant. Row 0 is the sync.
void decX10Rf(Bits& B, Out& out) {
    if (B.rows() < 2 || B.n[1] != 32) return;
    const uint8_t* b = B.row(1);
    if ((b[0] ^ b[1]) != 0xff || (b[2] ^ b[3]) != 0xff) return;
    static const uint8_t mask[4] = {0x0B, 0x0B, 0x07, 0x07}, val[4] = {0x00, 0x0B, 0x00, 0x07};
    for (int i = 0; i < 4; i++) if ((b[i] & mask[i]) != val[i]) return;
    const int h0 = (b[0] >> 7) & 1, h1 = (b[0] >> 6) & 1, h2 = (b[0] >> 5) & 1, h3 = (b[0] >> 4) & 1;
    const int house = ((~(h0 ^ h1) & 1) << 3) | ((~h1 & 1) << 2) | (((h1 ^ h2) & 1) << 1) | (h3 & 1);
    int unit = (((b[0] & 0x04) << 1) | ((b[2] & 0x40) >> 4) | ((b[2] & 0x08) >> 2) | ((b[2] & 0x10) >> 4)) + 1;
    std::string ev;
    if (b[2] & 0x80) {
        unit = 0;
        switch (b[2]) {
        case 0x98: ev = "DIM"; break;
        case 0x88: ev = "BRI"; break;
        case 0x90: ev = "ALL LTS ON"; break;
        case 0x80: ev = "ALL OFF"; break;
        default: ev = "UNKNOWN"; break;
        }
    } else ev = (b[2] & 0x20) == 0 ? "ON" : "OFF";
    IsmReading R = mk("X10-RF", "Remote", decs(unit), "PARITY");
    R.channelName = std::string(1, (char)('A' + house));
    R.addText("State", ev);
    R.add("Command code", b[2], "", 0);
    R.addText("Data", hexs((unsigned)b[0] << 24 | (unsigned)b[1] << 16 | (unsigned)b[2] << 8 | b[3], 8));
    out.push_back(R);
}

// insteon.c: Insteon RF (915 MHz FSK, 9.124 kbit/s, inverted). After the preamble CE AA every byte is a 28 bit block: 5 bit index and 8 bit
// data, both Manchester and LSB first, then "11". The first index is 31, the others count down. Standard messages are 10 bytes (flags,
// to, from, cmd1, cmd2, CRC), extended ones 23 (14 user data bytes and a two's complement sum).
namespace {
uint8_t insteonCrc(const uint8_t* d) {
    uint8_t r = 0;
    for (int i = 0; i < 9; i++) { r ^= d[i]; r ^= (uint8_t)(((r ^ (r << 1)) & 0x0F) << 4); }
    return r;
}
uint8_t insteonExtCrc(const uint8_t* d) {
    uint8_t r = 0;
    for (int i = 7; i < 22; i++) r = (uint8_t)(r + d[i]);
    return (uint8_t)(~r + 1);
}
bool insteonPacket(const Bits& B, int row, unsigned start, Out& out) {
    uint8_t res[35] = {0};
    int n = 0;
    start += 7;
    Bits ib, db;
    unsigned next = manchester(B, row, start, ib, 5);
    uint8_t pi = reverse8(ib.byteAt(0, 0));
    next = manchester(B, row, next, db, 8);
    res[n++] = reverse8(db.byteAt(0, 0));
    if (pi != 31) return false;
    uint8_t two = 0;
    B.extract(row, start + 26, &two, 2);
    if (two != 0xc0) return false;
    const bool ext = res[0] & 0x10;
    const int maxLen = ext ? 32 : 13, minLen = ext ? 23 : 10;
    uint8_t prev = 33;
    for (int j = 1; j < maxLen; j++) {
        start += 28;
        ib.clear(); db.clear();
        next = manchester(B, row, start, ib, 5);
        next = manchester(B, row, next, db, 8);
        if (next - start != 26) break;
        pi = reverse8(ib.byteAt(0, 0));
        res[n++] = reverse8(db.byteAt(0, 0));
        if (pi < prev) prev = pi;
        else return false;
    }
    if (n < minLen) return false;
    if (res[minLen - 1] != (ext ? insteonExtCrc(res) : insteonCrc(res))) return false;
    static const char* const kType[8] = {"Direct Message", "ACK of Direct Message", "Group Cleanup Direct Message", "ACK of Group Cleanup Direct Message",
                                         "Broadcast Message", "NAK of Direct Message", "Group Broadcast Message", "NAK of Group Cleanup Direct Message"};
    char from[8], to[8];
    snprintf(from, sizeof from, "%02X%02X%02X", res[6], res[5], res[4]);
    snprintf(to, sizeof to, "%02X%02X%02X", res[3], res[2], res[1]);
    std::string cmd;
    for (int j = 7; j < minLen - 1; j++) { char h[4]; snprintf(h, sizeof h, "%s%02X", cmd.empty() ? "" : " ", res[j]); cmd += h; }
    IsmReading R = mk("Insteon", "Sensor", from, "CRC");
    R.addText("To", to);
    R.addText("Message", kType[(res[0] >> 5) & 7]);
    R.add("Message type", (res[0] >> 5) & 7, "", 0);
    R.add("Cmd1", res[7], "", 0);
    R.add("Extended", ext ? 1 : 0, "", 0);
    R.add("Hops max", res[0] & 3, "", 0);
    R.add("Hops left", (res[0] >> 2) & 3, "", 0);
    R.addText("Command", cmd);
    out.push_back(R);
    return true;
}
} // namespace

void decInsteon(Bits& B, Out& out) {
    B.invert();
    static const uint8_t pre[2] = {0xCE, 0xAA};
    const unsigned minBits = 10 * 28 + 2;
    for (int row = 0; row < B.rows(); row++) {
        if (B.n[row] < minBits) continue;
        unsigned idx = 0;
        while (B.n[row] - idx >= minBits) {
            const unsigned s = B.search(row, idx, pre, 16);
            if (s >= B.n[row]) break;
            idx = insteonPacket(B, row, s, out) ? s + minBits : s + 16;
            if (idx >= B.n[row]) break;
        }
    }
}

// infactory.c: inFactory NC-3982-913, nor-tec 73383, DAY 73365, Tchibo NC-3982-675 (PPM, 2000 / 4000 us gaps, 40 bits). iiiiiiii cccc ub??
// tttttttt tttthhhh hhhh??nn: CRC-4 (poly 3, ITU G.704) over the frame with the CRC nibble replaced by the low nibble of byte 4, xor the
// high nibble of byte 4; temperature in F + 90, scale 10; humidity BCD.
void decInfactory(Bits& B, Out& out) {
    if (B.n[0] != 40 && B.n[0] != 41 && B.n[0] != 42) return;
    const uint8_t* b = B.row(0);
    const int channel = b[4] & 0x03;
    if (!channel) return;
    uint8_t m[5] = {b[0], (uint8_t)((b[1] & 0x0F) | (b[4] & 0x0F) << 4), b[2], b[3], b[4]};
    if ((uint8_t)(ismCrc4(m, 4, 0x13, 0) ^ (b[4] >> 4)) != (b[1] >> 4)) return;
    const int traw = (b[2] << 4) | (b[3] >> 4), hum = (b[3] & 0x0F) * 10 + (b[4] >> 4);
    const double tf = (traw - 900) * 0.1;
    if (hum > 100 || tf < -40 || tf > 158) return;
    IsmReading R = mk("inFactory-TH", "Weather", decs(b[0]), "CRC");
    R.channel = channel;
    R.battery = (b[1] >> 2) & 1 ? 0 : 1;
    R.add("Temperature", (tf - 32) / 1.8, "C", 1);
    R.add("Humidity", hum, "%", 0);
    R.add("Button", (b[1] >> 3) & 1, "", 0);
    out.push_back(R);
}

// ambientweather_wh31e.c: Ambient Weather WH31E / WH31B, Ecowitt (FSK PCM 56 us, aa 2d d4 sync). YY II CT TT HH XX AA: type 0x30 / 0x37,
// channel and battery, temperature 10 bit + 400 scale 10, humidity, CRC-8 poly 0x31, SUM-8.
void decWh31e(Bits& B, Out& out) {
    static const uint8_t pre[3] = {0xaa, 0x2d, 0xd4};
    for (int row = 0; row < B.rows(); row++) {
        const unsigned s = B.search(row, 0, pre, 24);
        if (s >= B.n[row] || B.n[row] - s < 24 + 56) continue;
        uint8_t b[18];
        B.extract(row, s + 24, b, 18 * 8);
        if (b[0] != 0x30 && b[0] != 0x37) continue;
        if (ismCrc8(b, 6, 0x31, 0x00)) continue;
        if ((uint8_t)(addBytes(b, 6) - b[6])) continue;
        IsmReading R = mk(b[0] == 0x30 ? "AmbientWeather-WH31E" : "AmbientWeather-WH31B", "Weather", decs(b[1]), "CRC");
        R.channel = ((b[2] & 0x70) >> 4) + 1;
        R.battery = (b[2] & 0x04) ? 0 : 1;
        R.add("Temperature", ((((b[2] & 0x03) << 8) | b[3]) - 400) * 0.1, "C", 1);
        R.add("Humidity", b[4], "%", 0);
        out.push_back(R);
        return;
    }
}

// tpms_nissan.c: Nissan TPMS (FSK PCM 120 us). Preamble f5 55 55 55 e, then 37 bits Manchester, inverted: MODE:3 ID:24 (PSI+3)*4:8 ?:2,
// a 2 bit checksum.
void decNissan(Bits& B, Out& out) {
    static const uint8_t pre[5] = {0xf5, 0x55, 0x55, 0x55, 0xe0};
    unsigned pos = 0;
    while ((pos = B.search(0, pos, pre, 36)) + 77 <= B.n[0]) {
        Bits pk;
        manchester(B, 0, pos + 36, pk, 113);
        pos += 1;
        if (pk.n[0] < 37) continue;
        pk.invert();
        const uint8_t* b = pk.row(0);
        uint8_t chk = 0;
        for (int i = 0; i < 4; i++) chk = (uint8_t)(chk + (b[i] >> 7) + (b[i] >> 5) + (b[i] >> 3) + (b[i] >> 1) + (uint8_t)(b[i] << 1));
        chk = (uint8_t)(chk + (b[4] >> 7) + (b[4] >> 5) + (b[4] >> 3));
        if ((~chk & 0x03) != 0) continue;
        const unsigned id = ((unsigned)(b[0] & 0x1F) << 19) | (unsigned)(b[1] << 11) | (unsigned)(b[2] << 3) | (unsigned)(b[3] >> 5);
        const int praw = ((b[3] & 0x1F) << 3) | (b[4] >> 5);
        IsmReading R = mk("Nissan", "TPMS", hexs(id, 6), "CHECKSUM");
        R.add("Pressure", praw / 4.0 - 3.0, "PSI", 2);
        R.add("Mode", b[0] >> 5, "", 0);
        out.push_back(R);
        return;
    }
}

// tpms_gm.c: GM aftermarket TPMS (OOK Manchester, 120 us). 130 bits: 48 zero bits, flags (16), device (8 + 32 bit id), pressure * 2.75 kPa,
// temperature + 60, modulo 256 sum.
void decGmTpms(Bits& B, Out& out) {
    if (B.rows() != 1 || B.n[0] != 130) return;
    uint8_t b[17] = {0};
    B.extract(0, 0, b, 130);
    for (int i = 0; i < 6; i++) if (b[i]) return;
    uint8_t sum = 0;
    bool zero = true;
    for (int i = 6; i < 15; i++) { sum = (uint8_t)(sum + b[i]); zero = zero && !b[i]; }
    if (sum != b[15] || (zero && !b[15])) return;
    const uint64_t id = ((uint64_t)b[8] << 32) | ((uint64_t)b[9] << 24) | ((uint64_t)b[10] << 16) | ((uint64_t)b[11] << 8) | b[12];
    const int flags = (b[6] << 8) | b[7];
    char ids[24];
    snprintf(ids, sizeof ids, "%llu", (unsigned long long)id);
    IsmReading R = mk("GM-Aftermarket", "TPMS", ids, "CHECKSUM");
    R.battery = (flags >> 5) & 1 ? 0 : 1;
    R.add("Pressure", b[13] * 2.75, "kPa", 2);
    R.add("Temperature", b[14] - 60.0, "C", 0);
    R.add("Learn mode", (((flags >> 8) & 1) == 0 && ((flags >> 1) & 1) == 0 && (flags & 1) == 0) ? 1 : 0, "", 0);
    R.add("Flags", flags, "", 0);
    out.push_back(R);
}

// efergy_optical.c: Efergy optical pulse counter (433.55 MHz FSK PWM, 64 / 136 us, 500 us sync). 12 bytes: id (3), interval code, zeros,
// pulse count, sample period, CRC-16 XMODEM over 10 bytes; sent inverted by some units.
void decEfergyOptical(Bits& B, Out& out) {
    unsigned nb = B.n[0];
    if (nb < 96 || nb > 100) return;
    uint8_t* b = const_cast<uint8_t*>(B.row(0));
    while ((b[0] & 0xf0) != 0xf0 && (b[0] & 0xf0) != 0x00) {
        nb -= 1;
        if (nb < 96) return;
        for (unsigned i = 0; i < (nb + 7) / 8; ++i) b[i] = (uint8_t)(b[i] << 1 | (b[i + 1] & 0x80) >> 7);
    }
    if (b[0] & 0xf0) for (int i = 0; i < 12; i++) b[i] = (uint8_t)~b[i];
    if (!b[8] && !b[9] && !b[10] && !b[11]) return;
    if (ismCrc16(b, 10, 0x1021, 0x0000) != (uint16_t)(b[10] << 8 | b[11])) return;
    const unsigned id = (unsigned)b[0] << 16 | (unsigned)b[1] << 8 | b[2];
    const double sec = (((b[3] & 0x30) >> 4) + 1) * 30.0;
    IsmReading R = mk("Efergy-Optical", "Energy", decs(id), "CRC");
    R.add("Interval", sec, "s", 0);
    R.add("Pulse count", b[8], "", 0);
    static const int imp[] = {1000, 2000, 3200, 4000, 500};
    for (int k : imp) {
        char nm[40];
        snprintf(nm, sizeof nm, "Energy at %d imp/kWh", k);
        IsmField f; f.name = nm; f.unit = "kWh"; f.value = (double)b[8] / k * (3600 / sec); f.decimals = 3;
        R.fields.push_back(f);
    }
    out.push_back(R);
}

// esic_emt7110.c: ESIC EMT7110 plug power meter (868.28 MHz FSK, 9.579 kbit/s, aaaa 2dd4). II II II II FP PP CC CC VV UE EE XX: pairing and
// connected flags, power in 0.5 W, current in mA, voltage (v + 256) / 2, energy in 10 Wh, the 12 bytes sum to 0.
void decEmt7110(Bits& B, Out& out) {
    if (B.rows() != 1 || B.n[0] < 120 || B.n[0] > 140) return;
    static const uint8_t pre[3] = {0xAA, 0x2D, 0xD4};
    unsigned off = B.search(0, 0, pre, 24);
    off += 24;
    if (off > B.n[0]) return;
    uint8_t b[12];
    B.extract(0, off, b, 96);
    if (addBytes(b, 12) & 0xff) return;
    const unsigned id = (unsigned)b[0] << 24 | (unsigned)b[1] << 16 | (unsigned)b[2] << 8 | b[3];
    IsmReading R = mk("ESIC-EMT7110", "Energy", hexs(id, 8), "CHECKSUM");
    R.add("Power", (((b[4] & 0x3f) << 8) | b[5]) * 0.5, "W", 1);
    R.add("Current", ((b[6] << 8) | b[7]) * 0.001, "A", 3);
    R.add("Voltage", (b[8] + 256) * 0.5, "V", 1);
    R.add("Energy", (((b[9] & 0x3f) << 8) | b[10]) * 0.01, "kWh", 2);
    R.add("Pairing", (b[4] >> 7) & 1, "", 0);
    R.add("Connected", (b[4] >> 6) & 1, "", 0);
    out.push_back(R);
}

// scmplus.c: Itron ERT SCM+ (900 MHz OOK Manchester, 30 us). 16 bytes from the sync 16 A3: protocol id 0x1E, endpoint type, endpoint id (32),
// consumption (32), tamper (16), CRC-16 poly 0x1021 init 0x0971 over bytes 2..13.
void decScmPlus(Bits& B, Out& out) {
    if (B.n[0] < 128) return;
    static const uint8_t sync[3] = {0x16, 0xA3, 0x1E};
    const unsigned s = B.search(0, 0, sync, 24);
    if (s >= B.n[0] || B.n[0] - s < 128) return;
    uint8_t b[16];
    B.extract(0, s, b, 128);
    if (ismCrc16(&b[2], 12, 0x1021, 0x0971) != (uint16_t)(b[14] << 8 | b[15])) return;
    const unsigned id = (unsigned)b[4] << 24 | (unsigned)b[5] << 16 | (unsigned)b[6] << 8 | b[7];
    const unsigned cons = (unsigned)b[8] << 24 | (unsigned)b[9] << 16 | (unsigned)b[10] << 8 | b[11];
    const char* type;
    switch (b[3] & 0x0f) {
    case 4: case 5: case 7: case 8: type = "Electric"; break;
    case 0: case 1: case 2: case 9: case 12: type = "Gas"; break;
    case 3: case 11: case 13: type = "Water"; break;
    default: type = "unknown"; break;
    }
    char ids[16];
    snprintf(ids, sizeof ids, "%u", id);
    IsmReading R = mk("SCMplus", "Meter", ids, "CRC");
    R.addText("Meter type", type);
    R.add("Consumption", cons, "", 0);
    R.add("Endpoint type", b[3], "", 0);
    R.add("Tamper", (b[12] << 8) | b[13], "", 0);
    out.push_back(R);
}

} // namespace ism_detail
} // namespace dect2
