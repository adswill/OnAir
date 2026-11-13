// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wi-Fi HaLow (802.11ah) pieces: the tone layouts, preamble sequences, SIG field and its CRC-4, scrambler, convolutional code and Viterbi decoder,
// the PPDU length, S1G beacon building and parsing (SSID, compressed SSID, security, S1G Operation), channel plans.
#include "dect2/halow/halow_proto.h"
#include "dect2/common/stable_random.h"
#include "dect2/wifi/wifi_proto.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::halow;
#include "../common/test_util.h"

int main() {
    // tone layouts: 24 + 2, 52 + 4, 108 + 6 used tones; no DC tone
    CHECK(info(W1).data.dataK.size() == 24 && info(W1).data.pilotK.size() == 2 && info(W1).nUsed == 26, "1 MHz layout");
    CHECK(info(W2).data.dataK.size() == 52 && info(W2).data.pilotK.size() == 4 && info(W2).nUsed == 56, "2 MHz layout");
    CHECK(info(W4).data.dataK.size() == 108 && info(W4).data.pilotK.size() == 6 && info(W4).nUsed == 114, "4 MHz layout");
    for (int w = 0; w < 3; w++) {
        const WidthInfo& wi = info(w);
        CHECK(wi.ncol * wi.nrow == wi.ncbps && wi.sigNcol * wi.sigNrow == wi.sigNcbps, "interleaver %d", w);
        std::vector<int> seen((size_t)wi.ncbps, 0);
        for (int k = 0; k < wi.ncbps; k++) seen[(size_t)interleave(wi.ncol, wi.nrow, k)]++;
        bool perm = true;
        for (int v : seen) if (v != 1) perm = false;
        CHECK(perm, "interleaver is not a permutation (%d)", w);
        for (const Layout& l : wi.sig) for (int k : l.dataK) CHECK(std::abs(k) <= 58, "SIG tone %d", k);
        CHECK(wi.sig[0].dataK.size() == (size_t)wi.sigNcbps, "SIG data tones %d", w);
    }
    // the short training field repeats every 8 us (32 samples), the long one is real in frequency (a +-1 on every used tone)
    for (int w = 0; w < 3; w++) {
        cf32 x[kFft];
        stfTime(w, x);
        double e = 0, d = 0;
        for (int i = 0; i < kFft; i++) { e += std::norm(x[i]); d += std::norm(x[i] - x[(i + kLag) % kFft]); }
        CHECK(std::fabs(e / kFft - 1) < 0.01 && d < 1e-6 * e, "STF %d: power %.3f, period error %.2e", w, e / kFft, d / e);
        cf32 l[kFft];
        ltfTime(w, l);
        e = 0;
        for (int i = 0; i < kFft; i++) e += std::norm(l[i]);
        CHECK(std::fabs(e / kFft - symScale(w) * symScale(w) * info(w).nUsed) < 0.01, "LTF %d power", w);
        float lf[kFft];
        ltfSpectrum(w, lf);
        int nz = 0;
        for (int i = 0; i < kFft; i++) if (lf[i] != 0.f) nz++;
        CHECK(nz == info(w).nUsed && lf[0] == 0.f, "LTF %d tones %d", w, nz);
    }
    // CRC-4 and the SIG field
    for (int w = 0; w < 3; w++) {
        Sig s; s.w = w; s.mcs = w == W1 ? 10 : 0; s.len = 137 + w; s.id = 0x1A5; s.sgi = true; s.agg = false;
        std::vector<uint8_t> b = sigBits(s);
        CHECK((int)b.size() == info(w).sigInfoBits, "SIG length %d", w);
        Sig r;
        CHECK(parseSig(b.data(), w, r) && r.len == s.len && r.mcs == s.mcs && r.w == w && r.sgi, "SIG roundtrip %d", w);
        if (w != W1) CHECK(r.id == 0x1A5, "partial AID");
        b[3] ^= 1;
        CHECK(!parseSig(b.data(), w, r), "SIG with a flipped bit passed (%d)", w);
    }
    {   // a 2 MHz SIG heard as 4 MHz is refused by the bandwidth field
        Sig s; s.w = W2; s.len = 50;
        std::vector<uint8_t> b = sigBits(s);
        Sig r;
        CHECK(!parseSig(b.data(), W4, r), "2 MHz SIG accepted as 4 MHz");
    }
    // scrambler: the sequence from a state, and from its own first seven bits
    {
        const auto a = scramblerSeq(0x5D, 300);
        const auto b = scramblerFrom(a.data(), 300);
        CHECK(a == b, "scrambler restart from the first 7 bits");
        int ones = 0;
        for (int i = 0; i < 127; i++) ones += a[(size_t)i];
        CHECK(ones == 64, "scrambler period / balance (%d ones in 127)", ones);
        const auto l = scramblerSeq(0x7F, 254), s1 = scramblerSeq(0x7F, 127);
        CHECK(std::vector<uint8_t>(l.begin(), l.begin() + 127) == s1 && std::vector<uint8_t>(l.begin() + 127, l.end()) == s1, "period 127");
    }
    // convolutional code + Viterbi, with errors
    {
        std::mt19937 rng(5);
        std::vector<uint8_t> bits(600);
        for (auto& b : bits) b = rng() & 1;
        for (int i = 0; i < 6; i++) bits[bits.size() - 1 - (size_t)i] = 0;
        const auto coded = convEncode(bits);
        CHECK(coded.size() == 2 * bits.size(), "rate 1/2");
        std::vector<float> llr(coded.size());
        for (size_t i = 0; i < coded.size(); i++) llr[i] = coded[i] ? 1.f : -1.f;
        CHECK(viterbi(llr) == bits, "clean decode");
        dect2::normal_distribution<float> nd(0.f, 0.8f);
        for (auto& v : llr) v += nd(rng);
        const auto dec = viterbi(llr);
        int err = 0;
        for (size_t i = 0; i < bits.size(); i++) err += dec[i] != bits[i];
        CHECK(err == 0, "%d bit errors at 1.9 dB Es/N0 equivalent", err);
        // a known vector: the first output pair of a single one is 11 (both generators have the current-input tap)
        const auto one = convEncode({1, 0, 0, 0, 0, 0, 0});
        CHECK(one[0] == 1 && one[1] == 1, "impulse response start");
        int sum0 = 0, sum1 = 0;
        for (size_t i = 0; i < one.size(); i += 2) { sum0 += one[i]; sum1 += one[i + 1]; }
        CHECK(sum0 == 5 && sum1 == 5, "impulse response weights %d %d (133 and 171 octal have five taps each)", sum0, sum1);
    }
    // PPDU length
    for (int w = 0; w < 3; w++) {
        const std::vector<uint8_t> psdu(77, 0xA5);
        const int mcs = w == W1 ? 10 : 0;
        CHECK(buildPpdu(psdu, w, mcs).size() == ppduSamples(w, mcs, 77), "PPDU length %d", w);
    }
    CHECK(dataSymbols(W2, 0, 100) == (8 + 800 + 6 + 25) / 26, "symbols");
    CHECK(ppduSamples(W1, 0, 0) == 640 + 640 + 6 * 160 + (size_t)dataSymbols(W1, 0, 0) * 160, "1 MHz preamble: 160 + 160 + 240 us");
    // S1G beacon
    {
        BssSpec b;
        const uint8_t mac[6] = {0x24, 0x0A, 0xC4, 1, 2, 3};
        std::copy(mac, mac + 6, b.bssid);
        b.ssid = "HaLow-Home"; b.security = Wpa3Sae; b.bwInd = 1; b.opClass = 6; b.primary = 3; b.ccf = 3; b.opWidthMhz = 1; b.intervalTu = 250; b.timestamp = 123456;
        const auto f = makeS1gBeacon(b);
        CHECK(fcsOk(f.data(), f.size()), "FCS");
        Frame r;
        CHECK(parseFrame(f.data(), f.size() - 4, r) && r.s1gBeacon, "parse");
        CHECK(r.hasSsid && r.ssid == "HaLow-Home", "SSID '%s'", r.ssid.c_str());
        CHECK(r.hasComp && r.compSsid == wifi::crc32(reinterpret_cast<const uint8_t*>("HaLow-Home"), 10), "compressed SSID");
        CHECK(r.security == Wpa3Sae && r.hasRsn, "security %s", securityName(r.security));
        CHECK(r.hasOp && r.ccf == 3 && r.opClass == 6 && r.opWidthMhz == 1 && r.primary == 3, "S1G Operation");
        CHECK(chanMhz(r.opClass, r.ccf) == 864.5, "frequency %.2f", chanMhz(r.opClass, r.ccf));
        CHECK(r.intervalTu == 250 && r.timestamp == 123456 && r.bwInd == 1 && r.secBit, "interval, timestamp, bandwidth indication");
        CHECK(macText(r.bssid) == "24:0A:C4:01:02:03", "BSSID %s", macText(r.bssid).c_str());
        auto bad = f;
        bad[12] ^= 1;
        CHECK(!fcsOk(bad.data(), bad.size()), "damaged frame passed the FCS");
        for (int sec : {Open, Wpa2Psk, Wpa3Sae, Owe, Wpa2Ent}) {
            b.security = sec;
            const auto g = makeS1gBeacon(b);
            Frame q;
            parseFrame(g.data(), g.size() - 4, q);
            CHECK(q.security == sec, "security %d parsed as %d", sec, q.security);
        }
        b.fullSsid = false;   // compressed SSID only
        const auto h = makeS1gBeacon(b);
        Frame q;
        parseFrame(h.data(), h.size() - 4, q);
        CHECK(!q.hasSsid && q.hasComp, "compressed-only beacon");
        const uint8_t sta[6] = {1, 2, 3, 4, 5, 6};
        const auto pr = makeProbeResponse(b, sta);
        parseFrame(pr.data(), pr.size() - 4, q);
        CHECK(q.probeResp && q.hasSsid && q.ssid == "HaLow-Home", "probe response");
        const auto d = makeDataFrame(b.bssid, sta, 20);
        parseFrame(d.data(), d.size() - 4, q);
        CHECK(q.type == 2 && !q.s1gBeacon, "data frame");
        // truncated and garbage input never crash
        std::mt19937 rng(9);
        for (int i = 0; i < 2000; i++) {
            std::vector<uint8_t> g((size_t)(rng() % 120));
            for (auto& v : g) v = (uint8_t)rng();
            if (!g.empty() && i % 2) g[0] = 0x1C;
            Frame z;
            parseFrame(g.data(), g.size(), z);
        }
    }
    // channel plans: EU 863 - 868 (1 MHz: 5, 2 MHz: 2), US 902 - 928 (26, 13, 6)
    CHECK(euChannels(1).size() == 5 && euChannels(1).front().mhz == 863.5 && euChannels(1).back().mhz == 867.5, "EU 1 MHz");
    CHECK(euChannels(2).size() == 2, "EU 2 MHz");
    CHECK(usChannels(1).size() == 26 && usChannels(1).front().mhz == 902.5 && usChannels(1).back().mhz == 927.5, "US 1 MHz");
    CHECK(usChannels(2).size() == 13 && usChannels(4).size() == 6, "US 2 and 4 MHz");
    CHECK(nearestChannel(864.4, 1, true) == 3 && nearestChannel(915.5, 1, false) == 27 && nearestChannel(880.0, 1, true) == 0, "nearest channel");
    CHECK(inHalowBand(867.0) && inHalowBand(915.0) && !inHalowBand(433.9) && !inHalowBand(2437.0), "band check");
    printf(fails ? "halow proto: %d FAILED\n" : "halow proto: all passed\n", fails);
    return fails ? 1 : 0;
}
