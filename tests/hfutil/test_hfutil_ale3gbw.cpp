// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// 3G-ALE traffic bursts (hfutil/hfu_ale3gbw.h, MIL-STD-188-141B Appendix C): the interleavers, codes and CRCs against the text, the
// TM / HDL / LDL PDUs, and BW1 to BW4 generated and read back with frequency error, drift, a sample clock error, noise in 3 kHz, an echo,
// a Watterson ITU-R Poor channel (tests/common/watterson.h), chunk sizes, bursts in a row and noise only. Simulated signals only.
#include "dect2/hfutil/hfu_ale3gbw.h"
#include "dect2/hfutil/hfu_ale3g.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/impair.h"
#include "../common/watterson.h"

namespace {

int gFails = 0;
void check(bool ok, const char* what) { if (!ok) { printf("FAIL: %s\n", what); gFails++; } }

struct Imp { double cfo = 0, drift = 0, ppm = 0, snr = 99, echoDb = -99; bool poor = false; unsigned seed = 1; size_t chunk = 4096; };

std::vector<cf32> withImp(std::vector<cf32> x, const Imp& m) {
    const double ps = watterson::power(x, (size_t)(0.25 * 24000), x.size() - (size_t)(0.25 * 24000));
    if (m.poor) x = watterson::apply(x, watterson::ituPoor(m.seed));
    if (m.echoDb > -90) impair::echo(x, 24, m.echoDb);
    if (m.ppm != 0) x = impair::clock(x, m.ppm);
    if (m.cfo != 0) impair::shift(x, m.cfo, 24000);
    if (m.drift != 0) {
        double ph = 0;
        for (size_t i = 0; i < x.size(); i++) { ph += 2 * M_PI * (m.drift * (double)i / 24000.0) / 24000.0; x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); }
    }
    if (m.snr < 90) watterson::noise3k(x, m.snr, ps, 24000, m.seed + 50);
    return x;
}

std::vector<hfu::Ale3gTraffic> run(const std::vector<cf32>& x, size_t chunk = 4096) {
    std::vector<hfu::Ale3gTraffic> out;
    hfu::Ale3gTrafficRx rx;
    rx.setSink([&](const hfu::Ale3gTraffic& t) { out.push_back(t); });
    for (size_t i = 0; i < x.size(); i += chunk) rx.push(x.data() + i, std::min(chunk, x.size() - i));
    std::vector<cf32> pad(24000, cf32(0, 0));
    rx.push(pad.data(), pad.size());
    return out;
}

int count(const std::vector<hfu::Ale3gTraffic>& v, const char* type, bool okOnly = true) {
    int n = 0;
    for (auto& t : v) n += t.type == type && (!okOnly || t.crcOk);
    return n;
}

std::vector<cf32> cat(std::initializer_list<std::vector<cf32>> parts) { std::vector<cf32> x; for (auto& p : parts) x.insert(x.end(), p.begin(), p.end()); return x; }

std::string text(size_t n, unsigned seed) {
    static const char* w[] = {"THIRD", "GENERATION", "HF", "LINK", "DATA", "PACKET", "ROUTINE", "TRAFFIC", "STATION", "0123456789"};
    std::mt19937 r(seed);
    std::string s;
    while (s.size() < n) { s += w[r() % 10]; s += ' '; }
    s.resize(n);
    return s;
}

// an HDL datagram as BW2 packets (first, interior, last), padded to a whole forward transmission with copies of the last packet
std::vector<std::vector<uint8_t>> hdlDatagram(const std::string& s, size_t np) {
    std::vector<std::vector<uint8_t>> pk;
    const int n = (int)((s.size() + 232) / 233);
    for (int i = 0; i < n; i++) {
        std::vector<uint8_t> d(s.begin() + 233 * i, s.begin() + std::min<size_t>(s.size(), 233 * (size_t)(i + 1)));
        uint32_t seq = n == 1 ? hfu::seqOnly((int)d.size()) : i == 0 ? hfu::seqFirst(n) : i == n - 1 ? hfu::seqLast((int)d.size()) : hfu::seqInterior(n - 1 - i);
        pk.push_back(hfu::hdlPacket(d, seq));
    }
    while (pk.size() < np) pk.push_back(pk.back());
    return pk;
}

}  // namespace

