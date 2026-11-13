// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// SatDump tier 3 links: each generator through its receiver, with the impairments that matter. Everything here is simulated signals (docs/SATDUMP-TIER3-3.8.md).
#include "dect2/sd3/sd3_gen.h"
#include "dect2/sd3/sd3_proto.h"
#include "dect2/sd3/sd3_rx.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s: ", #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Res { Sd3Telemetry t; };

static Res run(int profile, int link, double rate, double seconds, double snr, double cfo, double ppm = 0, double drift = 0, double startOff = 0, bool invert = false) {
    Sd3GenConfig g;
    g.profile = profile; g.link = link; g.rate = rate; g.snrDb = snr; g.cfoHz = cfo; g.clockPpm = ppm; g.driftHzPerS = drift; g.invert = invert; g.phaseDeg = 33;
    Sd3Generator gen(g);
    Sd3Receiver rx(profile);
    rx.setLink(link);
    rx.setBlocking(true);
    if (getenv("SD3_LOG")) rx.setLogCallback([](const std::string& s) { printf("%s\n", s.c_str()); });
    rx.configure(rate);
    if (startOff != 0) rx.setSignalOffset(startOff);
    std::vector<cf32> buf(16384);
    const size_t total = (size_t)(rate * seconds);
    for (size_t at = 0; at < total;) { const size_t n = std::min(buf.size(), total - at); gen.generate(buf.data(), n); rx.feed(buf.data(), n); at += n; }
    if (getenv("SD3_LOG")) printf("fed %zu samples, flushing\n", total);
    rx.flush();
    Res r;
    rx.telemetry(r.t, ~0ull);
    return r;
}

