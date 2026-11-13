// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ISM sensors, smart-home buses: KNX RF and HomeMatic BidCoS. Every frame is built here from the protocol description (block structure and CRC of
// EN 13757-4 format A with the KNX values; the BidCoS obfuscation and CC1101 CRC), turned into a pulse train with timing errors, noise spikes and
// both line polarities, and decoded; the values must come back, a damaged frame must give nothing. Simulated signals only: no recording of
// either protocol was available.
#include "dect2/ism/ism_proto.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

using BitRow = std::vector<int>;

static void putB(BitRow& v, const std::vector<uint8_t>& b) { for (uint8_t x : b) for (int i = 7; i >= 0; i--) v.push_back((x >> i) & 1); }

// NRZ with edge jitter (fraction of a bit) and a bit-rate error; mark = 1
static bool gFsk = true;   // the train type nrz() produces
static IsmTrain nrz(const BitRow& b, float T, std::mt19937& g, float jitter, float rateErr, bool invert, bool spikes) {
    std::uniform_real_distribution<float> u(-1.f, 1.f);
    std::vector<float> edge;                       // times of the level changes
    int cur = invert ? !b[0] : b[0];
    const float Tr = T * (1 + rateErr);
    for (size_t i = 1; i < b.size(); i++) {
        const int v = invert ? !b[i] : b[i];
        if (v != cur) { edge.push_back((float)i * Tr + u(g) * jitter * Tr); cur = v; }
    }
    edge.push_back((float)b.size() * Tr);
    IsmTrain t; t.fsk = gFsk;
    const int lvl = invert ? !b[0] : b[0];
    float last = 0;
    std::vector<IsmPulse> pr;                     // mark / space pairs; a leading space is dropped (the train starts at the first mark)
    for (size_t k = 0; k < edge.size(); k++) {
        const float run = edge[k] - last;
        last = edge[k];
        const bool mark = ((k % 2) == 0) == (lvl != 0);
        if (mark) pr.push_back({run, 6000});
        else if (!pr.empty()) pr.back().gapUs = run;
    }
    for (size_t i = 0; i < pr.size(); i++) {
        const float m = pr[i].pulseUs, sp = pr[i].gapUs;
        if (spikes && i % 7 == 3 && m > 3 * T && sp > 3 * T) {   // an FM discriminator blip inside the space
            t.p.push_back({m, sp / 2 - 4});
            t.p.push_back({8, sp / 2 - 4});
            continue;
        }
        t.p.push_back({m, sp});
    }
    if (!t.p.empty()) t.p.back().gapUs = 8000;
    return t;
}

static uint16_t crcMbus(const uint8_t* p, unsigned n) { return (uint16_t)~ismCrc16(p, n, 0x3D65, 0); }

// ------------------------------------------------------------ KNX RF

struct KnxCase { unsigned src, dst; bool group; int hops; int ndata; uint8_t sn[6]; };

static BitRow knxChips(const KnxCase& c, std::mt19937& g, std::vector<uint8_t>* frameOut = nullptr) {
    std::vector<uint8_t> blk2 = {0x29, (uint8_t)(c.src >> 8), (uint8_t)c.src, (uint8_t)(c.dst >> 8), (uint8_t)c.dst,
                                 (uint8_t)((c.group ? 0x80 : 0) | (c.hops << 4) | ((c.ndata - 8) & 0x0F)), 0x00, 0x80};
    std::uniform_int_distribution<int> r(0, 255);
    while ((int)blk2.size() < c.ndata) blk2.push_back((uint8_t)r(g));
    std::vector<uint8_t> f = {(uint8_t)(9 + c.ndata), 0x44, 0xFF, 0x03};
    f.insert(f.end(), c.sn, c.sn + 6);
    uint16_t cr = crcMbus(f.data(), 10);
    f.push_back((uint8_t)(cr >> 8)); f.push_back((uint8_t)cr);
    for (size_t o = 0; o < blk2.size(); o += 16) {
        const size_t n = std::min<size_t>(16, blk2.size() - o);
        cr = crcMbus(&blk2[o], (unsigned)n);
        f.insert(f.end(), blk2.begin() + (long)o, blk2.begin() + (long)(o + n));
        f.push_back((uint8_t)(cr >> 8)); f.push_back((uint8_t)cr);
    }
    if (frameOut) *frameOut = f;
    BitRow v;
    putB(v, {0x55, 0x55, 0x55, 0x55, 0x54, 0x76, 0x96});
    for (uint8_t x : f) for (int i = 7; i >= 0; i--) { const int d = (x >> i) & 1; v.push_back(!d); v.push_back(d); }
    return v;
}

