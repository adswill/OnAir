// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors: device decoders for TPMS (tyre pressure) sensors, the EV1527 remote and the Bresser/LaCrosse/Fine Offset FSK stations. Written from rtl_433 src/devices/.
#include "ism_internal.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace dect2 {

namespace ism_detail {

void decSchrader(Bits& B, Out& out) {
    if (B.n[0] != 68) return;
    uint8_t b[8];
    B.extract(0, 4, b, 64);
    if (b[7] != ismCrc8(b, 7, 0x07, 0xf0)) return;
    IsmReading R = mk("Schrader", "TPMS", hexU((b[1] & 0x0F) << 24 | b[2] << 16 | b[3] << 8 | b[4], 7), "CRC");
    R.add("Pressure", b[5] * 25 * 0.1, "kPa");
    R.add("Temperature", b[6] - 50, "C", 0);
    R.addText("Flags", hexs((b[0] & 0x0F) << 4 | b[1] >> 4, 2));
    out.push_back(R);
}

// schraeder.c: Schrader EG53MA4 (Opel, Saab, Chevrolet; 120 bits Manchester, add checksum)
void decSchraderEg53(Bits& B, Out& out) {
    if (B.n[0] != 120) return;
    uint8_t b[10];
    B.extract(0, 40, b, 80);
    if (!b[1] && !b[2] && !b[4] && !b[5] && !b[7] && !b[8]) return;
    if ((addBytes(b, 9) & 0xff) != b[9]) return;
    IsmReading R = mk("Schrader-EG53MA4", "TPMS", hexU((b[4] << 16) | (b[5] << 8) | b[6], 6), "CHECKSUM");
    R.add("Pressure", b[7] * 25 * 0.1, "kPa");
    R.add("Temperature", b[8], "F", 0);
    out.push_back(R);
}

// generic_remote.c: PT2260 / PT2262 / SC2260 / EV1527 remotes and sensors (25 bits PWM; no check, so the code must come twice)
void decGenericRemote(Bits& B, Out& out, std::vector<uint32_t>& seen) {
    if (B.n[0] != 25) return;
    uint8_t b[4] = {(uint8_t)~B.row(0)[0], (uint8_t)~B.row(0)[1], (uint8_t)~B.row(0)[2], B.row(0)[3]};
    if ((b[3] & 0x80) == 0 || (b[0] == 0 && b[1] == 0) || b[2] == 0) return;
    const uint32_t full = (uint32_t)b[0] << 16 | b[1] << 8 | b[2];
    if (std::find(seen.begin(), seen.end(), full) == seen.end()) { seen.push_back(full); return; }   // the first frame of a burst
    for (const auto& r : out) if (r.model == "Generic-Remote" && r.id == decs(b[0] << 8 | b[1])) return;
    char tri[13];
    for (int i = 22, k = 0; i >= 0; i -= 2, k++) tri[k] = "0ZX1"[(full >> i) & 3];
    tri[12] = 0;
    IsmReading R = mk("Generic-Remote", "Remote", decs(b[0] << 8 | b[1]), "REPEAT");
    R.addText("Code", hexs(full, 6));
    R.add("Command", b[2], "", 0);
    R.addText("Tri-state", tri);
    out.push_back(R);
}

// bresser_5in1.c: Bresser Weather Center 5-in-1 (FSK, 26 bytes: the first 13 inverted copies of the last 13)
int bcd(int x) { return (x >> 4) * 10 + (x & 0xf); }
void decBresser5in1(Bits& B, Out& out) {
    static const uint8_t pre[5] = {0xaa, 0xaa, 0xaa, 0x2d, 0xd4};
    if (B.rows() != 1 || B.n[0] < 248 || B.n[0] > 440) return;
    unsigned sp = B.search(0, 0, pre, 40);
    if (sp == B.n[0]) return;
    sp += 40;
    unsigned len = B.n[0] - sp;
    uint8_t msg[26];
    if ((len + 7) / 8 < sizeof msg) return;
    len = std::min<unsigned>(len, sizeof msg * 8);
    B.extract(0, sp, msg, len);
    for (int c = 0; c < 13; c++) if ((msg[c] ^ msg[c + 13]) != 0xff) return;
    // uu: the number of set bits in bytes 14 to 25, as the file documents (its examples agree; rtl_433 does not test it). Without it a
    // message shifted by a bit after the sync word still passes the inverted copy about half the time.
    int ones = 0;
    for (int i = 14; i < 26; i++) for (int k = 0; k < 8; k++) ones += (msg[i] >> k) & 1;
    if (ones != msg[13]) return;
    const bool tok = (msg[20] & 0x0f) <= 9, hok = (msg[22] & 0x0f) <= 9;
    int traw = (msg[20] & 0x0f) + ((msg[20] & 0xf0) >> 4) * 10 + (msg[21] & 0x0f) * 100;
    if (msg[25] & 0x0f) traw = -traw;
    const int hum = (msg[22] & 0x0f) + ((msg[22] & 0xf0) >> 4) * 10;
    const int stype = msg[15] & 0x7f;
    double rain = ((msg[23] & 0x0f) + ((msg[23] & 0xf0) >> 4) * 10 + (msg[24] & 0x0f) * 100 + ((msg[24] & 0xf0) >> 4) * 1000) * 0.1;
    const bool gauge = stype >= 0x39 && stype <= 0x3b;
    IsmReading R = mk(gauge ? "Bresser-ProRainGauge" : "Bresser-5in1", "Weather", decs(msg[14]), "CHECKSUM");
    R.battery = !(msg[25] & 0x80);
    if (tok) R.add("Temperature", traw * 0.1, "C");
    if (gauge) { R.add("Rain", rain * 2.5, "mm"); out.push_back(R); return; }
    if (hok) R.add("Humidity", hum, "%", 0);
    R.add("Wind gust", (((msg[17] & 0x0f) << 8) + msg[16]) * 0.1, "m/s");
    R.add("Wind speed", ((msg[18] & 0x0f) + ((msg[18] & 0xf0) >> 4) * 10 + (msg[19] & 0x0f) * 100) * 0.1, "m/s");
    R.add("Wind direction", ((msg[17] & 0xf0) >> 4) * 22.5, "deg");
    R.add("Rain", rain, "mm");
    out.push_back(R);
}

// bresser_6in1.c: Bresser 6-in-1, 7-in-1 indoor, new 5-in-1, 3-in-1 wind, soil, thermo-hygro, pool (FSK, LFSR-16 digest and add checksum)
void decBresser6in1(Bits& B, Out& out) {
    static const uint8_t pre[4] = {0xaa, 0xaa, 0x2d, 0xd4};
    static const int moist[16] = {0, 7, 13, 20, 27, 33, 40, 47, 53, 60, 67, 73, 80, 87, 93, 99};
    if (B.rows() != 1 || B.n[0] < 160 || B.n[0] > 440) return;
    const unsigned sp = B.search(0, 0, pre, 32) + 32;
    if (sp >= B.n[0] || B.n[0] - sp < 18 * 8) return;
    uint8_t msg[18];
    B.extract(0, sp, msg, 144);
    if (((msg[0] << 8) | msg[1]) != ismLfsrDigest16(&msg[2], 15, 0x8810, 0x5412)) return;
    if ((addBytes(&msg[2], 16) & 0xff) != 0xff) return;
    const uint32_t id = ((uint32_t)msg[2] << 24) | (msg[3] << 16) | (msg[4] << 8) | msg[5];
    const int stype = msg[6] >> 4, chan = msg[6] & 7, bat = (msg[13] >> 1) & 1;
    const bool tok = msg[12] <= 0x99 && (msg[13] & 0xf0) <= 0x90;
    const int traw = (msg[12] >> 4) * 100 + (msg[12] & 0x0f) * 10 + (msg[13] >> 4);
    double tc = ((msg[13] >> 3) & 1) ? (traw - 1000) * 0.1 : traw * 0.1;
    if (tc < -50.0) tc = -traw * 0.1;
    const int hum = (msg[14] >> 4) * 10 + (msg[14] & 0x0f);
    bool uvok = (msg[16] & 0x0f) == 0 && (~msg[15] & 0xff) <= 0x99 && (~msg[16] & 0xf0) <= 0x90;
    const int uvraw = ((~msg[15] & 0xf0) >> 4) * 100 + (~msg[15] & 0x0f) * 10 + ((~msg[16] & 0xf0) >> 4);
    uint8_t w7 = (uint8_t)~msg[7], w8 = (uint8_t)~msg[8], w9 = (uint8_t)~msg[9];
    bool wok = w7 <= 0x99 && w8 <= 0x99 && w9 <= 0x99;
    const int gust = (w7 >> 4) * 100 + (w7 & 0x0f) * 10 + (w8 >> 4), wavg = (w9 >> 4) * 100 + (w9 & 0x0f) * 10 + (w8 & 0x0f);
    const int wdir = ((msg[10] & 0xf0) >> 4) * 100 + (msg[10] & 0x0f) * 10 + ((msg[11] & 0xf0) >> 4);
    const uint8_t r12 = (uint8_t)~msg[12], r13 = (uint8_t)~msg[13], r14 = (uint8_t)~msg[14];
    const bool rok = msg[16] & 1;
    const int rraw = (r12 >> 4) * 100000 + (r12 & 0x0f) * 10000 + (r13 >> 4) * 1000 + (r13 & 0x0f) * 100 + (r14 >> 4) * 10 + (r14 & 0x0f);
    if (stype == 2 || stype == 4) { wok = false; uvok = false; }
    int mo = -1;
    if (stype == 4 && tok && hum >= 1 && hum <= 16) mo = moist[hum - 1];
    IsmReading R = mk("Bresser-6in1", "Weather", hexs(id, 8), "CRC");
    R.channel = chan;
    if (!rok) R.battery = bat;
    if (tok) R.add("Temperature", tc, "C");
    if (tok && mo < 0) R.add("Humidity", hum, "%", 0);
    if (mo >= 0) R.add("Moisture", mo, "%", 0);
    if (wok) { R.add("Wind gust", gust * 0.1, "m/s"); R.add("Wind speed", wavg * 0.1, "m/s"); R.add("Wind direction", wdir, "deg", 0); }
    if (rok) R.add("Rain", rraw * 0.1, "mm");
    if (uvok) R.add("UV index", uvraw * 0.1, "");
    R.add("Sensor type", stype, "", 0);
    out.push_back(R);
}

// bresser_7in1.c: Bresser 7-in-1 / 3-in-1 / 8-in-1 weather, PM2.5/PM10, CO2, HCHO/VOC (FSK, whitened 0xaa, LFSR-16 digest)
void decBresser7in1(Bits& B, Out& out) {
    static const uint8_t pre[5] = {0xaa, 0xaa, 0xaa, 0x2d, 0xd4};
    if (B.rows() != 1 || B.n[0] < 160) return;
    const unsigned sp = B.search(0, 0, pre, 40) + 40;
    if (sp >= B.n[0] || sp + 21 * 8 >= B.n[0]) return;
    uint8_t msg[25];
    B.extract(0, sp, msg, 200);
    if (msg[21] == 0x00) return;
    const int stype = msg[6] >> 4, nstartup = (msg[6] & 0x08) >> 3, chan = msg[6] & 0x07;
    (void)nstartup;
    for (auto& v : msg) v ^= 0xaa;
    if ((((msg[0] << 8) | msg[1]) ^ ismLfsrDigest16(&msg[2], 23, 0x8810, 0xba95)) != 0x6df1) return;
    const int id = (msg[2] << 8) | msg[3];
    const int batLow = (msg[15] & 0x06) == 0x06;
    if (stype == 1 || stype == 12 || stype == 13) {
        const int wdir = (msg[4] >> 4) * 100 + (msg[4] & 0x0f) * 10 + (msg[5] >> 4);
        const int wg = (msg[7] >> 4) * 100 + (msg[7] & 0x0f) * 10 + (msg[8] >> 4), wa = (msg[8] & 0x0f) * 100 + (msg[9] >> 4) * 10 + (msg[9] & 0x0f);
        const int rr = (msg[10] >> 4) * 100000 + (msg[10] & 0x0f) * 10000 + (msg[11] >> 4) * 1000 + (msg[11] & 0x0f) * 100 + (msg[12] >> 4) * 10 + (msg[12] & 0x0f);
        const int traw = (msg[14] >> 4) * 100 + (msg[14] & 0x0f) * 10 + (msg[15] >> 4);
        const double tc = traw > 600 ? (traw - 1000) * 0.1 : traw * 0.1;
        const int hum = (msg[16] >> 4) * 10 + (msg[16] & 0x0f);
        const int lux = (msg[17] >> 4) * 100000 + (msg[17] & 0x0f) * 10000 + (msg[18] >> 4) * 1000 + (msg[18] & 0x0f) * 100 + (msg[19] >> 4) * 10 + (msg[19] & 0x0f);
        const int uv = (msg[20] >> 4) * 100 + (msg[20] & 0x0f) * 10 + (msg[21] >> 4);
        IsmReading R = mk("Bresser-7in1", "Weather", decs(id), "CRC");
        R.battery = !batLow;
        R.add("Temperature", tc, "C");
        R.add("Humidity", hum, "%", 0);
        if (stype != 12) { R.add("Wind gust", wg * 0.1, "m/s"); R.add("Wind speed", wa * 0.1, "m/s"); R.add("Wind direction", wdir, "deg", 0); }
        R.add("Rain", rr * 0.1, "mm");
        if (stype != 12) { R.add("Light", lux, "lux", 0); R.add("UV index", uv * 0.1, ""); }
        if (stype == 13 && (msg[23] >> 4) < 10) R.add("Globe temperature", (msg[22] >> 4) * 10 + (msg[22] & 0x0f) + (msg[23] >> 4) * 0.1, "C");
        out.push_back(R);
    } else if (stype == 8) {
        IsmReading R = mk("Bresser-7in1", "Sensor", decs(id), "CRC");
        R.channel = chan; R.battery = !batLow;
        if ((msg[10] & 0x0f) != 0x0f) R.add("PM2.5", (msg[10] & 0x0f) * 1000 + (msg[11] >> 4) * 100 + (msg[11] & 0x0f) * 10 + (msg[12] >> 4), "ug/m3", 0);
        if ((msg[12] & 0x0f) != 0x0f) R.add("PM10", (msg[12] & 0x0f) * 1000 + (msg[13] >> 4) * 100 + (msg[13] & 0x0f) * 10 + (msg[14] >> 4), "ug/m3", 0);
        out.push_back(R);
    } else if (stype == 10 || stype == 11) {
        IsmReading R = mk(stype == 10 ? "Bresser-CO2" : "Bresser-HCHOVOC", "Sensor", decs(id), "CRC");
        R.channel = chan; R.battery = !batLow;
        const int v = ((msg[4] & 0xf0) >> 4) * 1000 + (msg[4] & 0x0f) * 100 + ((msg[5] & 0xf0) >> 4) * 10 + (msg[5] & 0x0f);
        if ((msg[5] & 0x0f) != 0x0f) R.add(stype == 10 ? "CO2" : "Formaldehyde", v, stype == 10 ? "ppm" : "ppb", 0);
        if (stype == 11 && (msg[22] & 0x0f) != 0x0f) R.add("VOC level", msg[22] & 0x0f, "", 0);
        out.push_back(R);
    }
}

// lacrosse_tx35.c: LaCrosse TX29-IT / TX35DTH-IT, TFA 30.3159.IT / 30.3155 (FSK, CRC-8 poly 0x31)
void decTx29(Bits& B, Out& out, bool tx35) {
    static const uint8_t pre[3] = {0xa2, 0xdd, 0x49};
    // the two share the frame and differ in the bit width (55 / 105 us); the slicer's measured width tells them apart, so one burst is
    // not reported as both
    if (std::fabs(tlPcmBitUs - (tx35 ? 105.f : 55.f)) > (tx35 ? 105.f : 55.f) * 0.2f) return;
    for (int r = 0; r < B.rows(); r++) {
        const unsigned sp = B.search(r, 0, pre, 24);
        if (sp >= B.n[r]) continue;
        uint8_t b[5];
        B.extract(r, sp + 20, b, 40);
        if (b[4] != ismCrc8(b, 4, 0x31, 0x00)) continue;
        int id = ((b[0] & 0x0f) << 2) | (b[1] >> 6);
        const double tc = 10 * (b[1] & 0x0f) + 1 * ((b[2] >> 4) & 0x0f) + 0.1 * (b[2] & 0x0f) - 40.0;
        const int hum = b[3] & 0x7f;
        if (hum == 0x7d) id += 0x40;
        IsmReading R = mk(tx35 ? "LaCrosse-TX35DTHIT" : "LaCrosse-TX29IT", "Weather", decs(id), "CRC");
        R.battery = !(b[3] >> 7);
        R.add("Temperature", tc, "C");
        if (hum != 0x6a && hum != 0x7d) R.add("Humidity", hum, "%", 0);
        if ((b[1] >> 5) & 1) R.add("New battery", 1, "", 0);
        out.push_back(R);
    }
}

// fineoffset.c (fineoffset_WH24_callback): Fine Offset / Ecowitt WH24, WH65, WS69, HP1000, Misol WS2320 (FSK, CRC-8 and add checksum)
void decWh24(Bits& B, Out& out) {
    static const uint8_t pre[3] = {0xAA, 0x2D, 0xD4};
    const unsigned nb = B.n[0];
    if (nb < 190 || nb > 268) return;
    const unsigned off = B.search(0, 0, pre, 24) + 24;
    if (off + 17 * 8 > nb) return;
    int type = (nb - off - 17 * 8 < 8) ? (off < 61 ? 24 : 65) : 65;
    if (nb > 215) type = 69;
    uint8_t b[25];
    B.extract(0, off, b, 200);
    if (b[0] != 0x24) return;
    if (ismCrc8(b, 16, 0x31, 0x00) != 0 || (uint8_t)addBytes(b, 16) != b[16]) return;
    const int wdir = b[2] | (b[3] & 0x80) << 1, traw = (b[3] & 0x07) << 8 | b[4], hum = b[5];
    const int wsraw = b[6] | (b[3] & 0x10) << 4, gust = b[7], rain = b[8] << 8 | b[9], uv = b[10] << 8 | b[11];
    const int light = b[12] << 16 | b[13] << 8 | b[14];
    const double wf = type == 24 ? 1.12 : 0.51, rf = type == 24 ? 0.3 : 0.254;
    IsmReading R = mk(type == 24 ? "Fineoffset-WH24" : type == 65 ? "Fineoffset-WH65B" : "Fineoffset-WS69", "Weather", decs(b[1]), "CRC");
    R.battery = !((b[3] & 0x08) >> 3);
    if (traw != 0x7ff) R.add("Temperature", (traw - 400) * 0.1, "C");
    if (hum != 0xff) R.add("Humidity", hum, "%", 0);
    if (wdir != 0x1ff) R.add("Wind direction", wdir, "deg", 0);
    if (wsraw != 0x1ff) R.add("Wind speed", wsraw * 0.125 * wf, "m/s");
    if (gust != 0xff) R.add("Wind gust", gust * wf, "m/s");
    R.add("Rain", rain * rf, "mm");
    if (uv != 0xffff) {
        static const int up[13] = {432, 851, 1210, 1570, 2017, 2450, 2761, 3100, 3512, 3918, 4277, 4650, 5029};
        int i = 0;
        while (i < 13 && up[i] < uv) ++i;
        R.add("UV index", i, "", 0);
    }
    if (light != 0xffffff) R.add("Light", light * 0.1, "lux");
    out.push_back(R);
}

// tpms_toyota.c: Toyota / Pacific PMV-C210 (FSK, differential Manchester, CRC-8 poly 0x07 init 0x80, inverted pressure copy)
void decToyota(Bits& B, Out& out) {
    static const uint8_t pre[2] = {0xa9, 0xe0};
    unsigned pos = 0;
    while ((pos = B.search(0, pos, pre, 12)) + 156 <= B.n[0]) {
        Bits pk;
        const unsigned sp = diffManchester(B, 0, pos + 11, pk, 80);
        pos += 2;
        if (sp - (pos - 2 + 11) < 144) continue;
        const uint8_t* b = pk.row(0);
        if (ismCrc8(b, 8, 0x07, 0x80) != b[8]) continue;
        const unsigned p1 = (b[4] & 0x7f) << 1 | b[5] >> 7, t = (b[5] & 0x7f) << 1 | b[6] >> 7, p2 = b[7] ^ 0xff;
        if (p1 != p2) continue;
        IsmReading R = mk("Toyota", "TPMS", hexs((unsigned)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3], 8), "CRC");
        R.add("Pressure", p1 * 0.25 - 7.0, "PSI", 2);
        R.add("Temperature", t - 40.0, "C", 0);
        R.add("Status", (b[4] & 0x80) | (b[6] & 0x7f), "", 0);
        out.push_back(R);
        return;
    }
}

