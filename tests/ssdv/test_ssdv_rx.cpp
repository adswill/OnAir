// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SSDV receiver on the generator's signals (FSK / GFSK bit streams, plain or RTTY-framed): clean, then the impairments one by one: carrier
// offsets of +-3 kHz, the swapped frequencies, a clock error of 1000 ppm, a carrier drift, noise, and every speed; noise alone must give nothing.
// A picture counts as received when it is complete and its JPEG is byte-for-byte the one the packets give directly.
#include "dect2/ssdv/ssdv_gen.h"
#include "dect2/ssdv/ssdv_proto.h"
#include "dect2/ssdv/ssdv_rx.h"
#include "dect2/ssdv/ssdv_tel.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static const double kRate = 1e6;
static std::vector<std::array<uint8_t, ssdv::kPktSize>> gPk;
static std::vector<uint8_t> gGoodJpeg;

struct Run { SsdvTelemetry t; };

static SsdvTelemetry run(SsdvGenConfig c, double sec, const std::string& dir = "", int fixedBaud = 0) {
    c.rate = kRate;
    c.dialOffsetHz = -50000;
    if (c.packets.empty()) c.packets = gPk;
    SsdvGenerator g(c);
    SsdvReceiver r;
    r.configure(kRate);
    r.setSignalOffset(-50000);
    r.setBaud(fixedBaud);
    if (!dir.empty()) r.setSaveDir(dir);
    std::vector<cf32> buf(20000);
    const size_t tot = (size_t)(sec * kRate);
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

static double secFor(double baud, bool async, size_t npk) { return (npk * (256 + 8) * (async ? 11.0 : 8.0)) / baud + 1.0; }

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::vector<uint8_t> jpg = ssdv::testJpeg(48, 32);
    gPk = ssdv::encodePackets(jpg, "ONAIR", 3, 5, false);
    printf("  test picture: %zu packets\n", gPk.size());
    {
        ssdv::Assembler a; for (auto& p : gPk) a.add(p.data());
        gGoodJpeg = a.jpeg();
    }
    CHECK(gPk.size() >= 2 && !gGoodJpeg.empty(), "test picture");
    const size_t N = gPk.size();
    {   // 9600 baud GFSK, clean, saved to a folder
        const std::string dir = (std::filesystem::temp_directory_path() / "onair_ssdv_test").string();
        std::filesystem::remove_all(dir);
        SsdvGenConfig c; c.baud = 9600; c.gaussBt = 0.5;
        const SsdvTelemetry t = run(c, secFor(9600, false, N) * 1.2, dir);
        printf("  9600 clean: %llu packets, %zu pictures, baud %d\n", (unsigned long long)t.blocksOk, t.images.size(), t.baud);
        CHECK(pictureOk(t) && t.baud == 9600 && !t.async, "9600 clean: complete %d baud %d", pictureOk(t), t.baud);
        CHECK(t.imagesDone == 1 && !t.images.empty() && !t.images[0].savedPath.empty(), "saved");
        if (!t.images.empty() && !t.images[0].savedPath.empty()) {
            std::ifstream f(t.images[0].savedPath, std::ios::binary);
            std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            CHECK(file == gGoodJpeg, "the saved file is the JPEG");
        }
        std::filesystem::remove_all(dir);
    }
    for (double baud : {1200.0, 2400.0, 4800.0, 19200.0}) {
        SsdvGenConfig c; c.baud = baud; c.gaussBt = 0.5;
        const SsdvTelemetry t = run(c, secFor(baud, false, N) * 1.25);
        CHECK(pictureOk(t) && t.baud == (int)baud, "%g baud: complete %d baud %d (%llu packets)", baud, pictureOk(t), t.baud, (unsigned long long)t.blocksOk);
    }
    {   // RTTY-framed bytes, 300 and 600 baud, a narrow shift
        for (double baud : {300.0, 600.0}) {
            SsdvGenConfig c; c.baud = baud; c.async = true;
            const SsdvTelemetry t = run(c, secFor(baud, true, N) * 1.2);
            CHECK(pictureOk(t) && t.baud == (int)baud && t.async, "%g baud async: complete %d baud %d async %d", baud, pictureOk(t), t.baud, t.async);
        }
    }
    {   // impairments at 9600 baud
        struct Case { const char* name; double cfo, drift, ppm, ebn0; bool inv; };
        const Case cs[] = {
            {"carrier +3 kHz", 3000, 0, 0, 99, false}, {"carrier -3 kHz", -3000, 0, 0, 99, false}, {"swapped frequencies", 0, 0, 0, 99, true},
            {"clock +1000 ppm", 0, 0, 1000, 99, false}, {"clock -1000 ppm", 0, 0, -1000, 99, false}, {"drift 200 Hz/s", 0, 200, 0, 99, false},
            {"noise Eb/N0 16 dB", 0, 0, 0, 16, false}, {"all: +2 kHz, 500 ppm, drift, 17 dB, swapped", 2000, 100, 500, 17, true},
        };
        for (const Case& k : cs) {
            SsdvGenConfig c; c.baud = 9600; c.gaussBt = 0.5; c.cfoHz = k.cfo; c.driftHzPerSec = k.drift; c.clockPpm = k.ppm; c.ebn0Db = k.ebn0; c.invert = k.inv; c.seed = 7;
            const SsdvTelemetry t = run(c, secFor(9600, false, N) * 1.4);
            printf("  %-44s %llu packets ok %llu bad, picture %s\n", k.name, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, pictureOk(t) ? "complete" : "NOT complete");
            CHECK(pictureOk(t), "%s: picture not complete (%llu packets)", k.name, (unsigned long long)t.blocksOk);
        }
    }
    {   // a fixed speed picks only that chain
        SsdvGenConfig c; c.baud = 4800; c.gaussBt = 0.5;
        const SsdvTelemetry t = run(c, secFor(4800, false, N) * 1.25, "", 4800);
        CHECK(pictureOk(t) && t.fixedBaud == 4800, "fixed 4800");
        const SsdvTelemetry w = run(c, secFor(4800, false, N) * 1.25, "", 9600);
        CHECK(w.blocksOk == 0, "wrong fixed speed gives %llu packets", (unsigned long long)w.blocksOk);
    }
    {   // damaged packets: Reed-Solomon corrects burst errors in the stream (flip bytes in some packets)
        SsdvGenConfig c; c.baud = 9600; c.gaussBt = 0.5;
        c.packets = gPk;
        for (size_t i = 0; i < c.packets.size(); i++) for (int k = 0; k < 6; k++) c.packets[i][(size_t)(20 + 17 * k + (int)i)] ^= 0x5A;
        const SsdvTelemetry t = run(c, secFor(9600, false, N) * 1.3);
        CHECK(pictureOk(t) && t.corrected >= N, "RS repaired the packets: complete %d corrected %llu", pictureOk(t), (unsigned long long)t.corrected);
    }
    {   // noise alone and the wrong signals give nothing
        SsdvGenConfig c; c.baud = 9600; c.ebn0Db = 8; c.packets.assign(1, gPk[0]);
        for (auto& b : c.packets[0]) b = 0xA5;   // not a packet: no sync word
        const SsdvTelemetry t = run(c, 10.0);
        CHECK(t.blocksOk == 0 && t.images.empty(), "no packets from a non-SSDV bit stream: %llu", (unsigned long long)t.blocksOk);
        SsdvGenConfig n; n.baud = 9600; n.ebn0Db = -40;   // practically noise
        const SsdvTelemetry t2 = run(n, 10.0);
        CHECK(t2.blocksOk == 0 && t2.images.empty(), "no packets from noise: %llu", (unsigned long long)t2.blocksOk);
    }
    return testSummary("ssdv rx");
}
