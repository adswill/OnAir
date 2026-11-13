// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the Wi-SUN frame layers (rust/onair_wisun behind dect2::wisun): all 65536 PHR values, whitening of buffers up to 3000 octets (past the
// 2048 octet wrap), both FCS types, every name function, scheduleText of random schedules, and parse() of ONAIR_DIFF_ITERS (default 200000) frames: built with the
// real builder (all UTT types, secured or not, schedules, PAN IE, network names, EAPOL), randomly structured MAC headers and IE lists (all frame versions and address
// modes, header and payload IEs with random sub-IEs, channel plans with exclusions), mutated and truncated. Every result goes into a digest (tests/common/golden.h;
// doubles rounded to 1e-6) compared with the one recorded from the C++ layers the Rust code replaced.
#include "dect2/wisun/wisun_proto.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
namespace A = dect2::wisun;
static golden::Digest dig;
static std::mt19937 g(2468);
static uint32_t r32() { return g(); }
static uint8_t rb() { return (uint8_t)g(); }
static uint64_t r64() { const uint64_t hi = (uint64_t)r32() << 32; return hi | r32(); }   // (two draws in one expression would be evaluated in an unspecified order)

static void addSched(const A::Schedule& a) {
    dig.add(a.present, a.bcastIntervalMs, a.bsi, a.dwellMs, a.clockDrift, a.timingAccuracy, a.chPlan, a.chFunction, a.excluded, a.regDomain);
    dig.add(a.opClass, a.chPlanId, a.ch0Mhz, a.spacingCode, a.numChannels, a.fixedChannel, a.excludedCount);
}
static void addFrame(const A::Frame& a) {
    dig.add(a.ok, a.type, a.version, a.secured, a.pending, a.ackReq, a.panCompress, a.seqSuppressed, a.ieList, a.destMode, a.srcMode, a.seq, a.hasDestPan);
    dig.add(a.hasSrcPan, a.destPan, a.srcPan, a.destShort, a.srcShort, a.destExt, a.srcExt, a.secLevel, a.keyIdMode, a.keyIndex, a.frameCounter, a.utt);
    dig.add(a.ufsi, a.hasBt, a.bsn, a.bio, a.rslDbm, a.hasFc, a.txFlow, a.rxFlow, a.eaEui, a.hasLbc, a.lfnBcastMs);
    addSched(a.us); addSched(a.bs);
    dig.add(a.hasPanIe, a.panSize, a.routingCost, a.tpsVersion, a.useParentBs, a.rplRouting, a.netName, a.hasNetName, a.panVersion, a.hasGtkHash, a.lfnVersion);
    dig.add(a.phyModes, a.mpxId, a.kmpId, a.eapolType, a.eapCode, a.eapType, a.ies, a.payloadAt, a.payloadLen, a.encrypted, a.info);
}

static std::vector<uint8_t> mutate(std::vector<uint8_t> v) {
    for (int k = (int)(g() % 4); k > 0; k--) {
        if (v.empty()) { v.push_back(rb()); continue; }
        switch (g() % 5) {
        case 0: v[g() % v.size()] = rb(); break;
        case 1: v[g() % v.size()] ^= (uint8_t)(1u << (g() % 8)); break;
        case 2: v.erase(v.begin() + (long)(g() % v.size())); break;
        case 3: ORD_CALL(v.insert, v.begin() + (long)(g() % (v.size() + 1)), rb()); break;   // ORD_*: draws in a fixed order (arguments and operands are not evaluated in one)
        default: v.resize(g() % (v.size() + 1)); break;
        }
    }
    return v;
}

static void put16(std::vector<uint8_t>& v, unsigned x) { v.push_back((uint8_t)x); v.push_back((uint8_t)(x >> 8)); }
static void bytes(std::vector<uint8_t>& v, size_t n) { for (size_t i = 0; i < n; i++) v.push_back(rb()); }