// tpms_pmv107j.c: Pacific PMV-107J (Toyota, 315 MHz; FSK differential Manchester, CRC-8 poly 0x13)
void decPmv107j(Bits& B, Out& out) {
    static const uint8_t pre[1] = {0xf8};
    unsigned pos = 0;
    while ((pos = B.search(0, pos, pre, 6)) + 67 * 2 <= B.n[0]) {
        Bits pk;
        const unsigned start = pos + 6;
        const unsigned sp = diffManchester(B, 0, start, pk, 70);
        pos += 2;
        if (sp - start < 67 * 2) continue;
        uint8_t b[9];
        b[0] = pk.row(0)[0] >> 6;
        pk.extract(0, 2, b + 1, 64);
        if (ismCrc8(b, 8, 0x13, 0x00) != b[8]) continue;
        if (b[5] != (b[6] ^ 0xff)) continue;
        IsmReading R = mk("PMV-107J", "TPMS", hexs((unsigned)b[0] << 26 | b[1] << 18 | b[2] << 10 | b[3] << 2 | b[4] >> 6, 8), "CRC");
        R.battery = !((b[4] & 0x20) >> 5);
        R.add("Pressure", (b[5] - 40.0) * 2.48, "kPa");
        R.add("Temperature", b[7] - 40.0, "C", 0);
        out.push_back(R);
        return;
    }
}

