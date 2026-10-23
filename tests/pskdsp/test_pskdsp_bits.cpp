// PSK demodulator bit layer: Gray mapping, differential decoding (symbol DPSK, NRZ-M, NRZ-S), the G3RUH and CCSDS descramblers, the sync-word
// hunt that resolves the phase ambiguity (all rotations and spectral inversion), and the streaming decoder against the block one.
#include "pskdsp_testutil.h"
#include <set>

static Bits rnd(size_t n, uint32_t seed) {
    Bits b(n);
    uint32_t s = seed * 2654435761u + 12345;
    for (auto& v : b) { s = s * 1664525u + 1013904223u; v = (uint8_t)((s >> 24) & 1); }
    return b;
}

// frames recovered: the payloads that follow a sync word in the decoded bits
static int framesFound(const Bits& bits, const Bits& sync, const GenTruth& tr, int payloadBits) {
    std::set<std::vector<uint8_t>> truth(tr.payloads.begin(), tr.payloads.end());
    int ok = 0;
    for (size_t at : findSync(bits, sync, 1)) {
        if (at + (size_t)payloadBits > bits.size()) continue;
        if (truth.count(Bits(bits.begin() + (long)at, bits.begin() + (long)at + payloadBits))) ok++;
    }
    return ok;
}

int main() {
    // mapping and coding round trips
    for (Order o : {Order::Bpsk, Order::Qpsk, Order::Oqpsk, Order::Psk8}) {
        const int M = orderPoints(o), nb = orderBits(o);
        for (int k = 0; k < M; k++) {
            uint8_t b[3] = {};
            symbolBits(k, o, b);
            CHECK(bitsSymbol(b, o) == k, "%s symbol %d maps back", orderName(o), k);
        }
        if (o == Order::Psk8) {   // Gray: neighbours differ in one bit
            for (int k = 0; k < 8; k++) {
                uint8_t a[3], c[3]; symbolBits(k, o, a); symbolBits((k + 1) % 8, o, c);
                int d = 0; for (int i = 0; i < nb; i++) d += a[i] != c[i];
                CHECK(d == 1, "8PSK Gray neighbours %d", k);
            }
        }
    }
    const Bits r = rnd(2000, 3);
    for (Diff d : {Diff::NrzM, Diff::NrzS}) CHECK(diffDecode(diffEncode(r, d), d) == r, "%s round trip", diffName(d));
    CHECK(g3ruhDescramble(g3ruhScramble(r)) == r, "G3RUH round trip");
    {
        const Bits s = ccsdsSequence(64);
        const std::vector<uint8_t> bytes = packBits(s);
        CHECK(bytes.size() >= 5 && bytes[0] == 0xFF && bytes[1] == 0x48 && bytes[2] == 0x0E && bytes[3] == 0xC0 && bytes[4] == 0x9A, "CCSDS sequence starts FF 48 0E C0 9A");
    }
    CHECK(parseBitsOrHex("1ACFFC1D").size() == 32 && parseBitsOrHex("0x1ACFFC1D").size() == 32 && parseBitsOrHex("0101").size() == 4, "sync word text");
    {
        const Bits sync = parseBitsOrHex("1ACFFC1D");
        Bits stream = rnd(100, 1);
        stream.insert(stream.end(), sync.begin(), sync.end());
        const Bits tail = rnd(50, 2);
        stream.insert(stream.end(), tail.begin(), tail.end());
        stream[110] ^= 1;   // one bit error is allowed
        const auto at = findSync(stream, sync, 2);
        CHECK(at.size() == 1 && at[0] == 132, "sync found with one bit error (%zu hits)", at.size());
    }

    // through the signal: every order, differential coding, scrambler, with the carrier phase unknown, and with the spectrum inverted
    const Bits sync = parseBitsOrHex("1ACFFC1D");
    int n = 0;
    for (Order o : {Order::Bpsk, Order::Qpsk, Order::Oqpsk, Order::Psk8}) {
        for (Diff d : {Diff::None, Diff::Dpsk, Diff::NrzM, Diff::NrzS}) {
            for (Scrambler sc : {Scrambler::None, Scrambler::G3ruh, Scrambler::Ccsds}) {
                if (sc == Scrambler::G3ruh && d == Diff::Dpsk && o != Order::Bpsk) continue;   // the descrambler needs the bit stream the transmitter scrambled: kept to NRZ / BPSK
                if (o == Order::Oqpsk && d == Diff::Dpsk) continue;   // symbol differential coding needs the pairing of the rails, which the OQPSK read the other way round (one bit late) loses
                for (int variant = 0; variant < 3; variant++) {   // 0: phase 0.7, 1: phase 2.9, 2: conjugated (spectrum inverted)
                    GenSpec g;
                    g.order = o; g.fs = 8 * 2400; g.baud = 2400; g.esn0Db = 40; g.diff = d; g.scr = sc; g.sync = sync; g.payloadBits = 240; g.frames = 24;
                    g.phase0 = variant == 1 ? 2.9 : 0.7;
                    GenTruth tr;
                    std::vector<cf> x = generate(g, &tr);
                    if (variant == 2) for (auto& v : x) v = std::conj(v);
                    DemodConfig dc; dc.order = o; dc.sps = 8; dc.alpha = 0.35; dc.carrierBn = 0.02;
                    Demod dm; dm.configure(dc);
                    std::vector<cf> sym;
                    dm.process(x.data(), x.size(), sym);
                    BitOptions bo; bo.diff = d; bo.scr = sc; bo.sync = sync;
                    const BitResult br = symbolsToBits(sym, o, bo);
                    const int found = framesFound(br.bits, sync, tr, g.payloadBits);
                    // the first frames are lost to the loops' acquisition and the descrambler's start
                    CHECK(found >= (o == Order::Oqpsk ? 12 : 16), "%s %s %s variant %d: %d of %d frames (resolved %d, rot %d, conj %d)", orderName(o), diffName(d), scramblerName(sc), variant, found, g.frames,
                          (int)br.ambiguityResolved, br.rotation, (int)br.conj);
                    // the streaming decoder gives the same
                    BitDecoder dec;
                    dec.configure(o, bo);
                    Bits out;
                    for (size_t i = 0; i < sym.size(); i += 333) dec.push(sym.data() + i, std::min<size_t>(333, sym.size() - i), out);
                    dec.drain(out); dec.flush(out);
                    const int found2 = framesFound(out, sync, tr, g.payloadBits);
                    CHECK(found2 >= (o == Order::Oqpsk ? 12 : 16), "streaming %s %s %s variant %d: %d frames", orderName(o), diffName(d), scramblerName(sc), variant, found2);
                    n++;
                }
            }
        }
    }
    printf("test_pskdsp_bits: %d signals\n", n);
    return testSummary("test_pskdsp_bits");
}
