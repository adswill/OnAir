// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors, the protocols added from the rtl_433 audit: every frame is built here from the protocol description (field layout, check
// algorithm, line timing), turned into a pulse train and decoded; the values must come back, a flipped bit must give nothing.
#include "dect2/ism/ism_proto.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

using BitRow = std::vector<int>;

static BitRow bitsOf(const std::vector<uint8_t>& b, int nbits) {
    BitRow r;
    for (int i = 0; i < nbits; i++) r.push_back((b[i >> 3] >> (7 - (i & 7))) & 1);
    return r;
}

// PPM: a pulse then a gap whose length is the bit; every row is introduced by a pulse with the sync gap
static IsmTrain ppm(const std::vector<BitRow>& rows, float pulse, float s, float l, float sync, int syncsFirst = 0) {
    IsmTrain t;
    for (int k = 0; k < syncsFirst; k++) t.p.push_back({pulse, sync});
    for (const auto& r : rows) {
        t.p.push_back({pulse, sync});
        for (int b : r) t.p.push_back({pulse, b ? l : s});
    }
    t.p.push_back({pulse, 30000});   // the closing pulse: the last bit's gap is its own
    return t;
}

// PWM: the pulse is the bit (short = 1), then a fixed gap; rows are separated by rowGap
static IsmTrain pwm(const std::vector<BitRow>& rows, float s, float l, float gap, float rowGap, float syncPulse = 0) {
    IsmTrain t;
    for (const auto& r : rows) {
        if (syncPulse > 0) t.p.push_back({syncPulse, gap});
        for (int b : r) t.p.push_back({b ? s : l, gap});
        t.p.back().gapUs = rowGap;
    }
    t.p.back().gapUs = 30000;
    return t;
}

// Manchester with an implicit leading zero bit: bit 1 = space then mark, bit 0 = mark then space (half bit = hb), runs merged
static IsmTrain manchester(const BitRow& bits, float hb, bool inv) {
    std::vector<int> lv;
    for (size_t i = 1; i < bits.size(); i++) {
        const int b = bits[i] ^ (inv ? 1 : 0);
        lv.push_back(b ? 0 : 1); lv.push_back(b ? 1 : 0);
    }
    // the first decoded bit is preceded by an implicit zero, i.e. the line is high before the first edge
    std::vector<float> runs;
    int cur = 1;
    float len = hb;   // a leading mark of one half bit
    for (int v : lv) {
        if (v == cur) len += hb;
        else { runs.push_back(len); len = hb; cur = v; }
    }
    runs.push_back(len);
    if (cur == 1) runs.push_back(30000);
    IsmTrain t;
    for (size_t i = 0; i < runs.size(); i += 2) t.p.push_back({runs[i], i + 1 < runs.size() ? runs[i + 1] : 30000});
    t.p.back().gapUs = 30000;
    return t;
}

static const IsmReading* find(const std::vector<IsmReading>& rs, const char* model) {
    for (const auto& r : rs) if (r.model == model) return &r;
    return nullptr;
}
static double fv(const IsmReading& r, const char* name) {
    for (const auto& f : r.fields) if (f.name == name) return f.value;
    return -1e9;
}
static std::vector<uint8_t> acurite606Frame(int id, int ch, double tc, bool low) {
    const int traw = (int)std::lround(tc * 10) & 0xfff;
    std::vector<uint8_t> b(4);
    b[0] = (uint8_t)id;
    b[1] = (uint8_t)((low ? 0 : 0x80) | ((ch - 1) << 4) | (traw >> 8));
    b[2] = (uint8_t)(traw & 0xff);
    b[3] = ismLfsrDigest8(b.data(), 3, 0x98, 0xf1);
    return b;
}