static void show(const char* what, const Res& r) {
    printf("%-32s frames ok %6llu bad %5llu rs %5llu crc %llu pkts %6llu files %4llu lines %5llu snr %5.1f cfo %9.1f state %d %s\n", what, (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad, (unsigned long long)r.t.rsCorrected, (unsigned long long)r.t.crcBad, (unsigned long long)r.t.packets, (unsigned long long)r.t.files, (unsigned long long)r.t.lines, r.t.snrDb, r.t.cfoHz, r.t.state, r.t.lastText.empty() ? r.t.hint.c_str() : r.t.lastText.c_str());
    fflush(stdout);
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const std::string only = argc > 1 ? argv[1] : "";
    auto want = [&](const char* s) { return only.empty() || only == s; };
    if (want("gvar")) {
        auto r = run(kSd3Gvar, 0, 6e6, 2.5, 25, 3000);  show("GVAR 25 dB +3 kHz", r);
        CHECK(r.t.blocksOk >= 12, "gvar frames %llu", (unsigned long long)r.t.blocksOk);
        CHECK(r.t.image2 && r.t.image2->lines >= 1 && r.t.image, "gvar pictures");
    }
    if (want("cda")) {
        auto r = run(kSd3Cda, 0, 1e6, 3, 12, 2000); show("CDA 40k 12 dB", r); CHECK(r.t.blocksOk >= 30, "cda %llu", (unsigned long long)r.t.blocksOk);
        r = run(kSd3Cda, 2, 250000, 12, 15, 200); show("CDA GOES-N PM", r); CHECK(r.t.blocksOk >= 4, "cda-n %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("mdl")) {
        auto r = run(kSd3Mdl, 0, 3e6, 2, 18, -4000); show("MDL 18 dB -4 kHz", r); CHECK(r.t.blocksOk >= 40, "mdl %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("cdas")) {
        auto r = run(kSd3Cdas, 0, 8e6, 0.5, 18, 8000); show("CDAS 18 dB", r); CHECK(r.t.blocksOk >= 60, "cdas %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("geoscan")) {
        auto r = run(kSd3Geoscan, 0, 250000, 3, 22, 3000, 0, 0, 0); show("Geoscan 22 dB", r); CHECK(r.t.blocksOk >= 25, "geoscan %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("lucky7")) {
        auto r = run(kSd3Lucky7, 0, 250000, 3, 22, -2000); show("Lucky-7 22 dB", r); CHECK(r.t.blocksOk >= 25, "lucky7 %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("spino")) {
        auto r = run(kSd3Spino, 0, 250000, 3, 22, 1200); show("Spino 22 dB", r); CHECK(r.t.blocksOk >= 10, "spino %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("erminaz")) {
        auto r = run(kSd3Erminaz, 0, 250000, 3, 22, 1500); show("Erminaz 22 dB", r); CHECK(r.t.blocksOk >= 10, "erminaz %llu", (unsigned long long)r.t.blocksOk);
    }
    if (want("sband")) {
        int ok = 0, n = sd3LinkCount(kSd3Sband);
        for (int i = 0; i < n; i++) {
            const Sd3Link& l = sd3LinkAt(kSd3Sband, i);
            const double rate = std::max(4 * l.sym, 500000.0);
            const double frameSec = l.frameBits * (l.conv ? 2.0 : 1.0) / (l.mod == kSd3Qpsk || l.mod == kSd3Oqpsk ? 2.0 : 1.0) / l.sym;
            const double sec = std::min(2.0, std::max(0.3, 14 * frameSec)) + 0.05;   // at least 0.3 s: the fast links gave the loops about 0.1 s, too little on Linux for three of them
            auto r = run(kSd3Sband, i, rate, sec, l.conv ? 10 : 15, 0.002 * l.sym);
            char b[80]; snprintf(b, sizeof b, "%s", l.name);
            show(b, r);
            CHECK(r.t.blocksOk >= 3, "link %s frames %llu bad %llu", l.id, (unsigned long long)r.t.blocksOk, (unsigned long long)r.t.blocksBad);
            if (r.t.blocksOk >= 3) ok++;
        }
        printf("S-band links decoded: %d of %d\n", ok, n);
    }
    if (want("grb")) {
        auto r = run(kSd3Grb, 0, 20e6, 1.0, 14, 10000); show("GRB 14 dB", r);
        CHECK(r.t.packets >= 50, "grb packets %llu", (unsigned long long)r.t.packets);
        CHECK(!r.t.products.empty(), "grb products");
        CHECK(r.t.image && r.t.image->lines >= 3, "grb picture");
    }
    if (want("impair")) {
        struct Case { const char* name; int link; double es; };
        // one link of each kind: BPSK, QPSK with the convolutional code and NRZ-M, OQPSK with Reed-Solomon, PM, wide FSK
        const char* ids[] = {"formosat5_s_dump", "jason3_link", "swot_tlm", "hinode_s_dump", "falcon9_tlm"};
        const double snrFloor[] = {9, 7, 7, 9, 12};
        for (int k = 0; k < 5; k++) {
            int idx = 0;
            for (int i = 0; i < sd3LinkCount(kSd3Sband); i++) if (std::string(sd3LinkAt(kSd3Sband, i).id) == ids[k]) idx = i;
            const Sd3Link& l = sd3LinkAt(kSd3Sband, idx);
            const double rate = 4 * l.sym;
            const double frameSec = l.frameBits * (l.conv ? 2.0 : 1.0) / (l.mod == kSd3Qpsk || l.mod == kSd3Oqpsk ? 2.0 : 1.0) / l.sym;
            const double sec = std::min(1.5, 14 * frameSec) + 0.05;
            char b[100];
            auto rep = [&](const char* w, double snr, double cfo, double ppm, double drift, double off, bool inv) {
                auto r = run(kSd3Sband, idx, rate, sec, snr, cfo + off, ppm, drift, off, inv);
                snprintf(b, sizeof b, "%s: %s", l.id, w); show(b, r);
                CHECK(r.t.blocksOk >= 3, "%s frames %llu", b, (unsigned long long)r.t.blocksOk);
            };
            rep("offset +0.5% sym", snrFloor[k] + 8, 0.005 * l.sym, 0, 0, 0, false);
            rep("offset -1% sym", snrFloor[k] + 8, -0.01 * l.sym, 0, 0, 0, false);
            rep("clock +50 ppm", snrFloor[k] + 8, 0, 50, 0, 0, false);
            rep("Doppler 500 Hz/s", snrFloor[k] + 8, 0.002 * l.sym, 0, 500, 0, false);
            rep("off centre (0.15 Rs)", snrFloor[k] + 8, 0, 0, 0, 0.15 * l.sym, false);
            if (l.mod != kSd3Fsk && l.mod != kSd3Pm) rep("spectrum inverted", snrFloor[k] + 8, 0, 0, 0, 0, true);
            rep("near threshold", snrFloor[k], 0.001 * l.sym, 0, 0, 0, false);
        }
    }
    if (want("cadu")) {       // the CADU layers straight, with lost frames
        sd3::UhritDecoder u;
        int got = 0; std::string name;
        u.onFile = [&](const std::vector<uint8_t>& f, int, int) { got++; name = sd3::xritAnnotation(f); };
        for (int n = 0; n < 3; n++) {
            std::vector<uint8_t> file(10, 0);
            std::vector<uint8_t> ann = {'A', 'B', 'C'};
            const uint32_t hdr = 16 + 3 + (uint32_t)ann.size();
            file.insert(file.end(), {0, 0, 16, 0, (uint8_t)(hdr >> 24), (uint8_t)(hdr >> 16), (uint8_t)(hdr >> 8), (uint8_t)hdr, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, (uint8_t)(3 + ann.size())});
            file.insert(file.end(), ann.begin(), ann.end());
            file.resize(5000, (uint8_t)n);
            std::vector<uint8_t> r(6 + 8, 0);
            r[0] = 0x08; r[1] = 0x20; r[2] = 3 << 6;
            r.insert(r.end(), file.begin(), file.end());
            const size_t len = r.size() + 4 - 6 - 8 - 1;
            r[4] = (uint8_t)(len >> 8); r[5] = (uint8_t)len;
            const uint32_t c = sd3::crc32Hdlc(r.data(), r.size());
            for (int i = 24; i >= 0; i -= 8) r.push_back((uint8_t)(c >> i));
            std::vector<size_t> starts = {0};
            std::vector<uint8_t> cadu(sd3::kUhritCadu);
            size_t pos = 0;
            uint32_t cnt = (uint32_t)n * 10;
            while (pos < r.size()) { sd3::buildUhritCadu(1, 7, cnt++, r, pos, cadu.data(), starts); u.pushCadu(cadu.data()); }
        }
        CHECK(got == 3, "uhrit layer files %d", got);
        CHECK(name == "ABC", "uhrit name %s", name.c_str());
        std::vector<uint8_t> f(2048);
        for (int i = 0; i < 8; i++) { uint8_t pay[35] = {}; pay[1] = (uint8_t)(i * 3); uint8_t out[39]; sd3::lucky7Build(pay, out); CHECK(sd3::lucky7Frame(out), "lucky7 crc"); }
        std::vector<uint8_t> h(16, 1), body(20, 2);
        auto spino = sd3::spinoBuild(h, body);
        sd3::SpinoDeframer sp;
        int spinoGot = 0;
        sp.onFrame = [&](const std::vector<uint8_t>& v) { spinoGot += v.size() == spino.size() - 4 ? 1 : 0; };
        for (uint8_t b : spino) for (int i = 7; i >= 0; i--) sp.push((b >> i) & 1);
        CHECK(spinoGot == 1, "spino layer");
        printf("CADU layers ok\n");
    }
    if (want("uhrit") && (getenv("SD3_FULL") || only == "uhrit")) {
        auto r = run(kSd3Uhrit, 0, 30e6, 0.4, 16, 10000); show("UHRIT 14 dB", r);
        CHECK(r.t.files >= 2, "uhrit files %llu", (unsigned long long)r.t.files);
        CHECK(r.t.lastText.find("IMG_TEST") != std::string::npos, "uhrit name %s", r.t.lastText.c_str());
    }
    printf("%s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
