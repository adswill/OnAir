// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA broadcast PDUs (tetra_pdu.h).
#include "dect2/tetra/tetra_pdu.h"
#include <cstdio>
#include <random>

namespace dect2::tetra {

namespace {

struct W {
    Bits b;
    void put(uint64_t v, int n) { for (int i = n - 1; i >= 0; i--) b.push_back((uint8_t)((v >> i) & 1)); }
};

} // namespace

// The parsing side (everything that reads a received block) is Rust: tetra_pdu_rs.cpp over rust/onair_tetra. The encoders and the generator PDUs are here.

// ---------------------------------------------------------------- SYNC

Bits encodeSync(const Sync& s) {
    W w;
    w.put((uint64_t)s.systemCode, 4); w.put((uint64_t)s.colourCode, 6); w.put((uint64_t)(s.tn - 1), 2); w.put((uint64_t)s.fn, 5); w.put((uint64_t)s.mn, 6);
    w.put((uint64_t)s.sharingMode, 2); w.put((uint64_t)s.tsReservedFrames, 3); w.put(s.uplaneDtx, 1); w.put(s.frame18Ext, 1); w.put(0, 1);
    w.put((uint64_t)s.mcc, 10); w.put((uint64_t)s.mnc, 14); w.put((uint64_t)s.nbrBroadcast, 2); w.put((uint64_t)s.cellLoad, 2); w.put(s.lateEntry, 1);
    return w.b;
}


// ---------------------------------------------------------------- SYSINFO

Bits encodeSysinfo(const Sysinfo& s) {
    W w;
    w.put(2, 2); w.put(0, 2);
    w.put((uint64_t)s.mainCarrier, 12); w.put((uint64_t)s.band, 4); w.put((uint64_t)s.offset, 2); w.put((uint64_t)s.duplex, 3); w.put(s.reverse, 1);
    w.put((uint64_t)s.numScch, 2); w.put((uint64_t)s.msTxPwrMax, 3); w.put((uint64_t)s.rxLevAccessMin, 4); w.put((uint64_t)s.accessParam, 4); w.put((uint64_t)s.dlTimeout, 4);
    w.put(s.cckFlag, 1); w.put((uint64_t)s.hyperframeOrCck, 16);
    w.put((uint64_t)s.optFlag, 2); w.put(s.optValue, 20);
    w.put((uint64_t)s.la, 14); w.put((uint64_t)s.subscriberClass, 16); w.put((uint64_t)s.bsService, 12);
    return w.b;
}


// ---------------------------------------------------------------- generator PDUs

Bits macNullPdu(int type1Bits) {
    W w;
    w.put(0, 2); w.put(0, 1); w.put(0, 1); w.put(0, 2); w.put(0, 1); w.put(2, 6); w.put(0, 3);
    w.b.resize((size_t)type1Bits, 0);
    return w.b;
}

Bits macBroadcastResource(const Bits& mle, int type1Bits, int encMode, uint32_t seed) {
    W h;
    const int hdr = 2 + 1 + 1 + 2 + 1 + 6 + 3 + 24 + 3;
    int used = hdr + 4 + (int)mle.size();
    const bool fill = used % 8 != 0;
    if (fill) used += 1;
    int li = (used + 7) / 8;
    if (li * 8 > type1Bits) li = type1Bits / 8;
    h.put(0, 2); h.put(fill, 1); h.put(0, 1); h.put((uint64_t)encMode, 2); h.put(0, 1); h.put((uint64_t)li, 6); h.put(1, 3); h.put(0xFFFFFF, 24);
    h.put(0, 1); h.put(0, 1); h.put(0, 1);
    Bits sdu;
    W l; l.put(2, 4);   // LLC BL-UDATA
    sdu = l.b;
    sdu.insert(sdu.end(), mle.begin(), mle.end());
    if (encMode) { std::mt19937 g(seed); for (auto& v : sdu) v = (uint8_t)(g() & 1); }
    Bits out = h.b;
    out.insert(out.end(), sdu.begin(), sdu.end());
    if (fill) out.push_back(1);
    out.resize((size_t)li * 8, 0);
    if ((int)out.size() + 16 <= type1Bits) { const Bits nul = macNullPdu(16); out.insert(out.end(), nul.begin(), nul.end()); }
    out.resize((size_t)type1Bits, 0);
    return out;
}

Bits mleNwrkBroadcast(const NwrkBroadcast& n) {
    W w;
    w.put(5, 3); w.put(2, 3);
    w.put((uint64_t)n.reselect, 16); w.put((uint64_t)n.cellLoad, 2);
    w.put(1, 1);   // O-bit
    w.put(n.hasTime, 1); if (n.hasTime) w.put(n.time48, 48);
    w.put(1, 1); w.put(n.nbrs.size(), 3);
    for (const auto& nb : n.nbrs) {
        w.put((uint64_t)nb.cellId, 5); w.put((uint64_t)nb.reselTypes, 2); w.put(nb.synced, 1); w.put((uint64_t)nb.cellLoad, 2); w.put((uint64_t)nb.carrier, 12);
        w.put(1, 1);
        w.put(nb.hasExt, 1); if (nb.hasExt) { w.put((uint64_t)nb.band, 4); w.put((uint64_t)nb.offset, 2); w.put((uint64_t)nb.duplex, 3); w.put(nb.reverse, 1); }
        w.put(nb.mcc >= 0, 1); if (nb.mcc >= 0) w.put((uint64_t)nb.mcc, 10);
        w.put(nb.mnc >= 0, 1); if (nb.mnc >= 0) w.put((uint64_t)nb.mnc, 14);
        w.put(nb.la >= 0, 1); if (nb.la >= 0) w.put((uint64_t)nb.la, 14);
        w.put(0, 1); w.put(0, 1); w.put(0, 1);
        w.put(nb.bsService >= 0, 1); if (nb.bsService >= 0) w.put((uint64_t)nb.bsService, 12);
        w.put(0, 1); w.put(0, 1);
    }
    w.put(0, 1);   // M-bit: no type 3 elements
    return w.b;
}

} // namespace dect2::tetra