static const IsmReading* find(const std::vector<IsmReading>& rs, const char* model) {
    for (const auto& r : rs) if (r.model == model) return &r;
    return nullptr;
}
static std::string text(const IsmReading& r, const char* name) {
    for (const auto& f : r.fields) if (f.name == name) return f.text;
    return "?";
}
static double num(const IsmReading& r, const char* name) {
    for (const auto& f : r.fields) if (f.name == name) return f.value;
    return -1e9;
}
static std::string hex2(unsigned v) { char b[8]; snprintf(b, sizeof b, "%02X", v); return b; }

// ------------------------------------------------------------ HomeMatic

static std::vector<uint8_t> bidcosFrame(uint8_t cnt, uint8_t flags, uint8_t type, const uint8_t* src, const uint8_t* dst, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> m = {0, cnt, flags, type, src[0], src[1], src[2], dst[0], dst[1], dst[2]};
    m.insert(m.end(), payload.begin(), payload.end());
    m[0] = (uint8_t)(m.size() - 1);
    const unsigned len = m[0];
    std::vector<uint8_t> e(m.size());
    e[0] = m[0];
    e[1] = (uint8_t)~(m[1] ^ 0x89);
    for (unsigned l = 2; l < len; l++) e[l] = (uint8_t)(m[l] ^ (e[l - 1] + 0xDC));
    e[len] = (uint8_t)(m[len] ^ m[2]);
    const uint16_t cr = ismCrc16(e.data(), (unsigned)e.size(), 0x8005, 0xFFFF);
    e.push_back((uint8_t)(cr >> 8)); e.push_back((uint8_t)cr);
    return e;
}
static BitRow hmChips(const std::vector<uint8_t>& e) {
    BitRow v;
    putB(v, {0xAA, 0xAA, 0xAA, 0xAA, 0xE9, 0xCA});
    putB(v, e);
    return v;
}


// ------------------------------------------------------------ EnOcean ERP1

static BitRow eoChips(uint8_t rorg, const std::vector<uint8_t>& data, uint32_t sender, uint8_t status, bool corruptCrc = false) {
    std::vector<uint8_t> t = {rorg};
    t.insert(t.end(), data.begin(), data.end());
    for (int i = 3; i >= 0; i--) t.push_back((uint8_t)(sender >> (8 * i)));
    t.push_back(status);
    t.push_back((uint8_t)(ismCrc8(t.data(), (unsigned)t.size(), 0x07, 0) ^ (corruptCrc ? 0x10 : 0)));
    BitRow v = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1};   // alternating chips, then the 11-chip start 01010101001
    for (size_t k = 0; k < t.size(); k++) {
        const uint8_t x = t[k];
        auto bit = [&](int i) { return (x >> i) & 1; };
        const int c[10] = {bit(7), bit(6), bit(5), !bit(5), bit(4), bit(3), bit(2), !bit(2), bit(1), bit(0)};
        v.insert(v.end(), c, c + 10);
        const bool more = k + 1 < t.size();
        v.push_back(more ? 0 : 1); v.push_back(more ? 1 : 0);
    }
    for (int i = 0; i < 8; i++) v.push_back(0);
    return v;
}

// ------------------------------------------------------------ DASH7

