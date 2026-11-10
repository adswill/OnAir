// RC links added in 3.9: Futaba S-FHSS and Spektrum DSM2 / DSMX, on simulated signals only (no recording of either link was available).
//   1. S-FHSS packets: every hop channel and hop code round-trip, the hop sequence stays on 30 channels, a flipped bit fails the CRC
//   2. DSM codes: every byte despreads back from its 64 chips for all 40 code sets, also with 8 chip errors; DSMX hop list rules; 10 and 11 bit packets
//   3. over the air through RcLinkReceiver: S-FHSS at +-300 kHz and noise; DSM at -2 .. +2 MHz, 100 ppm clock error and noise, DSMX told from DSM2
//      by the hop channel; noise alone gives nothing
// Long sweeps run with ONAIR_TEST_LONG=1.
#include "dect2/rclink/rc_proto.h"
#include "dect2/rclink/rc_rx.h"
#include "rc_test_gen.h"
#include "../common/impair.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <set>
#include <string>
using namespace dect2;
using namespace dect2::rclink;
using namespace rctest;
#include "../common/test_util.h"

static RcTelemetry run(RcLinkReceiver& rx, std::vector<cf32>& x, double fs, double centre = 0) {
    rx.configure(fs);
    if (centre > 0) rx.setCenterHz(centre);
    const size_t step = 8192;
    for (size_t i = 0; i < x.size(); i += step) rx.feed(x.data() + i, std::min(step, x.size() - i));
    rx.flush();
    RcTelemetry t;
    rx.telemetry(t, ~0ull);
    return t;
}
static int count(const RcTelemetry& t, const char* type, const char* id = nullptr) {
    int n = 0;
    for (const auto& f : t.frames) if (f.type == type && (!id || f.id == id)) n++;
    return n;
}

