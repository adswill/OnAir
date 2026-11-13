// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/uplink/uplink_msg.h"
#include "dect2/adsb/adsb_msg.h"
#include <cstdio>
#include <cstring>

namespace dect2::uplink {

using adsb::getBits;
using adsb::setBits;

uint32_t uplinkOverlay(uint32_t addr) { return addr & 0xFFFFFF; }
uint32_t uplinkAddress(uint32_t overlay) { return overlay & 0xFFFFFF; }

int ufLength(int uf) {
    switch (uf) {
    case 0: case 4: case 5: case 11: return 56;
    case 16: case 20: case 21: case 24: return 112;
    default: return 0;
    }
}

int ufOf(const uint8_t* b) { return (b[0] >> 6) == 3 ? 24 : b[0] >> 3; }

void finish(Frame& f, uint32_t addr) {
    const uint32_t ap = adsb::crcParity(f.b, f.nbits) ^ uplinkOverlay(addr);
    setBits(f.b, f.nbits - 23, 24, ap);
}

Frame uf0(uint32_t addr, int rl, int aq, int ds) {
    Frame f; f.nbits = 56;
    setBits(f.b, 1, 5, 0); setBits(f.b, 9, 1, rl); setBits(f.b, 14, 1, aq); setBits(f.b, 15, 8, ds);
    finish(f, addr); return f;
}
Frame uf45(int uf, uint32_t addr, int pc, int rr, int di, int sd) {
    Frame f; f.nbits = 56;
    setBits(f.b, 1, 5, uf); setBits(f.b, 6, 3, pc); setBits(f.b, 9, 5, rr); setBits(f.b, 14, 3, di); setBits(f.b, 17, 16, sd);
    finish(f, addr); return f;
}
Frame uf11(int pr, int ic, int cl) {
    Frame f; f.nbits = 56;
    setBits(f.b, 1, 5, 11); setBits(f.b, 6, 4, pr); setBits(f.b, 10, 4, ic); setBits(f.b, 14, 3, cl);
    finish(f, 0xFFFFFF); return f;
}
Frame uf16(uint32_t addr, int rl, int aq, const uint8_t mu[7]) {
    Frame f; f.nbits = 112;
    setBits(f.b, 1, 5, 16); setBits(f.b, 9, 1, rl); setBits(f.b, 14, 1, aq);
    memcpy(f.b + 4, mu, 7);
    finish(f, addr); return f;
}
Frame uf2021(int uf, uint32_t addr, int pc, int rr, int di, int sd, const uint8_t ma[7]) {
    Frame f; f.nbits = 112;
    setBits(f.b, 1, 5, uf); setBits(f.b, 6, 3, pc); setBits(f.b, 9, 5, rr); setBits(f.b, 14, 3, di); setBits(f.b, 17, 16, sd);
    memcpy(f.b + 4, ma, 7);
    finish(f, addr); return f;
}
Frame uf24(uint32_t addr, int rc, int nc, const uint8_t mc[10]) {
    Frame f; f.nbits = 112;
    setBits(f.b, 1, 2, 3); setBits(f.b, 3, 2, rc); setBits(f.b, 5, 4, nc);
    for (int i = 0; i < 10; i++) setBits(f.b, 9 + 8 * i, 8, mc[i]);
    finish(f, addr); return f;
}
int sdIis(int iis, int rrs) { return (iis & 15) << 12 | (rrs & 15) << 8; }
int sdSis(int sis, int lss, int rrs) { return (sis & 63) << 10 | (lss & 1) << 9 | (rrs & 15) << 5; }

void raMu(uint8_t mu[7], uint32_t mid, int mtb, int cvc, int vrc, int chc, int hrc, int hsb, int vsb) {
    uint8_t m[14] = {};
    // MU is bits 33-88 of the message; written here at the same bit numbers, then copied out
    setBits(m, 33, 8, 0x30);
    setBits(m, 42, 1, mtb); setBits(m, 43, 2, cvc); setBits(m, 45, 2, vrc); setBits(m, 47, 3, chc); setBits(m, 50, 3, hrc);
    setBits(m, 56, 5, hsb); setBits(m, 61, 4, vsb); setBits(m, 65, 24, mid);
    memcpy(mu, m + 4, 7);
}

bool decode(const uint8_t* b, int nbits, Uplink& u) {
    u = Uplink();
    u.uf = ufOf(b);
    if (ufLength(u.uf) != nbits) return false;
    u.nbits = nbits;
    memcpy(u.b, b, nbits / 8);
    u.ap = getBits(b, nbits - 23, 24);
    u.addr = uplinkAddress(u.ap ^ adsb::crcParity(b, nbits));
    switch (u.uf) {
    case 11:
        u.pr = getBits(b, 6, 4); u.ic = getBits(b, 10, 4); u.cl = getBits(b, 14, 3);
        u.allCallOk = u.addr == 0xFFFFFF;
        if (u.cl == 0) u.ii = u.ic;
        else if (u.cl <= 4) u.si = u.ic + 16 * (u.cl - 1);
        break;
    case 4: case 5: case 20: case 21:
        u.pc = getBits(b, 6, 3); u.rr = getBits(b, 9, 5); u.di = getBits(b, 14, 3); u.sd = getBits(b, 17, 16);
        if (u.di == 0 || u.di == 1 || u.di == 7) u.ii = u.sd >> 12;
        if (u.di == 7) u.rrs = (u.sd >> 8) & 15;
        if (u.di == 3) { u.si = (u.sd >> 10) & 63; u.lss = (u.sd >> 9) & 1; u.rrs = (u.sd >> 5) & 15; if (u.si == 0) u.si = -1; }
        if (u.rr >= 16) { u.bds1 = u.rr - 16; u.bds2 = u.rrs; }
        break;
    case 0: case 16:
        u.rl = getBits(b, 9, 1); u.aq = getBits(b, 14, 1);
        if (u.uf == 0) u.ds = getBits(b, 15, 8);
        else {
            u.uds = getBits(b, 33, 8);
            if (u.uds == 0x30) {
                u.ra = true;
                u.mtb = getBits(b, 42, 1); u.cvc = getBits(b, 43, 2); u.vrc = getBits(b, 45, 2); u.chc = getBits(b, 47, 3); u.hrc = getBits(b, 50, 3);
                u.hsb = getBits(b, 56, 5); u.vsb = getBits(b, 61, 4); u.mid = getBits(b, 65, 24);
            }
        }
        break;
    case 24:
        u.rc = getBits(b, 3, 2); u.nc = getBits(b, 5, 4);
        break;
    }
    return true;
}

std::string bdsText(int bds1, int bds2) {
    char s[16];
    snprintf(s, sizeof s, "%X,%X", bds1 & 15, bds2 & 15);
    return s;
}
const char* vrcText(int v) { return v == 1 ? "don't pass below" : v == 2 ? "don't pass above" : v == 3 ? "(3)" : "no restriction"; }
const char* cvcText(int v) { return v == 1 ? "cancel don't pass below" : v == 2 ? "cancel don't pass above" : v == 3 ? "(3)" : "no cancel"; }

static std::string ic(const Uplink& u) {
    char s[16];
    if (u.ii >= 0) snprintf(s, sizeof s, "II %d", u.ii);
    else if (u.si >= 0) snprintf(s, sizeof s, "SI %d", u.si);
    else return "";
    return s;
}

std::string describe(const Uplink& u) {
    char s[200];
    static const char* kPr[] = {"reply 1", "reply 1/2", "reply 1/4", "reply 1/8", "reply 1/16"};
    switch (u.uf) {
    case 11:
        snprintf(s, sizeof s, "all-call %s, %s%s%s", ic(u).c_str(), (u.pr & 7) <= 4 ? kPr[u.pr & 7] : "PR ?", u.pr >= 8 ? ", lockout override" : "",
                 u.allCallOk ? "" : ", bad parity");
        break;
    case 4: case 5: case 20: case 21: {
        const char* what = u.uf == 4 ? "altitude" : u.uf == 5 ? "identity" : u.uf == 20 ? "Comm-A, altitude" : "Comm-A, identity";
        std::string rr;
        if (u.bds1 == 0) rr = ", air-initiated Comm-B";
        else if (u.bds1 > 0) rr = ", Comm-B BDS " + bdsText(u.bds1, u.bds2);
        const std::string c = ic(u);
        snprintf(s, sizeof s, "%s request%s%s%s, DI %d PC %d", what, rr.c_str(), c.empty() ? "" : ", ", c.c_str(), u.di, u.pc);
        break;
    }
    case 0: snprintf(s, sizeof s, "ACAS surveillance%s%s", u.rl ? ", long reply" : "", u.aq ? ", acquisition" : ""); break;
    case 16:
        if (u.ra) snprintf(s, sizeof s, "ACAS coordination from %06X: %s, %s", u.mid, vrcText(u.vrc), cvcText(u.cvc));
        else snprintf(s, sizeof s, "ACAS long air-air, UDS %02X", u.uds);
        break;
    case 24: snprintf(s, sizeof s, "Comm-C (ELM) segment %d, RC %d", u.nc, u.rc); break;
    default: snprintf(s, sizeof s, "UF%d", u.uf); break;
    }
    return s;
}

} // namespace dect2::uplink