static void d7Whiten(std::vector<uint8_t>& v) {
    uint16_t r = 0x1FF;
    for (auto& b : v) {
        b ^= (uint8_t)r;
        for (int i = 0; i < 8; i++) { const uint16_t n = (uint16_t)((((r & 0x20) >> 5) ^ r) << 8) | ((r >> 1) & 0xFF); r = n & 0x1FF; }
    }
}
// CRC-16 as the Sub-IoT crc.c computes it bit by bit (an independent form of CCITT-FALSE, to catch a wrong polynomial in the decoder)
static uint16_t d7Crc(const uint8_t* d, unsigned n) {
    uint16_t crc = 0xFFFF;
    for (unsigned i = 0; i < n; i++) {
        uint16_t c = (uint16_t)((uint8_t)(crc >> 8) | (crc << 8));
        c ^= d[i];
        c ^= (uint8_t)(c & 0xFF) >> 4;
        c ^= (uint16_t)(c << 12);
        c ^= (uint16_t)((c & 0xFF) << 5);
        crc = c;
    }
    return crc;
}
static BitRow d7Chips(std::vector<uint8_t> f, uint16_t sync, int preBytes = 6) {   // f: the frame without CRC (foreground: length first)
    const uint16_t crc = d7Crc(f.data(), (unsigned)f.size());
    f.push_back((uint8_t)(crc >> 8)); f.push_back((uint8_t)crc);
    d7Whiten(f);
    BitRow v;
    for (int i = 0; i < preBytes; i++) putB(v, {0xAA});
    putB(v, {(uint8_t)(sync >> 8), (uint8_t)sync});
    putB(v, f);
    for (int i = 0; i < 4; i++) v.push_back(0);
    return v;
}
static std::vector<uint8_t> d7Foreground(uint8_t subnet, int targetType, const std::vector<uint8_t>& target, uint8_t nls, int originType, uint8_t oclass,
                                         const std::vector<uint8_t>& origin, int eirp, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> f = {0, subnet, (uint8_t)(targetType << 6 | ((eirp + 32) & 0x3F))};
    f.insert(f.end(), target.begin(), target.end());
    f.push_back((uint8_t)(nls | (originType << 4)));                 // origin void when originType == 4 is not used here: always with an origin
    f.push_back(oclass);
    f.insert(f.end(), origin.begin(), origin.end());
    f.insert(f.end(), payload.begin(), payload.end());
    f[0] = (uint8_t)(f.size() + 2 - 1);                               // everything after the length byte, with the CRC
    return f;
}

