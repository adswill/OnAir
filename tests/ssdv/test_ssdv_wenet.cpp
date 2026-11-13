// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Wenet (projecthorus/wenet): the LDPC code against the reference test vector, the encoder / decoder round trip, and the whole chain on the
// generator's signals (2-FSK 115.2 kbaud with serial-word framing, and 96 kbaud raw scrambled): clean, carrier offsets, swapped frequencies, clock
// errors, a carrier drift, noise, bit slips, other sample rates, then noise alone and a fixed wrong speed must give nothing. No real recording was used.
#include "dect2/ssdv/ssdv_gen.h"
#include "dect2/ssdv/ssdv_proto.h"
#include "dect2/ssdv/ssdv_rx.h"
#include "dect2/ssdv/ssdv_tel.h"
#include "dect2/ssdv/wenet.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#include "wenet_vector.h"

static std::vector<std::array<uint8_t, ssdv::kPktSize>> gPk;
static std::vector<uint8_t> gGoodJpeg;

static SsdvTelemetry run(SsdvGenConfig c, double sec, double rate = 2e6, int fixedBaud = 0, double dial = -50000) {
    c.rate = rate;
    c.dialOffsetHz = dial;
    if (c.packets.empty()) c.packets = gPk;
    SsdvGenerator g(c);
    SsdvReceiver r;
    r.configure(rate);
    r.setSignalOffset(dial);
    r.setBaud(fixedBaud);
    std::vector<cf32> buf(20000);
    const size_t tot = (size_t)(sec * rate);
    for (size_t i = 0; i < tot; i += buf.size()) { g.generate(buf.data(), buf.size()); r.feed(buf.data(), buf.size()); }
    r.flush();
    SsdvTelemetry t;
    r.telemetry(t, 0);
    return t;
}

static bool pictureOk(const SsdvTelemetry& t) {
    if (t.images.empty()) return false;
    const SsdvImage& im = t.images[0];
    return im.complete && im.jpeg && *im.jpeg == gGoodJpeg;
}

static double frameSec(int wenetMode) { return wenetMode == 2 ? (16 + 4 + 323) * 8 / 96000.0 : (16 + 4 + 323) * 10 / 115200.0; }

