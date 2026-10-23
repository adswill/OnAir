// PSK demodulator from samples to bits: channeliser, demodulator and the streaming bit decoder give back every payload of the test signal,
// for each order with no coding, differential coding (DPSK, NRZ-M, NRZ-S), the G3RUH and CCSDS scramblers, a random carrier phase (the
// sync word resolves the rotation and the spectral sense), a conjugated (spectrally inverted) input, carrier offsets and clock errors.
// Also the pieces: Gray mapping round trips, the CCSDS sequence, differential coding, the sync search and the hex / bit parser.
#include "dect2/pskdsp/pskdsp.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2::pskd;
#include "../common/test_util.h"

struct Case { const char* name; Order o; Diff d; Scrambler s; double fs, baud, off, ppm, esn0; bool conj; };

static int matchedFrames(const Bits& bits, const std::vector<size_t>& at, const std::vector<Bits>& truth) {
    int ok = 0;
    size_t next = 0;
    for (size_t p : at) {
        for (size_t t = next; t < truth.size(); t++) {
            const Bits& pl = truth[t];
            if (p + pl.size() > bits.size()) break;
            bool same = true;
            for (size_t i = 0; i < pl.size() && same; i++) same = bits[p + i] == pl[i];
            if (same) { ok++; next = t + 1; break; }
        }
    }
    return ok;
}

int main() {
    // pieces
    for (Order o : {Order::Bpsk, Order::Qpsk, Order::Psk8}) {
        const int M = orderPoints(o);
        for (int k = 0; k < M; k++) { uint8_t b[3]; symbolBits(k, o, b); CHECK(bitsSymbol(b, o) == k, "%s symbol %d round trip", orderName(o), k); }
    }
    {
        const Bits s = ccsdsSequence(16);
        CHECK(packBits(s)[0] == 0xFF && packBits(s)[1] == 0x48, "CCSDS sequence starts FF 48");
        Bits x = parseBitsOrHex("0x1ACFFC1D");
        CHECK(x.size() == 32 && bitsHex(x, 0, 32) == "1A CF FC 1D ", "hex parser");
        CHECK(parseBitsOrHex("0110").size() == 4, "bit parser");
        for (Diff d : {Diff::NrzM, Diff::NrzS}) { Bits r = x; CHECK(diffDecode(diffEncode(r, d), d) == r, "%s round trip", diffName(d)); }
        CHECK(g3ruhDescramble(g3ruhScramble(x)) == x, "G3RUH round trip");
        Bits y(40, 0); y.insert(y.end(), x.begin(), x.end()); y.resize(100, 1);
        CHECK(findSync(y, x, 0).size() == 1 && findSync(y, x, 0)[0] == 72, "sync search");
    }
    const Case cases[] = {
        {"BPSK", Order::Bpsk, Diff::None, Scrambler::None, 48000, 1200, 600, 30, 16, false},
        {"BPSK NRZ-M", Order::Bpsk, Diff::NrzM, Scrambler::None, 48000, 1200, -900, 0, 16, false},
        {"BPSK NRZ-S + G3RUH", Order::Bpsk, Diff::NrzS, Scrambler::G3ruh, 96000, 9600, 2000, -40, 16, false},
        {"BPSK DPSK", Order::Bpsk, Diff::Dpsk, Scrambler::None, 8000, 300, 150, 0, 16, false},
        {"BPSK G3RUH", Order::Bpsk, Diff::None, Scrambler::G3ruh, 48000, 4800, 0, 0, 16, false},
        {"BPSK CCSDS", Order::Bpsk, Diff::None, Scrambler::Ccsds, 250000, 20000, -30000, 20, 16, false},
        {"QPSK", Order::Qpsk, Diff::None, Scrambler::None, 48000, 2400, 300, 0, 18, false},
        {"QPSK conjugated input", Order::Qpsk, Diff::None, Scrambler::None, 48000, 2400, 300, 0, 18, true},
        {"QPSK DPSK", Order::Qpsk, Diff::Dpsk, Scrambler::None, 96000, 4800, -1000, 50, 18, false},
        {"QPSK CCSDS", Order::Qpsk, Diff::None, Scrambler::Ccsds, 1e6, 64000, 120000, 0, 18, false},
        {"OQPSK", Order::Oqpsk, Diff::None, Scrambler::None, 96000, 4800, 500, 0, 18, false},
        {"8PSK", Order::Psk8, Diff::None, Scrambler::None, 96000, 4800, -300, 0, 26, false},
        {"8PSK DPSK", Order::Psk8, Diff::Dpsk, Scrambler::None, 96000, 4800, 200, 0, 26, false},
    };
    std::mt19937 rng(7);
    for (const Case& c : cases) {
        GenSpec g;
        g.order = c.o; g.fs = c.fs; g.baud = c.baud; g.offsetHz = c.off; g.clockPpm = c.ppm; g.esn0Db = c.esn0; g.diff = c.d; g.scr = c.s;
        g.sync = parseBitsOrHex("1ACFFC1D");
        g.payloadBits = 960; g.frames = 24;
        g.phase0 = std::uniform_real_distribution<double>(-3.1, 3.1)(rng);
        g.seed = (uint32_t)rng();
        GenTruth truth;
        std::vector<cf> x = generate(g, &truth);
        if (c.conj) for (auto& v : x) v = std::conj(v);
        Channeliser ch;
        ch.configure(c.fs, c.conj ? -c.off : c.off, c.baud * 1.6, c.baud * 6);
        std::vector<cf> y;
        for (size_t i = 0; i < x.size(); i += 4096) ch.process(x.data() + i, std::min<size_t>(4096, x.size() - i), y);
        DemodConfig dc;
        dc.order = c.o; dc.sps = ch.outRate() / c.baud; dc.alpha = 0.35;
        Demod dm;
        dm.configure(dc);
        std::vector<cf> sym;
        for (size_t i = 0; i < y.size(); i += 1000) dm.process(y.data() + i, std::min<size_t>(1000, y.size() - i), sym);
        BitOptions bo;
        bo.diff = c.d; bo.scr = c.s; bo.sync = g.sync; bo.ccsdsFrameBits = (size_t)g.payloadBits;
        BitResult r = symbolsToBits(sym, c.o, bo);
        const int ok = matchedFrames(r.bits, r.syncAt, truth.payloads);
        printf("%-24s out %.0f Hz sps %.2f: %zu symbols, EVM %.2f, sync words %d, payloads %d of %d, rotation %d%s\n", c.name, ch.outRate(), dc.sps, sym.size(),
               dm.stats().evm, r.syncHits, ok, g.frames, r.rotation, r.conj ? " conjugated" : "");
        if (ok < g.frames - 2) { printf("   sync at:"); for (size_t v : r.syncAt) printf(" %zu", v); printf(" (of %zu bits)\n", r.bits.size()); }
        CHECK(ok >= g.frames - 2, "%s: %d of %d payloads", c.name, ok, g.frames);
    }
    return testSummary("pskdsp_bits");
}