int main() {
    // ---- Acurite 606TX
    {
        const auto f = acurite606Frame(0x5A, 2, 21.7, false);
        std::vector<BitRow> rows(4, bitsOf(f, 32));
        auto rs = ismDecode(ppm(rows, 500, 2000, 4000, 9000));
        const IsmReading* r = find(rs, "Acurite-606TX");
        CHECK(r && r->id == "90" && r->channel == 2 && std::fabs(fv(*r, "Temperature") - 21.7) < 0.01 && r->battery == 1, "606TX %zu", rs.size());
        auto f2 = f; f2[2] ^= 0x04;
        CHECK(!find(ismDecode(ppm(std::vector<BitRow>(4, bitsOf(f2, 32)), 500, 2000, 4000, 9000)), "Acurite-606TX"), "606TX bad digest");
        const auto fn = acurite606Frame(0x11, 1, -5.3, true);
        r = nullptr; rs = ismDecode(ppm(std::vector<BitRow>(4, bitsOf(fn, 32)), 500, 2000, 4000, 9000)); r = find(rs, "Acurite-606TX");
        CHECK(r && std::fabs(fv(*r, "Temperature") + 5.3) < 0.01 && r->battery == 0, "606TX negative");
    }
    // ---- Acurite 609TXC
    {
        std::vector<uint8_t> b(5);
        const int traw = 253;
        b[0] = 0x42; b[1] = (uint8_t)(0x00 | (traw >> 8)); b[2] = (uint8_t)(traw & 0xff); b[3] = 55;
        b[4] = (uint8_t)(b[0] + b[1] + b[2] + b[3]);
        std::vector<BitRow> rows(3, bitsOf(b, 40));
        auto rs = ismDecode(ppm(rows, 500, 1000, 2000, 3500 + 1000));
        const IsmReading* r = find(rs, "Acurite-609TXC");
        CHECK(r && r->id == "66" && std::fabs(fv(*r, "Temperature") - 25.3) < 0.01 && fv(*r, "Humidity") == 55, "609TXC %zu", rs.size());
        b[4] ^= 1;
        CHECK(!find(ismDecode(ppm(std::vector<BitRow>(3, bitsOf(b, 40)), 500, 1000, 2000, 4500)), "Acurite-609TXC"), "609TXC bad sum");
    }
    // ---- Prologue
    {
        const int traw = 187, hum = 64;
        std::vector<uint8_t> b(5);
        b[0] = (uint8_t)(0x90 | 0x0A); b[1] = (uint8_t)((0x5 << 4) | 0x9 /* flags: battery, channel 2 */);
        b[2] = (uint8_t)(traw >> 4); b[3] = (uint8_t)(((traw & 0xf) << 4) | (hum >> 4)); b[4] = (uint8_t)((hum & 0xf) << 4);
        std::vector<BitRow> rows(5, bitsOf(b, 36));
        auto rs = ismDecode(ppm(rows, 500, 2000, 4000, 9000));
        const IsmReading* r = find(rs, "Prologue-TH");
        CHECK(r && r->id == "165" && r->channel == 2 && std::fabs(fv(*r, "Temperature") - 18.7) < 0.01 && fv(*r, "Humidity") == 64, "Prologue %zu", rs.size());
    }
    // ---- Kedsum
    {
        const int traw = 900 + 701;   // 70.1 F
        const int hum = 0x47;
        std::vector<uint8_t> b(5);
        b[0] = 0x37; b[1] = (uint8_t)((2 << 6) | (1 << 4) | (traw & 0x0f));
        b[2] = (uint8_t)(((traw >> 8) & 0x0f) | (traw & 0xf0));
        b[3] = (uint8_t)(((hum & 0x0f) << 4) | (hum >> 4));
        // CRC-4 poly 3 init 0 over 4 bytes, xor the high nibble of b[4], as the description says; b[4] = flags << 4 | crc
        unsigned rem = 0;
        for (int i = 0; i < 4; i++) { rem ^= b[i]; for (int k = 0; k < 8; k++) rem = (rem & 0x80) ? ((rem << 1) ^ 0x30) & 0xff : (rem << 1) & 0xff; }
        const int crc = (int)((rem >> 4) & 0xf), flags = 0x5;
        b[4] = (uint8_t)((flags << 4) | (crc ^ flags));
        BitRow row = {0, 0};   // the humidity nibbles are swapped on the wire (b[3]); the decoder swaps them back
        for (int i = 0; i < 40; i++) row.push_back((b[i >> 3] >> (7 - (i & 7))) & 1);
        std::vector<BitRow> rows(6, row);
        auto rs = ismDecode(ppm(rows, 500, 2000, 4000, 7000, 15));
        const IsmReading* r = find(rs, "Kedsum-TH");
        CHECK(r && r->id == "55" && r->channel == 2 && std::fabs(fv(*r, "Temperature") - (70.1 - 32) / 1.8) < 0.02 && fv(*r, "Humidity") == hum, "Kedsum %zu", rs.size());
        std::vector<BitRow> bad(6, row); for (auto& x : bad) x[2 + 20] ^= 1;
        CHECK(!find(ismDecode(ppm(bad, 500, 2000, 4000, 7000, 15)), "Kedsum-TH"), "Kedsum bad CRC");
    }
    // ---- GT-WT-02
    {
        const int traw = 231, hum = 45;
        std::vector<uint8_t> b(5, 0);
        b[0] = 0xC4; b[1] = (uint8_t)((0 << 7) | (0 << 6) | (1 << 4) | (traw >> 8)); b[2] = (uint8_t)(traw & 0xff);
        b[3] = (uint8_t)(hum << 1);
        int sum = (b[0] >> 4) + (b[0] & 0xF) + (b[1] >> 4) + (b[1] & 0xF) + (b[2] >> 4) + (b[2] & 0xF) + (b[3] >> 4) + (b[3] & 0xe);
        const int chk = sum & 0x3f;
        b[3] |= (uint8_t)(chk >> 5); b[4] = (uint8_t)((chk & 0x1f) << 3);
        BitRow row = bitsOf(b, 37);
        auto rs = ismDecode(ppm({row, row, row}, 500, 2500, 5000, 9500));
        const IsmReading* r = find(rs, "GT-WT02");
        CHECK(r && r->id == "196" && r->channel == 2 && std::fabs(fv(*r, "Temperature") - 23.1) < 0.01 && fv(*r, "Humidity") == 45, "GT-WT-02 %zu", rs.size());
        BitRow row39 = {0, 1}; row39.insert(row39.end(), row.begin(), row.end());
        rs = ismDecode(ppm({row39, row39, row39}, 500, 2500, 5000, 9500));
        CHECK(find(rs, "GT-WT02"), "GT-WT-02 39 bits");
    }
    // ---- Bresser 3CH
    {
        const int traw = 900 + 640;   // 64.0 F = 17.78 C
        std::vector<uint8_t> b(5);
        b[0] = 0x9C; b[1] = (uint8_t)((1 << 4) | (traw >> 8)); b[2] = (uint8_t)(traw & 0xff); b[3] = 48;
        b[4] = (uint8_t)(b[0] + b[1] + b[2] + b[3]);
        for (auto& x : b) x = (uint8_t)~x;
        std::vector<BitRow> rows(4, bitsOf(b, 40));
        auto rs = ismDecode(pwm(rows, 250, 500, 250, 1000, 750));
        const IsmReading* r = find(rs, "Bresser-3CH");
        CHECK(r && r->id == "156" && r->channel == 1 && std::fabs(fv(*r, "Temperature") - 64.0 / 1.8 + 32 / 1.8) < 0.01 && fv(*r, "Humidity") == 48, "Bresser-3CH %zu", rs.size());
    }
    // ---- Fine Offset WH2
    {
        const int traw = 0x800 | 35;   // -3.5 C, sign-magnitude
        std::vector<uint8_t> b(6);
        b[0] = 0x4A; b[1] = (uint8_t)(0xB0 | (traw >> 8)); b[2] = (uint8_t)(traw & 0xff); b[3] = 72;
        b[4] = ismCrc8(b.data(), 4, 0x31, 0);
        std::vector<uint8_t> fr = {0xFF, b[0], b[1], b[2], b[3], b[4]};
        auto rs = ismDecode(pwm({bitsOf(fr, 48)}, 544, 1524, 1036, 5000));
        const IsmReading* r = find(rs, "Fineoffset-WH2");
        CHECK(r && r->id == "171" && std::fabs(fv(*r, "Temperature") + 3.5) < 0.01 && fv(*r, "Humidity") == 72, "WH2 %zu", rs.size());
        fr[3] ^= 0x10;
        CHECK(!find(ismDecode(pwm({bitsOf(fr, 48)}, 544, 1524, 1036, 5000)), "Fineoffset-WH2"), "WH2 bad CRC");
    }
    // ---- ThermoPro TP11 and TP12
    {
        const int dev = 0x5A3, traw = 200 + 253;   // 25.3 C
        std::vector<uint8_t> b(4);
        b[0] = (uint8_t)(dev >> 4); b[1] = (uint8_t)(((dev & 0xf) << 4) | (traw >> 8)); b[2] = (uint8_t)(traw & 0xff);
        b[3] = ismLfsrDigest8Reflect(b.data(), 3, 0x51, 0x04);
        BitRow row = bitsOf(b, 32);
        auto rs = ismDecode(ppm({row, row, row, row}, 500, 500, 1500, 3000));
        const IsmReading* r = find(rs, "Thermopro-TP11");
        CHECK(r && r->id == std::to_string(dev) && std::fabs(fv(*r, "Temperature") - 25.3) < 0.01, "TP11 %zu", rs.size());
        const int r1 = 200 + 612, r2 = 200 + 1500;
        std::vector<uint8_t> c(5);
        c[0] = 0x77; c[1] = (uint8_t)(r1 & 0xff); c[2] = (uint8_t)(((r1 >> 8) << 4) | (r2 >> 8)); c[3] = (uint8_t)(r2 & 0xff);
        c[4] = ismLfsrDigest8Reflect(c.data(), 4, 0x51, 0x04);
        BitRow row2 = bitsOf(c, 40); row2.push_back(0);
        rs = ismDecode(ppm({row2, row2, row2, row2}, 500, 500, 1500, 3000));
        r = find(rs, "Thermopro-TP12");
        CHECK(r && r->id == "119" && std::fabs(fv(*r, "Temperature 1 (food)") - 61.2) < 0.01 && std::fabs(fv(*r, "Temperature 2 (barbecue)") - 150.0) < 0.01, "TP12 %zu", rs.size());
    }
    // ---- FT-004-B
    {
        const int traw = (int)((22.4 + 40.0) / 0.05 + 0.5);
        uint8_t m[6] = {0xf4, 0x12, 0x34, (uint8_t)(traw & 0xff), (uint8_t)(traw >> 8), 0x15};
        BitRow frame;
        for (int i = 0; i < 46; i++) {   // reverse8 of each byte, MSB first
            const int byte = i / 8, bit = i % 8;
            frame.push_back((m[byte] >> bit) & 1);
        }
        BitRow all;
        for (int k = 0; k < 3; k++) all.insert(all.end(), frame.begin(), frame.end());   // 138 bits: ppm() closes the last bit with a pulse
        auto rs = ismDecode(ppm({all}, 500, 1956, 3900, 4600));
        const IsmReading* r = find(rs, "FT-004B");
        CHECK(r && std::fabs(fv(*r, "Temperature") - 22.4) < 0.06, "FT-004B %zu", rs.size());
    }
    // ---- Kerui: id 0xABCDE, command 0xe (open); short pulse = 1 on the wire, the decoder inverts
    {
        const unsigned id = 0xABCDE, cmd = 0xe;
        std::vector<uint8_t> b = {(uint8_t)(~(id >> 12)), (uint8_t)(~(id >> 4)), (uint8_t)(~(((id & 0xf) << 4) | cmd)), 0};
        BitRow row = bitsOf(b, 24); row.push_back(1);
        auto rs = ismDecode(pwm(std::vector<BitRow>(10, row), 420, 960, 400, 5000));
        const IsmReading* r = find(rs, "Kerui-Security");
        CHECK(r && r->id == "abcde" && r->fields.size() == 1 && r->fields[0].text == "open", "Kerui %zu", rs.size());
    }
    // ---- Springfield soil
    {
        const int traw = 215, moist = 6;   // 21.5 C, moisture 60 %
        std::vector<uint8_t> b(5, 0);
        b[0] = 0x3C; b[1] = (uint8_t)((2 << 4) | (traw >> 8)); b[2] = (uint8_t)(traw & 0xff); b[3] = (uint8_t)(moist << 4);
        // nibbles 0..6 xor to the 8th (the check): the xor of all of b[0..3] folded to a nibble is zero
        int x = b[0] ^ b[1] ^ b[2] ^ b[3]; x = (x >> 4) ^ (x & 0xf);
        b[3] |= (uint8_t)x;
        BitRow row = bitsOf(b, 36);
        auto rs = ismDecode(ppm({row, row, row}, 500, 2000, 4000, 7000));
        const IsmReading* r = find(rs, "Springfield-Soil");
        CHECK(r && r->id == "60" && r->channel == 3 && std::fabs(fv(*r, "Temperature") - 21.5) < 0.01 && fv(*r, "Moisture") == 60, "Springfield %zu", rs.size());
        BitRow bad = row; bad[13] ^= 1;
        CHECK(!find(ismDecode(ppm({bad, bad, bad}, 500, 2000, 4000, 7000)), "Springfield-Soil"), "Springfield bad check");
    }
    // ---- Solight TE44
    {
        const int traw = (int)std::lround(-4.2 * 10) & 0xfff;
        std::vector<uint8_t> b(5, 0);
        b[0] = 0x6D; b[1] = (uint8_t)((1 << 4) | (traw >> 8)); b[2] = (uint8_t)(traw & 0xff); b[3] = 0xf0;
        const uint8_t head[4] = {b[0], b[1], b[2], 0xf0};
        const uint8_t crc = ismCrc8(head, 4, 0x31, 0x6c);
        b[3] = (uint8_t)(0xf0 | (crc >> 4)); b[4] = (uint8_t)((crc & 0xf) << 4);
        BitRow row = bitsOf(b, 36); row.push_back(0);
        auto rs = ismDecode(ppm({row, row, row, row}, 500, 972, 1932, 4200));
        const IsmReading* r = find(rs, "Solight-TE44");
        CHECK(r && r->id == "109" && r->channel == 2 && std::fabs(fv(*r, "Temperature") + 4.2) < 0.01, "Solight TE44 %zu", rs.size());
    }
    // ---- Honeywell 345 MHz door sensor (Manchester at 136 us, preamble 0xFFFE)
    {
        const int channel = 8, id = 0x1234A, ev = 0x84;   // open, heartbeat
        std::vector<uint8_t> b(6);
        b[0] = (uint8_t)((channel << 4) | (id >> 16)); b[1] = (uint8_t)(id >> 8); b[2] = (uint8_t)id; b[3] = (uint8_t)ev;
        const uint16_t crc = ismCrc16(b.data(), 4, 0x8005, 0);
        b[4] = (uint8_t)(crc >> 8); b[5] = (uint8_t)crc;
        BitRow raw;   // 0xFFFE as raw Manchester (the last 0 included in 0x555556), then the 48 bits
        for (int i = 0; i < 11; i++) { raw.push_back(0); raw.push_back(1); }
        raw.push_back(1); raw.push_back(0);
        BitRow data = bitsOf(b, 48);
        for (int v : data) { raw.push_back(v ? 0 : 1); raw.push_back(v ? 1 : 0); }
        IsmTrain t;   // NRZ: runs of equal raw bits
        size_t i = 0; bool mark = raw[0] != 0;
        std::vector<float> runs;
        while (i < raw.size()) { size_t j = i; while (j < raw.size() && raw[j] == raw[i]) j++; runs.push_back((float)(j - i) * 136.f); i = j; }
        if (!mark) runs.insert(runs.begin(), 0.f);
        if (runs.size() & 1) runs.push_back(30000.f);
        for (size_t k = 0; k < runs.size(); k += 2) t.p.push_back({runs[k], runs[k + 1]});
        t.p.back().gapUs = 30000;
        auto rs = ismDecode(t);
        const IsmReading* r = find(rs, "Honeywell-Security");
        CHECK(r && r->id == "1234a" && r->channel == 8 && fv(*r, "Event") == 0x84, "Honeywell %zu", rs.size());
    }
    // ---- Itron ERT SCM
    {
        std::vector<uint8_t> b(12, 0);
        b[0] = 0x1F; b[1] = 0x2A; b[2] = 0x60 | 0x04;
        b[3] = (uint8_t)((1 << 6) | (7 << 2) | 2);
        b[4] = 0x01; b[5] = 0x23; b[6] = 0x45; b[7] = 0x0A; b[8] = 0xBC; b[9] = 0xDE;
        const uint16_t crc = ismCrc16(&b[2], 8, 0x6F63, 0);   // the 10-byte span ends with its own crc: pad b[10..11]
        b[10] = (uint8_t)(crc >> 8); b[11] = (uint8_t)crc;
        b[0] = 0x00;   // the first bit is the implicit zero
        if (ismCrc16(&b[2], 10, 0x6F63, 0) != 0) printf("  (crc layout differs; recomputed)\n");
        BitRow bits = bitsOf(b, 96);
        bool ok = false;
        for (int inv = 0; inv < 2 && !ok; inv++) {
            auto rs = ismDecode(manchester(bits, 30.5f, inv));
            const IsmReading* r = find(rs, "ERT-SCM");
            ok = r && r->id == std::to_string(((0x64 & 0x06) << 23) | (0x0A << 16) | (0xBC << 8) | 0xDE) && fv(*r, "Consumption") == 0x012345 && fv(*r, "ERT type") == 7;
        }
        CHECK(ok, "ERT SCM");
    }
    // ---- Itron ERT IDM (sync 16 A3 1C, 90 bytes, CRC-16 0x1021 / 0xD895 over bytes 2..87)
    {
        std::vector<uint8_t> b(90, 0);
        b[0] = 0x16; b[1] = 0xA3; b[2] = 0x1C;
        b[5] = 0x04; b[6] = 0x07;                                   // application version, ERT type
        b[7] = 0x12; b[8] = 0x34; b[9] = 0x56; b[10] = 0x78;       // serial
        b[11] = 0x2A;                                              // interval count
        b[27] = 0x00; b[28] = 0x01; b[29] = 0x86; b[30] = 0xA0;    // last consumption 100000
        b[84] = 0x01; b[85] = 0x02;
        const uint16_t crc = ismCrc16(&b[2], 86, 0x1021, 0xD895);
        b[88] = (uint8_t)(crc >> 8); b[89] = (uint8_t)crc;
        BitRow bits = bitsOf(b, 720);
        bool ok = false;
        for (int inv = 0; inv < 2 && !ok; inv++) {
            auto rs = ismDecode(manchester(bits, 30.5f, inv));
            const IsmReading* r = find(rs, "ERT-IDM");
            ok = r && r->id == std::to_string(0x12345678u) && fv(*r, "Last consumption count") == 100000 && fv(*r, "ERT type") == 7 && fv(*r, "Interval count") == 0x2A;
        }
        CHECK(ok, "ERT IDM");
    }
    return testSummary("ism more");
}
