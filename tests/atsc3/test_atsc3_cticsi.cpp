// ATSC 3.0, convolutional time interleaver with channel state (4.1, docs/SENSITIVITY-4.1-TV.md). FEC blocks (QPSK 8/15, 16K LDPC) go through
// the transmitter's CTI (512 rows), then a fading channel in transmit order (Rayleigh gain held for 400 cells, about a third of an 8K symbol
// of data cells, then a new one) with white noise; the receiver gets the equalised cells y / h and, on the 4.1 path, each cell's weight |h|^2.
// Every frame goes to Atsc3Receiver::commitFrame, which de-interleaves (runCti) and decodes. The same seeds are run with and without the
// weights (the 4.0 path: one noise figure for the block). Pass: with the weights at least as many blocks decode at every SNR, clearly more
// at the SNR where the 4.0 path fails, and a clean signal decodes every block after the de-interleaver's start-up.
// Simulated signals only.
#include "dect2/atsc3/atsc3_bb.h"
#include "dect2/atsc3/atsc3_bicm.h"
#include "dect2/atsc3/atsc3_receiver.h"
#include "dect2/atsc3/atsc3_ti.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace dect2;
using namespace dect2::atsc3;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

struct Result { long bad = 0, blocks = 0; };

// frames of 4 FEC blocks; the first frames only fill the de-interleaver
static Result run(double snrDb, bool weights, int frames, uint32_t seed) {
    FramePlp cfg; cfg.id = 1; cfg.fecType = 0; cfg.mod = 0; cfg.cod = 6;
    const Bicm bicm(plpBicm(cfg));
    if (!bicm.ok()) { printf("bicm config not supported\n"); fails++; return {}; }
    const int cb = bicm.cells(), perFrame = 4, F = perFrame * cb, rows = ctiRows(0, false);
    CtiInterleaver ti(rows, 2, bicm.constellation(), 0);
    Atsc3Receiver rx;
    std::mt19937 rng(seed);
    std::normal_distribution<float> g(0.f, 1.f);
    const float sigma2 = (float)std::pow(10.0, -snrDb / 10.0);
    const int bytes = bicm.kPayload() / 8;
    cf32 h(1, 0);
    long cellIdx = 0;
    for (int f = 0; f < frames; f++) {
        std::vector<cf32> tx;
        for (int b = 0; b < perFrame; b++) {
            auto bits = bytesToBits(makeBbPacket(bytes, {}, -1, -1));
            bbScramble(bits);
            auto c = bicm.encode(bits);
            tx.insert(tx.end(), c.begin(), c.end());
        }
        const int startRow = (int)(((long)f * F) % rows);
        const std::vector<cf32> il = ti.process(tx);
        PlpResult pr;
        pr.id = 1; pr.cti = true; pr.cfg = cfg; pr.ctiRows = rows; pr.ctiStartRow = startRow;
        pr.ctiFecBlockStart = rows * startRow;   // C = 0: every frame starts with a whole FEC block
        pr.noise = sigma2;
        pr.cells.resize(il.size());
        if (weights) pr.cellW.resize(il.size());
        for (size_t i = 0; i < il.size(); i++, cellIdx++) {
            if (cellIdx % 400 == 0) h = cf32(g(rng), g(rng)) * (float)std::sqrt(0.5);
            const cf32 n = cf32(g(rng), g(rng)) * (float)std::sqrt(sigma2 / 2);
            const float p = std::max(std::norm(h), 1e-6f);
            pr.cells[i] = (h * il[i] + n) * std::conj(h) / p;
            if (weights) pr.cellW[i] = p;
        }
        FrameResult fr;
        fr.ok = true;
        fr.plps.push_back(pr);
        rx.commitFrame(fr, Bootstrap());
    }
    Result r;
    r.bad = rx.stats().bbBad;
    r.blocks = (long)frames * perFrame;
    return r;
}

int main() {
    const int frames = 24;   // 96 blocks; the start-up of 512 rows (261632 cells) takes about the first 8 frames
    // clean: everything after the start-up decodes on both paths
    const Result c1 = run(40, true, frames, 1), c0 = run(40, false, frames, 1);
    printf("clean: weights %ld of %ld blocks bad, without %ld\n", c1.bad, c1.blocks, c0.bad);
    CHECK(c1.bad <= 40 && c1.bad == c0.bad, "clean signal: only the start-up blocks fail, the same on both paths");
    const long startup = c1.bad;
    double gainAt = 0;
    long bestDiff = 0;
    for (double snr = 11; snr >= 4; snr -= 1) {
        const Result a = run(snr, true, frames, 7), b = run(snr, false, frames, 7);
        const long okB = b.blocks - b.bad;
        printf("%4.1f dB Rayleigh (gain held 400 cells): blocks decoded after the start-up: with weights %ld of %ld, without %ld\n", snr,
               a.blocks - a.bad, a.blocks - startup, okB);
        fflush(stdout);
        CHECK(a.bad <= b.bad, "weights never decode fewer blocks");
        if (b.bad - a.bad > bestDiff) { bestDiff = b.bad - a.bad; gainAt = snr; }
    }
    printf("largest gain: %ld blocks at %.1f dB\n", bestDiff, gainAt);
    CHECK(bestDiff >= 8, "the weights decode clearly more blocks somewhere in the sweep");
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails ? 1 : 0;
}
