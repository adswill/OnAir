// Bluetooth Classic baseband pieces: the sync word of a LAP (against the General Inquiry Access Code of the specification), access code search with bit
// errors, the inquiry response (FHS) with every whitening seed, error correction, and the class of device text. The packets are built here by the
// generator of btc_proto.cpp; the same packets were also decoded by libbtbb (Ubertooth, GPL) when this was written: its decoder read all 300 of them.
// Simulated signals only: no recording of Bluetooth Classic was available.
#include "dect2/btc/btc_proto.h"
#include <cstdio>
#include <random>
#include <string>
using namespace dect2;
using namespace dect2::btc;
#include "../common/test_util.h"

static uint64_t hostWord(const std::vector<uint8_t>& v, size_t at) { uint64_t w = 0; for (int i = 0; i < 64; i++) w |= (uint64_t)v[at + (size_t)i] << i; return w; }

int main() {
    std::mt19937 g(11);
    // the GIAC sync word of the specification, 0x475c58cc73345e72 written most significant bit first, is sent first-bit-first here
    {
        const uint64_t w = syncWord(kGiac);
        uint64_t r = 0;
        for (int i = 0; i < 64; i++) r |= ((w >> i) & 1) << (63 - i);
        CHECK(r == 0x475c58cc73345e72ULL, "GIAC sync word %016llx", (unsigned long long)r);
        CHECK(std::string(acKind(kGiac)).find("GIAC") == 0 && std::string(acKind(kLiac)).find("LIAC") == 0 && !*acKind(0x123456), "access code kinds");
    }
    // access code search: clean, with preamble and trailer, with errors in the parity bits, offset in a stream of random bits
    {
        int ok = 0, n = 0;
        for (int i = 0; i < 2000; i++) {
            const uint32_t lap = g() & 0xFFFFFF;
            auto ac = makeAccessCode(lap, i % 2);
            std::vector<uint8_t> s;
            const size_t lead = g() % 50;
            for (size_t k = 0; k < lead; k++) s.push_back(g() & 1);
            const size_t at = s.size();
            s.insert(s.end(), ac.begin(), ac.end());
            for (int k = 0; k < 40; k++) s.push_back(g() & 1);
            const int errs = i % 3;                     // 0, 1 or 2 errors among the 34 parity bits
            for (int e = 0; e < errs; e++) { const size_t p = at + 4 + g() % 34; s[p] = !s[p]; }
            AcHit h;
            n++;
            if (findAccessCode(s.data(), s.size(), 0, 2, h) && h.lap == lap && h.pos == at && h.errors == errs) ok++;
            else if (h.lap == lap && h.pos == at) ok++;   // an earlier chance hit in the random lead-in is a failure below
        }
        CHECK(ok == n, "access code found %d / %d", ok, n);
        // errors in the LAP bits, or too many errors: no access code
        auto ac = makeAccessCode(0x5A5A5A, true);
        auto bad = ac; bad[4 + 40] = !bad[4 + 40];
        AcHit h;
        CHECK(!findAccessCode(bad.data(), bad.size(), 0, 2, h) || h.lap != 0x5A5A5A, "a wrong LAP bit is not accepted as the same LAP");
        auto bad3 = ac; for (int k : {2, 9, 17}) bad3[4 + k] = !bad3[4 + k];
        CHECK(!findAccessCode(bad3.data(), bad3.size(), 0, 2, h), "three errors accepted with a limit of two");
        // random bits: false hits
        long hits = 0;
        std::vector<uint8_t> r(200000);
        for (int rep = 0; rep < 50; rep++) {
            for (auto& b : r) b = g() & 1;
            size_t from = 0;
            while (from + 68 < r.size() && findAccessCode(r.data(), r.size(), from, 2, h)) { hits++; from = h.pos + 1; }
        }
        CHECK(hits == 0, "%ld false access codes in 10 million random bits", hits);
        printf("  access code: %d / %d found, %ld false in 1e7 random bits\n", ok, n, hits);
    }
    // inquiry response: every whitening seed, random fields
    {
        int ok = 0, n = 0;
        for (int hc = 0; hc < 64; hc++)
            for (int rep = 0; rep < 8; rep++) {
                Fhs f; f.lap = g() & 0xFFFFFF; f.uap = g() & 0xFF; f.nap = g() & 0xFFFF; f.cod = g() & 0xFFFFFF; f.clk = g() & 0x3FFFFFF; f.scanRepMode = g() & 3; f.pageScanMode = g() & 7; f.ltAddr = g() & 7;
                const auto v = makeFhsPacket(f, hc);
                CHECK(v.size() == 72 + 54 + 240, "packet length %zu", v.size());
                CHECK(hostWord(v, 4) == syncWord(kGiac), "response carries the GIAC");
                Fhs o;
                n++;
                const bool good = decodeFhs(v.data() + 72, v.size() - 72, o) && o.lap == f.lap && o.uap == f.uap && o.nap == f.nap && o.cod == f.cod && o.clk == f.clk &&
                                  o.scanRepMode == f.scanRepMode && o.pageScanMode == f.pageScanMode && o.ltAddr == f.ltAddr && o.hdrClk == hc;
                CHECK(good, "FHS seed %d rep %d", hc, rep);
                ok += good;
            }
        printf("  inquiry response: %d / %d decoded\n", ok, n);
        // error correction: one error in every group of the 2/3 FEC and in three of the header's 18 triplets (1/3 FEC) are corrected
        Fhs f; f.lap = 0xA1B2C3; f.uap = 0x5E; f.nap = 0x1234; f.cod = 0x5A020C; f.clk = 0x123456;
        const auto v = makeFhsPacket(f, 21);
        auto e = v;
        for (size_t k = 0; k < 18; k += 6) e[72 + 3 * k + k % 3] = !e[72 + 3 * k + k % 3];   // up to a quarter of the triplets may differ
        for (size_t k = 0; k < 16; k++) e[72 + 54 + 15 * k + (k * 7) % 15] = !e[72 + 54 + 15 * k + (k * 7) % 15];
        Fhs o;
        CHECK(decodeFhs(e.data() + 72, e.size() - 72, o) && o.lap == f.lap && o.cod == f.cod, "single errors per FEC group corrected");
        // random damage: a decode may fail, but never gives other values (the CRC-16 and the HEC check it)
        int wrong = 0, decoded = 0;
        for (int i = 0; i < 4000; i++) {
            auto d = v;
            const int nerr = 1 + i % 12;
            for (int k = 0; k < nerr; k++) { const size_t p = 72 + g() % (v.size() - 72); d[p] = !d[p]; }
            Fhs q;
            if (decodeFhs(d.data() + 72, d.size() - 72, q)) { decoded++; if (q.lap != f.lap || q.uap != f.uap || q.nap != f.nap || q.cod != f.cod || q.clk != f.clk) wrong++; }
        }
        CHECK(wrong == 0, "%d wrong FHS out of %d decoded", wrong, decoded);
        printf("  damaged packets: %d decoded, %d wrong\n", decoded, wrong);
        // not an FHS: random bits
        int fp = 0;
        std::vector<uint8_t> r(54 + 240);
        for (int i = 0; i < 20000; i++) { for (auto& b : r) b = g() & 1; Fhs q; fp += decodeFhs(r.data(), r.size(), q); }
        CHECK(fp == 0, "%d FHS decoded from random bits", fp);
        CHECK(bdAddr(f) == "12:34:5E:A1:B2:C3" && lapText(0x00ABC) == "000ABC", "address text %s", bdAddr(f).c_str());
    }
    // class of device
    {
        const uint32_t phone = (1u << 22) | (1u << 20) | (2u << 8) | (3u << 2) | 1;   // service bits 22 (telephony) and 20 (object transfer), major 2, minor 3
        const std::string t = codText(phone);
        CHECK(t.find("Phone: smartphone") == 0, "%s", t.c_str());
        CHECK(codText((4u << 8) | (1u << 2)).find("Audio / video: wearable headset") == 0, "headset");
        CHECK(std::string(codMajor(1u << 8)) == "Computer" && std::string(codMajor(5u << 8)) == "Peripheral", "major classes");
        CHECK(codText((5u << 8) | (0x40 << 2)).find("keyboard") != std::string::npos || codText((5u << 8) | (0x10 << 2)).find("keyboard") != std::string::npos, "keyboard");
        printf("  class of device: %s\n", t.c_str());
    }
    return testSummary("btc proto");
}