// The VDO / Continental family: inverted bits, preamble ...aaa9, Manchester. lenBits: Manchester data bits wanted; need: raw bits after
// the preamble position the callback asks for.
template <typename F>
void vdoFamily(Bits& B, const uint8_t* pre, unsigned plen, unsigned need, unsigned step, unsigned maxBits, bool allRows, F&& fn) {
    B.invert();
    for (int r = 0; r < (allRows ? B.rows() : 1); r++) {
        unsigned pos = 0;
        while ((pos = B.search(r, pos, pre, plen)) + need <= B.n[r]) {
            Bits pk;
            manchester(B, r, pos + plen, pk, maxBits);
            if (fn(pk)) return;
            pos += step;
        }
    }
}

// tpms_citroen.c: Citroen / Peugeot / Fiat / Mitsubishi VDO (FSK Manchester, XOR of bytes 1 to 9)
void decCitroen(Bits& B, Out& out) {
    static const uint8_t pre[2] = {0xaa, 0xa9};
    vdoFamily(B, pre, 16, 178, 2, 88, false, [&](Bits& pk) {
        if (pk.n[0] < 80) return false;
        const uint8_t* b = pk.row(0);
        if (b[6] == 0 || b[7] == 0) return false;
        if (b[1] ^ b[2] ^ b[3] ^ b[4] ^ b[5] ^ b[6] ^ b[7] ^ b[8] ^ b[9]) return false;
        IsmReading R = mk("Citroen", "TPMS", hexs((unsigned)b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4], 8), "CHECKSUM");
        R.add("Pressure", b[6] * 1.364, "kPa", 0);
        R.add("Temperature", b[7] - 50.0, "C", 0);
        R.add("Repeat", b[5] & 0x0f, "", 0);
        R.add("Flags", b[5] >> 4, "", 0);
        out.push_back(R);
        return true;
    });
}