int main() {
    const bool longRun = getenv("ONAIR_TEST_LONG") != nullptr;
    // ---- 1. S-FHSS packets
    {
        const uint16_t ch[4] = {1520, 1020, 2020, 1777};
        int bad = 0;
        for (int rf = 0; rf < 30; rf++)
            for (int code = 0; code < 28; code += 3)
                for (int cmd : {0x02, 0x03, 0x0A, 0x0B}) {
                    const auto p = sfhssBuild(0xBC11, rf, code, cmd, ch);
                    const Decoded d = sfhssParse(p.data(), p.size());
                    char want[80];
                    snprintf(want, sizeof want, "CH%d-%d: 1520 1020 2020 1777, hop channel %d, hop code %d, packet %d", (cmd & 8) ? 5 : 1, (cmd & 8) ? 8 : 4, rf, code, (cmd & 1) + 1);
                    if (!d.ok || d.id != "BC11" || d.detail.find(want) != 0) bad++;
                }
        CHECK(bad == 0, "S-FHSS round trip: %d bad", bad);
        for (int code = 0; code < 28; code++) {
            std::set<int> seen;
            int c = 0;
            for (int i = 0; i < 200; i++) { c = sfhssNextChannel(c, code); CHECK(c >= 0 && c < 30, "hop channel %d out of range (code %d)", c, code); seen.insert(c); }
            CHECK(seen.size() >= 10, "code %d visits only %zu channels", code, seen.size());
        }
        const auto p = sfhssBuild(0x1234, 5, 7, 2, ch);
        for (size_t i = 0; i < 13; i++)
            for (int b = 0; b < 8; b++) {
                auto q = p;
                q[i] ^= (uint8_t)(1 << b);
                CHECK(!sfhssParse(q.data(), q.size()).ok, "S-FHSS: flipped bit %zu.%d accepted", i, b);
            }
    }
    // ---- 2. DSM codes and packets
    {
        std::mt19937 rng(39);
        int bad = 0, badNoisy = 0;
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 8; col++) {
                const DsmCodes k = dsmCodes(row, col);
                for (int v = 0; v < 256; v++) {
                    const uint64_t s = dsmSymbol(k, (uint8_t)v);
                    int e = 0;
                    if (dsmDespread(k, s, &e) != v || e != 0) bad++;
                    uint64_t n = s;
                    std::set<int> pos;
                    while (pos.size() < 8) pos.insert((int)(rng() % 64));
                    for (int j : pos) n ^= 1ull << j;
                    if (dsmDespread(k, n, &e) != v) badNoisy++;
                }
            }
        CHECK(bad == 0, "DSM despreading: %d of %d bytes wrong", bad, 40 * 256);
        CHECK(badNoisy == 0, "DSM despreading with 8 chip errors: %d wrong", badNoisy);
        const uint8_t id[4] = {0x12, 0x34, 0x56, 0x79};
        const auto hops = dsmxChannels(id);
        std::set<int> u(hops.begin(), hops.end());
        int lo = 0, mid = 0, hi = 0, parity = 0;
        for (int c : hops) { if (c < 28) lo++; else if (c < 52) mid++; else hi++; if ((c ^ id[3]) & 1) parity++; if (c < 3 || c > 75) parity++; }
        CHECK(hops.size() == 23 && u.size() == 23 && lo <= 8 && mid <= 7 && hi <= 8 && parity == 0, "DSMX hops: %zu unique %zu (%d/%d/%d), rule breaks %d", hops.size(), u.size(), lo, mid, hi, parity);
        CHECK(dsmSopColumn(id) == ((0x12 + 0x34 + 0x56 + 2) & 7), "SOP column");
        const int ix[7] = {1, 5, 2, 3, 0, 4, -1};
        const uint16_t val[7] = {0x156, 0x400, 0x6AA, 0x400, 0x300, 0x500, 0};
        for (int bits : {11, 10}) {
            const auto p = dsmDataPacket(id, true, false, bits, ix, val);
            const Decoded d = dsmParse(p.data(), p.size(), -1, -1);
            char want[80];
            if (bits == 11) snprintf(want, sizeof want, "ch2 342 ch6 1024 ch3 1706 ch4 1024 ch1 768 ch5 1280 (11 bit, 6 words");
            else snprintf(want, sizeof want, "ch2 342 ch6 0 ch3 682 ch4 0 ch1 768 ch5 256 (10 bit, 6 words");
            CHECK(d.ok && d.id == "5679" && d.detail.find(want) == 0, "DSM %d bit packet: %s", bits, d.detail.c_str());
        }
        std::vector<uint8_t> junk(16);
        int accepted = 0;
        for (int i = 0; i < 2000; i++) { for (auto& b : junk) b = (uint8_t)rng(); if (dsmParse(junk.data(), 16, -1, -1).ok) accepted++; }
        printf("  random packets taken by the format check: %d of 2000\n", accepted);
        CHECK(accepted < 20, "format check too weak: %d of 2000 random packets", accepted);
    }
    // ---- 3a. S-FHSS over the air
    for (double cfo : longRun ? std::vector<double>{-300e3, -100e3, 0, 150e3, 300e3} : std::vector<double>{-300e3, 300e3}) {
        for (double sigma : longRun ? std::vector<double>{0.005, 0.02, 0.05} : std::vector<double>{0.02}) {
            const double fs = 2.4e6;
            std::vector<cf32> x((size_t)(0.02 * fs), cf32(0, 0));
            const uint16_t ch[4] = {1520, 1300, 1900, 1100};
            for (int k = 0; k < 2; k++) {
                const auto p = sfhssBuild(0xBC11, 7 + k, 11, 2 | k, ch);
                std::vector<uint8_t> air = {0xAA, 0xAA, 0xAA, 0xAA, 0xD3, 0x91};
                air.insert(air.end(), p.begin(), p.end());
                addGfsk(x, (size_t)((0.002 + 0.0016 * k * 2) * fs), toBits(air), 128143, 38086, fs, cfo + k * 400e3, 0.4f);
            }
            addNoise(x, sigma);
            RcLinkReceiver rx; rx.setProtocols(Sfhss);
            const RcTelemetry t = run(rx, x, fs);
            printf("  S-FHSS cfo %+.0f kHz sigma %.3f: %d frames\n", cfo / 1e3, sigma, count(t, "S-FHSS channels", "BC11"));
            CHECK(count(t, "S-FHSS channels", "BC11") == 2, "S-FHSS cfo %.0f sigma %.3f: %d of 2", cfo, sigma, count(t, "S-FHSS channels", "BC11"));
            for (const auto& f : t.frames) CHECK(f.id == "BC11", "false S-FHSS frame %s", f.id.c_str());
        }
    }
    // ---- 3b. DSM over the air: two packets of one DSMX transmitter on two hop channels, one DSM2 packet
    struct DCase { double off, ppm, sigma; };
    std::vector<DCase> cases = {{-2.0e6, 0, 0.01}, {1.5e6, 100, 0.03}};
    if (longRun) for (double off : {-2.5e6, -1e6, 0.0, 0.7e6, 2.5e6}) for (double sg : {0.01, 0.05, 0.08}) cases.push_back({off, off > 0 ? 100.0 : -100.0, sg});
    for (const DCase& dc : cases) {
        const double fs = 8e6;
        const uint8_t id[4] = {0x12, 0x34, 0x56, 0x79};
        const int col = dsmSopColumn(id);
        const auto hops = dsmxChannels(id);
        const double centre = 2400e6 + 40e6;   // the capture's centre: rf channel n is at offset (n - 40) MHz
        std::vector<cf32> x((size_t)(0.012 * fs), cf32(0, 0));
        const int ix[7] = {1, 5, 2, 3, 0, 4, -1};
        const uint16_t val[7] = {0x156, 0x400, 0x6AA, 0x400, 0x300, 0x500, 0};
        // DSMX packets go to the hop channels nearest the burst offset under test; a DSM2 one 2 MHz away
        int a = -1, b = -1;
        for (int c : hops) if (std::fabs((c - 40) * 1e6 - dc.off) <= 3e6 && std::fabs((c - 40) * 1e6) < 3.2e6) { if (a < 0) a = c; else if (b < 0) b = c; }
        if (a < 0) a = 40 + (int)std::lround(dc.off / 1e6);
        if (b < 0) b = a;
        int n = 0;
        for (int c : {a, b}) {
            const DsmCodes k = dsmCodes(dsmPnRow(c, true), col);
            const auto pk = dsmDataPacket(id, true, n == 1, 11, ix, val);
            const auto chips = dsmChips(k, pk, (uint16_t)(n ? 0x1234 : ~0x1234));
            addGfsk(x, (size_t)((0.001 + 0.004 * n) * fs), chips, 1e6, 250e3, fs, (c - 40) * 1e6, 0.4f);
            n++;
        }
        const int c2 = std::abs(a - 40) < 2 ? a + 2 : 40;
        const DsmCodes k2 = dsmCodes(dsmPnRow(c2, false), 3);
        const uint8_t id2[4] = {0xAB, 0xCD, 0x0F, 0x1E};
        addGfsk(x, (size_t)(0.009 * fs), dsmChips(k2, dsmDataPacket(id2, false, false, 10, ix, val), 0xABCD), 1e6, 250e3, fs, (c2 - 40) * 1e6, 0.4f);
        if (dc.ppm != 0) x = impair::clock(x, dc.ppm);
        addNoise(x, dc.sigma);
        RcLinkReceiver rx; rx.setProtocols(Dsm);
        const RcTelemetry t = run(rx, x, fs, centre);
        const int gotX = count(t, "DSMX channels", "5679"), got2 = count(t, "DSM2 channels", "F0E1");
        printf("  DSM offset %+.1f MHz, %+.0f ppm, sigma %.2f: DSMX %d of 2, DSM2 %d of 1, %zu frames\n", dc.off / 1e6, dc.ppm, dc.sigma, gotX, got2, t.frames.size());
        CHECK(gotX == 2 && got2 == 1 && t.frames.size() == 3, "DSM offset %.1f MHz sigma %.2f: DSMX %d, DSM2 %d, %zu frames", dc.off / 1e6, dc.sigma, gotX, got2, t.frames.size());
        for (const auto& f : t.frames)
            if (f.id == "5679") CHECK(f.detail.find("ch2 342 ch6 1024 ch3 1706") == 0, "DSM channels: %s", f.detail.c_str());
    }
    // ---- noise only
    {
        const double fs = 8e6;
        std::vector<cf32> x((size_t)(0.05 * fs), cf32(0, 0));
        addNoise(x, 0.05, 13);
        RcLinkReceiver rx; rx.setProtocols(Sfhss | Dsm);
        const RcTelemetry t = run(rx, x, fs, 2440e6);
        CHECK(t.frames.empty(), "%zu frames from noise", t.frames.size());
    }
    return testSummary("rclink 3.9");
}
