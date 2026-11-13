// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// RC and mesh link receiver on simulated signals (no recording of a real link yet): every protocol, carrier offsets, hopping channels, overlap, noise, sample
// rates; ExpressLRS channels only with the phrase.
#include "dect2/rclink/rc_rx.h"
#include "rc_test_gen.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;
using namespace dect2::rclink;
using namespace rctest;
#include "../common/test_util.h"

static RcTelemetry run(RcLinkReceiver& rx, std::vector<cf32>& x, double fs) {
    rx.configure(fs);
    const size_t step = 8192;
    for (size_t i = 0; i < x.size(); i += step) rx.feed(x.data() + i, std::min(step, x.size() - i));
    rx.flush();
    RcTelemetry t;
    rx.telemetry(t, ~0ull);
    return t;
}
static int count(const RcTelemetry& t, const char* type) {
    int n = 0;
    for (const auto& f : t.frames) if (f.type == type) n++;
    return n;
}

static std::vector<uint8_t> frameBytes(std::vector<uint8_t> head, const std::vector<uint8_t>& body) { head.insert(head.end(), body.begin(), body.end()); head.push_back(0x55); head.push_back(0x55); return head; }

int main() {
    // ---- SiK / MAVLink: every air rate needed, hopping channels inside the capture, carrier offset, noise
    std::vector<uint8_t> pos(28, 0);
    const int32_t lat = 546872000, lon = 252797000, alt = 180000;
    std::memcpy(&pos[4], &lat, 4); std::memcpy(&pos[8], &lon, 4); std::memcpy(&pos[12], &alt, 4);
    const auto mav = mavlinkFrame(1, 33, 1, 1, 1, pos);
    const std::vector<uint8_t> sikPre = {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x2D, 0xD4};
    for (double fs : {2.4e6, 8e6})
        for (int kbps : {64, 128, 250})
            for (double cfo : {-30e3, 0.0, 25e3}) {
                std::vector<cf32> x((size_t)(0.05 * fs), cf32(0, 0));
                for (int b = 0; b < 3; b++) {
                    const auto air = frameBytes(sikPre, sikBuild(25, mav, 300, false));
                    addGfsk(x, (size_t)((0.003 + b * 0.012) * fs), toBits(air), kbps * 1e3, kbps * 1e3 * 0.5, fs, cfo + (b - 1) * fs * 0.15, 0.4f);
                }
                addNoise(x, 0.01);
                RcLinkReceiver rx;
                rx.setProtocols(Sik);
                rx.setSikAirRate(kbps);
                const RcTelemetry t = run(rx, x, fs);
                int got = 0; for (const auto& f : t.frames) if (f.type == "GLOBAL_POSITION_INT") got++;
                CHECK(got >= 3, "SiK %d kbps fs %.1f cfo %.0f: %d frames of 3 (%llu bursts)", kbps, fs / 1e6, cfo, got, (unsigned long long)t.bursts);
                bool hasPos = false; for (const auto& d : t.devices) if (d.hasPos && std::fabs(d.lat - 54.6872) < 1e-5) hasPos = true;
                CHECK(hasPos, "SiK position on the device table");
            }
    // every SiK air speed from 16 kbit/s up (2, 4 and 8 kbit/s are too slow for a short test), automatic search and overlap with a second link on another hop channel
    for (int kbps : {16, 19, 24, 32, 48, 64, 96, 128, 192, 250}) {
        const double fs = 8e6;
        const double sec = 0.02 + 450.0 * 8 / (kbps * 1e3);
        std::vector<cf32> x((size_t)(sec * fs), cf32(0, 0));
        const auto air = frameBytes(sikPre, sikBuild(25, mav, 300, false));
        addGfsk(x, 20000, toBits(air), kbps * 1e3, kbps * 1e3 * 0.5, fs, -1.5e6, 0.4f);
        addGfsk(x, 21000, toBits(frameBytes(sikPre, sikBuild(9, mav, 200, false))), kbps * 1e3, kbps * 1e3 * 0.5, fs, 2.0e6, 0.4f);
        addNoise(x, 0.01);
        RcLinkReceiver rx; rx.setProtocols(Sik);
        const RcTelemetry t = run(rx, x, fs);
        CHECK(count(t, "GLOBAL_POSITION_INT") == 2, "SiK auto %d kbit/s, two links overlapping: %d of 2", kbps, count(t, "GLOBAL_POSITION_INT"));
    }
    // Golay frames and auto air rate
    {
        const double fs = 4e6;
        std::vector<cf32> x((size_t)(0.03 * fs), cf32(0, 0));
        addGfsk(x, 10000, toBits(frameBytes(sikPre, sikBuild(7, mav, 100, true))), 64e3, 32e3, fs, 3e5, 0.4f);
        addNoise(x, 0.01);
        RcLinkReceiver rx; rx.setProtocols(Sik);
        const RcTelemetry t = run(rx, x, fs);
        CHECK(count(t, "GLOBAL_POSITION_INT") == 1, "SiK ECC with automatic air rate: %d", count(t, "GLOBAL_POSITION_INT"));
    }
    // ---- noise sweep for one FSK protocol: frames must stop at low SNR without false frames
    for (double sigma : {0.002, 0.01, 0.04, 0.1, 0.2, 0.3}) {
        const double fs = 2.4e6;
        std::vector<cf32> x((size_t)(0.04 * fs), cf32(0, 0));
        const uint16_t c8[8] = {1500, 1600, 1700, 1800, 2000, 2200, 2400, 2600};
        const auto d8 = frskyD8Build(0x4321, 9, c8);
        std::vector<uint8_t> air = {0xAA, 0xAA, 0xAA, 0xAA, 0xD3, 0x91};
        air.insert(air.end(), d8.begin(), d8.end());
        addGfsk(x, 5000, toBits(air), 31044, 31700, fs, 2e5, 0.4f);
        addNoise(x, sigma);
        RcLinkReceiver rx; rx.setProtocols(Frsky);
        const RcTelemetry t = run(rx, x, fs);
        printf("  FrSky D8 sigma %.3f: %d frames\n", sigma, count(t, "D8 channels"));
        if (sigma <= 0.04) CHECK(count(t, "D8 channels") == 1, "D8 sigma %.3f: %d", sigma, count(t, "D8 channels"));
        for (const auto& f : t.frames) CHECK(f.id == "4321", "false FrSky frame %s", f.id.c_str());
    }
    // ---- FrSky D16, FlySky both layouts, overlapping bursts on different frequencies
    {
        const double fs = 8e6;
        std::vector<cf32> x((size_t)(0.02 * fs), cf32(0, 0));
        std::vector<uint8_t> dx = {0xAA, 0xAA, 0xAA, 0xAA, 0xD3, 0x91};
        auto xf = frskyXBuild(0xBEEF, 3, std::vector<uint8_t>(20, 0x11));
        dx.insert(dx.end(), xf.begin(), xf.end());
        addGfsk(x, 20000, toBits(dx), 70e3, 57e3, fs, -2.2e6, 0.4f);
        const uint16_t fc[8] = {1500, 1400, 1300, 1200, 1100, 1000, 1700, 1900};
        const auto fp = flyskyBuild(0x11223344, fc);
        std::vector<uint8_t> fa = {0xAA, 0xAA, 0x54, 0x75, 0xC5, 0x2A};
        fa.insert(fa.end(), fp.begin(), fp.end());
        addGfsk(x, 20500, toBits(fa), 500e3, 186e3, fs, 1.5e6, 0.4f);        // overlaps the D16 burst in time
        const uint16_t f2c[14] = {1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500};
        const auto f2p = afhds2aBuild(0xAABBCCDD, 0x01020304, f2c);
        std::vector<uint8_t> f2a = {0xAA, 0xAA, 0x54, 0x75, 0xC5, 0x2A};
        f2a.insert(f2a.end(), f2p.begin(), f2p.end());
        addGfsk(x, 100000, toBits(f2a), 500e3, 186e3, fs, -1e6, 0.4f);
        addNoise(x, 0.01);
        RcLinkReceiver rx; rx.setProtocols(Frsky | Flysky);
        const RcTelemetry t = run(rx, x, fs);
        CHECK(count(t, "D16 frame") == 1, "D16 %d", count(t, "D16 frame"));
        CHECK(count(t, "AFHDS channels") == 1, "AFHDS %d", count(t, "AFHDS channels"));
        CHECK(count(t, "AFHDS 2A channels") == 1, "AFHDS 2A %d", count(t, "AFHDS 2A channels"));
    }
    // ---- noise only: nothing decoded
    {
        const double fs = 8e6;
        std::vector<cf32> x((size_t)(0.1 * fs), cf32(0, 0));
        addNoise(x, 0.05, 11);
        RcLinkReceiver rx; rx.setProtocols(Sik | Frsky | Flysky);
        const RcTelemetry t = run(rx, x, fs);
        CHECK(t.frames.empty(), "%zu frames from noise", t.frames.size());
    }
    // ---- Reticulum on LoRa, several settings
    {
        uint8_t seed[32], x25519[32], rnd[10];
        for (int i = 0; i < 32; i++) { seed[i] = (uint8_t)(i * 5 + 2); x25519[i] = (uint8_t)(150 + i); }
        for (int i = 0; i < 10; i++) rnd[i] = (uint8_t)(i * 7);
        const auto ann = reticulumAnnounce(seed, x25519, "nomadnetwork.node", rnd, {}, 1);
        std::vector<uint8_t> lp = {0x10};
        lp.insert(lp.end(), ann.begin(), ann.end());
        for (auto [bw, sf] : std::vector<std::pair<double, int>>{{125000, 8}, {250000, 9}, {62500, 7}})
            for (double cfo : {0.0, 900.0}) {
                const double fs = 1e6;
                std::vector<cf32> x((size_t)(2.4 * fs), cf32(0, 0));
                lora::Params p;
                p.sf = sf; p.bwHz = bw; p.cr = 5; p.preamble = 18; p.syncWord = 0x12; p.crc = true; p.ldro = lora::autoLdro(sf, bw);
                addLora(x, fs, p, lp, 0.05, cfo, 0.3f);
                addNoise(x, 0.01);
                RcLinkReceiver rx; rx.setProtocols(Reticulum); rx.setLoraPlan(bw, sf);
                const RcTelemetry t = run(rx, x, fs);
                CHECK(count(t, "ANNOUNCE") == 1, "Reticulum SF%d BW%.0f cfo %.0f: %d announces (%llu preambles)", sf, bw, cfo, count(t, "ANNOUNCE"), (unsigned long long)t.loraPreambles);
            }
    }
    // ---- ExpressLRS on a 900 MHz hop channel, IQ inverted or not, phrase or no phrase
    {
        const auto uid = elrsUidFromPhrase("onair test");
        std::vector<uint8_t> sync(8, 0), rc(8, 0);
        sync[0] = 2; sync[1] = 5; sync[2] = 42; sync[3] = 3; sync[5] = uid[4]; sync[6] = uid[5];
        uint8_t m[7]; std::memcpy(m, sync.data(), 7);
        uint16_t c = elrsCrc(14, kElrsCrc14Poly, m, 7, elrsCrcInit(uid[4], uid[5]));
        sync[0] |= (uint8_t)((c >> 8) << 2); sync[7] = (uint8_t)c;
        rc[1] = 0xAB; rc[2] = 0xCD; rc[3] = 0x12; rc[4] = 0x34; rc[5] = 0x56; rc[6] = 0x80;
        std::memcpy(m, rc.data(), 7);
        c = elrsCrc(14, kElrsCrc14Poly, m, 7, (uint16_t)(elrsCrcInit(uid[4], uid[5]) ^ 9));
        rc[0] |= (uint8_t)((c >> 8) << 2); rc[7] = (uint8_t)c;
        for (bool inv : {false, true})
            for (int sf : {6, 7, 8})
                for (bool phrase : {false, true}) {
                    const double fs = 8e6;
                    std::vector<cf32> x((size_t)(0.25 * fs), cf32(0, 0));
                    lora::Params p;
                    p.sf = sf; p.bwHz = 500000; p.cr = 7; p.preamble = sf >= 8 ? 10 : 8; p.syncWord = 0x12; p.crc = false; p.implicitHdr = true; p.implicitLen = 8;
                    // EU868: centre 866.425 MHz; channel 2 and channel 5 of 13
                    const auto& dm = rcElrsDomains()[2];
                    const double step = (dm.stopHz - dm.startHz) / (dm.channels - 1), mid = 0.5 * (dm.startHz + dm.stopHz);
                    addLora(x, fs, p, sync, 0.02, dm.startHz + 6 * step - mid + 400, 0.3f, inv);
                    addLora(x, fs, p, rc, 0.12, dm.startHz + 7 * step - mid - 300, 0.3f, inv);
                    addNoise(x, 0.005);
                    RcLinkReceiver rx; rx.setProtocols(Elrs); rx.setElrsDomain(2);
                    if (phrase) rx.setBindingPhrase("onair test");
                    const RcTelemetry t = run(rx, x, fs);
                    CHECK(count(t, "SYNC") == 1, "ELRS SF%d inv %d: %d SYNC (%llu preambles, %llu bad)", sf, inv, count(t, "SYNC"), (unsigned long long)t.loraPreambles, (unsigned long long)t.blocksBad);
                    int rcSeen = count(t, "RC or link stats");
                    CHECK(rcSeen == 1, "ELRS SF%d inv %d phrase %d: %d RC frames", sf, inv, phrase, rcSeen);
                    for (const auto& f : t.frames) if (f.type == "RC or link stats") CHECK((f.detail.find("ch1") != std::string::npos) == phrase, "ELRS channels shown only with the phrase: %s", f.detail.c_str());
                }
    }
    return testSummary("rclink_rx");
}
