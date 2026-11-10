// 3G-ALE BW0 (hfutil/hfu_ale3g.h): the tables and worked examples of MIL-STD-188-141B Appendix C, the coding, and generated bursts through the
// receiver with frequency error, drift, a sample clock error, an echo, noise down to below 0 dB in 3 kHz, bursts back to back and noise only.
// Simulated signals only.
#include "dect2/hfutil/hfu_ale3g.h"
#include "dect2/hfutil/hfutil_rx.h"
#include "dect2/hfutil/hfutil_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "../common/impair.h"

namespace {

void noise3k(std::vector<cf32>& x, double snrDb, double sigPower, unsigned seed) {
    const double n0 = sigPower / std::pow(10.0, snrDb / 10) * (24000.0 / 3000.0);
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, (float)std::sqrt(n0 / 2));
    for (auto& v : x) v += cf32(g(rng), g(rng));
}
double power(const std::vector<cf32>& x, size_t a, size_t b) { double p = 0; for (size_t i = a; i < b; i++) p += std::norm(x[i]); return p / (double)(b - a); }

std::vector<hfu::Ale3gPdu> run(const std::vector<cf32>& x, size_t chunk = 4096) {
    std::vector<hfu::Ale3gPdu> out;
    hfu::Ale3gRx rx;
    rx.setPduSink([&](const hfu::Ale3gPdu& p) { out.push_back(p); });
    for (size_t i = 0; i < x.size(); i += chunk) rx.push(x.data() + i, std::min(chunk, x.size() - i));
    return out;
}

bool got(const std::vector<hfu::Ale3gPdu>& v, uint32_t b) { for (auto& p : v) if (p.crcOk && p.bits == b) return true; return false; }

struct Imp { double cfo = 0, drift = 0, ppm = 0, snr = 99, echoDb = -99; };

std::vector<cf32> burst(uint32_t pdu, const Imp& m, unsigned seed = 1) {
    std::vector<cf32> x = hfu::genBw0(pdu, 0.3, 0.3);
    const double p = power(x, 7200, x.size() - 7200);
    if (m.echoDb > -90) impair::echo(x, 24, m.echoDb);
    if (m.ppm != 0) x = impair::clock(x, m.ppm);
    if (m.cfo != 0) impair::shift(x, m.cfo, 24000);
    if (m.drift != 0) { double ph = 0; for (size_t i = 0; i < x.size(); i++) { ph += 2 * M_PI * (m.drift * (double)i / 24000.0) / 24000.0; x[i] *= cf32((float)std::cos(ph), (float)std::sin(ph)); } }
    if (m.snr < 90) noise3k(x, m.snr, p, seed);
    return x;
}

}  // namespace