// tpms_renault.c: Renault Clio / Captur / Zoe, Dacia (FSK Manchester, CRC-8 poly 0x07)
void decRenault(Bits& B, Out& out) {
    static const uint8_t pre[2] = {0xaa, 0xa9};
    vdoFamily(B, pre, 16, 160, 15, 160, true, [&](Bits& pk) {
        if (pk.n[0] < 72) return false;
        const uint8_t* b = pk.row(0);
        if (ismCrc8(b, 8, 0x07, 0x00) != b[8]) return false;
        IsmReading R = mk("Renault", "TPMS", hexs(b[5] << 16 | b[4] << 8 | b[3], 6), "CRC");
        R.add("Pressure", (((b[0] & 0x03) << 8) | b[1]) * 0.75, "kPa");
        R.add("Temperature", b[2] - 30, "C", 0);
        R.addText("Flags", hexs(b[0] >> 2, 2));
        out.push_back(R);
        return true;
    });
}

// tpms_ford.c: Ford Fiesta / Focus / Kuga / Transit, VDO S180084730Z (FSK Manchester, add checksum, known flag patterns only)
void decFord(Bits& B, Out& out) {
    static const uint8_t pre[2] = {0xaa, 0xa9};
    vdoFamily(B, pre, 16, 144, 15, 160, true, [&](Bits& pk) {
        if (pk.n[0] < 64) return false;
        const uint8_t* b = pk.row(0);
        if (((b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6]) & 0xff) != b[7]) return false;
        int learn = 0, moving = 0, unknown = 0;
        switch (b[6] & 0x4c) {
        case 0x8: learn = 1; break;
        case 0x4: break;
        case 0x44: moving = 1; break;
        default: unknown = b[6] & 0x4c; break;
        }
        unknown |= b[6] & 0x90;
        if (unknown) return false;
        IsmReading R = mk("Ford", "TPMS", hexs((unsigned)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3], 8), "CHECKSUM");
        R.add("Pressure", (((b[6] & 0x20) << 3) | b[4]) * 0.25, "PSI", 2);
        if ((b[5] & 0x80) == 0) R.add("Temperature", (b[5] & 0x7f) - 56, "C", 0);
        R.add("Moving", moving, "", 0);
        R.add("Learn", learn, "", 0);
        out.push_back(R);
        return true;
    });
}

