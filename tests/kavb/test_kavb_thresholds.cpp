// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// BLER-versus-Es/N0 measurement of the whole KAVB chain (transmitter -> AWGN channel with random carrier offset, fractional delay and phase -> receiver with
// sync, V-sweep removal, pilot equaliser and LDPC) for the profiles. Only runs with KRIS_MEASURE=1 in the environment (slow); without it the test passes at once.
// For every configuration it sweeps Es/N0 (data power over noise in the symbol bandwidth; the chirp is extra, -10 dB) in 0.5 dB steps and prints
//   KAVB_THRESH cfg=A_16QAM_3/4_chirp_removed esn0_bler1e-2=12.00
// the first Es/N0 where at most 1% of the blocks fail (blocks of frames that were not found count as failed), and with KRIS_MEASURE_VERBOSE=1 every point:
//   KAVB_POINT cfg=... esn0=12.00 frames=200 bler=0.0037 frameloss=0.000 undetected=0
// KRIS_MEASURE_FRAMES=n sets the frames per point (default 200). Each frame is sent alone with 300 noise samples in front.
#include "kavb_test_util.h"
#include <cstdlib>
#include <string>
using namespace dect2::kavb;
using namespace kavbtest;

struct Cfg {
    const char* name;
    int profile;
    bool robust;
    bool chirp;
    bool remove;
};

struct Point { double bler, frameLoss; int undetected; };

static Point run(const Cfg& c, double esn0, int frames, Rng& rng) {
    const Profile p = profileOf(c.profile, c.robust);
    Transmitter tx(c.profile, c.robust, c.chirp);
    Receiver::Options o;
    o.removeChirp = c.remove;
    int blocksBad = 0, lost = 0, undetected = 0;
    for (int f = 0; f < frames; f++) {
        const auto pay = randomBytes(rng, (size_t)framePayloadBytes(p));
        Channel ch;
        ch.snrDb = esn0;
        ch.cfoCycPerSym = (rng.uniform() - 0.5) * 0.02;
        ch.delaySamples = rng.uniform();
        ch.phase = rng.uniform() * 6.283185307179586;
        const int sweep = (int)rng.below((uint64_t)p.sweepFrames);
        auto iq = applyChannel(tx.modulate({pay}, 0, 0, f, sweep), ch, rng, 300);
        Receiver rx(o);
        auto out = rx.push(iq.data(), iq.size());
        bool found = false;
        for (const auto& fr : out) {
            if (fr.hdr.seq != (f & 0xFFFF)) continue;
            found = true;
            for (int b = 0; b < kBlocksPerFrame; b++) {
                const bool ok = fr.blocks[b].ok && std::equal(fr.blocks[b].bytes.begin(), fr.blocks[b].bytes.end(), pay.begin() + b * p.blockPayloadBytes);
                if (!ok) blocksBad++;
                if (fr.blocks[b].ok && !ok) undetected++;
            }
            break;
        }
        if (!found) { blocksBad += kBlocksPerFrame; lost++; }
    }
    return {double(blocksBad) / (frames * kBlocksPerFrame), double(lost) / frames, undetected};
}

// Reference without any synchronisation: the coded, interleaved frame bits mapped to symbols, complex Gaussian noise, max-log demapper, the same decoder. The
// difference to the full chain is the implementation loss (timing, carrier and phase estimates, matched filter, chirp).
static Point runRef(const Cfg& c, double esn0, int frames, Rng& rng) {
    const Profile p = profileOf(c.profile, c.robust);
    int bad = 0;
    const double n0 = std::pow(10.0, -esn0 / 10.0), sd = std::sqrt(n0 / 2.0);
    const int kInfo = dect2::kris::ldpcK(kBlockN, p.rate);
    for (int f = 0; f < frames; f++) {
        const auto pay = randomBytes(rng, (size_t)framePayloadBytes(p));
        const auto bits = frameCodedBits(p, pay.data());
        auto sym = dect2::kris::qamMap(bits.data(), bits.size(), p.mod);
        for (auto& s : sym) s += cf((float)(sd * rng.gauss()), (float)(sd * rng.gauss()));
        std::vector<float> llrTx(sym.size() * p.bitsPerSym), llr(kFrameCodedBits);
        dect2::kris::qamDemapSoft(sym.data(), sym.size(), p.mod, (float)n0, llrTx.data());
        deinterleaveDescrambleLlr(llrTx.data(), llr.data());
        std::vector<uint8_t> info(kInfo);
        for (int b = 0; b < kBlocksPerFrame; b++) {
            const int it = dect2::kris::ldpcDecode(kBlockN, p.rate, llr.data() + b * kBlockN, info.data(), 50);
            std::vector<uint8_t> bytes(p.blockPayloadBytes + 2, 0);
            for (int i = 0; i < kInfo; i++) bytes[i / 8] |= (uint8_t)((info[i] & 1) << (7 - i % 8));
            const bool ok = it >= 0 && std::equal(bytes.begin(), bytes.begin() + p.blockPayloadBytes, pay.begin() + b * p.blockPayloadBytes);
            if (!ok) bad++;
        }
    }
    return {double(bad) / (frames * kBlocksPerFrame), 0.0, 0};
}