static SsdvGenConfig cfg(int mode) {
    SsdvGenConfig c;
    c.wenet = mode;
    c.gaussBt = 0.5;
    c.seed = 5;
    return c;
}

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // --- CRC ---
    {
        const uint8_t s[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        CHECK(wenet::crc16(s, 9) == 0x29B1, "CRC-16/CCITT-FALSE check value: %04x", wenet::crc16(s, 9));
    }
    // --- LDPC: the reference vector of the Wenet repository ---
    {
        float soft[wenet::kCodeBits];
        for (int i = 0; i < wenet::kCodeBits; i++) soft[i] = -kWenetVecIn[i];   // negative symbol = a 1 bit in the reference decoder
        uint8_t bits[wenet::kCodeBits];
        int it = 0;
        const bool ok = wenet::ldpcDecode(soft, bits, 50, &it);
        int diff = 0;
        for (int i = 0; i < wenet::kCodeBits; i++) diff += bits[i] != kWenetVecOut[i];
        CHECK(ok && diff == 0, "reference vector: converged %d, %d bits differ, %d iterations", ok, diff, it);
        // the encoder gives the same parity for the vector's information bits
        uint8_t info[wenet::kInfoBytes], par[wenet::kParityBytes];
        for (int k = 0; k < wenet::kInfoBytes; k++) { uint8_t b = 0; for (int i = 0; i < 8; i++) b = (uint8_t)((b << 1) | kWenetVecOut[8 * k + i]); info[k] = b; }
        wenet::ldpcEncode(info, par);
        int pd = 0;
        for (int p = 0; p < wenet::kParityBits; p++) pd += ((par[p >> 3] >> (7 - (p & 7))) & 1) != kWenetVecOut[wenet::kInfoBits + p];
        CHECK(pd == 0, "encoder parity differs from the vector in %d bits", pd);
    }
    // --- LDPC round trip with noise: the coding gain ---
    {
        std::mt19937 rng(3);
        std::normal_distribution<float> nd(0.f, 1.f);
        for (double ebn0 : {3.0, 4.0, 5.0}) {
            int okc = 0;
            const int trials = 100;
            for (int t = 0; t < trials; t++) {
                uint8_t info[wenet::kInfoBytes], par[wenet::kParityBytes];
                for (auto& b : info) b = (uint8_t)rng();
                wenet::ldpcEncode(info, par);
                // rate 0.8 BPSK: Es/N0 = Eb/N0 + 10log10(0.8); sigma^2 = 1 / (2 Es/N0)
                const double esn0 = std::pow(10.0, (ebn0 + 10 * std::log10(0.8)) / 10.0);
                const float sg = (float)std::sqrt(1.0 / (2 * esn0));
                float soft[wenet::kCodeBits];
                for (int i = 0; i < wenet::kCodeBits; i++) {
                    const int bit = i < wenet::kInfoBits ? (info[i >> 3] >> (7 - (i & 7))) & 1 : (par[(i - wenet::kInfoBits) >> 3] >> (7 - ((i - wenet::kInfoBits) & 7))) & 1;
                    soft[i] = (bit ? 1.f : -1.f) + sg * nd(rng);
                }
                uint8_t bits[wenet::kCodeBits];
                const bool ok = wenet::ldpcDecode(soft, bits, 40, nullptr);
                bool same = ok;
                for (int i = 0; i < wenet::kInfoBits && same; i++) same = bits[i] == ((info[i >> 3] >> (7 - (i & 7))) & 1);
                okc += same;
            }
            printf("  LDPC (random data) Eb/N0 %.0f dB: %d / %d frames\n", ebn0, okc, trials);
            if (ebn0 >= 5.0) CHECK(okc >= 85, "LDPC at 5 dB: %d / %d", okc, trials);
        }
    }
    // --- frames ---
    const std::vector<uint8_t> jpg = ssdv::testJpeg(48, 32);
    gPk = ssdv::encodePackets(jpg, "ONAIR", 3, 5, false);
    { ssdv::Assembler a; for (auto& p : gPk) a.add(p.data()); gGoodJpeg = a.jpeg(); }
    const size_t N = gPk.size();
    printf("  test picture: %zu packets\n", N);
    CHECK(N >= 2 && !gGoodJpeg.empty(), "test picture");
    {
        const auto fb = wenet::frameBits(gPk[0].data(), wenet::Framing::Uart);
        CHECK(fb.size() == (size_t)(16 + 4 + 323) * 10, "UART frame length %zu", fb.size());
        const auto fr = wenet::frameBits(gPk[0].data(), wenet::Framing::Raw);
        CHECK(fr.size() == (size_t)(16 + 4 + 323) * 8, "raw frame length %zu", fr.size());
        // first byte of the preamble 0x55 sent as start 0, 1 0 1 0 1 0 1 0 (LSB first), stop 1
        const uint8_t pre[10] = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
        CHECK(std::memcmp(fb.data(), pre, 10) == 0, "serial word of 0x55");
    }
    // --- the whole chain ---
    for (int mode : {1, 2}) {
        const double base = N * frameSec(mode);
        {
            const SsdvTelemetry t = run(cfg(mode), base * 1.6 + 0.1);
            printf("  mode %d clean: %llu packets, baud %d wenet %d\n", mode, (unsigned long long)t.blocksOk, t.baud, t.wenet);
            CHECK(pictureOk(t) && t.wenet && t.baud == (mode == 2 ? 96000 : 115200), "mode %d clean: picture %d wenet %d baud %d", mode, pictureOk(t), t.wenet, t.baud);
            CHECK(t.packets.size() > 0 && t.packets[0].wenet, "packet log marks Wenet");
        }
        struct Case { const char* name; double cfo, drift, ppm, ebn0, slip; bool inv; double bt; };
        const Case cs[] = {
            {"carrier +20 kHz", 20000, 0, 0, 99, 0, false, 0.5}, {"carrier -20 kHz", -20000, 0, 0, 99, 0, false, 0.5},
            {"carrier +50 kHz", 50000, 0, 0, 99, 0, false, 0.5}, {"carrier -50 kHz", -50000, 0, 0, 99, 0, false, 0.5},
            {"carrier +70 kHz", 70000, 0, 0, 99, 0, false, 0.5}, {"carrier -70 kHz", -70000, 0, 0, 99, 0, false, 0.5},
            {"swapped frequencies", 0, 0, 0, 99, 0, true, 0.5}, {"clock +1500 ppm", 0, 0, 1500, 99, 0, false, 0.5},
            {"clock -1500 ppm", 0, 0, -1500, 99, 0, false, 0.5}, {"drift 2 kHz/s", 0, 2000, 0, 99, 0, false, 0.5},
            {"rectangular bits (no Gaussian)", 0, 0, 0, 99, 0, false, 0.0},
            {"noise Eb/N0 14 dB", 0, 0, 0, 14, 0, false, 0.5},
            {"all: +30 kHz, 800 ppm, drift, 15 dB, swapped", 30000, 500, 800, 15, 0, true, 0.5},
        };
        for (const Case& k : cs) {
            SsdvGenConfig c = cfg(mode);
            c.cfoHz = k.cfo; c.driftHzPerSec = k.drift; c.clockPpm = k.ppm; c.ebn0Db = k.ebn0; c.invert = k.inv; c.gaussBt = k.bt;
            const SsdvTelemetry t = run(c, base * 2.5 + 0.2);
            printf("  mode %d %-44s %llu packets, picture %s\n", mode, k.name, (unsigned long long)t.blocksOk, pictureOk(t) ? "complete" : "NOT complete");
            CHECK(pictureOk(t), "mode %d %s: picture not complete (%llu packets)", mode, k.name, (unsigned long long)t.blocksOk);
        }
    }
    {   // bit slips: one bit lost every 15000 bits (about every 4th frame is damaged); the receiver has to find the next unique word
        for (int slip : {15000, -15000}) {
            SsdvGenConfig c = cfg(1); c.slipEveryBits = slip;
            const double sec = 40 * frameSec(1);
            const SsdvTelemetry t = run(c, sec);
            const double expect = 40 * 0.7;
            printf("  bit slip every %d bits: %llu of 40 frames\n", slip, (unsigned long long)t.blocksOk);
            CHECK(t.blocksOk >= (uint64_t)expect, "bit slips %d: only %llu frames", slip, (unsigned long long)t.blocksOk);
        }
    }
    {   // other sample rates
        for (double rate : {1e6, 2.4e6, 4e6, 3.2e6}) {
            const SsdvTelemetry t = run(cfg(1), N * frameSec(1) * 1.6 + 0.1, rate);
            printf("  sample rate %.1f MHz: %llu packets\n", rate / 1e6, (unsigned long long)t.blocksOk);
            CHECK(pictureOk(t), "sample rate %g: picture %d", rate, pictureOk(t));
        }
    }
    {   // noise sweep, for the record: frames per 60 sent
        for (double e : {8.0, 9.0, 10.0, 11.0, 12.0, 14.0}) {
            SsdvGenConfig c = cfg(1); c.ebn0Db = e;
            const SsdvTelemetry t = run(c, 60 * frameSec(1));
            printf("  Eb/N0 %4.1f dB: %3llu of 60 frames\n", e, (unsigned long long)t.blocksOk);
        }
        SsdvGenConfig c = cfg(1); c.ebn0Db = 12.5;
        const SsdvTelemetry t = run(c, 60 * frameSec(1));
        CHECK(t.blocksOk >= 50, "12.5 dB: %llu of 60 frames", (unsigned long long)t.blocksOk);
    }
    {   // a fixed speed picks only that chain
        const SsdvTelemetry t = run(cfg(1), N * frameSec(1) * 1.6 + 0.1, 2e6, 115200);
        CHECK(pictureOk(t) && t.fixedBaud == 115200, "fixed Wenet 115200");
        const SsdvTelemetry w = run(cfg(1), N * frameSec(1) * 1.6 + 0.1, 2e6, 96000);
        CHECK(w.blocksOk == 0, "fixed Wenet 96000 on a 115200 signal: %llu", (unsigned long long)w.blocksOk);
        const SsdvTelemetry x = run(cfg(1), N * frameSec(1) * 1.6 + 0.1, 2e6, 9600);
        CHECK(x.blocksOk == 0, "fixed 9600 on a Wenet signal: %llu", (unsigned long long)x.blocksOk);
    }
    {   // damaged payload that still passes the CRC cannot happen; a frame with a wrong CRC gives nothing
        std::array<uint8_t, 256> junk{};
        for (size_t i = 0; i < junk.size(); i++) junk[i] = (uint8_t)(i * 7 + 3);
        SsdvGenConfig c = cfg(1); c.packets.assign(1, junk);   // type byte 0x03: telemetry, counted but not a picture
        const SsdvTelemetry t = run(c, 10 * frameSec(1));
        CHECK(t.wenetOther >= 5 && t.images.empty(), "non-SSDV Wenet packets: other %llu images %zu", (unsigned long long)t.wenetOther, t.images.size());
    }
    {   // noise alone gives nothing
        SsdvGenConfig n = cfg(1); n.ebn0Db = -40;
        const SsdvTelemetry t = run(n, 3.0);
        CHECK(t.blocksOk == 0 && t.images.empty(), "no packets from noise: %llu", (unsigned long long)t.blocksOk);
    }
    return testSummary("ssdv wenet");
}
