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
    IsmTrain t; t.fsk = true;
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

int main() {
    std::mt19937 g(7);
    const auto& P = ismProtocols();
    bool hasKnx = false, hasHm = false;
    for (const auto& p : P) { hasKnx |= std::string(p.name).find("KNX") != std::string::npos; hasHm |= std::string(p.name).find("HomeMatic") != std::string::npos; }
    CHECK(hasKnx && hasHm, "both protocols are listed");

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