int main() {
    // table C-VIII rows (b3b2b1b0 = 0101 and 1110) and the Walsh rule behind it
    {
        const char* row5 = "0404404004044040", *row14 = "0044440044000044";
        const uint32_t a = hfu::bw0Encode(0).size();
        CHECK(a == 52, "52 coded bits");
        // chips of row n: 4 * parity(n & j); compare with the table text
        for (int j = 0; j < 16; j++) {
            CHECK((__builtin_parity(5 & j) ? '4' : '0') == row5[j], "Walsh row 0101 chip %d", j);
            CHECK((__builtin_parity(14 & j) ? '4' : '0') == row14[j], "Walsh row 1110 chip %d", j);
        }
    }
    // C.5.2.2.2 worked LinkID examples
    CHECK(hfu::ale3gLinkId(1, 2) == 0x3D && hfu::ale3gLinkId(1, 3) == 0x24 && hfu::ale3gLinkId(2, 1) == 0x04 && hfu::ale3gLinkId(3, 1) == 0x33 && hfu::ale3gLinkId(1951, 1) == 0x1E, "LinkID examples");
    // the PDUs: fields, CRC, and the LE_Broadcast start 0 1 1 1 0 (C.5.2.2)
    {
        const uint32_t c = hfu::ale3gCall(5, 2, 17, 3);
        const hfu::Ale3gPdu p = hfu::ale3gParse(c);
        CHECK(p.type == "LE_Call" && p.crcOk && p.text.find("member 17") != std::string::npos && p.text.find("analog voice") != std::string::npos, "LE_Call: %s %s", p.type.c_str(), p.text.c_str());
        CHECK(!hfu::ale3gParse(c ^ 0x100).crcOk, "LE_Call CRC catches an error");
        const hfu::Ale3gPdu h = hfu::ale3gParse(hfu::ale3gHandshake(0x3D, 1, 42));
        CHECK(h.type == "LE_Handshake" && h.crcOk && h.text.find("channel 42") != std::string::npos && h.text.find("3D") != std::string::npos, "LE_Handshake: %s", h.text.c_str());
        const uint32_t bc = hfu::ale3gBroadcast(3, 1, 9);
        CHECK((bc >> 21) == 0x0E, "LE_Broadcast starts 0 1 1 1 0");
        CHECK(hfu::ale3gParse(bc).type == "LE_Broadcast" && hfu::ale3gParse(bc).crcOk, "LE_Broadcast");
        CHECK(hfu::ale3gParse(hfu::ale3gNotification(1, 4, 9)).type == "LE_Notification", "LE_Notification");
        // CRC-4 x^4 + x^3 + x + 1: the register after one 1 bit is the polynomial
        CHECK(hfu::ale3gCrc(1, 1, 4) == 0x0B && hfu::ale3gCrc(1, 1, 8) == 0x9B, "CRC polynomials");
    }
    // coding: every fetch position is a coded bit once; random payloads, clean and with 4 coded bits wrong
    {
        std::mt19937 r(3);
        for (int t = 0; t < 200; t++) {
            const uint32_t b = r() & 0x3FFFFFFu;
            const auto c = hfu::bw0Encode(b);
            std::vector<float> s(52);
            for (int i = 0; i < 52; i++) s[(size_t)i] = c[(size_t)i] ? -1.f : 1.f;
            CHECK(hfu::bw0Decode(s) == b, "clean decode %07X", b);
            for (int e = 0; e < 4; e++) { const size_t k = r() % 52; s[k] = -s[k]; }
            if (t < 50) CHECK(hfu::bw0Decode(s) == b, "decode with 4 errors %07X", b);
        }
    }
    // bursts through the receiver
    const uint32_t pdus[] = {hfu::ale3gCall(5, 2, 17, 3), hfu::ale3gHandshake(0x3D, 1, 42), hfu::ale3gBroadcast(2, 0, 77), hfu::ale3gNotification(7, 30, 12)};
    struct C { const char* name; Imp m; };
    const C cases[] = {{"clean", {}}, {"+60 Hz", {60, 0, 0, 25}}, {"-75 Hz", {-75, 0, 0, 25}}, {"drift 3 Hz/s", {10, 3, 0, 20}}, {"clock +60 ppm", {0, 0, 60, 20}},
                       {"clock -60 ppm", {-20, 0, -60, 20}}, {"echo 1 ms -6 dB", {0, 0, 0, 20, -6}}, {"SNR 6 dB", {15, 0, 0, 6}}, {"SNR 0 dB", {-30, 0, 0, 0}}};
    for (const C& c : cases) {
        int ok = 0;
        for (uint32_t p : pdus) ok += got(run(burst(p, c.m, p & 0xFF)), p);
        printf("%-20s %d/4 PDUs\n", c.name, ok);
        CHECK(ok == 4, "%s: %d of 4", c.name, ok);
    }
    // weak: -4 and -8 dB in 3 kHz (BW0 is spread 64 times)
    for (double weak : {-4.0, -8.0}) {
        int ok = 0, n = 0;
        for (unsigned s = 1; s <= 8; s++) {
            const uint32_t p = pdus[s % 4];
            const auto v = run(burst(p, {20, 0, 0, weak}, s * 77));
            ok += got(v, p); n++;
            for (auto& q : v) if (!q.crcOk) printf("  %.0f dB burst: CRC bad, preamble SNR %.1f dB, %+.1f Hz\n", weak, q.snrDb, q.cfoHz);
        }
        printf("SNR %.0f dB: %d/%d PDUs\n", weak, ok, n);
        CHECK(ok >= (weak > -5 ? 8 : 5), "SNR %.0f dB: %d of %d", weak, ok, n);
    }
    // a call, the handshake and the answer back to back, fed in odd chunks
    {
        std::vector<cf32> x;
        for (uint32_t p : pdus) { const auto b = hfu::genBw0(p, 0.02, 0.02); x.insert(x.end(), b.begin(), b.end()); }
        impair::shift(x, 33, 24000);
        noise3k(x, 15, power(x, 0, x.size()), 5);
        const auto v = run(x, 333);
        int ok = 0; for (uint32_t p : pdus) ok += got(v, p);
        CHECK(ok == 4 && v.size() == 4, "back to back: %d good of %zu", ok, v.size());
    }
    // noise only: no PDU with a good CRC
    {
        std::vector<cf32> x(24000 * 10, cf32(0, 0));
        noise3k(x, 0, 1.0, 9);
        int good = 0;
        for (auto& p : run(x)) good += p.crcOk;
        CHECK(good == 0, "noise: %d good PDUs", good);
    }
    // through the HF utility receiver at 250 ksps, 20 kHz off the tuning
    {
        std::vector<cf32> b;
        for (uint32_t p : pdus) { const auto q = hfu::genBw0(p, 0.1, 0.1); b.insert(b.end(), q.begin(), q.end()); }
        const double rate = 250000;
        std::vector<cf32> x((size_t)((double)b.size() * rate / 24000.0));
        for (size_t i = 0; i < x.size(); i++) {
            const double q = (double)i * 24000.0 / rate;
            const size_t k = (size_t)q;
            const float fr = (float)(q - (double)k);
            const cf32 v = k + 1 < b.size() ? b[k] * (1 - fr) + b[k + 1] * fr : cf32(0, 0);
            const double a = 2 * M_PI * -20000.0 * (double)i / rate;
            x[i] = v * cf32((float)std::cos(a), (float)std::sin(a)) * 0.1f;
        }
        HfUtilReceiver rx(kHfuKindAle3g);
        rx.configure(rate);
        rx.setSignalOffset(-20000);
        for (size_t i = 0; i < x.size(); i += 65536) rx.feed(x.data() + i, std::min<size_t>(65536, x.size() - i));
        HfUtilTelemetry t;
        rx.telemetry(t, 0);
        printf("receiver: %s\n%s", t.mode.c_str(), t.text.c_str());
        CHECK(t.pdus == 4 && t.text.find("LE_Handshake") != std::string::npos, "receiver: %llu PDUs", (unsigned long long)t.pdus);
    }
    return testSummary("hfutil_ale3g");
}