// Random channel information for a US / BS IE (all plans, functions and exclusion kinds)
static void chanInfo(std::vector<uint8_t>& v) {
    const int plan = (int)(g() % 4), fn = (int)(g() % 4), ex = (int)(g() % 4);
    v.push_back((uint8_t)(plan | fn << 3 | ex << 6));
    if (plan == 0 || plan == 2) bytes(v, 2); else if (plan == 1) bytes(v, 6);
    if (fn == 0) bytes(v, 2);
    if (ex == 1) { const int r = (int)(g() % 4); v.push_back((uint8_t)r); for (int i = 0; i < r; i++) { put16(v, g() % 100); put16(v, g() % 200); } }
    else if (ex == 2) bytes(v, g() % 6);
    if (g() % 6 == 0) v.resize(g() % (v.size() + 1));
}

static std::vector<uint8_t> wpIes() {
    std::vector<uint8_t> wp;
    for (int k = (int)(1 + g() % 5); k > 0; k--) {
        std::vector<uint8_t> body;
        const bool lng = g() % 3 == 0;
        int sub;
        if (lng) {
            sub = (int)(g() % 6);
            if (sub == 1) { bytes(body, 3); chanInfo(body); } else if (sub == 2) { bytes(body, 9); chanInfo(body); } else bytes(body, g() % 8);
        } else {
            static const int subs[] = {0x04, 0x05, 0x06, 0x07, 0x08, 0x40, 0x41, 0x7F, 0x12};
            sub = subs[g() % 9];
            if (sub == 0x05) { const size_t n = g() % 40; for (size_t i = 0; i < n; i++) body.push_back((uint8_t)(32 + g() % 90)); }
            else if (sub == 0x08) { body.push_back((uint8_t)(g() % 20)); bytes(body, g() % 20); }
            else bytes(body, g() % 8 == 0 ? g() % 4 : (sub == 0x04 ? 5 : g() % 40));
        }
        size_t len = body.size();
        if (g() % 12 == 0) len += g() % 5;                          // a length that runs past the data
        put16(wp, lng ? (unsigned)(len & 0x7FF) | (unsigned)sub << 11 | 0x8000u : (unsigned)(len & 0xFF) | (unsigned)sub << 8);
        wp.insert(wp.end(), body.begin(), body.end());
    }
    return wp;
}

// A frame with a random but mostly well-formed layout: any frame control field, enough address octets, aux security header, header IEs, payload IEs
static std::vector<uint8_t> randFrame() {
    std::vector<uint8_t> v;
    unsigned fc = r32() & 0xFFFF;
    if (g() % 2) fc = (fc & ~(3u << 12)) | 2u << 12;                // version 2
    if (g() % 4) fc |= 1u << 9;                                      // IE list present
    if (g() % 3 == 0) fc &= ~(1u << 3);                              // not secured
    put16(v, fc);
    const int version = (int)(fc >> 12 & 3), dm = (int)(fc >> 10 & 3), sm = (int)(fc >> 14 & 3);
    const bool seqSup = (fc >> 8 & 1) != 0, pc = (fc >> 6 & 1) != 0, secured = (fc >> 3 & 1) != 0;
    if (!(version == 2 && seqSup)) v.push_back(rb());
    bool dPan, sPan;
    const bool dA = dm != 0, sA = sm != 0;
    if (version == 2) {
        dPan = false; sPan = false;
        if (!dA && !sA) dPan = pc; else if (dA && !sA) dPan = !pc; else if (!dA && sA) sPan = !pc; else if (dm == 3 && sm == 3) dPan = !pc; else { dPan = true; sPan = !pc; }
    } else { dPan = dA; sPan = sA && !(pc && dA); }
    if (dPan) bytes(v, 2);
    if (dm == 2) bytes(v, 2); else if (dm == 3) bytes(v, 8);
    if (sPan) bytes(v, 2);
    if (sm == 2) bytes(v, 2); else if (sm == 3) bytes(v, 8);
    if (secured) {
        const int kim = (int)(g() % 4);
        const bool noCtr = g() % 5 == 0;
        v.push_back((uint8_t)(0x0E & 7 | kim << 3 | (noCtr ? 1 << 5 : 0) | (g() % 8)));
        if (!noCtr) bytes(v, 4);
        static const int kl[4] = {0, 1, 5, 9};
        bytes(v, (size_t)kl[kim]);
    }
    if (fc >> 9 & 1) {
        for (int k = (int)(g() % 6); k > 0; k--) {                   // header IEs
            std::vector<uint8_t> body;
            unsigned id = g() % 3 ? 0x2A : (g() % 2 ? r32() & 0xFF : 0x7F);
            if (id == 0x7E || id == 0x7F) id = 0x2A;
            if (id == 0x2A) {
                static const int subs[] = {1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 0x80, 0x33};
                body.push_back((uint8_t)subs[g() % 18]);
                bytes(body, g() % 6 == 0 ? g() % 3 : (body[0] == 9 ? 8 : g() % 10));
            } else bytes(body, g() % 6);
            size_t len = body.size();
            if (g() % 15 == 0) len += g() % 4;
            put16(v, (unsigned)(len & 0x7F) | id << 7);
            v.insert(v.end(), body.begin(), body.end());
        }
        const int term = (int)(g() % 5);
        if (term != 0) put16(v, term == 1 ? 0x7Fu << 7 : 0x7Eu << 7);
        if (term > 1) {
            if (!secured) {
                for (int k = (int)(g() % 4); k > 0; k--) {
                    std::vector<uint8_t> body;
                    unsigned group = g() % 3 == 0 ? 0x4 : g() % 3 == 0 ? 0x3 : g() % 4 == 0 ? 0xF : g() % 16;
                    if (group == 0x4) body = wpIes();
                    else if (group == 0x3) { body.push_back((uint8_t)(g() % 8)); put16(body, g() % 3 ? 1 : r32() & 0xFFFF); body.push_back((uint8_t)(g() % 3 ? 1 : g() % 9)); bytes(body, g() % 14); }
                    else bytes(body, g() % 8);
                    size_t len = body.size();
                    if (g() % 15 == 0) len += g() % 4;
                    put16(v, (unsigned)(len & 0x7FF) | group << 11 | 0x8000u);
                    v.insert(v.end(), body.begin(), body.end());
                }
            }
            bytes(v, g() % 12);
        }
    }
    return v;
}

