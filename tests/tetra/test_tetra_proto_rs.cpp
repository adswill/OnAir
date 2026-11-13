// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Golden-vector test of the Rust TETRA network information layer (rust/onair_tetra, dect2::tetra): SYNC and SYSINFO parsing (valid, mutated, truncated,
// random), service / security / carrier helpers over all inputs, network time text, access-assign usage, and the signalling block walk over generated
// broadcasts (neighbours, time, encrypted, fragments, fill bits) and noise, over ONAIR_DIFF_ITERS (default 200000) seeded inputs per family. Every result goes
// into a digest (tests/common/golden.h; doubles rounded to 1e-6) compared with the one recorded from the C++ layer the Rust code replaced.
#include "dect2/tetra/tetra_pdu.h"
#include "../common/golden.h"
#include "../common/ordered.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
namespace A = dect2::tetra;
using Bits = std::vector<uint8_t>;

static golden::Digest dig;
static long okSync = 0, okSys = 0, nwrkSeen = 0, nbrSeen = 0, sysBlocks = 0;
static std::mt19937 g(24681);

static void addSync(const A::Sync& a) {
    dig.add(a.systemCode, a.colourCode, a.tn, a.fn, a.mn, a.sharingMode, a.tsReservedFrames, a.uplaneDtx, a.frame18Ext, a.mcc, a.mnc);
    dig.add(a.nbrBroadcast, a.cellLoad, a.lateEntry);
}
static void addSys(const A::Sysinfo& a) {
    dig.add(a.mainCarrier, a.band, a.offset, a.duplex, a.reverse, a.numScch, a.msTxPwrMax, a.rxLevAccessMin, a.accessParam, a.dlTimeout);
    dig.add(a.cckFlag, a.hyperframeOrCck, a.optFlag, a.optValue, a.la, a.subscriberClass, a.bsService);
}
static void addNb(const A::Neighbour& a) {
    dig.add(a.cellId, a.reselTypes, a.cellLoad, a.carrier, a.synced, a.hasExt, a.band, a.offset, a.duplex, a.reverse, a.mcc, a.mnc);
    dig.add(a.la, a.maxTx, a.minRx, a.subscriberClass, a.bsService, a.timeshare, a.frameOffset);
}
static void addBlk(const A::MacBlock& a) {
    dig.add(a.sysinfo, a.accessDefine, a.resources, a.encrypted, a.fragments);
    dig.bytes(a.byPd, sizeof a.byPd);
    dig.add((uint64_t)a.nwrk.size());
    if (a.sysinfo) addSys(a.si);
    for (const auto& x : a.nwrk) {
        dig.add(x.reselect, x.cellLoad, x.hasTime, x.time48, (uint64_t)x.nbrs.size());
        for (const auto& n : x.nbrs) addNb(n);
    }
}

static Bits rndBits(size_t n) { Bits b(n); for (auto& v : b) v = g() & 1; return b; }
static void mutate(Bits& b) {
    const int k = (int)(g() % 5);
    for (int i = 0; i < k && !b.empty(); i++) {
        switch (g() % 5) {
        case 0: b[g() % b.size()] ^= 1; break;
        case 1: b.resize(g() % (b.size() + 1)); break;
        case 2: b.erase(b.begin() + (long)(g() % b.size())); break;
        case 3: ORD_CALL(b.insert, b.begin() + (long)(g() % (b.size() + 1)), (uint8_t)(g() & 1)); break;   // ORD_*: draws in a fixed order (arguments and operands are not evaluated in one)
        default: { const size_t at = g() % b.size(); for (size_t j = at; j < b.size() && j < at + 8; j++) b[j] = g() & 1; }
        }
    }
}

template <class N> static N rndNwrk() {
    N n;
    n.reselect = (int)(g() & 0xFFFF); n.cellLoad = (int)(g() & 3);
    n.hasTime = g() & 1; const uint64_t tHi = (uint64_t)g() << 16; n.time48 = (tHi ^ g()) & 0xFFFFFFFFFFFFull;
    if (g() % 4 == 0) {
        const uint64_t f1 = (uint64_t)(g() % 40000000) << 24, f2 = (uint64_t)(g() & 1) << 23, f3 = (uint64_t)(g() % 64) << 17, f4 = (uint64_t)(g() % 64) << 11;
        n.time48 = f1 | f2 | f3 | f4;
    }
    const int cnt = (int)(g() % 8);
    for (int i = 0; i < cnt; i++) {
        typename std::remove_reference<decltype(n.nbrs[0])>::type nb;
        nb.cellId = (int)(g() & 31); nb.reselTypes = (int)(g() & 3); nb.synced = g() & 1; nb.cellLoad = (int)(g() & 3); nb.carrier = (int)(g() & 4095);
        nb.hasExt = g() & 1; nb.band = (int)(g() & 15); nb.offset = (int)(g() & 3); nb.duplex = (int)(g() & 7); nb.reverse = g() & 1;
        if (g() & 1) nb.mcc = (int)(g() & 1023);
        if (g() & 1) nb.mnc = (int)(g() & 16383);
        if (g() & 1) nb.la = (int)(g() & 16383);
        if (g() & 1) nb.bsService = (int)(g() & 4095);
        n.nbrs.push_back(nb);
    }
    return n;
}

