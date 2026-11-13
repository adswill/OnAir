// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// cdma2000 / IS-95: the pilot PN sequences (period, balance, the 15 zero run, autocorrelation), the K = 9 code through the Viterbi decoder
// with noise, CRC-30, the bit reversal interleaver, the Sync Channel Message and its capsule, and then the whole chain: the test transmitter
// into the search for one to three base stations at different PN offsets, carrier offsets, sample clock errors, noise levels and capture
// starts in the middle of a frame. Simulated signals only.
#include "dect2/cdma/cdma.h"
#include "dect2/cdma/cdma_rx.h"
#include <chrono>
#include <thread>
#include "dect2/common/stable_random.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <random>
#include <set>
#include <vector>
using namespace dect2;
using namespace dect2::cdma;
#include "../common/test_util.h"
#include "../common/impair.h"

int main() {
    // ---- PN sequences
    for (int q = 0; q < 2; q++) {
        const auto& s = q ? pnQ() : pnI();
        CHECK(s.size() == 32768, "PN length %zu", s.size());
        int ones = 0; for (auto b : s) ones += b;
        CHECK(ones == 16384, "PN %d balance %d", q, ones);
        int run = 0, maxRun = 0; for (size_t k = 0; k < 2 * s.size(); k++) { if (s[k % s.size()] == 0) maxRun = std::max(maxRun, ++run); else run = 0; }
        CHECK(maxRun == 15 && s[0] == 1 && s[32767] == 0 && s[32753] == 0 && s[32752] == 1, "PN %d: the 15 zero run before index 0 (max run %d)", q, maxRun);
        long side = 0;   // autocorrelation at a few lags: about 0 (within a few of the +-1 of a maximum length sequence plus the inserted chip)
        for (int lag : {1, 7, 64, 1000, 16384}) { long a = 0; for (size_t k = 0; k < s.size(); k++) a += (s[k] ^ s[(k + (size_t)lag) % s.size()]) ? -1 : 1; side = std::max(side, std::labs(a)); }
        CHECK(side <= 400, "PN %d sidelobe %ld", q, side);   // the inserted chip breaks the ideal -1 of the m-sequence by up to a partial sum
    }
    // ---- recursion check of the I sequence: i(n) = i(n-15) ^ i(n-10) ^ i(n-8) ^ i(n-7) ^ i(n-6) ^ i(n-2) away from the inserted zero
    { const auto& s = pnI(); int bad = 0; for (int n = 15; n < 32700; n++) if (s[(size_t)n] != (s[(size_t)n - 15] ^ s[(size_t)n - 10] ^ s[(size_t)n - 8] ^ s[(size_t)n - 7] ^ s[(size_t)n - 6] ^ s[(size_t)n - 2])) bad++; CHECK(bad == 0, "I recursion %d", bad); }
    { const auto& s = pnQ(); int bad = 0; for (int n = 15; n < 32700; n++) if (s[(size_t)n] != (s[(size_t)n - 15] ^ s[(size_t)n - 12] ^ s[(size_t)n - 11] ^ s[(size_t)n - 10] ^ s[(size_t)n - 9] ^ s[(size_t)n - 5] ^ s[(size_t)n - 4] ^ s[(size_t)n - 3])) bad++; CHECK(bad == 0, "Q recursion %d", bad); }
    // ---- Walsh
    { int bad = 0; for (int a = 0; a < 64; a++) for (int b = 0; b < 64; b++) { int c = 0; for (int k = 0; k < 64; k++) c += (walshBit(a, k) ^ walshBit(b, k)) ? -1 : 1; if (c != (a == b ? 64 : 0)) bad++; } CHECK(bad == 0, "Walsh orthogonality %d", bad); }
    // ---- convolutional code: impulse response = the generators
    { ConvEnc e; Bits o; e.push(1, o); for (int i = 0; i < 8; i++) e.push(0, o);
      uint32_t g0 = 0, g1 = 0; for (int i = 0; i < 9; i++) { g0 = (g0 << 1) | o[(size_t)(2 * i)]; g1 = (g1 << 1) | o[(size_t)(2 * i + 1)]; }
      CHECK(g0 == 0753 && g1 == 0561, "generators %o %o", g0, g1); }
    { std::mt19937 rng(3); dect2::normal_distribution<float> g(0.f, 1.f);
      for (double ebn0 : {4.0, 3.0}) {
        int errs = 0, n = 0;
        for (int blk = 0; blk < 20; blk++) {
          Bits u(400); for (auto& b : u) b = rng() & 1;
          ConvEnc e; Bits c; for (auto b : u) e.push(b, c);
          const double sig = std::sqrt(1.0 / (2 * 0.5 * std::pow(10.0, ebn0 / 10)));
          std::vector<float> l(c.size()); for (size_t i = 0; i < c.size(); i++) l[i] = (float)((c[i] ? -1 : 1) + sig * g(rng));
          Bits d = viterbiK9(l);
          for (size_t i = 0; i + 10 < u.size(); i++) { errs += d[i] != u[i]; n++; }
        }
        printf("  K=9 r=1/2 at Eb/N0 %.0f dB: BER %.2e\n", ebn0, (double)errs / n);
        CHECK((double)errs / n < (ebn0 >= 4 ? 1e-3 : 5e-3), "Viterbi BER %g at %g dB", (double)errs / n, ebn0);
      } }
    // ---- interleaver: a permutation; BRO_3(6) = 3 as in the text
    { CHECK(bro(6, 3) == 3, "BRO"); std::set<int> s; for (int i = 0; i < 128; i++) s.insert(interleaveAddr(i, 7, 1)); CHECK(s.size() == 128, "128 interleaver");
      std::set<int> s2; for (int i = 0; i < 384; i++) s2.insert(interleaveAddr(i, 6, 6)); CHECK(s2.size() == 384 && *s2.rbegin() == 383, "384 interleaver"); }
    // ---- CRC-30 of all-zero data is not zero (registers start at ones); a flipped bit is caught
    { Bits z(40, 0); CHECK(crc30(z, z.size()) != 0, "crc of zeros"); Bits a(100); for (size_t i = 0; i < a.size(); i++) a[i] = (uint8_t)((i * 7) % 3 == 0);
      const uint32_t c0 = crc30(a, a.size()); int same = 0; for (size_t i = 0; i < a.size(); i++) { a[i] ^= 1; same += crc30(a, a.size()) == c0; a[i] ^= 1; } CHECK(same == 0, "crc flips"); }
    // ---- message and capsule
    SyncMsg m; m.sid = 4139; m.nid = 65535; m.pilotPn = 168; m.lcState = 0x2A5A5A5A5A5ULL; m.sysTime = 17000000000ULL; m.lpSec = 18; m.ltmOff = -10; m.daylt = true; m.prat = 1; m.cdmaFreq = 283; m.extCdmaFreq = 384; m.minPRev = 3; m.pRev = 6;
    { Bits pdu = encodeSyncPdu(m); CHECK(pdu.size() % 8 == 2 && pdu.size() == 186, "PDU length %zu", pdu.size());
      Bits cap = encapsulate(pdu); CHECK(cap.size() == 224, "capsule %zu", cap.size());
      std::vector<Bits> got; SyncReassembler ra; uint8_t f[32];
      for (int fr = 0; fr < 9; fr++) { f[0] = fr == 0; for (int i = 0; i < 31; i++) { size_t k = (size_t)(31 * fr + i); f[i + 1] = fr < 8 && k < cap.size() ? cap[k] : 0; } ra.frame(f, got); }
      SyncMsg r; CHECK(got.size() == 1 && parseSyncPdu(got[0], r), "reassembled %zu", got.size());
      CHECK(r.sid == m.sid && r.nid == m.nid && r.pilotPn == m.pilotPn && r.lcState == m.lcState && r.sysTime == m.sysTime && r.lpSec == 18 && r.ltmOff == -10 && r.daylt && r.prat == 1 && r.cdmaFreq == 283 && r.extCdmaFreq == 384 && r.minPRev == 3, "fields");
      CHECK(sysTimeText(0, 0, 0, false) == "1980-01-06 00:00:00.00 UTC", "epoch %s", sysTimeText(0, 0, 0, false).c_str());
      CHECK(sysTimeText(12, 0, 0, false) == "1980-01-06 00:00:00.96 UTC", "80 ms units");
      CHECK(std::fabs(cdmaChannelHz(0, 283) - 878.49e6) < 1 && std::fabs(cdmaChannelHz(1, 25) - 1931.25e6) < 1 && std::fabs(cdmaChannelHz(0, 1013) - 869.7e6) < 1, "channels"); }

    // ---- the chain
    struct Case { std::vector<int> pns; std::vector<double> amps; double cfo, ppm, snr; uint64_t start; };
    const Case cases[] = {
        {{168}, {1}, 0, 0, 10, 0},
        {{12}, {1}, 3100, 0, 3, 12345},
        {{300}, {1}, -4400, 12, 0, 7777777},
        {{5, 260}, {1, 0.6}, 1200, -8, 6, 99999},
        {{40, 41, 400}, {1, 0.7, 0.5}, -700, 4, 8, 3333},
        {{499}, {1}, 250, 0, -3, 54321},
    };
    const bool longRun = getenv("ONAIR_TEST_LONG") != nullptr;
    int ci = -1;
    for (const Case& c : cases) {
        ci++;
        if (!longRun && (ci == 1 || ci == 3)) continue;   // ONAIR_TEST_LONG=1: every case
        std::vector<TxCell> tx;
        for (size_t k = 0; k < c.pns.size(); k++) { TxCell t; t.msg = m; t.msg.pilotPn = c.pns[k]; t.msg.sid = 100 + (int)k; t.msg.sysTime = 1000 + 37 * k; t.amp = c.amps[k]; t.cfoHz = c.cfo + 40.0 * (double)k; tx.push_back(t); }
        std::vector<cf32> x = generate(tx, (size_t)(kFs * 0.7), c.snr, c.start, 5 + (unsigned)ci);
        if (c.ppm != 0) x = impair::clock(x, c.ppm);
        double ms = 0;
        auto f = search(x.data(), x.size(), {}, &ms);
        int ok = 0;
        for (size_t k = 0; k < c.pns.size(); k++) {
            bool hit = false;
            for (auto& r : f) if (r.hasMsg && r.msg.pilotPn == c.pns[k] && r.msg.sid == 100 + (int)k && r.msg.cdmaFreq == 283 && r.msg.lcState == m.lcState) hit = true;
            ok += hit;
        }
        int fake = 0; for (auto& r : f) { bool known = false; for (int p : c.pns) if (r.hasMsg && r.msg.pilotPn == p) known = true; if (r.hasMsg && !known) fake++; }
        printf("  case %d: %zu pilot(s) at PN", ci, c.pns.size()); for (int p : c.pns) printf(" %d", p);
        printf(", %+.0f Hz, %+.0f ppm, SNR %.0f dB: %d decoded, %zu found, %.0f ms", c.cfo, c.ppm, c.snr, ok, f.size(), ms);
        for (auto& r : f) printf(" [PN %d msgs %d fails %d Ec/Io %.1f cfo %+.0f ppm %+.1f]", r.hasMsg ? r.msg.pilotPn : -1, r.msgs, r.crcFails, r.ecIoDb, r.cfoHz, r.clockPpm);
        printf("\n");
        CHECK(ok == (int)c.pns.size(), "case %d: %d of %zu decoded", ci, ok, c.pns.size());
        CHECK(fake == 0, "case %d: %d messages with a wrong PN", ci, fake);
        double sa = 0; for (double a : c.amps) sa += a * a;
        for (auto& r : f) if (r.hasMsg) for (size_t k = 0; k < c.pns.size(); k++) if (r.msg.pilotPn == c.pns[k]) {
            const double want = -7 + 20 * std::log10(c.amps[k]) - 10 * std::log10(sa) - 10 * std::log10(1 + 0.5 * std::pow(10.0, -c.snr / 10));   // half the noise band passes the matched filter
            CHECK(std::fabs(r.cfoHz - (c.cfo + 40.0 * (double)k)) < 60, "case %d cfo %.0f", ci, r.cfoHz);
            CHECK(std::fabs(r.ecIoDb - want) < 2, "case %d Ec/Io %.1f, want %.1f", ci, r.ecIoDb, want);
        }
    }
    // ---- noise only: nothing
    { std::vector<cf32> x((size_t)kFs); std::mt19937 rng(9); dect2::normal_distribution<float> g(0.f, 0.1f); for (auto& v : x) v = cf32(g(rng), g(rng));
      auto f = search(x.data(), x.size()); int msgs = 0; for (auto& r : f) msgs += r.hasMsg; CHECK(msgs == 0, "noise: %d messages", msgs); printf("  noise only: %zu candidates kept, %d messages\n", f.size(), msgs); }
    // ---- the receiver thread and the mode row
    { std::vector<TxCell> tx(1); tx[0].msg = m; tx[0].cfoHz = 700;
      auto s = generate(tx, (size_t)(kFs * 1.0), 6, 4321, 3);
      CdmaReceiver rx; rx.configure(kFs); CHECK(rx.ready(), "ready");
      for (size_t o = 0; o < s.size(); o += 8192) rx.feed(s.data() + o, std::min<size_t>(8192, s.size() - o));
      for (int w = 0; w < 100 && rx.busy(); w++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      CdmaTelemetry t; rx.telemetry(t, 0);
      CHECK(t.cells.size() == 1 && t.cells[0].hasMsg && t.cells[0].pilotPn == 168 && t.cells[0].msg.sid == 4139, "receiver: %zu cells", t.cells.size());
      ModeTuning mt = cdmaTuning(); CHECK(mt.stdMode == 662 && mt.minMhz < mt.defMhz && mt.defMhz < mt.maxMhz, "tuning"); }
    return testSummary("cdma phy");
}
