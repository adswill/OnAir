// sens_gk2a_gap: how far the GK-2A receivers are from an ideal one, in dB of Es/N0 (the symbol energy over the noise density; the noise
// bandwidth is the symbol rate), by the share of frames that pass Reed-Solomon. docs/GK2A.md.
//
//   sens_gk2a_gap [--link 0|1|both] [--frames N] [--lo dB] [--hi dB] [--step dB] [--threads N]
//
// "ideal": the same code chain (CADU -> K=7 r=1/2 encoder with G2 inverted -> BPSK, or QPSK with G1 on I and G2 on Q) with perfect timing,
// phase, frequency and frame alignment, 8 bit soft decisions scaled like the receiver does, the same Viterbi decoder and Reed-Solomon
// decoder. It is the loss of everything else (matched filter, timing, carrier, sync search) that the receiver adds on top of it.
// "receiver": the whole test signal (goes_gen / gk2a_gen) at a real sample rate, with a carrier offset, through Gk2aReceiver; frames
// counted are those Reed-Solomon passed, out of those sent after the first two seconds / ten frames of acquisition.
#include "dect2/gk2a/gk2a_gen.h"
#include "dect2/gk2a/gk2a_rx.h"
#include "dect2/hrpt/hrpt_proto.h"
#include "dect2/meteor/meteor_proto.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace dect2;

static double idealRate(bool qpsk, double esn0Db, int frames, uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> g(0, 1);
    const double esc = std::pow(10.0, (esn0Db - (qpsk ? 3.0103 : 0.0)) / 10.0);   // per code symbol
    const double sigma = std::sqrt(1.0 / (2 * esc));
    meteor::ConvEncoder enc;
    hrpt::Viterbi vit;
    std::vector<uint8_t> bits;
    std::vector<float> soft;
    uint8_t cadu[meteor::kCaduBytes];
    for (int f = 0; f < frames; f++) {
        uint8_t v[meteor::kVcduBytes];
        for (auto& b : v) b = (uint8_t)rng();
        meteor::encodeCadu(v, cadu, true);
        for (int i = 0; i < meteor::kCaduBytes; i++)
            for (int b = 7; b >= 0; b--) {
                uint8_t s0, s1;
                enc.push((cadu[i] >> b) & 1, s0, s1);
                s1 ^= 1;                                    // CCSDS: G2 inverted
                soft.push_back((float)((s0 ? -1.0 : 1.0) + sigma * g(rng)));
                soft.push_back((float)((s1 ? -1.0 : 1.0) + sigma * g(rng)));
            }
    }
    double mean = 0;
    for (float x : soft) mean += std::fabs(x);
    mean /= (double)soft.size();
    const float sc = (float)(40.0 / std::max(0.05, mean));
    for (size_t i = 0; i + 1 < soft.size(); i += 2) {
        const int a = (int)std::max(-127.f, std::min(127.f, -soft[i] * sc)), b = (int)std::max(-127.f, std::min(127.f, -soft[i + 1] * sc));
        // the receiver's convention: soft > 0 is code bit 1 (-re), G2 undone by negating the second
        vit.push(a, -b);
        for (uint8_t bit : vit.out()) bits.push_back(bit);
        vit.out().clear();
    }
    vit.flush();
    for (uint8_t bit : vit.out()) bits.push_back(bit);
    // frames: every ASM (<= 3 bit errors) followed by 1020 bytes
    int ok = 0, tried = 0;
    uint32_t reg = 0;
    size_t i = 0;
    while (i < bits.size()) {
        reg = reg << 1 | bits[i++];
        if (i < 32) continue;
        if (__builtin_popcount(reg ^ meteor::kAsm) > 3) continue;
        if (i + (size_t)meteor::kCodedBytes * 8 > bits.size()) break;
        uint8_t frame[meteor::kCodedBytes] = {};
        for (int k = 0; k < meteor::kCodedBytes * 8; k++) if (bits[i + (size_t)k]) frame[k >> 3] |= (uint8_t)(0x80 >> (k & 7));
        uint8_t v[meteor::kVcduBytes];
        tried++;
        if (meteor::decodeCadu(frame, v, true) >= 0) ok++;
        i += (size_t)meteor::kCodedBytes * 8;
        reg = 0;
    }
    (void)tried;
    return (double)ok / std::max(1, frames - 2);
}