int main() {
    std::mt19937 g(7);
    const auto& P = ismProtocols();
    bool hasKnx = false, hasHm = false, hasEo = false, hasD7 = false;
    for (const auto& p : P) { hasKnx |= std::string(p.name).find("KNX") != std::string::npos; hasHm |= std::string(p.name).find("HomeMatic") != std::string::npos; hasEo |= std::string(p.name).find("EnOcean") != std::string::npos; hasD7 |= std::string(p.name).find("DASH7") != std::string::npos; }
    CHECK(hasKnx && hasHm && hasEo && hasD7, "both protocols are listed");

    // ---- KNX RF: every payload length of one to three data blocks, both address kinds, every hop count, timing errors
    int knxOk = 0, knxN = 0;
    for (int nd = 8; nd <= 40; nd++)
        for (int grp = 0; grp < 2; grp++)
            for (int hops = 0; hops < 8; hops += 3) {
                KnxCase c{0x1103u + (unsigned)nd * 17, grp ? 0x0A07u + (unsigned)hops : 0x1105u, grp != 0, hops, nd, {0x00, 0x01, 0x02, 0x03, (uint8_t)nd, 0x7F}};
                const float jit = (nd % 3) * 0.04f, err = ((nd % 5) - 2) * 0.01f;   // up to 8 % edge jitter, +-2 % bit rate
                const IsmTrain t = nrz(knxChips(c, g), 30.5f, g, jit, err, false, false);
                const auto rs = ismDecode(t);
                knxN++;
                const IsmReading* r = find(rs, "KNX-RF");
                if (!r) { CHECK(false, "KNX nd %d grp %d hops %d: nothing", nd, grp, hops); continue; }
                char sn[16]; snprintf(sn, sizeof sn, "000102%02X%02X7F", 3, nd);
                const std::string want = hex2(0) + hex2(1) + hex2(2) + hex2(3) + hex2((unsigned)nd) + hex2(0x7F);
                const unsigned s = c.src;
                char sa[24]; snprintf(sa, sizeof sa, "%u.%u.%u", (s >> 12) & 15, (s >> 8) & 15, s & 255);
                char da[24];
                if (grp) snprintf(da, sizeof da, "%u/%u/%u", (c.dst >> 11) & 31, (c.dst >> 8) & 7, c.dst & 255); else snprintf(da, sizeof da, "%u.%u.%u", (c.dst >> 12) & 15, (c.dst >> 8) & 15, c.dst & 255);
                bool ok = r->id == want && text(*r, "Source") == sa && text(*r, grp ? "Group address" : "Destination") == da && num(*r, "Hop count") == hops &&
                          text(*r, "KNX control") == "29" && text(*r, "TPCI/APCI") == "0080" && r->mic == "CRC";
                if (nd > 8) ok = ok && text(*r, "Data").size() == (size_t)(nd - 8) * 2;
                CHECK(ok, "KNX nd %d grp %d hops %d: id %s src %s dst %s", nd, grp, hops, r->id.c_str(), text(*r, "Source").c_str(), text(*r, grp ? "Group address" : "Destination").c_str());
                knxOk += ok;
            }
    printf("  KNX RF: %d / %d frames decoded\n", knxOk, knxN);

    // a damaged KNX frame gives nothing: flip every chip pair in turn (a bit error in the data) for a two-block frame
    {
        KnxCase c{0x1103, 0x0A07, true, 3, 24, {1, 2, 3, 4, 5, 6}};
        const BitRow ok = knxChips(c, g);
        CHECK(find(ismDecode(nrz(ok, 30.5f, g, 0, 0, false, false)), "KNX-RF"), "clean 24-byte frame");
        int leaked = 0, tried = 0;
        for (size_t i = 56; i + 1 < ok.size(); i += 2) {       // from the first data byte on, one data bit at a time
            BitRow b = ok;
            b[i] = !b[i]; b[i + 1] = !b[i + 1];
            tried++;
            if (find(ismDecode(nrz(b, 30.5f, g, 0, 0, false, false)), "KNX-RF")) leaked++;
        }
        CHECK(leaked == 0, "KNX: %d of %d single-bit errors were accepted", leaked, tried);
        // without a sync word, and the wrong marker (0xFF 0x03 -> wireless M-Bus), nothing is reported as KNX
        BitRow w = ok;
        for (int i = 0; i < 16; i++) w[(4 + 1) * 8 + i] = !w[(4 + 1) * 8 + i];
        CHECK(!find(ismDecode(nrz(w, 30.5f, g, 0, 0, false, false)), "KNX-RF"), "damaged sync accepted");
        // truncated: the second block is cut off
        BitRow cut(ok.begin(), ok.begin() + 56 + 2 * 8 * 20);
        CHECK(!find(ismDecode(nrz(cut, 30.5f, g, 0, 0, false, false)), "KNX-RF"), "truncated frame accepted");
    }

    // ---- HomeMatic BidCoS
    struct Hm { uint8_t type, flags; std::vector<uint8_t> pl; };
    const uint8_t src[3] = {0x1A, 0x2B, 0x3C}, dst[3] = {0x00, 0x00, 0x00};
    int hmOk = 0, hmN = 0;
    for (int cnt = 0; cnt < 256; cnt += 5)
        for (int variant = 0; variant < 8; variant++)
            for (int pol = 0; pol < 2; pol++) {
                std::vector<uint8_t> pl;
                uint8_t type = 0, flags = 0x80;
                switch (variant) {
                case 0: type = 0x70; flags = 0x84; { int t = 215 + cnt * 3; pl = {(uint8_t)(t >> 8), (uint8_t)t, (uint8_t)(40 + cnt % 50), 0, 0}; } break;   // weather
                case 1: type = 0x70; flags = 0x84; { int t = 55 + cnt; pl = {(uint8_t)(0x40 | (t >> 8)), (uint8_t)t, 77, 0, 0}; } break;            // below zero
                case 2: type = 0x40; flags = 0xA0; pl = {(uint8_t)((cnt & 0x3F) | 0x40), (uint8_t)cnt}; break;                                         // remote, long
                case 3: type = 0x40; flags = 0x80; pl = {(uint8_t)((cnt & 0x3F) | 0x80), (uint8_t)(cnt + 1)}; break;                                   // remote, low battery
                case 4: type = 0x41; flags = 0xA0; pl = {(uint8_t)(cnt & 0x3F), 9, 200}; break;
                case 5: type = 0x02; flags = 0x80; pl = {0x01, (uint8_t)(cnt % 4), 0xC8, 0x00, (uint8_t)(60 + cnt % 40)}; break;                       // ack status
                case 6: type = 0x3E; flags = 0x94; pl = {0xAA, 0xBB, 0xCC, 0x00, (uint8_t)(1 + cnt % 8), (uint8_t)cnt}; break;                       // switch
                case 7: type = 0x11; flags = 0xA0; for (int i = 0; i < 1 + cnt % 40; i++) pl.push_back((uint8_t)(i * 7 + cnt)); break;               // set, long
                }
                const auto e = bidcosFrame((uint8_t)cnt, flags, type, src, dst, pl);
                const float jit = (cnt % 3) * 0.05f, err = ((cnt % 7) - 3) * 0.008f;
                const IsmTrain t = nrz(hmChips(e), 100.f, g, jit, err, pol == 1, variant % 2 == 0);
                const auto rs = ismDecode(t);
                hmN++;
                const IsmReading* r = find(rs, "HomeMatic-BidCoS");
                if (!r) { CHECK(false, "HM cnt %d variant %d pol %d: nothing", cnt, variant, pol); continue; }
                bool ok = r->id == "1A2B3C" && text(*r, "Destination") == "000000" && num(*r, "Message counter") == cnt && r->mic == "CRC";
                switch (variant) {
                case 0: { const double want = (215 + cnt * 3) / 10.0; ok = ok && std::fabs(num(*r, "Temperature") - want) < 0.05 && num(*r, "Humidity") == 40 + cnt % 50 && text(*r, "Type") == "Weather event"; } break;
                case 1: ok = ok && std::fabs(num(*r, "Temperature") + (55 + cnt) / 10.0) < 0.05; break;
                case 2: ok = ok && text(*r, "Press") == "long" && num(*r, "Button") == (cnt & 0x3F) && num(*r, "Counter") == cnt && r->battery == 1; break;
                case 3: ok = ok && r->battery == 0 && text(*r, "Press") == "short"; break;
                case 4: ok = ok && num(*r, "Value") == 200 && num(*r, "Number") == 9; break;
                case 5: ok = ok && num(*r, "Status") == 0xC8 && num(*r, "RSSI") == -(60 + cnt % 40) && text(*r, "Type") == "Ack"; break;
                case 6: ok = ok && text(*r, "Peer") == "AABBCC" && num(*r, "Counter") == cnt; break;
                case 7: ok = ok && text(*r, "Type") == "Set"; break;
                }
                CHECK(ok, "HM cnt %d variant %d pol %d: id %s", cnt, variant, pol, r->id.c_str());
                hmOk += ok;
            }
    printf("  HomeMatic: %d / %d frames decoded\n", hmOk, hmN);
    {   // flags
        const auto e = bidcosFrame(1, 0xB6, 0x10, src, dst, {0x06, 1, 0xC8, 0, 70});
        const auto rs = ismDecode(nrz(hmChips(e), 100.f, g, 0, 0, false, false));
        const IsmReading* r = find(rs, "HomeMatic-BidCoS");
        CHECK(r && text(*r, "Flags") == "WAKEMEUP BCAST BURST BIDI RPTEN" , "flags text: %s", r ? text(*r, "Flags").c_str() : "none");
        // every single-bit error in the frame (past the sync) is rejected
        const BitRow ok = hmChips(e);
        int leaked = 0, tried = 0;
        for (size_t i = 48; i < ok.size(); i++) {
            BitRow b = ok; b[i] = !b[i]; tried++;
            if (find(ismDecode(nrz(b, 100.f, g, 0, 0, false, false)), "HomeMatic-BidCoS")) leaked++;
        }
        CHECK(leaked == 0, "HM: %d of %d single-bit errors accepted (CRC-16 detects all)", leaked, tried);
        // truncated
        BitRow cut(ok.begin(), ok.end() - 24);
        CHECK(!find(ismDecode(nrz(cut, 100.f, g, 0, 0, false, false)), "HomeMatic-BidCoS"), "truncated frame accepted");
    }


    // ---- EnOcean ERP1 (ASK: the line is inverted, as rtl_433 reads it)
    gFsk = false;
    {
        int ok = 0, n = 0;
        std::uniform_int_distribution<int> r8(0, 255);
        for (int variant = 0; variant < 6; variant++)
            for (int k = 0; k < 40; k++) {
                const uint32_t sender = 0x01800000u + (uint32_t)k * 0x1357u + (uint32_t)variant;
                const uint8_t rep = (uint8_t)(k % 3);
                BitRow ch;
                uint8_t rorg = 0;
                std::vector<uint8_t> dat;
                if (variant == 0) { rorg = 0xF6; dat = {(uint8_t)(0x30 | 0x10)}; ch = eoChips(rorg, dat, sender, (uint8_t)(0x30 | rep)); }                      // N-message, R1 = 1, EB pressed
                else if (variant == 1) { rorg = 0xF6; dat = {0x00}; ch = eoChips(rorg, dat, sender, (uint8_t)(0x20 | rep)); }                                       // U-message, released
                else if (variant == 2) { rorg = 0xD5; dat = {(uint8_t)(0x08 | (k & 1))}; ch = eoChips(rorg, dat, sender, rep); }                                   // contact
                else if (variant == 3) { rorg = 0xA5; dat = {(uint8_t)(0x02 << 2 | 0), (uint8_t)(0x05 << 3 | 0), 0x7D, 0x80}; ch = eoChips(rorg, dat, sender, rep); }   // teach-in, EEP A5-02-05, manufacturer 0x7D
                else if (variant == 4) { rorg = 0xA5; dat = {0x00, (uint8_t)r8(g), (uint8_t)r8(g), 0x08}; ch = eoChips(rorg, dat, sender, rep); }
                else { rorg = 0xD2; for (int i = 0; i < 1 + k % 14; i++) dat.push_back((uint8_t)r8(g)); ch = eoChips(rorg, dat, sender, rep); }
                const auto rs = ismDecode(nrz(ch, 8.f, g, (k % 3) * 0.03f, ((k % 5) - 2) * 0.01f, true, false));
                n++;
                const IsmReading* r = find(rs, "EnOcean-ERP1");
                char id[16]; snprintf(id, sizeof id, "%08X", sender);
                bool good = r && r->id == id && num(*r, "Repeaters") == rep;
                if (good && variant == 0) good = num(*r, "Rocker 1") == 1 && text(*r, "Energy bow") == "pressed" && text(*r, "RORG") == "RPS (F6)";
                if (good && variant == 1) good = num(*r, "Buttons pressed") == 0 && text(*r, "Energy bow") == "released";
                if (good && variant == 2) good = text(*r, "Contact") == ((k & 1) ? "closed" : "open");
                if (good && variant == 3) good = text(*r, "Teach-in") == "4BS" && text(*r, "EEP") == "A5-02-05" && num(*r, "Manufacturer") == 0x7D;
                if (good && variant == 4) good = text(*r, "Data").size() == 8;
                if (good && variant == 5) good = text(*r, "Data").size() == dat.size() * 2;
                CHECK(good, "EnOcean variant %d k %d", variant, k);
                ok += good;
            }
        printf("  EnOcean ERP1: %d / %d telegrams decoded\n", ok, n);
        // a wrong CRC, a single damaged chip, a cut telegram: nothing
        std::mt19937 g2(3);
        const BitRow good = eoChips(0xA5, {0, 1, 2, 8}, 0x0180ABCD, 0x01);
        CHECK(find(ismDecode(nrz(good, 8.f, g2, 0, 0, true, false)), "EnOcean-ERP1"), "clean telegram");
        CHECK(!find(ismDecode(nrz(eoChips(0xA5, {0, 1, 2, 8}, 0x0180ABCD, 0x01, true), 8.f, g2, 0, 0, true, false)), "EnOcean-ERP1"), "bad CRC accepted");
        int leaked = 0, tried = 0;
        for (size_t i = 23; i < good.size() - 8 - 2; i++) {   // not the two end-marker chips: any value but "01" ends the telegram
            BitRow b = good; b[i] = !b[i]; tried++;
            if (find(ismDecode(nrz(b, 8.f, g2, 0, 0, true, false)), "EnOcean-ERP1")) leaked++;
        }
        CHECK(leaked == 0, "EnOcean: %d of %d single-chip errors accepted", leaked, tried);
        BitRow cut(good.begin(), good.end() - 8 - 36);
        CHECK(!find(ismDecode(nrz(cut, 8.f, g2, 0, 0, true, false)), "EnOcean-ERP1"), "cut telegram accepted");
        // random ASK noise
        int fp = 0;
        std::uniform_int_distribution<int> b01(0, 1);
        for (int i = 0; i < 3000; i++) {
            BitRow v;
            for (int q = 0; q < 300; q++) v.push_back(b01(g2));
            fp += find(ismDecode(nrz(v, 8.f, g2, 0.1f, 0, true, false)), "EnOcean-ERP1") != nullptr;
        }
        CHECK(fp == 0, "%d false EnOcean readings in noise", fp);
    }
    gFsk = true;

    // ---- DASH7: both rates, both frame classes, VID / UID / broadcast targets, origin IDs, secured frames
    {
        int ok = 0, n = 0;
        std::uniform_int_distribution<int> r8(0, 255);
        for (int rate = 0; rate < 2; rate++)
            for (int variant = 0; variant < 6; variant++)
                for (int k = 0; k < 24; k++) {
                    const float T = rate ? 104.17f : 18.0f;
                    std::vector<uint8_t> tgt, org;
                    int tt = 0, ot = 3;
                    uint8_t nls = 0;
                    if (variant == 0) { tt = 3; tgt = {0xAB, (uint8_t)k}; ot = 2; for (int i = 0; i < 8; i++) org.push_back((uint8_t)(0x40 + i + k)); }   // VID target, UID origin
                    else if (variant == 1) { tt = 2; for (int i = 0; i < 8; i++) tgt.push_back((uint8_t)(0x10 + i)); ot = 3; org = {0x12, (uint8_t)k}; }
                    else if (variant == 2) { tt = 1; ot = 3; org = {0xCA, 0xFE}; }                                                                       // broadcast
                    else if (variant == 3) { tt = 0; ot = 0; org = {(uint8_t)k}; }                                                                       // NBID
                    else if (variant == 4) { tt = 3; tgt = {1, 2}; ot = 3; org = {0xBE, 0xEF}; nls = 7; }                                                 // AES-CCM-32 secured
                    else { tt = 3; tgt = {1, 2}; ot = 3; org = {0xBE, 0xEF}; }
                    std::vector<uint8_t> pl;
                    for (int i = 0; i < 3 + k % 30; i++) pl.push_back((uint8_t)r8(g));
                    const int eirp = -20 + k % 30;
                    const auto f = d7Foreground(0xFF, tt, tgt, nls, ot, 0x01, org, eirp, pl);
                    const bool bg = variant == 5;
                    BitRow ch;
                    if (bg) { std::vector<uint8_t> b = {0xFF, (uint8_t)(3 << 6 | (k & 0x3F)), (uint8_t)(k * 3), (uint8_t)(k * 5)}; ch = d7Chips(b, 0xE6D0); }
                    else ch = d7Chips(f, 0x0B67);
                    const auto rs = ismDecode(nrz(ch, T, g, (k % 3) * 0.03f, ((k % 5) - 2) * 0.01f, k % 2 == 1, k % 4 == 1 && rate == 1));
                    n++;
                    const IsmReading* r = find(rs, bg ? "DASH7-Background" : "DASH7");
                    bool good = r != nullptr;
                    if (good && bg) good = num(*r, "Identifier tag") == (k & 0x3F) && num(*r, "ETA") == (double)(((k * 3) & 0xFF) << 8 | ((k * 5) & 0xFF)) && text(*r, "Subnet") == "FF";
                    if (good && !bg) {
                        good = text(*r, "Rate") == (rate ? "9.6 kbit/s" : "55.555 kbit/s") && text(*r, "Subnet") == "FF" && num(*r, "EIRP") == eirp && text(*r, "Origin access class") == "01";
                        std::string want;
                        for (uint8_t x : org) want += hex2(x);
                        std::string tw = "broadcast";
                        if (!tgt.empty()) { tw.clear(); for (uint8_t x : tgt) tw += hex2(x); }
                        good = good && text(*r, "Target") == tw && (org.empty() || r->id == want);
                        if (nls) good = good && num(*r, "Security (NLS method)") == 7 && text(*r, "Payload") == "?";
                        else good = good && text(*r, "Payload").size() == std::min<size_t>(24, pl.size()) * 2;
                    }
                    CHECK(good, "DASH7 rate %d variant %d k %d", rate, variant, k);
                    ok += good;
                }
        printf("  DASH7: %d / %d frames decoded\n", ok, n);
        const auto f = d7Foreground(0xFF, 3, {1, 2}, 0, 3, 1, {9, 9}, 10, {1, 2, 3, 4, 5, 6});
        const BitRow good = d7Chips(f, 0x0B67);
        CHECK(find(ismDecode(nrz(good, 18.f, g, 0, 0, false, false)), "DASH7"), "clean frame");
        int leaked = 0, tried = 0;
        for (size_t i = 6 * 8 + 16; i < good.size() - 4; i++) {   // every single-bit error from the first whitened byte on
            BitRow b = good; b[i] = !b[i]; tried++;
            if (find(ismDecode(nrz(b, 18.f, g, 0, 0, false, false)), "DASH7")) leaked++;
        }
        CHECK(leaked == 0, "DASH7: %d of %d single-bit errors accepted", leaked, tried);
        BitRow cut(good.begin(), good.end() - 4 - 24);
        CHECK(!find(ismDecode(nrz(cut, 18.f, g, 0, 0, false, false)), "DASH7"), "cut frame accepted");
        // the FEC-coded channel (other sync word) is not decoded: nothing, and no false frame
        CHECK(!find(ismDecode(nrz(d7Chips(f, 0x192F), 18.f, g, 0, 0, false, false)), "DASH7"), "FEC-class frame read as PN9");
    }

    // ---- noise: random bit rows at both bit rates must not produce either model (and nothing must crash)
    {
        int fp = 0, runs = 3000;
        std::uniform_int_distribution<int> b(0, 1);
        for (int i = 0; i < runs; i++) {
            BitRow v;
            for (int k = 0; k < 400; k++) v.push_back(b(g));
            if (i % 2) { const BitRow s = {1,1,1,0,1,0,0,1,1,1,0,0,1,0,1,0}; for (size_t k = 0; k < s.size(); k++) v[40 + k] = s[k]; }   // a sync word in noise
            for (float T : {100.f, 30.5f}) {
                const auto rs = ismDecode(nrz(v, T, g, 0.1f, 0, false, false));
                if (find(rs, "HomeMatic-BidCoS") || find(rs, "KNX-RF")) fp++;
            }
        }
        CHECK(fp == 0, "%d false readings in noise", fp);
        printf("  noise: %d false readings in %d trains\n", fp, runs * 2);
    }
    return testSummary("ism iot");
}
