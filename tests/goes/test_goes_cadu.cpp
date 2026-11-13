// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GOES HRIT .cadu files: the receiver writes every good frame as a 1024 byte CADU (sync word, derandomised bytes, the kind of file SatDump
// writes), and decodes such a file again through Reed-Solomon and the upper layers without any signal. Both Reed-Solomon bases, frames
// derandomised or as sent, and a file with damaged frames.
#include "dect2/goes/goes_gen.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/goes/goes_tel.h"
#include "dect2/meteor/meteor_proto.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static std::vector<uint8_t> record(bool dual, std::vector<std::vector<uint8_t>>& sentFiles) {
    GoesGenConfig g;
    g.rate = 2.4e6; g.snrDb = 12; g.cfoHz = 10e3; g.fillFrames = 12; g.seed = 3; g.dualBasis = dual;
    GoesGenerator gen(g);
    GoesReceiver rx;
    rx.configure(2.4e6);
    std::vector<uint8_t> cadu;
    rx.setCaduCallback([&](const uint8_t* f) { cadu.insert(cadu.end(), f, f + meteor::kCaduBytes); });
    rx.setFileCallback([&](const std::vector<uint8_t>& f, int, int) { sentFiles.push_back(f); });
    std::vector<cf32> buf(65536);
    for (double done = 0; done < 2.0 * 2.4e6; done += (double)buf.size()) { gen.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    return cadu;
}

static void replay(const char* name, const std::vector<uint8_t>& cadu, const std::vector<std::vector<uint8_t>>& want, size_t minFiles, bool damaged = false) {
    GoesReceiver rx;
    rx.configure(2.4e6);
    std::vector<std::vector<uint8_t>> files;
    rx.setFileCallback([&](const std::vector<uint8_t>& f, int, int) { files.push_back(f); });
    rx.importCadu(cadu);
    cf32 z(0, 0);
    rx.feed(&z, 1);
        size_t exact = 0;
    for (const auto& f : files) for (const auto& w : want) if (f == w) { exact++; break; }
    printf("  %s: %zu files, %zu exact\n", name, files.size(), exact);
    CHECK(files.size() >= minFiles && (damaged ? exact >= 3 : exact == files.size()), "%s: %zu files, %zu exact", name, files.size(), exact);
}

int main() {
    for (int dual = 1; dual >= 0; dual--) {
        std::vector<std::vector<uint8_t>> sent;
        const std::vector<uint8_t> cadu = record(dual, sent);
        CHECK(cadu.size() >= 50 * meteor::kCaduBytes, "dual %d: %zu bytes recorded", dual, cadu.size());
        CHECK(cadu[0] == 0x1A && cadu[1] == 0xCF && cadu[2] == 0xFC && cadu[3] == 0x1D, "sync word");
        replay(dual ? "dual basis, derandomised file" : "conventional basis, derandomised file", cadu, sent, 5);
        // as sent: randomised again
        std::vector<uint8_t> rnd = cadu;
        for (size_t o = 0; o + meteor::kCaduBytes <= rnd.size(); o += meteor::kCaduBytes) meteor::pnXor(rnd.data() + o + 4, meteor::kCodedBytes);
        replay(dual ? "dual basis, randomised file" : "conventional basis, randomised file", rnd, sent, 5);
        // damage: one frame in three has 60 bytes of one codeword flipped beyond what Reed-Solomon corrects
        std::vector<uint8_t> dmg = cadu;
        for (size_t o = 0, k = 0; o + meteor::kCaduBytes <= dmg.size(); o += meteor::kCaduBytes, k++)
            if (k % 3 == 1) for (int i = 0; i < 60; i++) dmg[o + 4 + (size_t)i * 4] ^= 0xA5;
        replay("damaged frames (a file with a lost packet is delivered flagged, the others byte exact)", dmg, sent, 0, true);
    }
    printf(fails ? "goes cadu: %d FAILED\n" : "goes cadu: all passed\n", fails);
    return fails ? 1 : 0;
}
