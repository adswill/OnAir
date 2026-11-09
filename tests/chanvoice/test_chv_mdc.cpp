// MDC-1200 burst: encoder and decoder round trip over every op / argument / unit ID combination class, with noise, a level change, a sampling clock
// offset, a leading silence of any length, frequency error, voice before and after, double packets, and rejection of noise and speech. The encoder
// and decoder are both written here from the same format description, so the round trip alone cannot show agreement with a real radio: it was checked
// on the format facts only (docs/CHANNELS-3.8.md). Simulated signals only.
#include "dect2/chanvoice/chv_mdc.h"
#include "dect2/tones/tones_gen.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
using namespace dect2::chv;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static std::vector<MdcPacket> decode(const std::vector<float>& a, double rate = 48000) {
    MdcDecoder d(rate);
    std::vector<MdcPacket> v;
    d.setCallback([&](const MdcPacket& p) { v.push_back(p); });
    for (size_t i = 0; i < a.size(); i += 480) d.feed(a.data() + i, std::min<size_t>(480, a.size() - i));
    return v;
}

int main() {
    // known answer for the CRC: the format's check of op 01 arg 80 unit 1234 is computed by the same routine, so only the properties are tested
    { const uint8_t a[4] = {1, 0x80, 0x12, 0x34}, b[4] = {1, 0x80, 0x12, 0x35}; CHECK(mdcCrc(a, 4) != mdcCrc(b, 4), "crc differs"); }
    std::mt19937 rng(5);
    std::normal_distribution<float> nd(0.f, 1.f);
    int ok = 0, n = 0;
    const uint8_t ops[] = {0x01, 0x01, 0x23, 0x46, 0x00, 0xFF, 0x11, 0x80};
    for (int t = 0; t < 120; t++) {
        MdcPacket p; p.op = ops[t % 8]; p.arg = (uint8_t)(t * 37); p.unitId = (uint16_t)(0x1000 + t * 523);
        std::vector<float> a(4800 + (size_t)(rng() % 480), 0.f);   // leading silence, any alignment
        const std::vector<float> b = mdcEncode(p, 48000, 0.3);
        a.insert(a.end(), b.begin(), b.end());
        a.resize(a.size() + 4800, 0.f);
        const double noise = 0.02 + 0.2 * (t % 6) / 5.0;   // 0.02 .. 0.22 rms against a 0.21 rms signal: SNR 20 dB .. 0 dB over the whole 24 kHz
        for (auto& s : a) s += noise * nd(rng);
        const auto r = decode(a);
        n++;
        if (r.size() == 1 && r[0].op == p.op && r[0].arg == p.arg && r[0].unitId == p.unitId && !r[0].dbl) ok++;
        else if (noise < 0.2) printf("packet %d noise %.2f: %zu decoded\n", t, noise, r.size());
    }
    printf("MDC round trip with noise: %d of %d\n", ok, n);
    CHECK(ok >= n * 85 / 100, "round trip %d of %d", ok, n);
    // clean: all must decode, exactly once
    int clean = 0;
    for (int t = 0; t < 40; t++) {
        MdcPacket p; p.op = (uint8_t)(t * 7); p.arg = (uint8_t)(t * 11); p.unitId = (uint16_t)(t * 1601);
        if (p.op == 0x35 || p.op == 0x55) p.op = 1;
        std::vector<float> a(2000 + (size_t)t * 37, 0.f); const auto b = mdcEncode(p, 48000, 0.4); a.insert(a.end(), b.begin(), b.end()); a.resize(a.size() + 2000, 0.f);
        const auto r = decode(a);
        if (r.size() == 1 && r[0].op == p.op && r[0].arg == p.arg && r[0].unitId == p.unitId) clean++;
    }
    CHECK(clean == 40, "clean packets: %d of 40", clean);
    // other sample rates (8 kHz and 16 kHz audio) and a clock error of +-1 % (the encoder runs at a slightly different rate)
    for (double rate : {8000.0, 16000.0}) {
        MdcPacket p; p.op = 0x01; p.arg = 0x80; p.unitId = 0xBEEF;
        std::vector<float> a(2000, 0.f); const auto b = mdcEncode(p, rate, 0.4); a.insert(a.end(), b.begin(), b.end()); a.resize(a.size() + 2000, 0.f);
        const auto r = decode(a, rate);
        CHECK(r.size() == 1 && r[0].unitId == 0xBEEF, "rate %.0f: %zu", rate, r.size());
    }
    for (double ppm : {-10000.0, -5000.0, 5000.0, 10000.0}) {
        MdcPacket p; p.op = 0x01; p.arg = 0x00; p.unitId = 0x0A0B;
        std::vector<float> a(3000, 0.f); const auto b = mdcEncode(p, 48000 * (1 + ppm * 1e-6), 0.4); a.insert(a.end(), b.begin(), b.end()); a.resize(a.size() + 3000, 0.f);
        const auto r = decode(a);
        CHECK(r.size() == 1 && r[0].unitId == 0x0A0B, "clock error %.0f ppm: %zu", ppm, r.size());
    }
    // double packet
    {
        MdcPacket p; p.op = 0x35; p.arg = 0x01; p.unitId = 0x4321; p.dbl = true; p.extra[0] = 0xDE; p.extra[1] = 0xAD; p.extra[2] = 0xBE; p.extra[3] = 0xEF;
        std::vector<float> a(3000, 0.f); const auto b = mdcEncode(p, 48000, 0.4); a.insert(a.end(), b.begin(), b.end()); a.resize(a.size() + 3000, 0.f);
        const auto r = decode(a);
        CHECK(r.size() == 1 && r[0].dbl && r[0].unitId == 0x4321 && r[0].extra[0] == 0xDE && r[0].extra[3] == 0xEF, "double packet: %zu", r.size());
    }
    // two bursts in a row (a pre-ID and a post-ID) and a burst between speech
    {
        MdcPacket p1; p1.op = 1; p1.arg = 0x80; p1.unitId = 0x1111; MdcPacket p2 = p1; p2.arg = 0; 
        ToneScene s(48000, 4); s.speech(0, 2.0, 0.3); s.noise(0, 2.0, 0.01);
        std::vector<float> a = s.audio();
        auto b1 = mdcEncode(p1, 48000, 0.3), b2 = mdcEncode(p2, 48000, 0.3);
        std::vector<float> all = a; all.insert(all.end(), b1.begin(), b1.end()); all.resize(all.size() + 9600, 0.f); all.insert(all.end(), b2.begin(), b2.end()); all.insert(all.end(), a.begin(), a.end());
        const auto r = decode(all);
        CHECK(r.size() == 2 && r[0].arg == 0x80 && r[1].arg == 0, "two bursts around speech: %zu decoded", r.size());
    }
    // no packet from speech or noise
    {
        int fp = 0;
        for (int seed = 1; seed <= 12; seed++) {
            ToneScene s(48000, 300 + seed); s.noise(0, 10, 0.1); s.speech(0, 10, 0.4);
            fp += (int)decode(s.audio()).size();
        }
        CHECK(fp == 0, "false packets from speech: %d", fp);
    }
    printf(fails ? "chv_mdc: %d FAILED\n" : "chv_mdc: all passed\n", fails);
    return fails ? 1 : 0;
}