static double receiverRate(int link, double esn0Db, double seconds, double rate, double cfo, uint32_t seed) {
    GoesGenConfig c;
    c.rate = rate; c.symbolRate = gk2aSymbolRate(link); c.qpsk = link == 1; c.snrDb = esn0Db; c.cfoHz = cfo; c.seed = seed;
    c.fillFrames = -1;                      // fill VCDUs only: constant traffic, no file bursts
    c.files = gk2aTestFiles(false);
    GoesGenerator gen(c);
    Gk2aReceiver rx(link);
    rx.configure(rate);
    std::vector<cf32> buf(65536);
    Gk2aTelemetry t;
    uint64_t sentAtLock = 0, okAtLock = 0, sentAtReport = 0, okAtReport = 0, seq = 0;
    bool locked = false;
    for (double done = 0; done < seconds * rate; done += (double)buf.size()) {
        gen.generate(buf.data(), buf.size());
        rx.feed(buf.data(), buf.size());
        if (rx.telemetry(t, seq)) {       // a new report: frames sent so far against frames the receiver had passed at that moment
            seq = t.seq;
            if (!locked && t.blocksOk > 0) { locked = true; sentAtLock = gen.framesSent(); okAtLock = t.blocksOk; }
            sentAtReport = gen.framesSent(); okAtReport = t.blocksOk;
        }
    }
    if (!locked) return 0;
    // frames sent after the one that gave the first good frame (the receiver delay of a few frames is inside the counts)
    const double expect = std::max(1.0, (double)sentAtReport - (double)sentAtLock - 6.0);
    return std::min(1.0, (double)(okAtReport - okAtLock) / expect);
}

int main(int argc, char** argv) {
    int link = 2, frames = 600, threads = 3;
    double lo = -2, hi = 4, step = 0.25;
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string a = argv[i];
        if (a == "--link") link = std::string(argv[i + 1]) == "both" ? 2 : atoi(argv[i + 1]);
        else if (a == "--frames") frames = atoi(argv[i + 1]);
        else if (a == "--lo") lo = atof(argv[i + 1]);
        else if (a == "--hi") hi = atof(argv[i + 1]);
        else if (a == "--step") step = atof(argv[i + 1]);
        else if (a == "--threads") threads = atoi(argv[i + 1]);
    }
    for (int l = 0; l < 2; l++) {
        if (link != 2 && link != l) continue;
        const bool q = l == 1;
        const double rate = q ? 10e6 : 2.4e6, secs = q ? (double)frames / 366.0 + 0.1 : (double)frames * 0.128 + 1.0;
        const double off = q ? 120e3 : 20e3;
        std::vector<double> snr;
        for (double s = lo; s <= hi + 1e-9; s += step) snr.push_back(s);
        std::vector<double> ideal(snr.size()), rcv(snr.size());
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (;;) {
                const size_t k = next++;
                if (k >= snr.size()) return;
                ideal[k] = idealRate(q, snr[k], frames, 100 + (uint32_t)k);
                rcv[k] = receiverRate(l, snr[k], secs, rate, off, 200 + (uint32_t)k);
            }
        };
        std::vector<std::thread> th;
        for (int t = 0; t < threads; t++) th.emplace_back(work);
        for (auto& t : th) t.join();
        printf("\n%s (%s, %.0f ksym/s, receiver at %.1f Msps, carrier offset %.0f kHz), %d frames per point\n", gk2aLinkName(l), q ? "QPSK" : "BPSK", gk2aSymbolRate(l) / 1e3, rate / 1e6, off / 1e3, frames);
        printf("  Es/N0 dB   ideal frames ok   receiver frames ok\n");
        for (size_t k = 0; k < snr.size(); k++) printf("  %7.2f   %10.1f %%   %12.1f %%\n", snr[k], ideal[k] * 100, rcv[k] * 100);
        auto cross = [&](const std::vector<double>& r, double lvl) {
            for (size_t k = 0; k + 1 < snr.size(); k++)
                if (r[k] < lvl && r[k + 1] >= lvl) return snr[k] + (lvl - r[k]) / std::max(1e-9, r[k + 1] - r[k]) * (snr[k + 1] - snr[k]);
            return r[0] >= lvl ? snr[0] : NAN;
        };
        for (double lvl : {0.5, 0.9, 0.99}) {
            const double i = cross(ideal, lvl), r = cross(rcv, lvl);
            printf("  %2.0f %% of frames: ideal %.2f dB, receiver %.2f dB, gap %.2f dB\n", lvl * 100, i, r, r - i);
        }
    }
    return 0;
}
