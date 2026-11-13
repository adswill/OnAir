// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// TETRA physical layer and PDUs: channel coding round trips with errors, Reed-Muller, the PDU layouts, and one carrier of the test
// signal decoded at 4 samples per symbol.
#include "dect2/tetra/tetra_gen.h"
#include "dect2/common/stable_random.h"
#include "dect2/tetra/tetra_phy.h"
#include "dect2/tetra/tetra_pdu.h"
#include "dect2/tetra/tetra_rx.h"
#include "dect2/tetra/tetra_tel.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::tetra;
#include "../common/test_util.h"

static std::vector<float> toLlr(const Bits& b, std::mt19937& g, double flipRate) {
    dect2::uniform_real_distribution<double> u(0, 1);
    std::vector<float> l(b.size());
    for (size_t i = 0; i < b.size(); i++) { int v = b[i]; if (u(g) < flipRate) v ^= 1; l[i] = v ? 1.f : -1.f; }
    return l;
}

int main() {
    std::mt19937 g(5);
    // CRC: the type-2 block of a known pattern checks, a flipped bit does not
    {
        Bits b(60, 0); for (int i = 0; i < 60; i += 3) b[(size_t)i] = 1;
        const uint16_t c = crc16(b.data(), 60);
        b[7] ^= 1;
        CHECK(crc16(b.data(), 60) != c, "crc detects a flip");
    }
    // logical channels with 0.8 % hard errors (rate 2/3, hard decisions)
    for (const ChanSpec* c : {&kBsch, &kSchHd, &kSchF}) {
        int ok = 0;
        for (int t = 0; t < 20; t++) {
            Bits t1((size_t)c->type1);
            for (auto& v : t1) v = (uint8_t)(g() & 1);
            const uint32_t scr = scramblerInit(1, 1, 5);
            const Bits t5 = encodeChannel(t1, *c, scr);
            CHECK((int)t5.size() == c->K, "type-5 length %zu", t5.size());
            const std::vector<float> l = toLlr(t5, g, 0.008);
            Bits back;
            if (decodeChannel(l.data(), *c, scr, back) && back == t1) ok++;
            // the wrong scrambling code must not pass
            Bits w;
            CHECK(!decodeChannel(l.data(), *c, scramblerInit(1, 1, 6), w), "decoded with the wrong colour code");
        }
        CHECK(ok >= 19, "channel K=%d: %d / 20 decoded", c->K, ok);
    }
    // Reed-Muller (30,14): up to 3 errors corrected
    {
        int ok = 0;
        for (int t = 0; t < 200; t++) {
            const uint16_t info = (uint16_t)(g() & 0x3FFF);
            uint32_t cw = rm3014Encode(info);
            for (int e = 0; e < t % 4; e++) cw ^= 1u << (g() % 30);
            uint16_t back = 0;
            if (rm3014Decode(cw, back) && back == info) ok++;
        }
        CHECK(ok == 200, "RM: %d / 200", ok);
        const uint32_t scr = scramblerInit(1, 1, 5);
        const Bits bb = encodeAach(0x1abc & 0x3FFF, scr);
        std::vector<float> l = toLlr(bb, g, 0);
        uint16_t aa = 0;
        CHECK(decodeAach(l.data(), scr, aa) && aa == (0x1abc & 0x3FFF), "AACH %04x", aa);
    }
    // PDUs
    {
        Sync s; s.colourCode = 33; s.tn = 3; s.fn = 18; s.mn = 47; s.mcc = 246; s.mnc = 9999; s.sharingMode = 2; s.lateEntry = true; s.systemCode = 2;
        const Bits b = encodeSync(s);
        Sync r;
        CHECK(b.size() == 60 && parseSync(b, r) && r.colourCode == 33 && r.tn == 3 && r.fn == 18 && r.mn == 47 && r.mcc == 246 && r.mnc == 9999 && r.sharingMode == 2 && r.lateEntry, "SYNC");
        // the bit positions osmo-tetra and others read (colour code at 4, MCC at 31, MNC at 41)
        unsigned cc = 0, mcc = 0; for (int i = 0; i < 6; i++) cc = cc << 1 | b[(size_t)(4 + i)]; for (int i = 0; i < 10; i++) mcc = mcc << 1 | b[(size_t)(31 + i)];
        CHECK(cc == 33 && mcc == 246, "SYNC field positions");
        const TetraCellCfg c = tetraTestCell(0, 0);
        const Bits si = encodeSysinfo(c.si);
        Sysinfo q;
        CHECK(si.size() == 124 && parseSysinfo(si, q) && q.mainCarrier == 3696 && q.band == 3 && q.offset == 3 && q.la == 1001 && q.bsService == c.si.bsService, "SYSINFO");
        CHECK(std::fabs(dlCarrierHz(3, 3696, 3) - 392.4125e6) < 1 && std::fabs(ulCarrierHz(3, 3696, 3, 0, false) - 382.4125e6) < 1, "carrier frequencies");
        const Security sec = parseSecurity(q);
        CHECK(sec.valid && sec.authRequired && sec.class3 && !sec.class1, "security");
        {   // a real cell (France, MCC 208, seen in a 427 MHz capture): extended services with security bits 0110 0000 and no air interface encryption service bit.
            // The class 2 / 3 bit is only meaningful with that service bit (EN 300 392-7 A.8.77): this is a clear cell, not class 3.
            Sysinfo real = q; real.optFlag = 3; real.optValue = 0x60u << 12; real.bsService = 0xd65;
            Security r = parseSecurity(real);
            CHECK(r.valid && r.class1 && !r.class2 && !r.class3 && !r.authRequired, "real cell without the encryption service bit is not class 2 / 3");
            // the same bits with the service bit set: class 3 and class 1 MS both supported; authentication bit as sent
            real.bsService = 0xd67; real.optValue = 0xe0u << 12; r = parseSecurity(real);
            CHECK(r.class1 && r.class3 && !r.class2 && r.authRequired, "service bit set: class 3");
            real.optValue = 0x80u << 12; r = parseSecurity(real);
            CHECK(!r.class1 && r.class2 && !r.class3, "service bit set, class 2 / 3 bit 0: class 2");
            real.optValue = 0x1fu << 12;   // the five low bits are the SCK number / key services, never a class
            real.bsService = 0xd65; r = parseSecurity(real);
            CHECK(!r.class2 && !r.class3, "SCK bits are not a class");
            real.optFlag = 2; CHECK(!parseSecurity(real).valid, "no security information outside the extended services broadcast");
            // MAC header: encryption mode 0 is not counted, 2 is; unreadable blocks are not encryption
            TetraCell tc; tc.hasSysinfo = true; tc.aie = false;
            CHECK(tc.airEnc() == 1 && !tc.encryption(), "none advertised");
            tc.aie = true; CHECK(tc.airEnc() == 4 && tc.encryption(), "service available, class unknown"); tc.secClass3 = true; CHECK(tc.airEnc() == 3, "class 3");
            tc.aie = false; tc.secClass3 = false; tc.blocksBad = 1000; CHECK(!tc.encryption(), "bad blocks are not encryption");
            tc.encryptedPdus = 1; CHECK(tc.encryption(), "encryption mode seen");
            TetraCell nc; CHECK(nc.airEnc() == 0 && !nc.encryption(), "unknown before SYSINFO");
        }
        MacBlock mb;
        parseMacBlock(macBroadcastResource(mleNwrkBroadcast(c.nwrk), 268, 0), mb);
        CHECK(mb.nwrk.size() == 1 && mb.nwrk[0].nbrs.size() == 2 && mb.nwrk[0].nbrs[1].la == 1003 && mb.nwrk[0].nbrs[1].hasExt && mb.nwrk[0].hasTime, "D-NWRK-BROADCAST");
        CHECK(networkTimeText(c.nwrk.time48).rfind("2026-10-12 09:30:00 UTC", 0) == 0, "time %s", networkTimeText(c.nwrk.time48).c_str());
        MacBlock me;
        parseMacBlock(macBroadcastResource(Bits(120, 0), 268, 1, 9), me);
        CHECK(me.encrypted == 1 && me.nwrk.empty() && me.byPd[5] == 0, "encrypted PDU only counted");
        MacBlock mn;
        parseMacBlock(macNullPdu(268), mn);
        CHECK(mn.resources == 0 && !mn.sysinfo, "null PDU");
        MacBlock ms;
        Bits sb = encodeSysinfo(c.si);
        parseMacBlock(sb, ms);
        CHECK(ms.sysinfo && ms.si.la == 1001, "SYSINFO in a block");
    }
    // one carrier of the test signal at 72 kHz, no noise, then 15 dB
    for (double snr : {99.0, 15.0}) {
        const TetraCellCfg c = tetraTestCell(0, 0);
        Modulator mod;
        std::vector<cf32> x;
        for (uint32_t a = 0; a < 72 * 2 + 40; a++) mod.push(cellSlot(c, c.startSlot + a), x);
        dect2::normal_distribution<float> nd(0, 1);
        const float sg = snr >= 99 ? 0.f : (float)std::sqrt((tetra::kRate / 25e3) / std::pow(10.0, snr / 10) / 2);
        const double w = 2 * M_PI * 700 / tetra::kRate;
        for (size_t i = 0; i < x.size(); i++) x[i] = x[i] * cf32((float)std::cos(w * i), (float)std::sin(w * i)) + cf32{sg * nd(g), sg * nd(g)};
        const CarrierDecode d = decodeCarrier(x.data(), x.size());
        printf("  72 kHz, SNR %.0f: tetra %d synced %d syncs %d cfo %.0f Hz snr %.1f dB slots %d ok %d bad %d traffic %d\n", snr, d.tetra, d.synced, d.syncs, d.cfoHz, d.snrDb, d.slots, d.blocksOk, d.blocksBad, d.trafficSlots);
        CHECK(d.synced && d.sync.mcc == 1 && d.sync.mnc == 1 && d.sync.colourCode == 5, "BSCH");
        CHECK(std::fabs(d.cfoHz - 700) < 60, "cfo %.0f", d.cfoHz);
        bool si = false, nw = false; int enc = 0;
        for (const auto& m : d.macs) { if (m.sysinfo && m.si.la == 1001) si = true; if (!m.nwrk.empty()) nw = true; enc += m.encrypted; }
        CHECK(si && nw && enc > 0, "SYSINFO %d, D-NWRK %d, encrypted %d", si, nw, enc);
        CHECK(d.tsUsage[0] == 2 && d.tsUsage[1] >= 4 && d.tsUsage[2] == 0, "timeslot use %d %d %d %d", d.tsUsage[0], d.tsUsage[1], d.tsUsage[2], d.tsUsage[3]);
        CHECK(d.trafficSlots > 0, "traffic slots skipped");
    }
    printf(fails ? "tetra phy: %d FAILED\n" : "tetra phy: all passed\n", fails);
    return fails ? 1 : 0;
}
