// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors: device decoders for weather stations and simple remotes (Nexus, Rubicson, LaCrosse, Acurite, Fine Offset, Oregon, Bresser...). Written from rtl_433 src/devices/.
#include "ism_internal.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace dect2 {

namespace ism_detail {

// nexus.c: Nexus, FreeTec NC-7345, NX-3980, Solight TE82S, TFA 30.3209 (36 bits PPM, no check: a row repeated 3 times)
void decNexus(Bits& B, Out& out) {
    const int r = B.findRepeatedRow(3, 36);
    if (r < 0 || B.n[r] > 37) return;
    const uint8_t* b = B.row(r);
    if ((b[3] & 0xf0) != 0xf0) return;
    if ((b[0] == 0 && b[2] == 0 && b[3] == 0) || (b[0] == 0xff && b[2] == 0xff && b[3] == 0xff)) return;
    if ((b[1] & 0x30) == 0x30) return;
    uint8_t c[5] = {b[0], b[1], b[2], (uint8_t)(b[3] & 0xf0), (uint8_t)((b[3] & 0x0f) << 4 | (b[4] & 0xf0) >> 4)};
    if (ismCrc8(c, 5, 0x31, 0x6c) == 0) return;   // a Rubicson frame
    const int hum = ((b[3] & 0x0F) << 4) | (b[4] >> 4);
    if (hum != 0 && hum > 100) return;
    const int traw = (int16_t)((b[1] << 12) | (b[2] << 4));
    IsmReading R = mk(hum ? "Nexus-TH" : "Nexus-T", "Weather", decs(b[0]), "REPEAT");
    R.channel = ((b[1] & 0x30) >> 4) + 1;
    R.battery = (b[1] & 0x80) ? 1 : 0;
    R.add("Temperature", (traw >> 4) * 0.1, "C");
    if (hum) R.add("Humidity", hum, "%", 0);
    out.push_back(R);
}

// rubicson.c: Rubicson, TFA 30.3197, InFactory PT-310 (36 bits PPM, CRC-8 poly 0x31 init 0x6c)
void decRubicson(Bits& B, Out& out) {
    const int r = B.findRepeatedRow(3, 36);
    if (r < 0 || B.n[r] < 36 || B.n[r] > 38) return;
    const uint8_t* b = B.row(r);
    if ((b[3] & 0xf0) != 0xf0) return;
    uint8_t c[5] = {b[0], b[1], b[2], (uint8_t)(b[3] & 0xf0), (uint8_t)((b[3] & 0x0f) << 4 | (b[4] & 0xf0) >> 4)};
    if (ismCrc8(c, 5, 0x31, 0x6c)) return;
    const int traw = (int16_t)((b[1] << 12) | (b[2] << 4));
    IsmReading R = mk("Rubicson-Temperature", "Weather", decs(b[0]), "CRC");
    R.channel = ((b[1] & 0x30) >> 4) + 1;
    R.battery = (b[1] & 0x80) ? 1 : 0;
    R.add("Temperature", (traw >> 4) * 0.1, "C");
    out.push_back(R);
}

// generic_temperature_sensor.c: Generic temperature sensor 1 (10 frames of 24 bits PPM, no check: 9 rows of exactly 24 bits)
void decGenericTemp(Bits& B, Out& out) {
    if (B.rows() < 10) return;
    for (int i = 1; i < 10; i++) if (B.n[i] != 24) return;
    const uint8_t* b = B.row(1);
    if ((b[0] == 0 && b[1] == 0 && b[2] == 0) || (b[0] == 0xff && b[1] == 0xff && b[2] == 0xff)) return;
    if (B.findRepeatedRow(3, 24) < 0) return;   // stricter than rtl_433: the frame must repeat (there is no check)
    const int traw = (int16_t)(((b[1] & 0x3f) << 10) | (b[2] << 2));
    IsmReading R = mk("Generic-Temperature", "Weather", decs(b[0]), "REPEAT");
    R.add("Temperature", (traw >> 4) * 0.1, "C", 2);
    R.add("Battery?", (b[1] & 0xC0) >> 6, "", 0);
    out.push_back(R);
}

// lacrosse_tx141x.c: LaCrosse TX141-Bv2, TX141TH-Bv2, TX141-Bv3, TX141W, TFA 30.3221.02 (PWM)
void decTx141(Bits& B, Out& out) {
    B.invert();
    int r = B.findRepeatedRow(B.rows() > 5 ? 5 : 3, 32);
    if (r < 0) r = B.findRepeatedRow(2, 64);
    if (r < 0 && B.rows() <= 4)
        for (int row = 0; row < B.rows(); row++)
            if ((B.n[row] == 40 || B.n[row] == 41) && ismLfsrDigest8Reflect(B.row(row), 4, 0x31, 0xf4) == B.row(row)[4]) { r = row; break; }
    if (r < 0) return;
    const unsigned nb = B.n[r];
    enum { B32, B37, TH, BV3, W } dev;
    if (nb >= 64) dev = W;
    else if (nb > 41) return;
    else if (nb >= 41) { if (B.rows() > 12) return; dev = TH; }
    else if (nb >= 40) dev = TH;
    else if (nb >= 37) dev = B37;
    else if (nb == 32) dev = B32;
    else dev = BV3;
    const uint8_t* b = B.row(r);
    if (dev == W) {
        if ((b[0] >> 3) != 0x01 || ismCrc8(b, 8, 0x31, 0x00)) return;
        const int id = ((b[0] & 0x07) << 16) | (b[1] << 8) | b[2], type = b[3] & 0x0f;
        const int traw = (b[4] << 4) | (b[5] >> 4), hum = ((b[5] & 0x0f) << 8) | b[6];
        IsmReading R = mk("LaCrosse-TX141W", "Weather", hexs(id, 5), "CRC");
        R.channel = (b[3] & 0x30) >> 4; R.battery = !(b[3] >> 7);
        if (type == 1) { R.add("Temperature", (traw - 500) * 0.1, "C"); R.add("Humidity", hum, "%", 0); }
        else if (type == 2) { R.add("Wind speed", traw * 0.1, "km/h"); R.add("Wind direction", hum, "deg", 0); }
        else return;
        out.push_back(R);
        return;
    }
    const int id = b[0];
    const int batLow = dev == TH ? (b[1] >> 7) : !(b[1] >> 7);
    const int traw = ((b[1] & 0x0F) << 8) | b[2];
    const double tc = (traw - 500) * 0.1;
    const int hum = dev == TH ? b[3] : 0;
    if (id == 0 || (dev == TH && (hum == 0 || hum > 100)) || tc < -40.0 || tc > 140.0) return;
    if (dev == TH && ismLfsrDigest8Reflect(b, 4, 0x31, 0xf4) != b[4]) return;
    IsmReading R = mk(dev == B32 ? "LaCrosse-TX141B" : dev == B37 ? "LaCrosse-TX141Bv2" : dev == BV3 ? "LaCrosse-TX141Bv3" : "LaCrosse-TX141THBv2",
                      "Weather", hexs(id, 2), dev == TH ? "CRC" : "REPEAT");
    if (dev != B32) R.channel = (b[1] & 0x30) >> 4;
    R.battery = !batLow;
    R.add("Temperature", tc, "C");
    if (dev == TH) R.add("Humidity", hum, "%", 0);
    out.push_back(R);
}

// acurite.c: Acurite 592TXR tower sensor and 5-in-1 (Iris) weather station (PWM, add checksum and byte parity)
const int kAcurite5n1Dir[16] = {14, 11, 13, 12, 15, 10, 0, 9, 3, 6, 4, 5, 2, 7, 1, 8};
bool acuriteCheck(const uint8_t* bb, int browlen, int explen) {
    if (browlen < 6 || browlen < explen) return false;
    if ((addBytes(bb, explen - 1) & 0xff) != bb[explen - 1]) return false;
    if (parityBytes(&bb[2], explen - 3)) return false;
    return ((bb[0] & 0xC0) >> 6) != 1;
}
void decAcurite(Bits& B, Out& out) {
    B.invert();
    static const char* const chan[4] = {"C", "E", "B", "A"};
    for (int r = 0; r < B.rows(); r++) {
        const int browlen = (int)(B.n[r] / 8);
        if (browlen < 6 || browlen > 10) continue;
        const uint8_t* bb = B.row(r);
        if (bb[0] == 0 && bb[1] == 0 && bb[2] == 0 && bb[browlen - 1] == 0) continue;
        const int mt = bb[2] & 0x3f;
        if (mt == 0x04) {
            if (!acuriteCheck(bb, browlen, 7)) continue;
            const int hum = bb[3] & 0x7f;
            if (hum > 100 && hum != 127) continue;
            const double tc = ((((bb[4] & 0x7F) << 7) | (bb[5] & 0x7F)) - 1000) * 0.1;
            if (tc < -40 || tc > 70) continue;
            IsmReading R = mk("Acurite-Tower", "Weather", decs(((bb[0] & 0x3f) << 8) | bb[1]), "CHECKSUM");
            R.channelName = chan[(bb[0] & 0xC0) >> 6];
            R.battery = (bb[2] & 0x40) != 0;
            R.add("Temperature", tc, "C");
            if (hum != 127) R.add("Humidity", hum, "%", 0);
            out.push_back(R);
        } else if (mt == 0x31 || mt == 0x38) {
            if (!acuriteCheck(bb, browlen, 8)) continue;
            IsmReading R = mk("Acurite-5n1", "Weather", decs(((bb[0] & 0x0f) << 8) | bb[1]), "CHECKSUM");
            R.channelName = chan[(bb[0] & 0xC0) >> 6];
            R.battery = (bb[2] & 0x40) != 0;
            const int wraw = ((bb[3] & 0x1F) << 3) | ((bb[4] & 0x70) >> 4);
            const double wkph = wraw > 0 ? wraw * 0.8278 + 1.0 : 0.0;
            R.add("Wind speed", wkph, "km/h");
            if (mt == 0x31) {
                R.add("Wind direction", kAcurite5n1Dir[bb[4] & 0x0f] * 22.5, "deg");
                R.add("Rain", (((bb[5] & 0x7f) << 7) | (bb[6] & 0x7F)) * 0.01, "in", 2);
            } else {
                const double tf = ((((bb[4] & 0x0F) << 7) | (bb[5] & 0x7F)) - 400) * 0.1;
                const int hum = bb[6] & 0x7f;
                if (tf < -40.0 || tf > 158.0 || hum > 100) continue;
                R.add("Temperature", tf, "F");
                R.add("Humidity", hum, "%", 0);
            }
            out.push_back(R);
        }
    }
}

// fineoffset_wh1080.c: Fine Offset WH1080 / WH3080 weather message (OOK PWM, CRC-8 poly 0x31 init 0xff over the preamble byte too)
const int kWh1080Dir[16] = {0, 23, 45, 68, 90, 113, 135, 158, 180, 203, 225, 248, 270, 293, 315, 338};
void decWh1080(Bits& B, Out& out) {
    if (B.rows() != 1) return;
    uint8_t buf[16] = {};
    const uint8_t* br;
    if (B.n[0] >= 88 && B.n[0] < 100) br = B.row(0);
    else if (B.n[0] == 87) { B.extract(0, 7, buf + 1, 80); buf[0] = (uint8_t)((B.row(0)[0] >> 1) | 0x80); br = buf; }
    else return;
    if (br[0] != 0xff || ismCrc8(br, 11, 0x31, 0xff)) return;
    if ((br[1] >> 4) != 0x0a) return;   // the weather message; time and UV messages are not shown
    IsmReading R = mk("Fineoffset-WHx080", "Weather", decs((br[1] << 4 & 0xf0) | (br[2] >> 4)), "CRC");
    R.battery = (br[9] >> 4) != 1;
    R.add("Temperature", ((((br[2] & 0x03) << 8) | br[3]) - 400) * 0.1, "C");
    R.add("Humidity", br[4], "%", 0);
    R.add("Wind direction", kWh1080Dir[br[9] & 0x0f], "deg", 0);
    R.add("Wind speed", br[5] * 0.34 * 3.6, "km/h", 2);
    R.add("Wind gust", br[6] * 0.34 * 3.6, "km/h", 2);
    R.add("Rain", (((br[7] & 0x0f) << 8) | br[8]) * 0.3, "mm");
    out.push_back(R);
}

// ambient_weather.c: Ambient Weather F007TH, F012TH, TFA 30.3208.02, SwitchDoc F016TH (Manchester, LFSR digest)
void decF007th(Bits& B, Out& out) {
    static const uint8_t pre[2] = {0x01, 0x45}, inv[2] = {0xfd, 0x45};
    auto one = [&](int row, unsigned pos) -> bool {
        uint8_t b[6];
        B.extract(row, pos, b, 48);
        if ((uint8_t)(ismLfsrDigest8(b, 5, 0x98, 0x3e) ^ 0x64) != b[5]) return false;
        const int hum = b[4], traw = ((b[2] & 0x0f) << 8) | b[3];
        const double tf = (traw - 400) * 0.1;
        if (hum > 100 || tf < -40.0 || tf >= 344.0) return false;
        IsmReading R = mk("Ambientweather-F007TH", "Weather", decs(b[1]), "CRC");
        R.channel = ((b[2] & 0x70) >> 4) + 1;
        R.battery = (b[2] & 0x80) == 0;
        R.add("Temperature", tf, "F");
        R.add("Humidity", hum, "%", 0);
        out.push_back(R);
        return true;
    };
    for (int row = 0; row < B.rows(); row++) {
        unsigned pos = 0;
        while ((pos = B.search(row, pos, pre, 12)) + 8 + 48 <= B.n[row]) { if (one(row, pos + 8)) return; pos += 16; }
        pos = 0;
        while ((pos = B.search(row, pos, inv, 12)) + 8 + 48 <= B.n[row]) { if (one(row, pos + 8)) return; pos += 15; }
    }
}

// oregon_scientific.c: v2.1 THGR122N / THGR228N / THGR968 and v3 THGR810 (Manchester, sum of nibbles)
double osTemp(const uint8_t* m) {
    double t = (((m[5] >> 4) * 100) + ((m[4] & 0x0f) * 10) + ((m[4] >> 4) & 0x0f)) / 10.0;
    t += (m[5] & 0x07) * 100.0;
    return (m[5] & 0x08) ? -t : t;
}
int osHum(const uint8_t* m) { return ((m[6] & 0x0f) * 10) + (m[6] >> 4); }
bool osChecksum(const uint8_t* m, int idx) {
    unsigned sum = 0, chk;
    for (int i = 0; i < idx - 1; i += 2) sum += (m[i >> 1] >> 4) + (m[i >> 1] & 0x0f);
    if (idx & 1) { sum += m[idx >> 1] >> 4; chk = (m[idx >> 1] & 0x0f) | (m[(idx + 1) >> 1] & 0xf0); }
    else chk = (m[idx >> 1] >> 4) | ((m[idx >> 1] & 0x0f) << 4);
    return (sum & 0xff) == chk;
}
void decOregon(Bits& B, Out& out) {
    const uint8_t* b = B.row(0);
    // v2.1
    if (((b[1] == 0x55 && b[2] == 0x55) || (b[1] == 0xAA && b[2] == 0xAA))) {
        Bits db;
        const unsigned sync = ((unsigned)b[3] << 24) | (b[4] << 16) | (b[5] << 8) | b[6];
        bool found = false;
        for (int k = 0; k < 8; k++) {
            const unsigned mask = 0xffff0000u >> k, p1 = 0x55990000u >> k, p2 = 0xaa990000u >> k;
            if ((sync & mask) != p1 && (sync & mask) != p2) continue;
            manchester(B, 0, k + 40, db, 173);
            found = true;
            break;
        }
        if (!found) return;
        const int bits = (int)db.n[0];
        uint8_t* m = const_cast<uint8_t*>(db.row(0));
        for (int i = 0; i < (bits + 7) / 8; i++) m[i] = reflect4(m[i]);
        const int sid = (m[0] << 8) | m[1];
        if (sid == 0x1d20 || sid == 0x1d30) {
            if (!((bits == 68 || bits == 76) && osChecksum(m, 15))) return;
            IsmReading R = mk(sid == 0x1d30 ? "Oregon-THGR968" : bits == 76 ? "Oregon-THGR122N" : "Oregon-THGR228N", "Weather",
                              decs((m[2] & 0x0f) | (m[3] & 0xf0)), "CHECKSUM");
            R.channel = (m[2] >> 4) & 0x0f;
            R.battery = !((m[3] >> 2) & 1);
            R.add("Temperature", osTemp(m), "C", 2);
            R.add("Humidity", osHum(m), "%", 0);
            out.push_back(R);
        }
        return;
    }
    // v3
    if ((((b[0] & 0xf) != 0x0f) || (b[1] != 0xff) || ((b[2] & 0xc0) != 0xc0)) && (((b[0] & 0xf) != 0x00) || (b[1] != 0x00) || ((b[2] & 0xc0) != 0x00))) return;
    static const uint8_t pat[2] = {0x00, 0x05};
    const unsigned pos = B.search(0, 0, pat, 16) + 16;
    if (pos > B.n[0] || B.n[0] - pos < 56) return;
    const unsigned len = B.n[0] - pos;
    if (len > 44 * 8) return;
    uint8_t m[44] = {};
    B.extract(0, pos, m, len);
    for (unsigned i = 0; i < (len + 7) / 8; i++) m[i] = reflect4(m[i]);
    const int sid = (m[0] << 8) | m[1];
    if ((sid & 0xf0ff) == 0xf024 || sid == 0xf8b4) {
        if (!osChecksum(m, 15)) return;
        if (((m[5] >> 4) & 0x0F) > 9 || (m[4] & 0x0F) > 9 || ((m[4] >> 4) & 0x0F) > 9 || (m[6] & 0x0F) > 9 || ((m[6] >> 4) & 0x0F) > 9) return;
        const double tc = osTemp(m);
        if (tc > 70 || tc < -50) return;
        IsmReading R = mk("Oregon-THGR810", "Weather", decs((m[2] & 0x0f) | (m[3] & 0xf0)), "CHECKSUM");
        R.channel = (m[2] >> 4) & 0x0f;
        R.battery = !((m[3] >> 2) & 1);
        R.add("Temperature", tc, "C", 2);
        R.add("Humidity", osHum(m), "%", 0);
        out.push_back(R);
    }
}

// schraeder.c: Schrader TPMS (FCC MRXGG4, 68 bits Manchester, CRC-8 poly 0x07 init 0xf0)

} // namespace ism_detail

} // namespace dect2