// tpms_hyundai_vdo.c: Hyundai / Kia VDO TG1C (FSK Manchester, CRC-8 poly 0x07 init 0xaa)
void decHyundai(Bits& B, Out& out) {
    static const uint8_t pre[4] = {0xaa, 0xaa, 0xaa, 0xa9};
    vdoFamily(B, pre, 32, 80, 2, 80, false, [&](Bits& pk) {
        if (pk.n[0] < 80) return false;
        const uint8_t* b = pk.row(0);
        if (ismCrc8(b, 9, 0x07, 0xaa) != b[9]) return false;
        IsmReading R = mk("Hyundai-VDO", "TPMS", hexs((unsigned)b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4], 8), "CRC");
        R.add("Pressure", b[6] * 1.375, "kPa", 0);
        R.add("Temperature", b[7] - 50.0, "C", 0);
        R.add("Repeat", b[5] & 0x0f, "", 0);
        R.add("State", b[0], "", 0);
        out.push_back(R);
        return true;
    });
}

// tpms_abarth124.c: Abarth 124 Spider / Fiat 124 / Mazda MX-5 ND, VDO TG1C (FSK Manchester, XOR of 9 bytes, temperature range)
void decAbarth(Bits& B, Out& out) {
    static const uint8_t pre[3] = {0xaa, 0xaa, 0xa9};
    const unsigned nb = B.n[0];
    if (!(nb > 150 && nb < 210)) return;
    vdoFamily(B, pre, 24, 80, 2, 72, false, [&](Bits& pk) {
        if (pk.n[0] < 72) return false;
        const uint8_t* b = pk.row(0);
        if (xorBytes(b, 9) != 0) return false;
        const double tc = b[6] - 50.0;
        if (tc < -50.0 || tc > 125.0) return false;
        IsmReading R = mk("Abarth-124Spider", "TPMS", hexs((unsigned)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3], 8), "CHECKSUM");
        R.add("Pressure", b[5] * 1.38, "kPa", 0);
        R.add("Temperature", tc, "C", 0);
        R.add("Status", b[7], "", 0);
        out.push_back(R);
        return true;
    });
}

// efergy_e2_classic.c: Efergy e2 classic current clamp (FSK PWM, add checksum)

} // namespace ism_detail

} // namespace dect2