int main() {
    // ---- tables, codes, CRCs ----
    {
        // TM PDU fields and CRC-12; HDL_EOM pattern starts 1 0 1 0 after the type bit
        const uint64_t tm = hfu::tmPdu(0, 2, (5 << 5) | 3, (17 << 5) | 3, 1);
        const auto p = hfu::bw1Parse(tm);
        check(p.crcOk && p.type == "TM_REQUEST" && p.text.find("HDL 12 packets") != std::string::npos && p.text.find("routine") != std::string::npos, "TM_REQUEST fields");
        check(!hfu::bw1Parse(tm ^ (1ull << 20)).crcOk, "TM CRC-12 catches an error");
        check(((hfu::hdlEomPdu() >> 40) & 0xF) == 0xA, "HDL_EOM check pattern 1010 first");
        const auto a = hfu::bw1Parse(hfu::hdlAckPdu(0xC00001u));
        check(a.crcOk && a.type == "HDL_ACK" && a.text.find("3 of 24") != std::string::npos && a.text.find("1 2 24") != std::string::npos, "HDL_ACK mask");
        check(!hfu::bw1Parse(hfu::hdlAckPdu(0x123456u) ^ (1ull << 30)).crcOk, "HDL_ACK CRC-16 catches an error");
        check(hfu::bw1Parse(hfu::hdlEomPdu()).crcOk && hfu::bw1Parse(hfu::tmPdu(2, 3, 0x7FF, 99, 0)).text.find("abort") != std::string::npos, "HDL_EOM, TM_TERM");
        // the CRC-32 polynomial is the common one: "123456789" as bits, no reflection, no inversion, gives 0x89A1897F
        std::vector<uint8_t> b;
        for (char c : std::string("123456789")) for (int i = 7; i >= 0; i--) b.push_back((uint8_t)((c >> i) & 1));
        check(hfu::ale3gCrcBits(b, hfu::kAle3gCrc32, 32) == 0x89A1897Fu, "CRC-32 (C.5.1.5.3) check value");
        // sequence numbers
        check(hfu::seqText(hfu::seqFirst(4)).find("4 packets") != std::string::npos && hfu::seqText(hfu::seqLast(17)).find("17 bytes") != std::string::npos, "sequence numbers");
        // BW1 coding round trip and error correction
        std::mt19937_64 r(3);
        int okClean = 0, okErr = 0;
        for (int i = 0; i < 50; i++) {
            const uint64_t v = r() & 0xFFFFFFFFFFFFull;
            const auto c = hfu::bw1Encode(v);
            std::vector<float> s(144);
            for (int k = 0; k < 144; k++) s[(size_t)k] = c[(size_t)k] ? -1.f : 1.f;
            okClean += hfu::bw1Decode(s) == v;
            for (int k = 0; k < 10; k++) s[(size_t)((k * 37 + i) % 144)] *= -1;
            okErr += hfu::bw1Decode(s) == v;
        }
        check(okClean == 50 && okErr == 50, "BW1 tail-biting code: round trip and 10 of 144 errors corrected");
        // the BW2 PN generator as drawn
        const auto pn = hfu::bw2Pn(0, 4);
        printf("BW2 PN (FTcount 0) starts %d %d %d %d\n", pn[0], pn[1], pn[2], pn[3]);
    }

    // ---- BW1 through the receiver ----
    const std::vector<uint64_t> bw1 = {hfu::tmPdu(0, 2, (5 << 5) | 3, (17 << 5) | 3, 7), hfu::tmPdu(1, 3, (17 << 5) | 3, (5 << 5) | 3, 7), hfu::hdlAckPdu(0xFFF000u),
                                       hfu::hdlEomPdu(), hfu::tmPdu(2, 3, (17 << 5) | 3, (5 << 5) | 3, 0)};
    auto bw1Signal = [&]() { std::vector<cf32> x; for (uint64_t v : bw1) { const auto b = hfu::genBw1(v, 0.25, 0.25); x.insert(x.end(), b.begin(), b.end()); } return x; };
    {
        const auto clean = bw1Signal();
        struct C { const char* name; Imp m; int need; };
        const C cases[] = {{"clean", {}, 5}, {"+60 Hz", {60}, 5}, {"-75 Hz", {-75}, 5}, {"drift 3 Hz/s", {0, 3}, 5}, {"+60 ppm", {0, 0, 60}, 5}, {"-60 ppm", {0, 0, -60}, 5},
                           {"echo 1 ms -6 dB", {0, 0, 0, 99, -6}, 5}, {"6 dB", {20, 0, 0, 6}, 5}, {"0 dB", {-30, 0, 0, 0}, 5}, {"-4 dB", {0, 0, 0, -4}, 4},
                           {"ITU-R Poor 10 dB", {0, 0, 0, 10, -99, true, 3}, 4}};
        for (const C& c : cases) {
            const auto o = run(withImp(clean, c.m), c.m.chunk);
            int good = 0;
            for (auto& t : o) good += t.crcOk && t.waveform == 1;
            printf("BW1 %-18s %d of 5 PDUs\n", c.name, good);
            check(good >= c.need, "BW1 PDUs read");
        }
        const auto o = run(clean, 77);
        check(count(o, "TM_REQUEST") == 1 && count(o, "TM_CONFIRM") == 1 && count(o, "HDL_ACK") == 1 && count(o, "HDL_EOM") == 1 && count(o, "TM_TERM") == 1, "BW1 types, chunks of 77");
    }

    // ---- BW2: an HDL datagram of 3 packets, forward transmissions with different FTcounts ----
    {
        const std::string msg = text(600, 5);
        const auto pk = hdlDatagram(msg, 3);
        for (int ft : {0, 1, 5, 7}) {
            for (const Imp& m : {Imp{}, Imp{35, 0, 0, 18}, Imp{-50, 1, 30, 20}}) {
                const auto o = run(withImp(hfu::genBw2(pk, ft, 0.25, 0.25), m));
                std::string dg;
                for (auto& t : o) if (t.type == "datagram" && t.crcOk) dg.assign(t.data.begin(), t.data.end());
                printf("BW2 FTcount %d, %+.0f Hz, %2.0f dB: %d of 3 packets, datagram %s\n", ft, m.cfo, m.snr, count(o, "HDL_DATA") - 1, dg == msg ? "complete" : "missing");
                check(dg == msg, "BW2 datagram");
            }
        }
        // 6 packets (a 1200-byte datagram), 12 dB
        const std::string m6 = text(1200, 6);
        const auto o = run(withImp(hfu::genBw2(hdlDatagram(m6, 6), 2, 0.25, 0.25), Imp{20, 0, 0, 18}));
        bool got = false;
        for (auto& t : o) got |= t.type == "datagram" && std::string(t.data.begin(), t.data.end()) == m6;
        check(got, "BW2 6 packets");
        // SNR sweep (one forward transmission carries only a quarter of the rate 1/4 code)
        for (double snr : {18.0, 15.0, 12.0}) {
            const auto r = run(withImp(hfu::genBw2(hdlDatagram(text(699, 9), 3), 0, 0.25, 0.25), Imp{0, 0, 0, snr, -99, false, 8}));
            printf("BW2 %2.0f dB: %d of 3 packets\n", snr, count(r, "HDL_DATA") - (count(r, "HDL_DATA") > 0));
        }
    }

    // ---- BW3: LDL_DATA, n = 64 and 128, both halves of the code, and the two halves combined ----
    {
        for (int n : {64, 128}) {
            const std::string s = text((size_t)n, (unsigned)n);
            const std::vector<uint8_t> d(s.begin(), s.end());
            const auto pk = hfu::ldlPacket(d, n, hfu::seqOnly(n));
            for (int ft : {0, 1}) for (const Imp& m : {Imp{}, Imp{-40, 0, 0, 3}, Imp{70, 2, -40, 0}, Imp{0, 0, 0, 3, -6}}) {
                const auto o = run(withImp(hfu::genBw3(pk, n, ft, 0.25, 0.25), m));
                bool got = false;
                for (auto& t : o) got |= t.type == "LDL_DATA" && t.crcOk && std::string(t.data.begin(), t.data.end()) == s;
                printf("BW3 n %d, FTcount %d, %+.0f Hz, %2.0f dB%s: %s\n", n, ft, m.cfo, m.snr, m.poor ? " ITU-R Poor" : "", got ? "read" : "missed");
                check(got, "BW3 packet");
            }
        }
        // ITU-R Poor fading (2 ms, 1 Hz): one forward transmission carries half of the code, so a deep fade inside the 1.2 s burst
        // costs the packet (the LDL protocol sends it again). The share read in one transmission is measured.
        {
            int got = 0;
            for (unsigned seed = 1; seed <= 10; seed++) {
                const std::string s = text(64, 200 + seed);
                const auto pk = hfu::ldlPacket(std::vector<uint8_t>(s.begin(), s.end()), 64, hfu::seqOnly(64));
                for (auto& t : run(withImp(hfu::genBw3(pk, 64, 0, 0.25, 0.25), Imp{0, 0, 0, 10, -99, true, seed}))) got += t.type == "LDL_DATA" && t.crcOk;
            }
            printf("BW3 n 64 in ITU-R Poor at 10 dB: %d of 10 read in one transmission\n", got);
            check(got >= 4, "BW3 in ITU-R Poor fading");
        }
        // a weak first transmission and its retransmission (the other half of the code): combined
        int single = 0, comb = 0;
        for (unsigned seed = 1; seed <= 6; seed++) {
            const std::string s = text(64, 100 + seed);
            const auto pk = hfu::ldlPacket(std::vector<uint8_t>(s.begin(), s.end()), 64, hfu::seqOnly(64));
            const auto x = cat({withImp(hfu::genBw3(pk, 64, 0, 0.25, 0.25), Imp{0, 0, 0, -7, -99, false, seed}), withImp(hfu::genBw3(pk, 64, 1, 0.25, 0.25), Imp{0, 0, 0, -7, -99, false, seed + 20})});
            for (auto& t : run(x)) if (t.type == "LDL_DATA" && t.crcOk) { if (t.text.find("combined") != std::string::npos) comb++; else single++; }
        }
        printf("BW3 at -7 dB, 6 pairs of transmissions: %d read alone, %d by combining both halves\n", single, comb);
        check(single + comb >= 4, "BW3 retransmission combining");
    }

    // ---- BW4: the four 2-bit words ----
    {
        int ok = 0, okNoisy = 0, okPoor = 0, wrong = 0;
        for (int v = 0; v < 4; v++) {
            static const char* want[4] = {"bits 00", "bits 01", "bits 10", "bits 11"};
            for (int k = 0; k < 3; k++) {
                const Imp m = k == 0 ? Imp{} : k == 1 ? Imp{55, 0, 0, -6, -99, false, (unsigned)v + 1} : Imp{0, 0, 0, 3, -99, true, (unsigned)v + 7};
                const auto o = run(withImp(hfu::genBw4(v, 0.25, 0.25), m));
                for (auto& t : o) if (t.type == "LDL_ACK") { const bool right = t.text.find(want[v]) == 0; (k == 0 ? ok : k == 1 ? okNoisy : okPoor) += right; wrong += !right; }
            }
        }
        printf("BW4: %d of 4 clean, %d of 4 at -6 dB +55 Hz, %d of 4 ITU-R Poor 3 dB, %d wrong\n", ok, okNoisy, okPoor, wrong);
        check(ok == 4 && okNoisy == 4 && okPoor >= 3 && wrong == 0, "BW4 words");
    }

    // ---- a whole exchange through the 3G-ALE receiver: BW0 call, TM_REQUEST, HDL_DATA, HDL_ACK, HDL_EOM, LDL_DATA, LDL_ACK ----
    {
        const std::string msg = text(300, 12), m3 = text(64, 13);
        const auto x = withImp(cat({hfu::genBw0(hfu::ale3gCall(5, 0, 17, 3), 0.2, 0.2), hfu::genBw1(hfu::tmPdu(0, 2, (5 << 5) | 3, (17 << 5) | 3, 3), 0.2, 0.2),
                                   hfu::genBw2(hdlDatagram(msg, 3), 0, 0.2, 0.2), hfu::genBw1(hfu::hdlAckPdu(0xE00000u), 0.2, 0.2), hfu::genBw1(hfu::hdlEomPdu(), 0.2, 0.2),
                                   hfu::genBw3(hfu::ldlPacket(std::vector<uint8_t>(m3.begin(), m3.end()), 64, hfu::seqOnly(64)), 64, 0, 0.2, 0.2), hfu::genBw4(2, 0.2, 0.2)}),
                              Imp{25, 0, 20, 15});
        hfu::Ale3gRx rx;
        std::vector<std::string> types;
        rx.setPduSink([&](const hfu::Ale3gPdu& p) { if (p.crcOk) types.push_back(p.type); });
        for (size_t i = 0; i < x.size(); i += 4096) rx.push(x.data() + i, std::min<size_t>(4096, x.size() - i));
        std::vector<cf32> pad(48000, cf32(0, 0));
        rx.push(pad.data(), pad.size());
        auto has = [&](const char* t) { return (int)std::count(types.begin(), types.end(), std::string(t)); };
        printf("exchange: LE_Call %d, TM_REQUEST %d, HDL_DATA %d, datagram %d, HDL_ACK %d, HDL_EOM %d, LDL_DATA %d, LDL_ACK %d; bursts %llu\n", has("LE_Call"), has("TM_REQUEST"),
               has("HDL_DATA"), has("datagram"), has("HDL_ACK"), has("HDL_EOM"), has("LDL_DATA"), has("LDL_ACK"), (unsigned long long)rx.bursts());
        check(has("LE_Call") == 1 && has("TM_REQUEST") == 1 && has("datagram") == 1 && has("HDL_ACK") == 1 && has("HDL_EOM") == 1 && has("LDL_DATA") == 1 && has("LDL_ACK") == 1,
              "every burst of the exchange");
    }

    // ---- noise only ----
    {
        std::vector<cf32> n(24000 * 20);
        std::mt19937 g(9);
        std::normal_distribution<float> nd(0.f, 1.f);
        for (auto& v : n) v = cf32(nd(g), nd(g));
        int good = 0;
        for (auto& t : run(n)) good += t.crcOk;
        check(good == 0, "noise only gives nothing");
    }

    if (gFails) { printf("%d FAILED\n", gFails); return 1; }
    printf("all passed\n");
    return 0;
}