int main() {
    const char* e = std::getenv("KRIS_MEASURE");
    if (!e || std::string(e) != "1") { printf("test_kavb_thresholds: skipped (set KRIS_MEASURE=1 to measure)\n"); return 0; }
    const bool verbose = std::getenv("KRIS_MEASURE_VERBOSE") && std::string(std::getenv("KRIS_MEASURE_VERBOSE")) == "1";
    int frames = 200;
    if (const char* fr = std::getenv("KRIS_MEASURE_FRAMES")) frames = std::max(20, std::atoi(fr));
    const char* only = std::getenv("KRIS_MEASURE_ONLY");   // substring of a configuration name
    const Cfg cfgs[] = {
        {"A_16QAM_3/4_chirp_removed", 0, false, true, true},
        {"A_16QAM_3/4_no_chirp", 0, false, false, true},
        {"A_16QAM_3/4_chirp_not_removed", 0, false, true, false},
        {"B_16QAM_3/4_chirp_removed", 1, false, true, true},
        {"C_16QAM_3/4_chirp_removed", 2, false, true, true},
        {"D_64QAM_3/4_chirp_removed", 3, false, true, true},
        {"D_64QAM_3/4_no_chirp", 3, false, false, true},
        {"A_QPSK_1/2_robust_chirp_removed", 0, true, true, true},
        {"A_QPSK_1/2_robust_no_chirp", 0, true, false, true},
    };
    Rng rng(20261019);
    // ideal-sync reference lines: cfg name with the suffix _reference_no_sync
    const Cfg refs[] = {
        {"A_16QAM_3/4_reference_no_sync", 0, false, false, false},
        {"D_64QAM_3/4_reference_no_sync", 3, false, false, false},
        {"A_QPSK_1/2_robust_reference_no_sync", 0, true, false, false},
    };
    for (const auto& c : refs) {
        if (only && std::string(c.name).find(only) == std::string::npos) continue;
        const Profile p = profileOf(c.profile, c.robust);
        const double eta = (c.robust ? 0.5 : 0.75) * p.bitsPerSym;
        const double est = 10.0 * std::log10(std::pow(2.0, eta) - 1.0) + 1.5;
        double found = 1e9;
        for (double s = est - 3.0; s <= est + 7.0 + 1e-9; s += 0.5) {
            const Point pt = runRef(c, s, frames, rng);
            if (verbose) printf("KAVB_POINT cfg=%s esn0=%.2f frames=%d bler=%.4f frameloss=0.000 undetected=0\n", c.name, s, frames, pt.bler);
            if (pt.bler <= 0.01) { found = s; break; }
        }
        if (found < 1e8) printf("KAVB_THRESH cfg=%s esn0_bler1e-2=%.2f\n", c.name, found);
        else printf("KAVB_THRESH cfg=%s esn0_bler1e-2=none\n", c.name);
        fflush(stdout);
    }
    for (const auto& c : cfgs) {
        if (only && std::string(c.name).find(only) == std::string::npos) continue;
        const Profile p = profileOf(c.profile, c.robust);
        const double eta = (c.robust ? 0.5 : 0.75) * p.bitsPerSym;
        const double est = 10.0 * std::log10(std::pow(2.0, eta) - 1.0) + 1.5;
        double found = 1e9;
        for (double s = est - 1.5; s <= est + 7.0 + 1e-9; s += 0.5) {
            const Point pt = run(c, s, frames, rng);
            if (verbose) printf("KAVB_POINT cfg=%s esn0=%.2f frames=%d bler=%.4f frameloss=%.3f undetected=%d\n", c.name, s, frames, pt.bler, pt.frameLoss, pt.undetected);
            if (pt.bler <= 0.01) { found = s; break; }
        }
        if (found < 1e8) printf("KAVB_THRESH cfg=%s esn0_bler1e-2=%.2f\n", c.name, found);
        else printf("KAVB_THRESH cfg=%s esn0_bler1e-2=none\n", c.name);
        fflush(stdout);
    }
    return 0;
}