static Bits mkBlock(int kind) {
    const int len = g() % 2 ? 124 : 268;
    switch (kind) {
    case 0: { A::Sysinfo s; s.mainCarrier = (int)(g() & 4095); s.band = (int)(g() & 15); s.optFlag = (int)(g() & 3); s.optValue = g() & 0xFFFFF; s.la = (int)(g() & 16383); s.bsService = (int)(g() & 4095); Bits b = A::encodeSysinfo(s); b.resize((size_t)len, 0); return b; }
    case 1: case 2: case 3: {
        const auto n = rndNwrk<A::NwrkBroadcast>();
        const int cellsel = kind == 3 ? (int)(g() % 4) : 0; const uint32_t mseed = g();
        Bits b = A::macBroadcastResource(A::mleNwrkBroadcast(n), len, cellsel, mseed);
        if (kind == 2) { const auto n2 = rndNwrk<A::NwrkBroadcast>(); const uint32_t seed2 = g(); const Bits more = A::macBroadcastResource(A::mleNwrkBroadcast(n2), len, 0, seed2); b.insert(b.begin() + 16, more.begin(), more.begin() + 40); }
        return b;
    }
    case 4: return A::macNullPdu(len);
    default: return rndBits(g() % 300);
    }
}

int main() {
    const char* e = getenv("ONAIR_DIFF_ITERS");
    const long iters = e ? atol(e) : 200000;
    for (long it = 0; it < iters; it++) {
        // SYNC
        {
            A::Sync as;
            as.mcc = 777; as.tn = 2;
            A::Sync src; src.systemCode = (int)(g() & 15); src.colourCode = (int)(g() & 63); src.tn = 1 + (int)(g() & 3); src.fn = 1 + (int)(g() % 18); src.mn = 1 + (int)(g() % 60);
            src.sharingMode = (int)(g() & 3); src.tsReservedFrames = (int)(g() & 7); src.uplaneDtx = g() & 1; src.frame18Ext = g() & 1;
            src.mcc = (int)(g() & 1023); src.mnc = (int)(g() & 16383); src.nbrBroadcast = (int)(g() & 3); src.cellLoad = (int)(g() & 3); src.lateEntry = g() & 1;
            Bits b = g() % 4 ? A::encodeSync(src) : rndBits(g() % 70);
            mutate(b);
            const bool a = A::parseSync(b, as);
            dig.add(a); addSync(as);
            okSync += a;
        }
        // SYSINFO
        {
            A::Sysinfo as;
            as.la = 4242; as.band = 5;
            A::Sysinfo src; src.mainCarrier = (int)(g() & 4095); src.band = (int)(g() & 15); src.offset = (int)(g() & 3); src.duplex = (int)(g() & 7); src.reverse = g() & 1;
            src.numScch = (int)(g() & 3); src.msTxPwrMax = (int)(g() & 7); src.rxLevAccessMin = (int)(g() & 15); src.accessParam = (int)(g() & 15); src.dlTimeout = (int)(g() & 15);
            src.cckFlag = g() & 1; src.hyperframeOrCck = (int)(g() & 0xFFFF); src.optFlag = (int)(g() & 3); src.optValue = g() & 0xFFFFF;
            src.la = (int)(g() & 16383); src.subscriberClass = (int)(g() & 0xFFFF); src.bsService = (int)(g() & 4095);
            Bits b = g() % 4 ? A::encodeSysinfo(src) : rndBits(g() % 140);
            mutate(b);
            const bool a = A::parseSysinfo(b, as);
            dig.add(a); addSys(as);
            okSys += a;
            const auto sa = A::parseSecurity(as);
            dig.add(sa.valid, sa.authRequired, sa.class1, sa.class2, sa.class3);
            const int bsLow = (int)(g() & 0xFFF); const int bs = bsLow | (g() % 8 == 0 ? (int)g() : 0);
            dig.add(A::serviceText(bs));
            const int band = (int)(g() % 40) - 8, car = (int)(g() % 9000) - 2000, off = (int)(g() % 12) - 4, dup = (int)(g() % 20) - 6;
            const bool rev = g() & 1;
            dig.add(A::dlCarrierHz(band, car, off));
            dig.add(A::duplexSpacingHz(band, dup));
            dig.add(A::ulCarrierHz(band, car, off, dup, rev));
        }
        // network time and AACH
        {
            const uint64_t tHi = (uint64_t)g() << 24; uint64_t t = (tHi ^ g()) & 0xFFFFFFFFFFFFull;
            if (g() % 3) t = (uint64_t)(g() % 17000000) << 24 | (t & 0xFFFFFF);
            if (g() % 50 == 0) t |= 0xFFFFFFull << 24;
            dig.add(A::networkTimeText(t));
            const uint16_t aa = (uint16_t)g(); const bool f18 = g() & 1;
            const int u = A::aachDlUsage(aa, f18);
            dig.add(u, A::dlUsageText(u));
        }
        // block walk
        {
            Bits b = mkBlock((int)(g() % 6));
            if (g() % 3 == 0) mutate(b);
            A::MacBlock a;
            if (g() % 4 == 0) { a.resources = 3; a.byPd[5] = 2; }   // accumulation onto a filled record
            A::parseMacBlock(b, a);
            addBlk(a);
            nwrkSeen += (long)a.nwrk.size(); sysBlocks += a.sysinfo;
            for (const auto& n : a.nwrk) nbrSeen += (long)n.nbrs.size();
        }
    }
    printf("tetra proto rs: valid sync %ld, sysinfo %ld, sysinfo blocks %ld, nwrk %ld, neighbours %ld\n", okSync, okSys, sysBlocks, nwrkSeen, nbrSeen);
    return golden::check("tetra_proto_rs", dig, 0xd821754b2b92273cull, !e);
}
