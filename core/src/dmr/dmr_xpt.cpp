// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Hytera XPT: see dmr_xpt.h. Field positions from DSD-FME (dmr_csbk.c, dmr_flco.c).
#include "dect2/dmr/dmr_xpt.h"
#include <cstdio>

namespace dect2 {

static int bitsAt(const uint8_t* d, int bit, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | ((d[(bit + i) / 8] >> (7 - (bit + i) % 8)) & 1);
    return v;
}
static void putBits(uint8_t* d, int bit, int n, int v) {
    for (int i = 0; i < n; i++) if ((v >> (n - 1 - i)) & 1) d[(bit + i) / 8] |= (uint8_t)(1 << (7 - (bit + i) % 8));
}

bool xptParseCsbk(const Csbk& c, double now, XptState& st) {
    if (c.fid != kXptFid) return false;
    if (c.opcode == kXptSiteStatusOp) {
        const int seq = (c.lb ? 2 : 0) | (c.pf ? 1 : 0);
        if (seq > 2) return false;                      // the fourth value has not been seen in use
        st.seen = true; st.lastSec = now;
        st.freeLcn = bitsAt(c.data, 0, 4);
        for (int i = 0; i < 6; i++) {
            XptLsn& l = st.lsn[seq * 6 + i];
            l.status = bitsAt(c.data, 4 + 2 * i, 2);
            l.tg = c.data[2 + i];
            l.lastSec = now;
        }
        return true;
    }
    if (c.opcode == kXptAdjacentOp) {
        st.seen = true; st.lastSec = now;
        for (int i = 0; i < 4; i++) {
            const int site = bitsAt(c.data, 16 * i, 5);
            if (!site) continue;
            const int freeR = bitsAt(c.data, 8 + 16 * i, 4);
            bool found = false;
            for (auto& a : st.adjacent) if (a.site == site) { a.freeLcn = freeR; a.lastSec = now; found = true; }
            if (!found) st.adjacent.push_back({site, freeR, now});
        }
        return true;
    }
    return false;
}

bool xptParseCall(const FullLc& f, double now, XptState& st) {
    if (f.fid != kXptFid || f.flco != kXptCallSetupFlco) return false;
    XptCall c;
    c.lcn = bitsAt(f.raw, 16, 4); c.freeLcn = bitsAt(f.raw, 24, 4); c.handshake = bitsAt(f.raw, 28, 4);
    c.target = (uint32_t)bitsAt(f.raw, 32, 16); c.source = (uint32_t)bitsAt(f.raw, 56, 16);
    c.sec = now;
    st.seen = true; st.lastSec = now;
    if (st.calls.empty() || st.calls.front().target != c.target || st.calls.front().source != c.source || now - st.calls.front().sec > 3.0) st.calls.insert(st.calls.begin(), c);
    else st.calls.front() = c;
    if (st.calls.size() > 32) st.calls.pop_back();
    return true;
}

Csbk xptSiteStatusCsbk(int seq, int freeLcn, const int status[6], const int tg[6]) {
    Csbk c;
    c.lb = (seq & 2) != 0; c.pf = (seq & 1) != 0;
    c.opcode = kXptSiteStatusOp; c.fid = kXptFid;
    putBits(c.data, 0, 4, freeLcn);
    for (int i = 0; i < 6; i++) { putBits(c.data, 4 + 2 * i, 2, status[i]); c.data[2 + i] = (uint8_t)tg[i]; }
    return c;
}

FullLc xptCallLc(int lcn, int freeLcn, int handshake, uint32_t target, uint32_t source) {
    FullLc f;
    f.fid = kXptFid; f.flco = kXptCallSetupFlco; f.dst = target; f.src = source;
    f.raw[0] = (uint8_t)kXptCallSetupFlco; f.raw[1] = (uint8_t)kXptFid;
    putBits(f.raw, 16, 4, lcn); putBits(f.raw, 24, 4, freeLcn); putBits(f.raw, 28, 4, handshake);
    putBits(f.raw, 32, 16, (int)target); putBits(f.raw, 56, 16, (int)source);
    return f;
}

std::string xptSummary(const XptState& st) {
    if (!st.seen) return "";
    char b[80];
    int busy = 0;
    for (const auto& l : st.lsn) if (l.status == 3 || l.status == 2) busy++;
    snprintf(b, sizeof b, "Hytera XPT: free LCN %d, %d slot%s in use", st.freeLcn, busy, busy == 1 ? "" : "s");
    return b;
}

} // namespace dect2