int main() {
    const char* itc = getenv("ONAIR_DIFF_ITERS");
    const long iters = itc ? atol(itc) : 200000;
    // PHR: every value, both directions
    for (uint32_t v = 0; v < 65536; v++) {
        const A::Phr a = A::parsePhr((uint16_t)v);
        dig.add(a.modeSwitch, a.fcs2, a.whitened, a.length);
        A::Phr x;
        x.modeSwitch = v & 1; x.fcs2 = v >> 1 & 1; x.whitened = v >> 2 & 1; x.length = (int)(r32() % 5000) - 100;
        dig.add(A::buildPhr(x));
    }
    // whitening, FCS
    long fcsGood = 0;
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> d(g() % 20 == 0 ? g() % 3000 : g() % 300);
        for (auto& b : d) b = rb();
        auto w1 = d;
        A::whiten(w1.data(), w1.size());
        dig.add(w1);
        dig.add(A::crc16(d.data(), d.size()), A::crc32(d.data(), d.size()));
        const bool fcs2 = g() % 2;
        if (g() % 2) { A::addFcs(d, fcs2); if (g() % 4 == 0) d[g() % d.size()] ^= 1; }
        const bool oa = A::fcsOk(d.data(), d.size(), fcs2);
        dig.add(oa);
        fcsGood += oa;
    }
    for (int t = -3; t < 300; t++)
        dig.add(A::uttName(t), A::uttShort(t), A::chFunctionName(t), A::regDomainName(t));
    // schedule text of random schedules
    for (long i = 0; i < iters; i++) {
        A::Schedule s;
        s.present = g() % 5 != 0; s.bcastIntervalMs = g() % 3 ? 0 : r32() % 100000; s.bsi = (int)(g() % 70000) - 1; s.dwellMs = (int)(g() % 300);
        s.clockDrift = (int)(g() % 256); s.timingAccuracy = (int)(g() % 256); s.chPlan = (int)(g() % 4); s.chFunction = (int)(g() % 5); s.excluded = (int)(g() % 4);
        s.regDomain = (int)(g() % 20) - 1; s.opClass = (int)(g() % 10) - 1; s.chPlanId = (int)(g() % 10) - 1; s.ch0Mhz = (double)(r32() % 16777216) / 1000.0;
        s.spacingCode = (int)(g() % 7) - 2; s.numChannels = (int)(g() % 70000); s.fixedChannel = (int)(g() % 400) - 5; s.excludedCount = g() % 3 ? 0 : (int)(g() % 90) - 20;
        dig.add(A::scheduleText(s));
    }
    // frames
    long parsedOk = 0, withNet = 0, secured = 0, withEap = 0;
    for (long i = 0; i < iters; i++) {
        std::vector<uint8_t> f;
        const int mode = (int)(g() % 10);
        if (mode < 4) {
            A::Build b;
            b.utt = (int)(g() % 12); b.secured = g() % 4 == 0; b.keyIndex = (int)(g() % 5); b.frameCounter = r32(); b.seq = g() % 2 ? -1 : (int)(g() % 256);
            b.src = g() % 4 ? r64() : 0; b.dst = g() % 2 ? r64() : 0;
            b.srcPanPresent = g() % 2; b.pan = (uint16_t)g(); b.ackReq = g() % 2; b.ufsi = r32() & 0xFFFFFF; b.bt = g() % 2; b.bsn = (int)(g() % 65536); b.bio = r32() & 0xFFFFFF;
            b.rslDbm = g() % 2 ? -999 : (int)(g() % 100) - 120; b.ea = g() % 2 ? 0 : r64(); b.lbc = g() % 3 == 0; b.lfnBcastMs = r32() & 0xFFFFFF;
            for (A::Schedule* s : {&b.us, &b.bs}) {
                s->present = g() % 2;
                s->bcastIntervalMs = r32() % 100000; s->bsi = (int)(g() % 65536); s->dwellMs = (int)(g() % 256); s->clockDrift = (int)(g() % 256); s->timingAccuracy = (int)(g() % 256);
                s->chPlan = (int)(g() % 3); s->chFunction = (int)(g() % 4); s->regDomain = (int)(g() % 17); s->opClass = (int)(g() % 4); s->chPlanId = (int)(g() % 20);
                s->ch0Mhz = (double)(r32() % 2400000) / 1000.0; s->spacingCode = (int)(g() % 4); s->numChannels = (int)(g() % 500); s->fixedChannel = (int)(g() % 130);
            }
            b.panIe = g() % 2; b.panSize = (int)(g() % 65536); b.routingCost = (int)(g() % 65536); b.tpsVersion = (int)(g() % 8); b.useParentBs = g() % 2;
            if (g() % 2) { const size_t n = g() % 40; for (size_t k = 0; k < n; k++) b.netName += (char)(33 + g() % 90); }
            b.panVersion = g() % 2 ? -1 : (int)(g() % 65536); b.gtkHash = g() % 2;
            if (g() % 2) { const size_t n = g() % 18; for (size_t k = 0; k < n; k++) b.phyModes.push_back((int)(g() % 256)); }
            if (g() % 2) { b.mpxId = g() % 3 ? 1 : (int)(g() % 65536); b.kmpId = g() % 3 ? 1 : (int)(g() % 4) - 1; b.payload = {0x03, 0x00, 0x00, 0x05, g() % 2 ? (uint8_t)1 : (uint8_t)2, 0x00, 0x00, 0x05, 0x01}; bytes(b.payload, g() % 6); }
            else bytes(b.payload, g() % 60);
            f = A::buildMpdu(b);
            if (mode == 3) f = mutate(f);
        } else if (mode < 9) {
            f = randFrame();
            if (g() % 6 == 0) f = mutate(f);
        } else {
            f.resize(g() % 40);
            for (auto& b : f) b = rb();
        }
        if (g() % 10 == 0) f.resize(g() % (f.size() + 1));
        const A::Frame a = A::parse(f.data(), f.size());
        addFrame(a);
        parsedOk += a.ok; withNet += a.hasNetName; secured += a.encrypted; withEap += a.eapolType >= 0;
    }
    printf("test_wisun_proto_rs: %ld FCS good, %ld frames parsed, %ld with a network name, %ld secured, %ld with EAPOL\n", fcsGood, parsedOk, withNet, secured, withEap);
    return golden::check("wisun_proto_rs", dig, 0x80fae39df2a8fef7ull, !itc);
}
