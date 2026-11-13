// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors: more thermometers and utility meters. Written from the protocol descriptions in rtl_433 (GPL-2.0-or-later) src/devices/:
// acurite.c (606TX, 609TXC), prologue.c, kedsum.c, gt_wt_02.c, bresser_3ch.c, fineoffset.c (WH2), thermopro_tp11.c, thermopro_tp12.c,
// ft004b.c, ert_scm.c, ert_idm.c. The code is our own; the formats, checks and constants are theirs.
#include "ism_internal.h"

namespace dect2 {
namespace ism_detail {

// acurite.c: Acurite 606TX / Technoline TX960 (PPM, 4 repeats of 32 bits, lfsr digest 0x98 / 0xf1)
void decAcurite606(Bits& B, Out& out) {
    const int r = B.findRepeatedRow(3, 32);
    if (r < 0 || B.n[r] > 33) return;
    const uint8_t* b = B.row(r);
    if (!b[0] && !b[1] && !b[2] && !b[3]) return;
    if (ismLfsrDigest8(b, 3, 0x98, 0xf1) != b[3]) return;
    const int traw = (int16_t)((b[1] << 12) | (b[2] << 4));
    IsmReading R = mk("Acurite-606TX", "Weather", decs(b[0]), "CHECKSUM");
    R.channel = ((b[1] & 0x30) >> 4) + 1;
    R.battery = (b[1] & 0x80) ? 1 : 0;
    R.add("Temperature", (traw >> 4) * 0.1, "C");
    out.push_back(R);
}

// acurite.c: Acurite 609TXC (40 bits: id, status + temperature, temperature, humidity, sum)
void decAcurite609(Bits& B, Out& out) {
    for (int row = 0; row < B.rows(); row++) {
        if (B.n[row] != 40) continue;
        const uint8_t* b = B.row(row);
        const int sum = b[0] + b[1] + b[2] + b[3];
        if (sum == 0 || (sum & 0xff) != b[4]) continue;
        const int traw = (int16_t)(((b[1] & 0x0f) << 12) | (b[2] << 4));
        if (b[3] > 100) continue;
        IsmReading R = mk("Acurite-609TXC", "Weather", decs(b[0]), "CHECKSUM");
        R.battery = (b[1] & 0x80) ? 0 : 1;
        R.add("Temperature", (traw >> 4) * 0.1, "C");
        R.add("Humidity", b[3], "%", 0);
        out.push_back(R);
        return;
    }
}

// prologue.c: Prologue, FreeTec NC-7104 (36 bits PPM, a row repeated 4 times, no check; type nibble 9 or 5)
void decPrologue(Bits& B, Out& out) {
    if (B.rows() < 1 || (B.n[0] != 0 && B.n[0] <= 8)) return;
    const int r = B.findRepeatedRow(4, 36);
    if (r < 0 || B.n[r] > 37) return;
    const uint8_t* b = B.row(r);
    const int type = b[0] >> 4;
    if (type != 9 && type != 5) return;
    const int hum = ((b[3] & 0x0f) << 4) | (b[4] >> 4);
    const int traw = (int16_t)((b[2] << 8) | (b[3] & 0xf0)) >> 4;
    IsmReading R = mk("Prologue-TH", "Weather", decs(((b[0] & 0x0f) << 4) | (b[1] >> 4)), "REPEAT");
    R.channel = (b[1] & 3) + 1;
    R.add("Temperature", traw * 0.1, "C");
    if (hum != 0xCC) R.add("Humidity", hum, "%", 0);
    out.push_back(R);
}

static int crc4(const uint8_t* m, unsigned n, uint8_t poly, uint8_t init) {
    unsigned rem = (unsigned)init << 4;
    const unsigned p = (unsigned)poly << 4;
    for (unsigned i = 0; i < n; i++) {
        rem ^= m[i];
        for (int k = 0; k < 8; k++) rem = (rem & 0x80) ? ((rem << 1) ^ p) & 0xff : (rem << 1) & 0xff;
    }
    return (int)((rem >> 4) & 0x0f);
}

// kedsum.c: Kedsum-TH, Pearl NC-7415 (at least 5 empty sync rows, then 42-bit rows repeated 4 times, CRC-4; temperature in Fahrenheit)
void decKedsum(Bits& B, Out& out) {
    if (B.rows() < 5) return;
    for (int i = 0; i < 5; i++) if (B.n[i] != 0) return;
    const int r = B.findRepeatedRow(4, 42);
    if (r < 0 || B.n[r] != 42) return;
    uint8_t b[5];
    B.extract(r, 2, b, 40);
    if ((crc4(b, 4, 0x3, 0) ^ (b[4] >> 4)) != (b[4] & 0xf)) return;
    const int traw = ((b[2] & 0x0f) << 8) | (b[2] & 0xf0) | (b[1] & 0x0f);
    const int hum = ((b[3] & 0x0f) << 4) | ((b[3] & 0xf0) >> 4);
    IsmReading R = mk("Kedsum-TH", "Weather", decs(b[0]), "CRC");
    R.channel = ((b[1] & 0x30) >> 4) + 1;
    R.battery = (b[1] >> 6) == 2 ? 1 : 0;
    R.add("Temperature", ((traw - 900) * 0.1 - 32.0) / 1.8, "C");
    R.add("Humidity", hum, "%", 0);
    out.push_back(R);
}

// gt_wt_02.c: Globaltronics GT-WT-02 (37 bits, or 39 with two extra leading bits; nibble sum modulo 64)
void decGtWt02(Bits& B, Out& out) {
    if (B.rows() < 2) return;
    for (int row = 0; row < B.rows(); row++) {
        uint8_t b[5] = {0};
        if (B.n[row] == 39) B.extract(row, 2, b, 37);
        else if (B.n[row] == 37) B.extract(row, 0, b, 37);
        else continue;
        if (!(b[0] || b[1] || b[2] || b[3] || b[4])) continue;
        const int sum = (b[0] >> 4) + (b[0] & 0xF) + (b[1] >> 4) + (b[1] & 0xF) + (b[2] >> 4) + (b[2] & 0xF) + (b[3] >> 4) + (b[3] & 0xe);
        const int chk = ((b[3] & 1) << 5) + (b[4] >> 3);
        if ((sum & 0x3F) != chk) continue;
        const int channel = (b[1] >> 4) & 3;
        const double tc = ((int16_t)(((b[1] & 0x0f) << 12) | (b[2] << 4)) >> 4) * 0.1;
        if (channel > 2 || tc < -20 || tc > 60) continue;
        int hum = b[3] >> 1;
        if (hum != 10 && hum != 110 && (hum < 20 || hum > 90)) continue;
        if (hum == 10) hum = 0; else if (hum == 110) hum = 100;
        IsmReading R = mk("GT-WT02", "Weather", decs(b[0]), "CHECKSUM");
        R.channel = channel + 1;
        R.battery = ((b[1] >> 7) & 1) ? 0 : 1;
        R.add("Temperature", tc, "C");
        R.add("Humidity", hum, "%", 0);
        out.push_back(R);
        return;
    }
}

// bresser_3ch.c: Bresser Thermo-/Hygro-Sensor 3CH (40 bits, inverted, sum check, temperature in Fahrenheit)
void decBresser3ch(Bits& B, Out& out) {
    const int r = B.findRepeatedRow(3, 40);
    if (r < 0 || B.n[r] > 42) return;
    uint8_t b[5];
    for (int i = 0; i < 5; i++) b[i] = (uint8_t)~B.row(r)[i];
    if (((b[0] + b[1] + b[2] + b[3] - b[4]) & 0xFF) != 0) return;
    const int channel = (b[1] & 0x30) >> 4, hum = b[3];
    const int traw = ((b[1] & 0x0F) << 8) + b[2];
    const double tf = (traw - 900) * 0.1;
    if (channel == 0 || hum > 100 || tf < -20.0 || tf > 160.0) return;
    IsmReading R = mk("Bresser-3CH", "Weather", decs(b[0]), "CHECKSUM");
    R.channel = channel;
    R.battery = (b[1] & 0x80) ? 0 : 1;
    R.add("Temperature", (tf - 32.0) / 1.8, "C");
    R.add("Humidity", hum, "%", 0);
    out.push_back(R);
}

// fineoffset.c: WH2 (48 bits, 0xFF preamble), WH2A (55, 0xFE), WH5 (47, 0xFE), Telldus Proove (49): CRC-8 poly 0x31
void decWh2(Bits& B, Out& out) {
    if (B.rows() < 1) return;
    const unsigned nb = B.n[0];
    const uint8_t b0 = B.byteAt(0, 0);
    uint8_t b[6] = {0};
    enum { WH2, WH2A, WH5, TP, TFA } m;
    if (nb == 48 && b0 == 0xFF) { B.extract(0, 8, b, 40); m = WH2; }
    else if (nb == 55 && b0 == 0xFE) { B.extract(0, 7, b, 48); m = b[3] == 0xff ? TFA : WH2A; }
    else if (nb == 47 && b0 == 0xFE) { B.extract(0, 7, b, 40); m = WH5; }
    else if (nb == 49 && b0 == 0xFF && (B.byteAt(0, 1) & 0x80)) { B.extract(0, 9, b, 40); m = TP; }
    else return;
    if (b[4] != ismCrc8(b, 4, 0x31, 0)) return;
    if (m == TFA && (addBytes(b, 5) & 0xff) != b[5]) return;
    if ((b[0] >> 4) != 4) return;
    const int id = ((b[0] & 0x0F) << 4) | ((b[1] & 0xF0) >> 4);
    int temp = ((b[1] & 0x0F) << 8) | b[2];
    int low = 0;
    if (m == TFA) { low = (temp & 0x800) != 0; temp &= 0x7FF; temp -= 400; }
    else if (m == WH5) temp -= 400;
    else if (temp & 0x800) { temp &= 0x7FF; temp = -temp; }
    const double t = temp * 0.1;
    if (m == WH5 && (t < -40.0 || t > 60.0)) return;
    const char* name = m == WH2 ? "Fineoffset-WH2" : m == WH2A ? "Fineoffset-WH2A" : m == WH5 ? "Fineoffset-WH5" : m == TP ? "Fineoffset-TelldusProove" : "TFA-303225";
    IsmReading R = mk(name, "Weather", decs(id), "CRC");
    if (m == TFA) R.battery = !low;
    R.add("Temperature", t, "C");
    if (b[3] != 0xff) R.add("Humidity", b[3], "%", 0);
    out.push_back(R);
}

// thermopro_tp11.c: ThermoPro TP11 (32 / 33 bits PPM, lfsr digest reflect 0x51 / 0x04)
void decTp11(Bits& B, Out& out) {
    const int r = B.findRepeatedRow(2, 32);
    if (r < 0 || B.n[r] > 33) return;
    const uint8_t* b = B.row(r);
    if (ismLfsrDigest8Reflect(b, 3, 0x51, 0x04) != b[3]) return;
    if ((!b[0] && !b[1] && !b[2] && !b[3]) || (b[0] == 0xff && b[1] == 0xff && b[2] == 0xff && b[3] == 0xff)) return;
    const int dev = (b[0] << 4) | (b[1] >> 4), traw = ((b[1] & 0x0f) << 8) | b[2];
    IsmReading R = mk("Thermopro-TP11", "Weather", decs(dev), "CRC");
    R.add("Temperature", (traw - 200) * 0.1, "C");
    out.push_back(R);
}

// thermopro_tp12.c: ThermoPro TP08 / TP12 / TP20 (41 bits PPM, two probes, lfsr digest reflect 0x51 / 0x04)
void decTp12(Bits& B, Out& out) {
    const int need = B.rows() > 5 ? 5 : 2;
    int r = -1;
    for (int i = 0; i < B.rows() && r < 0; i++) {
        if (B.n[i] != 41) continue;
        int c = 0;
        for (int j = 0; j < B.rows(); j++) {
            if (B.n[j] < 40) continue;
            bool same = true;
            for (unsigned k = 0; k < 40 && same; k++) same = B.bit(i, k) == B.bit(j, k);
            c += same;
        }
        if (c >= need) r = i;
    }
    if (r < 0) return;
    const uint8_t* b = B.row(r);
    if (!(b[0] | b[1] | b[2] | b[3])) return;
    if (ismLfsrDigest8Reflect(b, 4, 0x51, 0x04) != b[4]) return;
    const int raw1 = ((b[2] & 0xf0) << 4) | b[1], raw2 = ((b[2] & 0x0f) << 8) | b[3];
    IsmReading R = mk("Thermopro-TP12", "Weather", decs(b[0]), "CRC");
    R.add("Temperature 1 (food)", (raw1 - 200) * 0.1, "C");
    R.add("Temperature 2 (barbecue)", (raw2 - 200) * 0.1, "C");
    out.push_back(R);
}

// ft004b.c: FT-004-B temperature sensor (a 46-bit frame sent three times without pauses, 137 / 138 bits; majority vote)
void decFt004b(Bits& B, Out& out) {
    if (B.rows() < 1 || (B.n[0] != 137 && B.n[0] != 138)) return;
    uint8_t m[6] = {0};
    for (int i = 0; i < 46; i++) {
        const int a = B.bit(0, i), b = B.bit(0, i + 46), c = B.bit(0, i + 92);
        if ((a & b) | (b & c) | (a & c)) m[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
    }
    for (int i = 0; i < 6; i++) {   // reverse8
        const uint8_t v = m[i];
        uint8_t r = 0;
        for (int k = 0; k < 8; k++) r |= (uint8_t)(((v >> k) & 1) << (7 - k));
        m[i] = r;
    }
    if (m[0] != 0xf4) return;
    const int traw = ((m[4] & 0x7) << 8) | m[3];
    IsmReading R = mk("FT-004B", "Weather", "0", "REPEAT");
    R.add("Temperature", traw * 0.05 - 40.0, "C");
    out.push_back(R);
}

// ert_scm.c: Itron ERT Standard Consumption Message (96 bits Manchester, CRC-16 poly 0x6F63 over bytes 2..11)
void decErtScm(Bits& B, Out& out) {
    if (B.rows() < 1 || B.n[0] != 96) return;
    const uint8_t* b = B.row(0);
    if (!b[0] && !b[1] && !b[2] && !b[3]) return;
    if (ismCrc16(&b[2], 10, 0x6F63, 0)) return;
    const unsigned id = ((unsigned)(b[2] & 0x06) << 23) | (unsigned)(b[7] << 16) | (unsigned)(b[8] << 8) | b[9];
    IsmReading R = mk("ERT-SCM", "Meter", decs(id), "CRC");
    R.add("Physical tamper", (b[3] & 0xC0) >> 6, "", 0);
    R.add("ERT type", (b[3] >> 2) & 0x0F, "", 0);
    R.add("Encoder tamper", b[3] & 0x03, "", 0);
    R.add("Consumption", (b[4] << 16) | (b[5] << 8) | b[6], "", 0);
    out.push_back(R);
}

// ert_idm.c: Itron ERT Interval Data Message (720 bits after the sync word 0x16A31C, CRC-16 poly 0x1021 init 0xD895)
void decErtIdm(Bits& B, Out& out) {
    if (B.rows() < 1 || B.n[0] < 720) return;
    static const uint8_t sync[] = {0x16, 0xA3, 0x1C};
    const unsigned s = B.search(0, 0, sync, 24);
    if (s >= B.n[0] || B.n[0] - s < 720) return;
    uint8_t b[92];
    B.extract(0, s, b, 720);
    if (ismCrc16(&b[2], 86, 0x1021, 0xD895) != (uint16_t)(b[88] << 8 | b[89])) return;
    const unsigned serial = ((unsigned)b[7] << 24) | (unsigned)(b[8] << 16) | (unsigned)(b[9] << 8) | b[10];
    IsmReading R = mk("ERT-IDM", "Meter", decs(serial), "CRC");
    R.add("ERT type", b[6], "", 0);
    R.add("Packet type", b[2], "", 0);
    R.add("Application version", b[5], "", 0);
    R.add("Interval count", b[11], "", 0);
    R.add("Last consumption count", ((unsigned)b[27] << 24) | (unsigned)(b[28] << 16) | (unsigned)(b[29] << 8) | b[30], "", 0);
    R.add("Transmit time offset", (b[84] << 8) | b[85], "", 0);
    out.push_back(R);
}

// solight_te44.c: Solight TE44 / TE66, EMOS E0107T (the Rubicson frame, 37 bits at 972 / 1932 us)
void decSolightTe44(Bits& B, Out& out) {
    const int r = B.findRepeatedRow(3, 36);
    if (r < 0 || B.n[r] != 37) return;
    const uint8_t* b = B.row(r);
    if ((b[3] & 0xf0) != 0xf0) return;
    const uint8_t t[5] = {b[0], b[1], b[2], (uint8_t)(b[3] & 0xf0), (uint8_t)((b[3] & 0x0f) << 4 | (b[4] & 0xf0) >> 4)};
    if (ismCrc8(t, 5, 0x31, 0x6c)) return;
    const int traw = (int16_t)((b[1] << 12) | (b[2] << 4));
    IsmReading R = mk("Solight-TE44", "Weather", decs(b[0]), "CRC");
    R.channel = ((b[1] & 0x30) >> 4) + 1;
    R.add("Temperature", (traw >> 4) * 0.1, "C", 2);
    out.push_back(R);
}

// kerui.c: Kerui PIR / contact sensors (25-bit rows, at least 9 identical ones; bits inverted; 20-bit id, 4-bit command)
void decKerui(Bits& B, Out& out) {
    const int r = B.findRepeatedRow(9, 25);
    if (r < 0 || B.n[r] != 25) return;
    const uint8_t* p = B.row(r);
    if (!p[0] && !p[1] && !p[2]) return;
    const uint8_t b0 = (uint8_t)~p[0], b1 = (uint8_t)~p[1], b2 = (uint8_t)~p[2];
    const unsigned id = ((unsigned)b0 << 12) | (unsigned)(b1 << 4) | (b2 >> 4);
    const int cmd = b2 & 0x0F;
    const char* st = cmd == 0xa ? "motion" : cmd == 0xe ? "open" : cmd == 0x7 ? "close" : cmd == 0xb ? "tamper" : cmd == 0x5 ? "water" : cmd == 0xf ? "battery low" : nullptr;
    if (!st) return;
    IsmReading R = mk("Kerui-Security", "Sensor", hexs(id, 5), "REPEAT");
    R.addText("State", st);
    if (cmd == 0xf) R.battery = 0;
    out.push_back(R);
}

// springfield.c: Springfield PreciseTemp temperature and soil moisture (36 / 37 bits PPM, nibble xor check)
void decSpringfield(Bits& B, Out& out) {
    const int r = B.findRepeatedRow(3, 36);
    if (r < 0 || (B.n[r] != 36 && B.n[r] != 37)) return;
    const uint8_t* b = B.row(r);
    const unsigned all = ((unsigned)b[0] << 24) | (unsigned)(b[1] << 16) | (unsigned)(b[2] << 8) | b[3];
    if (all == 0xffffffffu || all == 0) return;
    int chk = xorBytes(b, 4);
    chk = (chk >> 4) ^ (chk & 0x0f);
    if (chk) return;
    const int traw = (int16_t)(((b[1] & 0x0f) << 12) | (b[2] << 4));
    const double tc = (traw >> 4) * 0.1;
    const int moist = (b[3] >> 4) * 10;
    if (tc < -30 || tc > 70 || moist > 100) return;
    IsmReading R = mk("Springfield-Soil", "Weather", decs(b[0]), "CHECKSUM");
    R.channel = ((b[1] >> 4) & 3) + 1;
    R.battery = (b[1] & 0x80) ? 0 : 1;
    R.add("Temperature", tc, "C");
    R.add("Moisture", moist, "%", 0);
    out.push_back(R);
}

// honeywell.c: Honeywell / Ademco 345 MHz door and window sensors, 2Gig DW10 / DW11 (Manchester at 136 us, preamble 0xFFFE, CRC-16)
void decHoneywell(Bits& B, Out& out) {
    if (B.rows() < 1 || B.n[0] < 120) return;
    static const uint8_t pre[3] = {0x55, 0x55, 0x56};
    unsigned pos = 0;
    const unsigned len = B.n[0];
    while ((pos = B.search(0, pos, pre, 24)) + 24 < len) {
        Bits d;
        manchester(B, 0, pos + 24, d, 96);
        pos += 1;
        if (d.n[0] < 48) continue;
        uint8_t b[10] = {0};
        d.extract(0, 0, b, std::min<unsigned>(d.n[0], 80));
        const int channel = b[0] >> 4;
        const int id = ((b[0] & 0xf) << 16) | (b[1] << 8) | b[2];
        const int crc = (b[4] << 8) | b[5];
        if (id == 0 && crc == 0) continue;
        const uint16_t poly = (channel == 0x2 || channel == 0x4 || channel == 0x9 || channel == 0xA || channel == 0xC) ? 0x8050 : 0x8005;
        if (crc != ismCrc16(b, 4, poly, 0)) continue;
        const int ev = b[3];
        IsmReading R = mk("Honeywell-Security", "Sensor", hexs((unsigned)id, 5), "CRC");
        R.channel = channel;
        R.battery = (ev & 0x08) ? 0 : 1;
        R.addText("State", (ev & 0x80) ? "open" : "closed");
        R.add("Event", ev, "", 0);
        R.add("Tamper", (ev & 0x40) >> 6, "", 0);
        R.add("Alarm", (ev & 0x10) >> 4, "", 0);
        R.add("Heartbeat", (ev & 0x04) >> 2, "", 0);
        out.push_back(R);
        return;
    }
}

} // namespace ism_detail
} // namespace dect2
