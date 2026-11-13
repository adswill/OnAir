// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors test signal. See ism_gen.h. Each frame follows the layout documented in the rtl_433 file named beside its encoder.
#include "dect2/ism/ism_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/ism/ism_rx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

namespace dect2 {

namespace {

using Bitv = std::vector<int>;

void put(Bitv& v, const uint8_t* b, int nbits) { for (int i = 0; i < nbits; i++) v.push_back((b[i >> 3] >> (7 - (i & 7))) & 1); }
void putBytes(Bitv& v, std::initializer_list<uint8_t> l) { for (uint8_t b : l) put(v, &b, 8); }
void putStr(Bitv& v, const char* s) { for (; *s; s++) if (*s == '0' || *s == '1') v.push_back(*s - '0'); }

// NRZ: a run of ones is a mark, a run of zeros a space (the train starts at the first mark)
IsmTrain nrz(const Bitv& b, float T, bool fsk, float tailUs) {
    IsmTrain t; t.fsk = fsk;
    size_t i = 0;
    while (i < b.size() && !b[i]) i++;
    while (i < b.size()) {
        size_t a = i;
        while (i < b.size() && b[i]) i++;
        const float p = (float)(i - a) * T;
        a = i;
        while (i < b.size() && !b[i]) i++;
        const float g = i < b.size() ? (float)(i - a) * T : (float)(i - a) * T + tailUs;
        t.p.push_back({p, g});
    }
    return t;
}

// Manchester as rtl_433's OOK_PULSE_MANCHESTER_ZEROBIT reads it: 1 = high then low, 0 = low then high, half bits of s; the first bit
// must be 0 (its low half is not seen)
IsmTrain mcZero(const Bitv& raw, float s) {
    Bitv h;
    for (int b : raw) { h.push_back(b ? 1 : 0); h.push_back(b ? 0 : 1); }
    if (!h.empty()) h.erase(h.begin());
    return nrz(h, s, false, 50000);
}

// Manchester pairs for bitbuffer_manchester_decode (the second bit of each pair is the data bit)
void mcPairs(Bitv& v, const uint8_t* b, int nbits) { for (int i = 0; i < nbits; i++) { const int d = (b[i >> 3] >> (7 - (i & 7))) & 1; v.push_back(!d); v.push_back(d); } }

// Differential Manchester for bitbuffer_differential_manchester_decode: a transition at every bit start, 1 = no transition in the middle
void diffMc(Bitv& v, const uint8_t* b, int nbits, int prev) {
    for (int i = 0; i < nbits; i++) {
        const int d = (b[i >> 3] >> (7 - (i & 7))) & 1;
        const int h1 = !prev, h2 = d ? h1 : !h1;
        v.push_back(h1); v.push_back(h2);
        prev = h2;
    }
}

Bitv inv(const Bitv& v) { Bitv r(v.size()); for (size_t i = 0; i < v.size(); i++) r[i] = !v[i]; return r; }

uint8_t reflect4(uint8_t x) {
    x = (uint8_t)(((x & 0xCC) >> 2) | ((x & 0x33) << 2));
    return (uint8_t)(((x & 0xAA) >> 1) | ((x & 0x55) << 1));
}

int bcd2(int v) { return ((v / 10) % 10) << 4 | (v % 10); }

// Oregon temperature digits (get_os_temperature) and sum-of-nibbles checksum at nibble 15
void osTemp(uint8_t* m, double tc) {
    const int t = (int)std::lround(std::fabs(tc) * 10);
    m[4] = (uint8_t)(((t % 10) << 4) | ((t / 10) % 10));
    m[5] = (uint8_t)(((t / 100) % 10) << 4 | (tc < 0 ? 0x08 : 0));
}
void osChecksum15(uint8_t* m) {
    unsigned s = 0;
    for (int i = 0; i < 7; i++) s += (m[i] >> 4) + (m[i] & 0x0f);
    s += m[7] >> 4;
    s &= 0xff;
    m[7] = (uint8_t)((m[7] & 0xf0) | (s & 0x0f));
    m[8] = (uint8_t)((m[8] & 0x0f) | (s & 0xf0));
}

struct Pwm { IsmTrain t; void add(float p, float g) { t.p.push_back({p, g}); } };

double nexusT(int k) { return 21.3 + 0.1 * (k % 4); }
double tx141T(int k) { return 18.7 + 0.1 * (k % 3); }
double acuT(int k) { return 22.5 + 0.1 * (k % 3); }
double wh1080T(int k) { return 12.4 + 0.1 * (k % 3); }
double f007F(int k) { return 68.5 + 0.1 * (k % 3); }
double os3T(int k) { return 19.6 + 0.1 * (k % 2); }
double os21T(int k) { return 5.2 + 0.1 * (k % 2); }

// Z-Wave (G.9959): preamble 0x55, SOF 0xF0, HomeID Src FC(2) Len Dst payload, then 0xFF^XOR (R1, R2) or CRC-16 (R3)
IsmTrain zwFrame(int rate, uint32_t home, uint8_t src, uint8_t dst, const std::vector<uint8_t>& payload, int k, bool badCheck = false) {
    std::vector<uint8_t> m = {(uint8_t)(home >> 24), (uint8_t)(home >> 16), (uint8_t)(home >> 8), (uint8_t)home, src, 0x41, (uint8_t)(k & 0x0F), 0, dst};
    m.insert(m.end(), payload.begin(), payload.end());
    m[7] = (uint8_t)(m.size() + (rate == 3 ? 2 : 1));
    if (rate == 3) {
        const uint16_t c = ismCrc16(m.data(), (unsigned)m.size(), 0x1021, 0x1D0F);
        m.push_back((uint8_t)(c >> 8)); m.push_back((uint8_t)c);
    } else {
        uint8_t x = 0xFF;
        for (uint8_t b : m) x ^= b;
        m.push_back(x);
    }
    if (badCheck) m.back() ^= 0x10;
    Bitv raw;
    for (int i = 0; i < (rate == 3 ? 10 : 12); i++) putBytes(raw, {0x55});
    putBytes(raw, {0xF0});
    put(raw, m.data(), (int)m.size() * 8);
    putBytes(raw, {0x00});
    if (rate != 1) return nrz(raw, rate == 2 ? 25.f : 10.f, true, rate == 2 ? 60.f : 40.f);
    Bitv h;   // Manchester: 1 = high then low
    for (int b : raw) { h.push_back(b ? 1 : 0); h.push_back(b ? 0 : 1); }
    return nrz(h, 1e6f / 9600 / 2, true, 200.f);
}

} // namespace

const char* ismTxName(IsmTx p) {
    static const char* const n[] = {"Nexus", "LaCrosse TX141TH-Bv2", "Acurite tower", "Fine Offset WH1080", "Ambient F007TH", "Oregon THGR810 (v3)",
                                    "Oregon THGR228N (v2.1)", "Schrader TPMS", "EV1527 remote", "Toyota TPMS", "Renault TPMS", "Ford TPMS", "Efergy e2",
                                    "CurrentCost TX", "Bresser 6-in-1", "Bresser 5-in-1", "LaCrosse TX29-IT", "Fine Offset WH24", "Wireless M-Bus T", "Z-Wave R1", "Z-Wave R2", "Z-Wave R3", "KNX RF", "HomeMatic BidCoS", "EnOcean ERP1", "DASH7 (55.555 kbit/s)",
                                    "X10 RF", "Insteon", "inFactory TH", "Ambient WH31E", "Nissan TPMS", "GM aftermarket TPMS", "Efergy Optical", "ESIC EMT7110", "ERT SCM+"};
    return (int)p >= 0 && (int)p < (int)IsmTx::Count ? n[(int)p] : "?";
}

IsmTrain ismEncodeZwave(int rate, uint32_t home, uint8_t src, uint8_t dst, const std::vector<uint8_t>& payload, int k, bool badCheck) {
    return zwFrame(rate, home, src, dst, payload, k, badCheck);
}

IsmTrain ismEncode(IsmTx p, int k) {
    switch (p) {
    case IsmTx::Nexus: {   // nexus.c: [id0][id1][flags][temp0][temp1][temp2][const][humi0][humi1], PPM 500 us pulses, 1000 / 2000 / 4000 us gaps
        const int t = (int)std::lround(nexusT(k) * 10) & 0xfff, hum = 45;
        const uint8_t b[5] = {0x5A, (uint8_t)(0x90 | (t >> 8)), (uint8_t)(t & 0xff), (uint8_t)(0xF0 | (hum >> 4)), (uint8_t)((hum & 0xf) << 4)};
        Pwm w;
        for (int r = 0; r < 12; r++) {
            for (int i = 0; i < 36; i++) w.add(500, ((b[i >> 3] >> (7 - (i & 7))) & 1) ? 2000 : 1000);
            w.add(500, r == 11 ? 30000 : 4000);
        }
        return w.t;
    }
    case IsmTx::Tx141th: {   // lacrosse_tx141x.c: 4 sync pulses of 833 us, 40 bits PWM (1 = 417 us high, 0 = 208 us), digest 0x31 / 0xf4
        const int t = (int)std::lround(tx141T(k) * 10) + 500;
        uint8_t b[5] = {0x3C, (uint8_t)(0x10 | (t >> 8)), (uint8_t)(t & 0xff), 62, 0};
        b[4] = ismLfsrDigest8Reflect(b, 4, 0x31, 0xf4);
        Pwm w;
        for (int r = 0; r < 12; r++) {
            for (int s = 0; s < 4; s++) w.add(833, 833);
            for (int i = 0; i < 40; i++) { const int d = (b[i >> 3] >> (7 - (i & 7))) & 1; w.add(d ? 417 : 208, d ? 208 : 417); }
        }
        w.t.p.back().gapUs = 30000;
        return w.t;
    }
    case IsmTx::AcuriteTower: {   // acurite.c (592TXR): CCII IIII | IIII IIII | pB00 0100 | pHHH HHHH | p??T TTTT | pTTT TTTT | KKKK KKKK
        const int id = 0x1234, hum = 55, t = (int)std::lround(acuT(k) * 10) + 1000;
        auto par = [](uint8_t v) { int c = 0; for (int i = 0; i < 7; i++) c += (v >> i) & 1; return (uint8_t)(v | ((c & 1) << 7)); };
        uint8_t b[7] = {(uint8_t)(0xC0 | (id >> 8)), (uint8_t)(id & 0xff), par(0x44), par((uint8_t)hum), par((uint8_t)((t >> 7) & 0x7f)), par((uint8_t)(t & 0x7f)), 0};
        int s = 0;
        for (int i = 0; i < 6; i++) s += b[i];
        b[6] = (uint8_t)s;
        Pwm w;
        for (int r = 0; r < 3; r++) {
            for (int q = 0; q < 4; q++) w.add(620, 596);
            for (int i = 0; i < 56; i++) { const int d = (b[i >> 3] >> (7 - (i & 7))) & 1; w.add(d ? 408 : 220, d ? 204 : 392); }
            w.t.p.back().gapUs = r == 2 ? 30000 : 2192;
        }
        return w.t;
    }
    case IsmTx::Wh1080: {   // fineoffset_wh1080.c: ff, type 0xa + 8 bit id, 10 bit temperature + 400, humidity, speed, gust, rain, battery / direction, CRC-8 0x31 / 0xff
        const int id = 0x7B, t = (int)std::lround(wh1080T(k) * 10) + 400;
        uint8_t b[11] = {0xff, (uint8_t)(0xA0 | (id >> 4)), (uint8_t)(((id & 0xf) << 4) | ((t >> 8) & 0x03)), (uint8_t)(t & 0xff), 71, 3, 7, 0x01, 0x23, 0x04, 0};
        b[10] = ismCrc8(b, 10, 0x31, 0xff);
        Pwm w;
        for (int i = 0; i < 88; i++) w.add(((b[i >> 3] >> (7 - (i & 7))) & 1) ? 544 : 1524, 1036);
        w.t.p.back().gapUs = 30000;
        return w.t;
    }
    case IsmTx::F007th: {   // ambient_weather.c: preamble 0x00145, xxxxMMMM IIIIIIII BCCCTTTT TTTTTTTT HHHHHHHH MMMMMMMM, digest 0x98 / 0x3e ^ 0x64
        const int t = (int)std::lround(f007F(k) * 10) + 400;
        uint8_t b[6] = {0x45, 0x9C, (uint8_t)((2 << 4) | ((t >> 8) & 0x0f)), (uint8_t)(t & 0xff), 40, 0};
        b[5] = (uint8_t)(ismLfsrDigest8(b, 5, 0x98, 0x3e) ^ 0x64);
        Bitv raw;
        putBytes(raw, {0x00, 0x01});
        put(raw, b, 48);
        return mcZero(raw, 500);
    }
    case IsmTx::OregonV3: {   // oregon_scientific.c (v3, THGR810): preamble 00 00 00 5, nibble-reflected id f824, channel, house code, BCD temperature and humidity
        uint8_t m[9] = {0xF8, 0x24, 0x15, 0xA0, 0, 0, 0x25, 0x00, 0x00};
        osTemp(m, os3T(k));
        osChecksum15(m);
        Bitv raw;
        putBytes(raw, {0x00, 0x00, 0x00, 0x05});
        for (auto& v : m) v = reflect4(v);
        put(raw, m, 72);
        return mcZero(raw, 488);
    }
    case IsmTx::OregonV21: {   // oregon_scientific.c (v2.1, THGR228N): 55 55 55 55 99 sync, then each bit sent inverted and normal, 68 bits
        uint8_t m[9] = {0x1D, 0x20, 0x1E, 0x30, 0, 0, 0x08, 0x00, 0x00};
        osTemp(m, os21T(k));
        osChecksum15(m);
        for (auto& v : m) v = reflect4(v);
        Bitv raw;
        putBytes(raw, {0x55, 0x55, 0x55, 0x55, 0x99});
        mcPairs(raw, m, 68);
        return mcZero(raw, 488);
    }
    case IsmTx::Schrader: {   // schraeder.c: S PF FI II II II PP TT CC, 68 bits, CRC-8 0x07 / 0xf0 from nibble 1
        const unsigned id = 0x1A2B3C4, flags = 0x0B;
        uint8_t b[8] = {(uint8_t)(0xF0 | (flags >> 4)), (uint8_t)(((flags & 0xf) << 4) | (id >> 24)), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id,
                        (uint8_t)(92 + (k % 2)), 75, 0};
        b[7] = ismCrc8(b, 7, 0x07, 0xf0);
        Bitv raw;
        putStr(raw, "0111");
        put(raw, b, 64);
        return mcZero(raw, 120);
    }
    case IsmTx::Ev1527: {   // generic_remote.c: 24 bits PWM (1 = long pulse) and a short sync pulse, repeated
        const uint32_t code = 0x5A3C81;
        Pwm w;
        for (int r = 0; r < 6; r++) {
            for (int i = 23; i >= 0; i--) { const int d = (code >> i) & 1; w.add(d ? 1404 : 464, d ? 464 : 1404); }
            w.add(464, r == 5 ? 30000 : 14000);
        }
        return w.t;
    }
    case IsmTx::Toyota: {   // tpms_toyota.c: 0101 0101 0011 11 sync, 72 bits differential Manchester: id, state, pressure, temperature, ~pressure, CRC-8 0x07 / 0x80
        const unsigned id = 0xA1B2C3D4, pr = 156 + (k % 2), te = 62;
        uint8_t b[10] = {(uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id, (uint8_t)(pr >> 1), (uint8_t)(((pr & 1) << 7) | (te >> 1)),
                        (uint8_t)((te & 1) << 7), (uint8_t)(pr ^ 0xff), 0, 0};
        b[8] = ismCrc8(b, 8, 0x07, 0x80);
        Bitv raw;
        putStr(raw, "01010101001111");
        diffMc(raw, b, 76, 1);   // 4 more bits keep the clock going as the trailer
        return nrz(raw, 52, true, 150);
    }
    case IsmTx::Renault: {   // tpms_renault.c: 55 55 55 56, 72 bits Manchester: flags / pressure, temperature, id little-endian, ffff, CRC-8 0x07
        const int pr = 333 + (k % 2);
        uint8_t b[9] = {(uint8_t)((0x34 << 2) | (pr >> 8)), (uint8_t)(pr & 0xff), 48, 0xEE, 0xFF, 0xC0, 0xff, 0xff, 0};
        b[8] = ismCrc8(b, 8, 0x07, 0x00);
        Bitv in;   // as the decoder sees it after inverting
        putBytes(in, {0xaa, 0xaa, 0xaa, 0xa9});
        mcPairs(in, b, 72);
        putBytes(in, {0xaa});
        return nrz(inv(in), 52, true, 150);
    }
    case IsmTx::Ford: {   // tpms_ford.c: 55 55 55 56, 64 bits Manchester: id, pressure (PSI x 4), temperature + 56, flags, add checksum
        uint8_t b[8] = {0x12, 0xAB, 0x34, 0xCD, (uint8_t)(140 + (k % 2)), 76, 0x05, 0};
        int s = 0;
        for (int i = 0; i < 7; i++) s += b[i];
        b[7] = (uint8_t)s;
        Bitv in;
        putBytes(in, {0xaa, 0xaa, 0xaa, 0xa9});
        mcPairs(in, b, 64);
        putBytes(in, {0xaa});
        return nrz(inv(in), 52, true, 150);
    }
    case IsmTx::Efergy: {   // efergy_e2_classic.c: FSK PWM (64 us = 1, 136 us = 0) after a 500 us sync: 00, id, flags, current, exponent, add checksum
        uint8_t b[8] = {0x00, 0xD2, 0x04, 0x50, (uint8_t)(0x58 + (k % 2)), 0x00, 0x03, 0};
        int s = 0;
        for (int i = 0; i < 7; i++) s += b[i];
        b[7] = (uint8_t)s;
        IsmTrain t; t.fsk = true;
        t.p.push_back({500, 136});
        for (int i = 0; i < 64; i++) { const int d = (b[i >> 3] >> (7 - (i & 7))) & 1; t.p.push_back({d ? 64.f : 136.f, d ? 136.f : 64.f}); }
        t.p.back().gapUs = 100;
        return t;
    }
    case IsmTx::CurrentCost: {   // current_cost.c: (inverted) cc cc cc ce 91 5d init pattern, then 64 bits Manchester: id, three power values
        const int w0 = 1234 + (k % 2);
        uint8_t b[8] = {0x07, 0xA5, (uint8_t)(0x80 | (w0 >> 8)), (uint8_t)(w0 & 0xff), 0, 0, 0, 0};
        static const uint8_t pat[6] = {0xcc, 0xcc, 0xcc, 0xce, 0x91, 0x5d};
        Bitv in;
        putBytes(in, {0xcc});
        put(in, pat, 45);
        mcPairs(in, b, 64);
        return nrz(inv(in), 250, true, 500);
    }
    case IsmTx::Bresser6in1: {   // bresser_6in1.c: aa aa 2d d4, DIGEST:16h ID:32h STYPE:4h STARTUP:1b CH:3d WSPEED WDIR | TEMP HUM | UV CHKSUM
        const int t = (int)std::lround((15.6 + 0.1 * (k % 3)) * 10);
        uint8_t m[18] = {0, 0, 0x12, 0x34, 0x56, 0x78, 0x18, 0xFC, 0xBE, 0xFD, 0x22, 0x50,
                         (uint8_t)bcd2(t / 10), (uint8_t)(((t % 10) << 4) | 0x02), 0x67, 0xFC, 0xD0, 0};
        int s = 0;
        for (int i = 2; i < 17; i++) s += m[i];
        m[17] = (uint8_t)(0xff - (s & 0xff));
        const uint16_t d = ismLfsrDigest16(&m[2], 15, 0x8810, 0x5412);
        m[0] = (uint8_t)(d >> 8); m[1] = (uint8_t)d;
        Bitv v;
        putBytes(v, {0xaa, 0xaa, 0xaa, 0xaa, 0x2d, 0xd4});
        put(v, m, 144);
        putBytes(v, {0x00});
        return nrz(v, 124, true, 248);
    }
    case IsmTx::Bresser5in1: {   // bresser_5in1.c: aa aa aa aa aa 2d d4, 13 bytes inverted, then uu II sS GG DG WW W TT T HH RR RR Bt
        const int t = (int)std::lround((8.7 + 0.1 * (k % 2)) * 10);
        uint8_t m[26] = {};
        uint8_t hi[13] = {0x00, 0x6C, 0x80, 0x34, 0x40, 0x31, 0x00, (uint8_t)bcd2(t % 100), (uint8_t)((t / 100) % 10), 0x81, 0x23, 0x01, 0x00};
        int ones = 0;
        for (int i = 1; i < 13; i++) for (int q = 0; q < 8; q++) ones += (hi[i] >> q) & 1;
        hi[0] = (uint8_t)ones;   // uu: the set bits of bytes 14 to 25
        for (int i = 0; i < 13; i++) { m[13 + i] = hi[i]; m[i] = (uint8_t)~hi[i]; }
        Bitv v;
        putBytes(v, {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x2d, 0xd4});
        put(v, m, 208);
        putBytes(v, {0x00});
        return nrz(v, 124, true, 248);
    }
    case IsmTx::Tx29: {   // lacrosse_tx35.c: aa 2d d4, model 9, 6 bit id, new battery, BCD temperature + 40, weak battery / humidity (0x6a: none), CRC-8 0x31
        const int id = 43, t = (int)std::lround((21.7 + 0.1 * (k % 2) + 40) * 10);
        uint8_t b[5] = {(uint8_t)(0x90 | (id >> 2)), (uint8_t)(((id & 3) << 6) | (t / 100)), (uint8_t)bcd2(t % 100), 0x6a, 0};
        b[4] = ismCrc8(b, 4, 0x31, 0x00);
        Bitv v;
        putBytes(v, {0xaa, 0xaa, 0xaa, 0x2d, 0xd4});
        put(v, b, 40);
        putBytes(v, {0x00});
        return nrz(v, 55, true, 110);
    }
    case IsmTx::Wh24: {   // fineoffset.c (WH24): aa aa aa aa aa 2d d4, FF II DD VT TT HH WW GG RR RR UU UU LL LL LL CC BB
        const int t = (int)std::lround((11.8 + 0.1 * (k % 2)) * 10) + 400;
        uint8_t b[17] = {0x24, 0xBF, 0x0A, (uint8_t)(0x80 | (t >> 8)), (uint8_t)(t & 0xff), 78, 8, 2, 0x00, 0x4A, 0x00, 0x00, 0x00, 0x00, 0x64, 0, 0};
        b[15] = ismCrc8(b, 15, 0x31, 0x00);
        int s = 0;
        for (int i = 0; i < 16; i++) s += b[i];
        b[16] = (uint8_t)s;
        Bitv v;
        putBytes(v, {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x2d, 0xd4});
        put(v, b, 136);
        putStr(v, "000");
        return nrz(v, 58, true, 116);
    }
    case IsmTx::MbusT: {   // m_bus.c (mode T, format A): 54 3D sync, 3-of-6 coded L C M A(id, version, type) CRC, a data block and its CRC
        static const uint8_t enc[16] = {22, 13, 14, 11, 28, 25, 26, 19, 44, 37, 38, 35, 52, 49, 50, 41};
        uint8_t f[20] = {0x0F, 0x44, 0x2D, 0x2C, 0x78, 0x56, 0x34, 0x12, 0x1B, 0x16, 0, 0, 0x7A, (uint8_t)(k & 0xff), 0x00, 0x00, 0x00, 0x00, 0, 0};
        uint16_t c = (uint16_t)~ismCrc16(f, 10, 0x3D65, 0);
        f[10] = (uint8_t)(c >> 8); f[11] = (uint8_t)c;
        c = (uint16_t)~ismCrc16(f + 12, 6, 0x3D65, 0);
        f[18] = (uint8_t)(c >> 8); f[19] = (uint8_t)c;
        Bitv v;
        putBytes(v, {0x55, 0x55, 0x55, 0x55, 0x54, 0x3D});
        for (uint8_t x : f) {
            const uint8_t h = enc[x >> 4], l = enc[x & 0xf];
            for (int i = 5; i >= 0; i--) v.push_back((h >> i) & 1);
            for (int i = 5; i >= 0; i--) v.push_back((l >> i) & 1);
        }
        putStr(v, "0101");
        return nrz(v, 10, true, 40);
    }
    case IsmTx::KnxRf: {   // m_bus.c format A with the KNX marker: block 1 = L C FF 03 serial(6) CRC, block 2 = ctrl src dst L/NPCI TPCI APCI data CRC; S-mode chips
        uint8_t f[12 + 18] = {20, 0x44, 0xFF, 0x03, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0, 0};
        const uint8_t blk[11] = {0x29, 0x11, 0x03, 0x0A, 0x07, (uint8_t)(0x80 | 0x30 | 3), 0x00, 0x80, (uint8_t)(k & 0xff), 0x12, 0x34};
        uint16_t c = (uint16_t)~ismCrc16(f, 10, 0x3D65, 0);
        f[10] = (uint8_t)(c >> 8); f[11] = (uint8_t)c;
        memcpy(f + 12, blk, 11);
        c = (uint16_t)~ismCrc16(f + 12, 11, 0x3D65, 0);
        f[23] = (uint8_t)(c >> 8); f[24] = (uint8_t)c;
        Bitv v;
        putBytes(v, {0x55, 0x55, 0x55, 0x55, 0x54, 0x76, 0x96});
        mcPairs(v, f, 25 * 8);
        return nrz(v, 30.52f, true, 120);
    }
    case IsmTx::HomeMatic: {   // BidCoS: AA preamble, E9 CA sync, length, obfuscated bytes, CC1101 CRC-16; a weather event (type 0x70)
        const int t = 195 + (k % 2);
        uint8_t m[15] = {14, (uint8_t)(0x40 + (k & 0x3f)), 0x84, 0x70, 0x1A, 0x2B, 0x3C, 0, 0, 0, (uint8_t)(t >> 8), (uint8_t)t, 56, 0, 0};
        uint8_t e[17];
        const unsigned len = m[0];
        e[0] = m[0];
        e[1] = (uint8_t)~(m[1] ^ 0x89);
        for (unsigned l = 2; l < len; l++) e[l] = (uint8_t)(m[l] ^ (e[l - 1] + 0xDC));
        e[len] = (uint8_t)(m[len] ^ m[2]);
        const uint16_t c = ismCrc16(e, len + 1, 0x8005, 0xFFFF);
        e[len + 1] = (uint8_t)(c >> 8); e[len + 2] = (uint8_t)c;
        Bitv v;
        putBytes(v, {0xAA, 0xAA, 0xAA, 0xAA, 0xE9, 0xCA});
        put(v, e, (len + 3) * 8);
        return nrz(v, 100.08f, true, 200);
    }
    case IsmTx::EnOcean: {   // enocean_erp1.c: ASK 125 kbit/s, inverted; 4BS telegram RORG A5, 4 data bytes, sender ID, status, CRC-8; 8-of-12 coding
        uint8_t t[11] = {0xA5, 0x00, (uint8_t)(0x80 + (k % 2)), (uint8_t)(100 + k % 4), 0x08, 0x01, 0x80, 0x12, 0x34, 0x01, 0};
        t[10] = ismCrc8(t, 10, 0x07, 0);
        Bitv v = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1};
        for (int j = 0; j < 11; j++) {
            auto bit = [&](int i) { return (t[j] >> i) & 1; };
            const int c[10] = {bit(7), bit(6), bit(5), !bit(5), bit(4), bit(3), bit(2), !bit(2), bit(1), bit(0)};
            v.insert(v.end(), c, c + 10);
            v.push_back(j < 10 ? 0 : 1); v.push_back(j < 10 ? 1 : 0);
        }
        for (int i = 0; i < 4; i++) v.push_back(0);
        return nrz(inv(v), 8, false, 150);
    }
    case IsmTx::Dash7: {   // Sub-IoT dll.c / d7anp.c: foreground frame (sync 0B67), PN9-whitened, CRC-16 CCITT; VID target, UID origin, an ALP payload
        uint8_t f[40] = {0, 0xFF, (uint8_t)((3 << 6) | (10 + 32)), 0xAB, 0xCD, 0x00 | (2 << 4), 0x01, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x01, 0x02, 0x03, (uint8_t)k, 0x34, 0x56, 0, 0};
        const unsigned n = 21;                     // bytes before the CRC
        f[0] = (uint8_t)(n + 2 - 1);
        uint16_t crc = 0xFFFF;
        for (unsigned i = 0; i < n; i++) crc = ismCrc16(&f[i], 1, 0x1021, crc);
        f[n] = (uint8_t)(crc >> 8); f[n + 1] = (uint8_t)crc;
        uint16_t r = 0x1FF;
        for (unsigned i = 0; i < n + 2; i++) {
            f[i] ^= (uint8_t)r;
            for (int j = 0; j < 8; j++) r = (uint16_t)(((((r & 0x20) >> 5) ^ r) << 8 | ((r >> 1) & 0xFF)) & 0x1FF);
        }
        Bitv v;
        putBytes(v, {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x0B, 0x67});
        put(v, f, (n + 2) * 8);
        putStr(v, "0000");
        return nrz(v, 18.0f, true, 100);
    }
    case IsmTx::ZwaveR1: return zwFrame(1, 0xC0FFEE01, 2, 1, {0x25, 0x03, (uint8_t)((k % 2) ? 0xFF : 0x00)}, k);   // Switch Binary report
    case IsmTx::ZwaveR2: {   // Sensor Multilevel report: temperature, 1 decimal, 2 bytes
        const int t = 215 + (k % 2);
        return zwFrame(2, 0xDEADBEEF, 7, 1, {0x31, 0x05, 0x01, 0x22, (uint8_t)(t >> 8), (uint8_t)t}, k);
    }
    case IsmTx::ZwaveR3: {   // Meter report: electric, W, 1 decimal, 2 bytes
        const int w = 12345 + (k % 2);
        return zwFrame(3, 0x12345678, 9, 1, {0x32, 0x02, 0x01, 0x32, (uint8_t)(w >> 8), (uint8_t)w}, k);
    }
    // ---- 3.9 (decoders in ism_dec_39.cpp)
    case IsmTx::X10Rf: {   // x10_rf.c: 9 ms sync pulse, 4.5 ms gap, 32 bits PPM (562 us pulses, 562 / 1687 us gaps): house C, unit 5, on / off
        const int house = 2, u = 5 - 1;
        const int h3 = house & 1, h1 = !((house >> 2) & 1), h2 = ((house >> 1) & 1) ^ h1, h0 = h1 ^ !((house >> 3) & 1);
        const uint8_t b0 = (uint8_t)(h0 << 7 | h1 << 6 | h2 << 5 | h3 << 4 | ((u >> 3) & 1) << 2);
        const uint8_t b2 = (uint8_t)(((u >> 2) & 1) << 6 | ((u >> 1) & 1) << 3 | (u & 1) << 4 | ((k % 2) ? 0x20 : 0));
        const uint8_t b[4] = {b0, (uint8_t)~b0, b2, (uint8_t)~b2};
        Pwm w;
        for (int r = 0; r < 3; r++) {
            w.add(9000, 4500);
            for (int i = 0; i < 32; i++) w.add(562.5f, ((b[i >> 3] >> (7 - (i & 7))) & 1) ? 1687.5f : 562.5f);
            w.add(562.5f, r == 2 ? 50000 : 40000);
        }
        return w.t;
    }
    case IsmTx::Insteon: {   // insteon.c: (inverted) CE AA preamble, then per byte 2 + 10 + 16 bits: "11", index and data Manchester LSB first
        uint8_t m[10] = {0x0F, 0x3F, 0x6B, 0x22, 0x11, 0x78, 0x2B, (uint8_t)(0x11 + 2 * (k % 2)), (uint8_t)((k % 2) ? 0x00 : 0xFF), 0};
        uint8_t r = 0;
        for (int i = 0; i < 9; i++) { r ^= m[i]; r ^= (uint8_t)(((r ^ (r << 1)) & 0x0F) << 4); }
        m[9] = r;
        Bitv v;   // as the decoder sees it after inverting
        for (int i = 0; i < 12; i++) { v.push_back(1); v.push_back(0); }
        putStr(v, "1100111");
        for (int j = 0; j < 10; j++) {
            const int idx = j == 0 ? 31 : 9 - j;
            for (int i = 0; i < 5; i++) { const int d = (idx >> i) & 1; v.push_back(!d); v.push_back(d); }
            for (int i = 0; i < 8; i++) { const int d = (m[j] >> i) & 1; v.push_back(!d); v.push_back(d); }
            if (j < 9) putStr(v, "11");
        }
        putStr(v, "11000000");
        return nrz(inv(v), 109.6f, true, 300);
    }
    case IsmTx::Infactory: {   // infactory.c: 4 x 1000 us preamble pairs, 500 / 8000 us sync, 40 bits PPM (500 us pulses, 2000 / 4000 us gaps), CRC-4
        const int traw = (int)std::lround((70.2 + 0.1 * (k % 2)) * 10) + 900, hum = 55, ch = 2;
        uint8_t b[5] = {0x5C, 0, (uint8_t)(traw >> 4), (uint8_t)((traw & 0x0f) << 4 | (hum / 10)), (uint8_t)((hum % 10) << 4 | ch)};
        uint8_t m[5] = {b[0], (uint8_t)((b[1] & 0x0F) | (b[4] & 0x0F) << 4), b[2], b[3], b[4]};
        b[1] = (uint8_t)((ismCrc4(m, 4, 0x13, 0) ^ (b[4] >> 4)) << 4 | (b[1] & 0x0F));
        Pwm w;
        for (int r = 0; r < 2; r++) {
            for (int i = 0; i < 4; i++) w.add(1000, 1000);
            w.add(500, 8000);
            for (int i = 0; i < 40; i++) w.add(500, ((b[i >> 3] >> (7 - (i & 7))) & 1) ? 4000 : 2000);
            w.add(500, 16000);
        }
        return w.t;
    }
    case IsmTx::Wh31e: {   // ambientweather_wh31e.c: warm-up, aa x 6, 2d d4, YY II CT TT HH XX AA (CRC-8 0x31, SUM-8); k = 0 is a payload from its notes
        const int traw = 469 + (k % 2);
        uint8_t b[11] = {0x30, 0xC3, (uint8_t)(0x80 | (traw >> 8)), (uint8_t)traw, 0x5C, 0, 0, 0x08, 0x35, 0x44, 0x2C};
        b[5] = ismCrc8(b, 5, 0x31, 0x00);
        b[6] = (uint8_t)(b[0] + b[1] + b[2] + b[3] + b[4] + b[5]);
        Bitv v;
        putBytes(v, {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x2d, 0xd4});
        put(v, b, 88);
        putBytes(v, {0x00});
        IsmTrain t = nrz(v, 56, true, 112);
        t.p.insert(t.p.begin(), IsmPulse{1336, 1996});
        return t;
    }
    case IsmTx::Nissan: {   // tpms_nissan.c: f5 55 55 55 e, 37 bits Manchester (inverted): mode, id, (PSI + 3) * 4, 2 bits that make the checksum 0
        const int praw = (int)std::lround((32 + 0.25 * (k % 2) + 3) * 4);
        const uint32_t id = 0xABC123;
        uint8_t d[5] = {};
        for (int u = 0; u < 4; u++) {
            uint64_t v = ((uint64_t)1 << 34) | ((uint64_t)id << 10) | ((uint64_t)praw << 2) | (uint64_t)u;   // 37 bits
            for (int i = 0; i < 5; i++) d[i] = 0;
            for (int i = 0; i < 37; i++) if ((v >> (36 - i)) & 1) d[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
            uint8_t chk = 0;
            for (int i = 0; i < 4; i++) chk = (uint8_t)(chk + (d[i] >> 7) + (d[i] >> 5) + (d[i] >> 3) + (d[i] >> 1) + (uint8_t)(d[i] << 1));
            chk = (uint8_t)(chk + (d[4] >> 7) + (d[4] >> 5) + (d[4] >> 3));
            if ((~chk & 3) == 0) break;
        }
        Bitv v;
        putStr(v, "111101010101010101010101010101011110");
        for (int i = 0; i < 37; i++) { const int D = (d[i >> 3] >> (7 - (i & 7))) & 1; v.push_back(D); v.push_back(!D); }   // Manchester of ~D
        putStr(v, "1010");
        return nrz(v, 120, true, 250);
    }
    case IsmTx::GmTpms: {   // tpms_gm.c: 130 bits OOK Manchester (120 us): 48 zero bits, flags, id, kPa / 2.75, C + 60, sum; k = 0 is the frame of its notes
        uint8_t b[17] = {0, 0, 0, 0, 0, 0, 0x4c, 0x90, 0x00, 0x78, 0x49, 0x17, 0x66, 0x00, (uint8_t)(0x53 + (k % 2)), 0, 0};
        uint8_t s = 0;
        for (int i = 6; i < 15; i++) s = (uint8_t)(s + b[i]);
        b[15] = s;
        Bitv raw;
        put(raw, b, 130);
        return mcZero(raw, 120);
    }
    case IsmTx::EfergyOptical: {   // efergy_optical.c: FSK PWM (64 us = 1, 136 us = 0) after a 500 us sync: id, interval, zeros, pulse count, period, CRC-16 XMODEM
        uint8_t b[12] = {0x00, 0x1A, 0x2B, 0x50, 0, 0, 0, 0, (uint8_t)(12 + (k % 2)), 15, 0, 0};
        const uint16_t c = ismCrc16(b, 10, 0x1021, 0x0000);
        b[10] = (uint8_t)(c >> 8); b[11] = (uint8_t)c;
        IsmTrain t; t.fsk = true;
        t.p.push_back({500, 136});
        for (int i = 0; i < 96; i++) { const int d = (b[i >> 3] >> (7 - (i & 7))) & 1; t.p.push_back({d ? 64.f : 136.f, d ? 136.f : 64.f}); }
        t.p.back().gapUs = 100;
        return t;
    }
    case IsmTx::Emt7110: {   // esic_emt7110.c: aaaa 2dd4, II II II II FP PP CC CC VV UE EE XX at 9.579 kbit/s, the 12 bytes sum to 0
        const int pw = 461 + (k % 2), cur = 1234, en = 1234;
        uint8_t b[12] = {0x12, 0x34, 0x56, 0x78, (uint8_t)(0x40 | (pw >> 8)), (uint8_t)pw, (uint8_t)(cur >> 8), (uint8_t)cur, 204, (uint8_t)(en >> 8), (uint8_t)en, 0};
        uint8_t s = 0;
        for (int i = 0; i < 11; i++) s = (uint8_t)(s + b[i]);
        b[11] = (uint8_t)(0x100 - s);
        Bitv v;
        putBytes(v, {0xaa, 0xaa, 0x2d, 0xd4});
        put(v, b, 96);
        putStr(v, "0");
        return nrz(v, 104.4f, true, 30);
    }
    case IsmTx::ScmPlus: {   // scmplus.c: OOK Manchester (30 us), 16 A3 1E, endpoint type, id, consumption, tamper, CRC-16 0x1021 / 0x0971
        const uint32_t id = 68211547, cons = 6883 + (k % 2);
        uint8_t b[16] = {0x16, 0xA3, 0x1E, 0xAB, (uint8_t)(id >> 24), (uint8_t)(id >> 16), (uint8_t)(id >> 8), (uint8_t)id,
                         (uint8_t)(cons >> 24), (uint8_t)(cons >> 16), (uint8_t)(cons >> 8), (uint8_t)cons, 0x49, 0x00, 0, 0};
        const uint16_t c = ismCrc16(&b[2], 12, 0x1021, 0x0971);
        b[14] = (uint8_t)(c >> 8); b[15] = (uint8_t)c;
        Bitv raw;
        put(raw, b, 128);
        return mcZero(raw, 30);
    }
    default: return IsmTrain();
    }
}

IsmTruth ismTruth(IsmTx p, int k) {
    switch (p) {
    case IsmTx::Nexus: return {"Nexus-TH", "90", "Temperature", nexusT(k), 0.05};
    case IsmTx::Tx141th: return {"LaCrosse-TX141THBv2", "3c", "Temperature", tx141T(k), 0.05};
    case IsmTx::AcuriteTower: return {"Acurite-Tower", "4660", "Temperature", acuT(k), 0.05};
    case IsmTx::Wh1080: return {"Fineoffset-WHx080", "123", "Temperature", wh1080T(k), 0.05};
    case IsmTx::F007th: return {"Ambientweather-F007TH", "156", "Temperature", f007F(k), 0.05};
    case IsmTx::OregonV3: return {"Oregon-THGR810", "165", "Temperature", os3T(k), 0.05};
    case IsmTx::OregonV21: return {"Oregon-THGR228N", "62", "Temperature", os21T(k), 0.05};
    case IsmTx::Schrader: return {"Schrader", "1A2B3C4", "Pressure", (92 + (k % 2)) * 2.5, 0.01};
    case IsmTx::Ev1527: return {"Generic-Remote", "23100", "Command", 129, 0.01};
    case IsmTx::Toyota: return {"Toyota", "a1b2c3d4", "Pressure", (156 + (k % 2)) * 0.25 - 7.0, 0.01};
    case IsmTx::Renault: return {"Renault", "c0ffee", "Pressure", (333 + (k % 2)) * 0.75, 0.01};
    case IsmTx::Ford: return {"Ford", "12ab34cd", "Pressure", (140 + (k % 2)) * 0.25, 0.01};
    case IsmTx::Efergy: return {"Efergy-e2CT", "1234", "Current", ((0x58 + (k % 2)) << 8) / 4096.0, 0.001};
    case IsmTx::CurrentCost: return {"CurrentCost-TX", "1957", "Power 0", 1234.0 + (k % 2), 0.01};
    case IsmTx::Bresser6in1: return {"Bresser-6in1", "12345678", "Temperature", 15.6 + 0.1 * (k % 3), 0.05};
    case IsmTx::Bresser5in1: return {"Bresser-5in1", "108", "Temperature", 8.7 + 0.1 * (k % 2), 0.05};
    case IsmTx::Tx29: return {"LaCrosse-TX29IT", "43", "Temperature", 21.7 + 0.1 * (k % 2), 0.05};
    case IsmTx::Wh24: return {"Fineoffset-WH24", "191", "Temperature", 11.8 + 0.1 * (k % 2), 0.05};
    case IsmTx::MbusT: return {"Wireless-MBus", "12345678", "Version", 27, 0.01};
    case IsmTx::KnxRf: return {"KNX-RF", "000102030405", "Hop count", 3, 0.01};
    case IsmTx::HomeMatic: return {"HomeMatic-BidCoS", "1A2B3C", "Temperature", 19.5 + 0.1 * (k % 2), 0.05};
    case IsmTx::EnOcean: return {"EnOcean-ERP1", "01801234", "Repeaters", 1, 0.01};
    case IsmTx::Dash7: return {"DASH7", "4041424344454647", "EIRP", 10, 0.01};
    case IsmTx::ZwaveR1: return {"Z-Wave-R1", "C0FFEE01:2", "Command Class", 0x25, 0.01};
    case IsmTx::ZwaveR2: return {"Z-Wave-R2", "DEADBEEF:7", "Temperature", 21.5 + 0.1 * (k % 2), 0.05};
    case IsmTx::ZwaveR3: return {"Z-Wave-R3", "12345678:9", "Power", 1234.5 + 0.1 * (k % 2), 0.05};
    case IsmTx::X10Rf: return {"X10-RF", "5", "Command code", (k % 2) ? 96.0 : 64.0, 0.01};
    case IsmTx::Insteon: return {"Insteon", "2B7811", "Cmd1", 0x11 + 2.0 * (k % 2), 0.01};
    case IsmTx::Infactory: return {"inFactory-TH", "92", "Temperature", (70.2 + 0.1 * (k % 2) - 32) / 1.8, 0.05};
    case IsmTx::Wh31e: return {"AmbientWeather-WH31E", "195", "Temperature", 6.9 + 0.1 * (k % 2), 0.05};
    case IsmTx::Nissan: return {"Nissan", "abc123", "Pressure", 32 + 0.25 * (k % 2), 0.01};
    case IsmTx::GmTpms: return {"GM-Aftermarket", "2018056038", "Temperature", 23.0 + (k % 2), 0.01};
    case IsmTx::EfergyOptical: return {"Efergy-Optical", "6699", "Pulse count", 12.0 + (k % 2), 0.01};
    case IsmTx::Emt7110: return {"ESIC-EMT7110", "12345678", "Power", 230.5 + 0.5 * (k % 2), 0.01};
    case IsmTx::ScmPlus: return {"SCMplus", "68211547", "Consumption", 6883.0 + (k % 2), 0.01};
    default: return {"", "", "", 0, 0};
    }
}

bool ismTruthMatches(const IsmReading& r, IsmTx* which, int* kk) {
    for (int p = 0; p < (int)IsmTx::Count; p++) {
        for (int k = 0; k < 4; k++) {
            const IsmTruth t = ismTruth((IsmTx)p, k);
            if (r.model != t.model || r.id != t.id) continue;
            for (const auto& f : r.fields)
                if (f.name == t.field && std::fabs(f.value - t.value) <= t.tol) { if (which) *which = (IsmTx)p; if (kk) *kk = k; return true; }
        }
    }
    return false;
}

// ---------------------------------------------------------------- the RF scene

namespace {
double defaultDev(IsmTx p) {
    switch (p) {
    case IsmTx::MbusT: return 50e3;
    case IsmTx::ZwaveR1: case IsmTx::ZwaveR2: return 20e3;
    case IsmTx::ZwaveR3: return 29e3;
    case IsmTx::KnxRf: return 50e3;
    case IsmTx::HomeMatic: return 20e3;
    case IsmTx::Dash7: return 50e3;
    case IsmTx::Bresser6in1: case IsmTx::Bresser5in1: return 40e3;
    case IsmTx::Efergy: case IsmTx::CurrentCost: return 20e3;
    default: return 30e3;
    }
}
}

struct IsmGenerator::Impl {
    IsmGenConfig c;
    std::mt19937 rng;
    dect2::normal_distribution<float> g{0.f, 1.f};
    float sigma = 0;
    uint64_t n = 0;
    double carrPh = 0;
    struct Seg { uint64_t len; int level; };   // level: 0 off, 1 mark, 2 space
    struct Burst { size_t dev; std::vector<Seg> segs; size_t si = 0; uint64_t pos = 0; double ph = 0, amp = 0, ampS = 0, fS = 0; bool fsk = false; };
    std::vector<Burst> active;
    std::vector<int> count;
    std::vector<double> next;

    explicit Impl(const IsmGenConfig& cfg) : c(cfg), rng(cfg.seed) {
        const double a = c.amp;
        sigma = c.snrDb >= 99 ? 0.f : (float)std::sqrt(a * a * c.rate / (2 * 1e5 * std::pow(10.0, c.snrDb / 10)));
        count.assign(c.devs.size(), 0);
        for (const auto& d : c.devs) next.push_back(d.phase);
    }

    void start(size_t di) {
        const IsmGenDevice& d = c.devs[di];
        const IsmTrain t = ismEncode(d.proto, count[di]++);
        Burst b; b.dev = di; b.fsk = t.fsk;
        const double sp = c.rate / 1e6;
        if (t.fsk) b.segs.push_back({(uint64_t)(100 * sp), 2});
        uint64_t total = 0;
        for (size_t i = 0; i < t.p.size(); i++) {
            b.segs.push_back({(uint64_t)std::llround(t.p[i].pulseUs * sp), 1});
            const bool lastOok = !t.fsk && i + 1 == t.p.size();
            if (!lastOok) b.segs.push_back({(uint64_t)std::llround(t.p[i].gapUs * sp), t.fsk ? 2 : 0});
        }
        for (const auto& s : b.segs) total += s.len;
        if (d.truncate < 1) {   // cut the burst off
            uint64_t keep = (uint64_t)(total * std::max(0.0, d.truncate)), acc = 0;
            std::vector<Seg> o;
            for (const auto& s : b.segs) { if (acc >= keep) break; o.push_back({std::min(s.len, keep - acc), s.level}); acc += o.back().len; }
            b.segs = o;
        }
        b.amp = c.amp * std::pow(10.0, d.levelDb / 20);
        b.ph = dect2::uniform_real_distribution<double>(0, 2 * M_PI)(rng);
        active.push_back(b);
    }

    void gen(cf32* out, size_t len) {
        const double carrA = c.carrierDb != 0 ? c.amp * std::pow(10.0, c.carrierDb / 20) : 0.0;
        const double kEdge = 1 - std::exp(-1.0 / (1.5e-6 * c.rate)), kF = 1 - std::exp(-1.0 / (3e-6 * c.rate));
        for (size_t i = 0; i < len; i++, n++) {
            const double ts = (double)n / c.rate;
            for (size_t di = 0; di < c.devs.size(); di++)
                if (ts >= next[di]) { start(di); next[di] += c.devs[di].period; }
            std::complex<double> v(0, 0);
            for (auto& b : active) {
                const IsmGenDevice& d = c.devs[b.dev];
                int lvl = 0;
                while (b.si < b.segs.size() && b.pos >= b.segs[b.si].len) { b.pos = 0; b.si++; }
                if (b.si < b.segs.size()) { lvl = b.segs[b.si].level; b.pos++; }
                const double dev = d.devHz > 0 ? d.devHz : defaultDev(d.proto);
                const double targetA = lvl == 0 ? 0.0 : b.amp;
                const double targetF = b.fsk ? (lvl == 1 ? dev : -dev) : 0.0;
                b.ampS += kEdge * (targetA - b.ampS);
                b.fS += kF * (targetF - b.fS);
                const double f = c.dialOffsetHz + d.offsetHz + c.cfoHz + b.fS;
                b.ph += 2 * M_PI * f / c.rate;
                if (b.ph > M_PI) b.ph -= 2 * M_PI; else if (b.ph < -M_PI) b.ph += 2 * M_PI;
                v += std::polar(b.ampS, b.ph);
            }
            active.erase(std::remove_if(active.begin(), active.end(), [](const Burst& b) { return b.si >= b.segs.size() && b.ampS < 1e-6; }), active.end());
            if (carrA > 0) {
                carrPh += 2 * M_PI * (c.dialOffsetHz + c.carrierHz + c.cfoHz) / c.rate;
                if (carrPh > M_PI) carrPh -= 2 * M_PI;
                v += std::polar(carrA, carrPh);
            }
            out[i] = cf32{(float)v.real() + sigma * g(rng), (float)v.imag() + sigma * g(rng)};
        }
    }
};

IsmGenerator::IsmGenerator(const IsmGenConfig& c) : p_(std::make_unique<Impl>(c)) {}
IsmGenerator::~IsmGenerator() = default;
void IsmGenerator::generate(cf32* out, size_t n) { p_->gen(out, n); }
double IsmGenerator::timeSec() const { return (double)p_->n / p_->c.rate; }
int IsmGenerator::sent(size_t dev) const { return dev < p_->count.size() ? p_->count[dev] : 0; }

std::vector<IsmGenDevice> ismTestScene(int scene) {
    std::vector<IsmGenDevice> v;
    auto add = [&](IsmTx p, double off, double phase, double period) { IsmGenDevice d; d.proto = p; d.offsetHz = off; d.phase = phase; d.period = period; v.push_back(d); };
    if (scene == 2) {   // dial 868.4 MHz: Z-Wave R1 / R2 / R3 on 868.38 / 868.42 / 868.40 (the EU channels are 868.40, 868.42 and 869.85)
        add(IsmTx::ZwaveR1, -20e3, 0.3, 5);
        add(IsmTx::ZwaveR2, 20e3, 1.5, 5);
        add(IsmTx::ZwaveR3, 0, 2.7, 5);
        return v;
    }
    if (scene == 3) {   // dial 868.3 MHz: KNX RF and HomeMatic on the 868.3 MHz sub-band, with the drift of cheap radios
        add(IsmTx::KnxRf, -25e3, 0.4, 5);
        add(IsmTx::HomeMatic, 30e3, 2.2, 5);
        add(IsmTx::EnOcean, 70e3, 3.6, 5);
        add(IsmTx::Dash7, -80e3, 1.3, 5);
        return v;
    }
    if (scene == 4) {   // the 3.9 devices side by side (their real bands differ; here only the decoders matter)
        add(IsmTx::X10Rf, -60e3, 0.2, 6);
        add(IsmTx::Insteon, 40e3, 1.0, 6);
        add(IsmTx::Infactory, -20e3, 1.6, 6);
        add(IsmTx::Wh31e, 70e3, 2.6, 6);
        add(IsmTx::Nissan, -40e3, 3.0, 6);
        add(IsmTx::GmTpms, 10e3, 3.4, 6);
        add(IsmTx::EfergyOptical, -80e3, 3.8, 6);
        add(IsmTx::Emt7110, 25e3, 4.2, 6);
        add(IsmTx::ScmPlus, -5e3, 4.6, 6);
        return v;
    }
    if (scene == 1) {   // dial 868.3 MHz
        add(IsmTx::Bresser6in1, 0, 0.3, 5);
        add(IsmTx::Bresser5in1, 20e3, 1.2, 5);
        add(IsmTx::Tx29, -15e3, 2.1, 5);
        add(IsmTx::Wh24, 30e3, 3.0, 5);
        add(IsmTx::MbusT, 650e3, 3.9, 5);   // 868.95 MHz
        return v;
    }
    // dial 433.92 MHz: one slot each in a 10 s cycle (all sent within 5.1 s), with the drift of cheap sensors; Efergy on its usual 433.55 MHz
    add(IsmTx::Nexus, -30e3, 0.20, 10);
    add(IsmTx::Tx141th, 40e3, 1.30, 10);
    add(IsmTx::AcuriteTower, -50e3, 1.90, 10);
    add(IsmTx::Wh1080, 15e3, 2.20, 10);
    add(IsmTx::F007th, 48e3, 2.50, 10);
    add(IsmTx::OregonV3, -10e3, 2.80, 10);
    add(IsmTx::OregonV21, 25e3, 3.10, 10);
    add(IsmTx::Schrader, -45e3, 3.40, 10);
    add(IsmTx::Ev1527, 0, 3.60, 10);
    add(IsmTx::Toyota, 20e3, 4.20, 10);
    add(IsmTx::Renault, -35e3, 4.40, 10);
    add(IsmTx::Ford, 45e3, 4.60, 10);
    add(IsmTx::Efergy, -370e3, 4.80, 10);
    add(IsmTx::CurrentCost, 5e3, 5.00, 10);
    return v;
}

namespace {
class IsmSynth : public ModeSynth {
public:
    IsmSynth(const SynthConfig& s, double rate) : rate_(rate) {
        IsmGenConfig c;
        c.rate = rate;
        c.dialOffsetHz = -ismTuning().tuneOffsetHz;
        c.snrDb = s.snrDb;
        c.cfoHz = s.cfoHz;
        c.devs = ismTestScene(s.modeOpt[0] >= 1 && s.modeOpt[0] <= 4 ? s.modeOpt[0] : 0);
        if (s.modeOpt[1] == 1) { c.carrierHz = 300e3; c.carrierDb = 30; }
        gen_ = std::make_unique<IsmGenerator>(c);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { gen_->generate(out, n); }
private:
    double rate_;
    std::unique_ptr<IsmGenerator> gen_;
};
} // namespace

std::unique_ptr<ModeSynth> makeIsmSynth(const SynthConfig& cfg, double sampleRate) {
    if (sampleRate <= 0) return nullptr;
    return std::make_unique<IsmSynth>(cfg, sampleRate);
}

} // namespace dect2
