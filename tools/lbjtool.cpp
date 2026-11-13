// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// LBJ analysis tool: runs the LBJ modem + POCSAG layer on an IQ recording and prints every burst and page.
//   lbjtool file.{cs8,cu8,cf32} sampleRateHz signalOffsetHz [-v]
// cs8 = interleaved signed 8-bit I/Q (HackRF), cu8 = unsigned 8-bit (RTL-SDR), cf32 = float32 I/Q. The offset is where the
// 821.2375 MHz channel sits relative to the recording's centre frequency (Hz).
#include "dect2/lbj/lbj_modem.h"
#include "dect2/lbj/lbj_proto.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
using namespace dect2;

int main(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "usage: lbjtool file.{cs8,cu8,cf32} rate offsetHz [-v]\n"); return 2; }
    const std::string path = argv[1];
    const double rate = atof(argv[2]), off = atof(argv[3]);
    const bool verbose = argc > 4 && !strcmp(argv[4], "-v");
    const std::string ext = path.substr(path.find_last_of('.') + 1);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { perror("open"); return 1; }
    lbj::Modem m;
    m.configure(rate);
    m.setOffset(off);
    lbj::LbjSessionState st;
    std::vector<lbj::Burst> bursts;
    std::vector<cf32> x(32768);
    int nBurst = 0, nPage = 0, nLbj = 0, nMsg = 0;
    for (;;) {
        size_t n = 0;
        if (ext == "cf32") n = fread(x.data(), sizeof(cf32), x.size(), f);
        else {
            std::vector<uint8_t> raw(2 * x.size());
            const size_t got = fread(raw.data(), 2, x.size(), f);
            for (size_t i = 0; i < got; i++) {
                if (ext == "cu8") x[i] = cf32((raw[2*i] - 127.5f) / 127.5f, (raw[2*i+1] - 127.5f) / 127.5f);
                else x[i] = cf32((int8_t)raw[2*i] / 128.f, (int8_t)raw[2*i+1] / 128.f);
            }
            n = got;
        }
        if (!n) break;
        bursts.clear();
        m.feed(x.data(), n, bursts);
        for (const lbj::Burst& b : bursts) {
            nBurst++;
            const auto pages = lbj::pagesFromBurst(b);
            if (verbose) printf("%.2fs burst sync_err=%d level=%.1f dB cfo=%+.0f Hz pages=%zu\n", b.timeSec, b.syncErrors, b.levelDb, b.cfoHz, pages.size());
            for (const auto& p : pages) {
                nPage++;
                const bool isL = lbj::isLbjAddress(p.ric);
                nLbj += isL;
                if (verbose || isL) printf("%.2fs ric=%u fn=%d fix=%d miss=%d '%s'%s\n", b.timeSec, p.ric, p.function, p.fixed, p.missing, p.digits.c_str(), isL ? "  <LBJ>" : "");
                LbjMessage out;
                if (lbj::parsePage(p, b.timeSec, st, out)) {
                    nMsg++;
                    printf("   -> train %s speed %s km %s dir %s loco %s route %s\n", out.train.c_str(), out.speedKmh.c_str(), out.kmMarker.c_str(), out.direction.c_str(), out.loco.c_str(), out.route.c_str());
                }
            }
        }
    }
    printf("bursts %d pages %d lbj-addressed %d messages %d syncs %llu\n", nBurst, nPage, nLbj, nMsg, (unsigned long long)m.syncs());
    fclose(f);
}
