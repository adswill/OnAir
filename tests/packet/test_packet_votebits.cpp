// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// ax25::voteRepair at the bit level: copies of one frame from several demodulators, each wrong where the signal was doubtful.
// Measures how often the vote gives the frame back when no copy passes the check, and that it never gives a wrong one.
#include "dect2/packet/packet_ax25.h"
#include <cmath>
#include <cstdio>
#include <random>
using namespace dect2;
#include "../common/test_util.h"

static std::mt19937 rng(4242);

static std::vector<uint8_t> frameBits(size_t infoLen, std::vector<uint8_t>& truth) {
    ax25::Frame f;
    f.from.call = "N0TEST"; f.from.ssid = 7; f.to.call = "APRS";
    ax25::Address d; d.call = "WIDE1"; d.ssid = 1; f.path.push_back(d);
    f.info = "!5125.50N/00512.50E-";
    while (f.info.size() < infoLen) f.info.push_back((char)('A' + rng() % 26));
    const std::vector<uint8_t> wf = ax25::buildFrame(f);
    truth.assign(wf.begin(), wf.end() - 2);
    return ax25::hdlcBits(wf, 0, 0);
}

int main() {
    // copies that differ in k doubtful positions: each copy flips every doubtful bit with probability 1/2, independently
    for (int copies : {2, 3, 4, 6}) {
        for (int doubt : {2, 4, 6, 8, 12}) {
            int ok = 0, wrong = 0, any = 0, trials = 300;
            for (int t = 0; t < trials; t++) {
                std::vector<uint8_t> truth;
                const std::vector<uint8_t> bits = frameBits(20 + rng() % 40, truth);
                std::vector<size_t> pos;
                while ((int)pos.size() < doubt) { const size_t p = rng() % bits.size(); bool dup = false; for (size_t q : pos) dup = dup || q == p; if (!dup) pos.push_back(p); }
                std::vector<std::vector<uint8_t>> cp;
                bool anyGood = false;
                for (int c = 0; c < copies; c++) {
                    std::vector<uint8_t> b = bits;
                    for (size_t p : pos) if (rng() & 1) b[p] ^= 1;
                    std::vector<uint8_t> o;
                    if (ax25::frameFromStuffedBits(b, o)) anyGood = true;
                    cp.push_back(b);
                }
                if (anyGood) continue;     // a single copy already passes: not a case for the vote
                any++;
                ax25::VoteResult r;
                if (ax25::voteRepair(cp, ax25::plausibleFrame, r)) { if (r.frame == truth) ok++; else wrong++; }
            }
            printf("copies %d, doubtful bits %2d: %3d frames none of the copies passed, vote got %3d back (%.0f%%), wrong %d\n", copies, doubt, any, ok, any ? 100.0 * ok / any : 0.0, wrong);
            CHECK(wrong == 0, "no wrong frame (copies %d doubt %d)", copies, doubt);
            // a doubtful bit that every copy got wrong the same way cannot be told from a good one, so fewer copies get fewer back
            if (copies >= 6 && doubt >= 4 && doubt <= 8 && any >= 30) CHECK(ok * 10 >= any * 8, "6 copies, %d doubtful bits: %d of %d back", doubt, ok, any);
            if (copies >= 3 && copies <= 4 && doubt >= 4 && doubt <= 8 && any >= 30) CHECK(ok * 100 >= any * 33, "%d copies, %d doubtful bits: %d of %d back", copies, doubt, ok, any);
        }
    }
    // random wrong bits in each copy (not shared positions): the copies disagree everywhere they are wrong
    {
        int ok = 0, wrong = 0, n = 0;
        for (int t = 0; t < 400; t++) {
            std::vector<uint8_t> truth;
            const std::vector<uint8_t> bits = frameBits(30, truth);
            std::vector<std::vector<uint8_t>> cp;
            for (int c = 0; c < 5; c++) { std::vector<uint8_t> b = bits; for (int e = 0; e < 3; e++) b[rng() % b.size()] ^= 1; cp.push_back(b); }
            n++;
            ax25::VoteResult r;
            if (ax25::voteRepair(cp, ax25::plausibleFrame, r)) { if (r.frame == truth) ok++; else wrong++; }
        }
        printf("5 copies with 3 independent wrong bits each: %d of %d back, wrong %d\n", ok, n, wrong);
        CHECK(ok * 10 >= n * 9 && wrong == 0, "independent errors are outvoted: %d of %d", ok, n);
    }
    // different lengths never mix; one copy is no vote; a copy set with no solution gives nothing
    {
        std::vector<uint8_t> truth;
        const std::vector<uint8_t> a = frameBits(40, truth);
        std::vector<uint8_t> b = a; b.push_back(0);
        ax25::VoteResult r;
        CHECK(!ax25::voteRepair({a}, ax25::plausibleFrame, r), "one copy is not a vote");
        std::vector<uint8_t> a2 = a, b2 = b; a2[10] ^= 1; a2[40] ^= 1; a2[90] ^= 1; b2[11] ^= 1; b2[50] ^= 1; b2[95] ^= 1;
        CHECK(!ax25::voteRepair({a2, b2}, ax25::plausibleFrame, r), "copies of different lengths do not vote together");
        std::vector<uint8_t> x1 = a, x2 = a;
        for (int i = 0; i < 20; i++) { x1[(size_t)(i * 13 + 5) % x1.size()] ^= 1; x2[(size_t)(i * 17 + 9) % x2.size()] ^= 1; }
        CHECK(!ax25::voteRepair({x1, x2}, ax25::plausibleFrame, r), "too many contested bits: nothing");
    }
    return testSummary("packet_votebits");
}
