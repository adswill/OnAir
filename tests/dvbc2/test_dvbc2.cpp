// DVB-C2 (EN 302 769): the reference sequences, the Reed-Muller headers, the L1 part 2 fields and coding, the FEC blocks, and the whole
// receiver against the generator with a carrier offset, a sample clock error, noise and an echo, both guard intervals, Data Slice Types 1
// and 2 (robust and high efficiency headers), time interleaving, a notch, two PLPs, and the carrier index search without a known centre.
// Synthetic signals only: nothing here has met a cable network. ONAIR_TEST_LONG=1 adds the modulation / code rate sweep.
#include "dect2/dvbc2/dvbc2.h"
#include "dect2/dvbc2/dvbc2_gen.h"
#include "dect2/dvbc2/dvbc2_rx.h"
#include "dect2/dsp/gen_util.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>

using namespace dect2;
using namespace dect2::dvbc2;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// packet n of stream `tag`: PID 0x100 + tag, counter in bytes 4..7, bytes after it depend on n
static void testPacket(int tag, uint64_t n, uint8_t* p) {
    uint32_t st = (uint32_t)(n * 2654435761u) ^ (0x9e3779b9u + (uint32_t)tag);
    p[0] = 0x47; p[1] = (uint8_t)(0x01 + (tag >> 8)); p[2] = (uint8_t)tag; p[3] = (uint8_t)(0x10 | (n & 15));
    p[4] = (uint8_t)(n >> 24); p[5] = (uint8_t)(n >> 16); p[6] = (uint8_t)(n >> 8); p[7] = (uint8_t)n;
    for (int i = 8; i < 188; i++) { st = st * 1664525u + 1013904223u; p[i] = (uint8_t)(st >> 24); }
}
static std::function<void(uint8_t*)> source(int tag) {
    auto n = std::make_shared<uint64_t>(0);
    return [tag, n](uint8_t* p) { testPacket(tag, (*n)++, p); };
}

static void addNoise(std::vector<cf32>& c, double snrDb, std::mt19937& rng) {
    const float s = (float)std::sqrt(std::pow(10.0, -snrDb / 10) / 2);
    std::normal_distribution<float> nd(0.f, 1.f);
    for (auto& v : c) v += cf32(s * nd(rng), s * nd(rng));
}

// ---------------------------------------------------------------- clause 9.2: the sequences
static void testSequences() {
    // w_k: X^11 + X^2 + 1, all ones -> 1 x 11, 0, 0, ...; w'_i: X^10 + X^3 + 1 -> 1 x 10, 0, 0, ...
    for (int i = 0; i < 11; i++) CHECK(dataScramble(i) == 1, "w_%d", i);
    CHECK(dataScramble(11) == 0 && dataScramble(12) == 0, "w_11 w_12 = %d %d", dataScramble(11), dataScramble(12));
    for (int i = 0; i < 10; i++) CHECK(pilotPrbs(i) == 1, "w'_%d", i);
    CHECK(pilotPrbs(10) == 0 && pilotPrbs(11) == 0, "w'_10 w'_11 = %d %d", pilotPrbs(10), pilotPrbs(11));
    CHECK(dataScramble(2047) == 1 && dataScramble(2047 + 11) == 0, "w period 2047");
    CHECK(pilotPrbs(1023) == 1 && pilotPrbs(1023 + 10) == 0, "w' period 1023");
    int ones = 0;
    for (int i = 0; i < 2047; i++) ones += dataScramble(i);
    CHECK(ones == 1024, "maximal length PRBS balance: %d ones", ones);
    // r_k differential within an L1 block: r_k xor r_(k-6) = w_k xor w'_i
    ReferenceBlock rb;
    rb.set(45);
    const int64_t k0 = 45 * (int64_t)kL1Carriers;
    CHECK(rb.bit(k0) == (dataScramble(k0) ^ pilotPrbs(0)), "r at the block start");
    for (int j = 1; j < 568; j++) {
        const int64_t k = k0 + 6 * j;
        CHECK((rb.bit(k) ^ rb.bit(k - 6)) == (dataScramble(k) ^ pilotPrbs(j)), "r_k differential at j=%d", j);
    }
    CHECK(referenceBit(k0 + 600) == rb.bit(k0 + 600), "referenceBit agrees with the block");
    CHECK(isContinualPilot(96) && isContinualPilot(3366 + 3408 * 7) && !isContinualPilot(97), "continual pilots");
    for (int c : kContinualPilots) CHECK(c % 6 == 0, "continual pilot %d on the preamble pilot grid", c);
    CHECK(isScatteredPilot(0, 0, 1) && isScatteredPilot(12, 1, 1) && !isScatteredPilot(12, 0, 1) && isScatteredPilot(48 + 24, 2, 1), "scattered pilots GI 1/64");
    CHECK(isScatteredPilot(24, 1, 0) && isScatteredPilot(96, 0, 0) && !isScatteredPilot(24, 0, 0), "scattered pilots GI 1/128");
    // preamble cell index <-> carrier
    for (int q = 0; q < kL1DataCells; q++) { const int kk = preambleDataCarrier(q); CHECK(kk % 6 != 0 && kk < kL1Carriers && preambleCellIndex(kk) == q, "preamble cell %d <-> carrier %d", q, kk); }
    // the time interleaver write order: a permutation; depth 1 is the identity
    std::vector<std::pair<int, int>> o;
    tiWriteOrder(1, 50, o);
    for (int i = 0; i < 50; i++) CHECK(o[(size_t)i] == std::make_pair(0, i), "TI depth 1 identity");
    tiWriteOrder(4, 50, o);
    std::vector<int> seen(200, 0);
    for (auto& rc : o) seen[(size_t)(rc.first * 50 + rc.second)]++;
    for (int v : seen) CHECK(v == 1, "TI write order is a permutation");
    CHECK(o[1] == std::make_pair(1, 1) && o[50] == std::make_pair(1, 0), "TI twist: (R, C) of cell 1 = (%d, %d), of cell 50 = (%d, %d)", o[1].first, o[1].second, o[50].first, o[50].second);
}

// ---------------------------------------------------------------- clause 7.2.2: Reed-Muller (32,16) and the headers
static void testHeaders() {
    std::mt19937 rng(5);
    CHECK(rmEncode(0) == 0 && rmEncode(0x8000) == 0xFFFFFFFFu, "RM rows: b0 is the all ones row");
    // minimum distance 8: every pair of the first few hundred codewords
    int minD = 32;
    for (int a = 0; a < 300; a++) for (int b = a + 1; b < 300; b++) minD = std::min(minD, __builtin_popcount(rmEncode((uint16_t)a) ^ rmEncode((uint16_t)b)));
    CHECK(minD == 8, "RM(32,16) minimum distance %d", minD);
    for (int t = 0; t < 200; t++) {
        const uint16_t info = (uint16_t)rng();
        uint32_t cw = rmEncode(info);
        // up to 3 hard errors
        const int ne = t % 4;
        for (int e = 0; e < ne; e++) cw ^= 1u << (rng() % 32);
        float llr[32];
        for (int i = 0; i < 32; i++) llr[i] = ((cw >> i) & 1) ? -1.f : 1.f;
        float m = 0;
        const uint16_t got = rmDecode(llr, &m);
        CHECK(got == info, "RM decode with %d errors: %04x -> %04x", ne, info, got);
    }
    // headers through noise, robust and high efficiency; the correlation pre-test
    for (int hem = 0; hem < 2; hem++) {
        int bad = 0;
        for (int t = 0; t < 60; t++) {
            FecFrameHeader h;
            h.plpId = (int)(rng() % 256); h.fecType = t & 1; h.mod = 1 + t % 3; h.cod = 1 + t % 5; h.counter = (t >> 1) & 1;
            std::vector<cf32> cells;
            encodeHeader(h.pack(), hem != 0, cells);
            CHECK((int)cells.size() == headerCells(hem != 0), "header cells");
            const double snr = hem ? 14 : 8;
            addNoise(cells, snr, rng);
            std::vector<float> n0(cells.size(), (float)(std::pow(10.0, -snr / 10) / 2));
            float metric = 0;
            const uint16_t v = decodeHeader(cells.data(), n0.data(), hem != 0, &metric);
            if (v != h.pack()) bad++;
            CHECK(headerCorrelation(cells.data(), hem != 0) > 0.5f, "header correlation %.2f (hem %d)", headerCorrelation(cells.data(), hem != 0), hem);
        }
        CHECK(bad <= 1, "%s headers through noise: %d of 60 wrong", hem ? "16QAM" : "QPSK", bad);
        // random QAM cells are not headers
        std::vector<cf32> r(32);
        int high = 0;
        for (int t = 0; t < 100; t++) {
            for (auto& v : r) v = qamPoint(hem ? 1 : 0, false, rng() % (hem ? 16 : 4));
            if (headerCorrelation(r.data(), hem != 0) > 0.45f) high++;
        }
        CHECK(high <= 3, "random cells look like headers %d of 100 times (hem %d)", high, hem);
    }
    PreambleHeader ph; ph.l1InfoSize = 1234; ph.tiMode = 2;
    CHECK(PreambleHeader::unpack(ph.pack()).l1InfoSize == 1234 && PreambleHeader::unpack(ph.pack()).tiMode == 2, "preamble header fields");
    FecFrameHeader fh; fh.plpId = 200; fh.fecType = 0; fh.mod = 3; fh.cod = 5; fh.counter = 1;
    const FecFrameHeader u = FecFrameHeader::unpack(fh.pack());
    CHECK(u.plpId == 200 && u.fecType == 0 && u.mod == 3 && u.cod == 5 && u.counter == 1, "FECFrame header fields (%04x)", fh.pack());
    CHECK(xfecFrameCells(1, 3) == 8100 && xfecFrameCells(0, 1) == 4050 && xfecFrameCells(1, 0) == 900 && xfecFrameCells(0, 5) == 1350, "table 16(b)");
}

// ---------------------------------------------------------------- clause 8: L1 part 2
static L1Part2 sampleL1(int gi) {
    L1Part2 l1;
    l1.networkId = 0x1234; l1.systemId = 7; l1.startFrequency = 154000; l1.bandwidth = gi == 1 ? 284 : 142; l1.guardInterval = gi;
    l1.changeCounter = 3;
    for (int s = 0; s < 2; s++) {
        L1DataSlice d;
        d.id = 10 + s; d.tunePos = 142 + s * 20; d.offsetLeft = -30 - s; d.offsetRight = 25 + s; d.tiDepth = s; d.type = s; d.fecHeaderType = s; d.constConf = s; d.leftNotch = 0;
        for (int p = 0; p < 2 + s; p++) {
            L1Plp lp;
            lp.id = 100 + p; lp.type = p == 0 ? 0 : 2; lp.payloadType = 3; lp.groupId = 5; lp.start = 1234 + p; lp.fecType = p & 1; lp.mod = 1 + p; lp.cod = 2 + p;
            lp.psiSiReprocessing = p & 1; lp.tsId = 0x4321 + p; lp.onId = 0x8888;
            d.plps.push_back(lp);
        }
        l1.slices.push_back(d);
    }
    L1Notch n; n.start = 90; n.width = 2;
    l1.notches.push_back(n);
    l1.reservedTone = 0; l1.ews = 1; l1.version = 1;
    return l1;
}
static bool sameL1(const L1Part2& a, const L1Part2& b) {
    std::vector<uint8_t> x, y;
    packL1Part2(a, x); packL1Part2(b, y);
    return x == y;
}
static void testL1() {
    std::mt19937 rng(9);
    for (int gi = 0; gi < 2; gi++) {
        L1Part2 l1 = sampleL1(gi);
        std::vector<uint8_t> bits;
        packL1Part2(l1, bits);
        // field widths: the loops of table 18
        const int posBits = gi == 0 ? 13 : 14, offBits = gi == 0 ? 8 : 9;
        int expect = 16 + 16 + 24 + 16 + 2 + 10 + 8 + 8 + 4;
        for (const auto& s : l1.slices) {
            expect += 8 + posBits + 2 * offBits + 2 + 1 + (s.type ? 1 : 0) + 1 + 1 + 8;
            for (const auto& p : s.plps) expect += 8 + 1 + 2 + 5 + ((p.type == 0 || p.type == 1) ? 8 : 0) + (s.type == 0 ? 14 + 1 + 3 + 3 : 0) + 1 + (p.psiSiReprocessing ? 0 : 32) + 8;
            expect += 8;
        }
        expect += (int)l1.notches.size() * (posBits + offBits + 8) + 1 + 1 + 4 + 11;
        CHECK((int)bits.size() == expect, "L1 part 2 size GI %d: %zu bits, expected %d", gi, bits.size(), expect);
        L1Part2 u;
        CHECK(unpackL1Part2(bits, u) && sameL1(l1, u), "L1 part 2 unpack GI %d", gi);
        CHECK(u.slices[0].offsetLeft == -30 && u.slices[1].offsetRight == 26 && u.kMax() == u.kMin() + (int64_t)u.bandwidth * dx(gi), "signed offsets and the band");
        CHECK(u.sliceKMin(u.slices[0]) == 154000 + (142 - 30) * dx(gi) && u.sliceKMax(u.slices[0]) == 154000 + (142 + 25) * dx(gi) - 1, "slice carriers");
        CHECK(u.notchKMin(u.notches[0]) == 154000 + 90 * dx(gi) + 1 && u.notchKMax(u.notches[0]) == 154000 + 92 * dx(gi) - 1, "notch carriers");
    }
    // coding: one block, two blocks (a long L1), the time interleaving modes, through noise
    for (int tiMode = 0; tiMode < 4; tiMode++) {
        L1Part2 l1 = sampleL1(1);
        if (tiMode & 1) {   // make it long: many PLPs -> two FEC blocks
            for (int p = 0; p < 90; p++) { L1Plp lp; lp.id = 50 + p; lp.psiSiReprocessing = 0; lp.tsId = p; l1.slices[1].plps.push_back(lp); }
        }
        L1Coded coded;
        CHECK(encodeL1(l1, tiMode, coded), "encodeL1 TI mode %d", tiMode);
        std::vector<uint8_t> bits;
        packL1Part2(l1, bits);
        const int kExPad = (int)(bits.size() + (bits.size() & 1)) + 32;
        CHECK(coded.header.l1InfoSize == (kExPad - 32) / 2, "L1_INFO_SIZE");
        const int fb = (kExPad + kMaxL1InfoPerBlock - 1) / kMaxL1InfoPerBlock;
        CHECK(coded.fecBlocks == fb && coded.fecBlocks == ((tiMode & 1) ? 2 : 1), "FEC blocks %d (TI mode %d)", coded.fecBlocks, tiMode);
        CHECK(coded.preambleSymbols == (tiMode == 2 ? 4 : tiMode == 3 ? 8 : fb), "preamble symbols %d (TI mode %d)", coded.preambleSymbols, tiMode);
        CHECK(coded.ksig * coded.fecBlocks >= kExPad && coded.ksig <= kMaxL1InfoPerBlock, "Ksig %d", coded.ksig);
        const int width = coded.fecBlocks * coded.cellsPerBlock / coded.preambleSymbols;
        CHECK(kPreambleHeaderCells + width <= kL1DataCells, "L1 fits one preamble block: %d cells", width);
        for (auto& r : coded.rows) CHECK((int)r.size() == width, "row width %zu vs %d", r.size(), width);
        const double snr = 9.0;
        std::vector<std::vector<cf32>> rows = coded.rows;
        std::vector<std::vector<float>> n0;
        for (auto& r : rows) { addNoise(r, snr, rng); n0.push_back(std::vector<float>(r.size(), (float)(std::pow(10.0, -snr / 10) / 2))); }
        L1Part2 out;
        L1DecodeResult d = decodeL1(coded.header, rows, n0, out);
        CHECK(d.ok && sameL1(l1, out), "decodeL1 TI mode %d at %.0f dB (blocks ok %d)", tiMode, snr, d.blocksOk);
        // a corrupted CRC is refused
        rows[0][5] = cf32(3, 3); rows[0][6] = cf32(-3, 3);
        for (auto& r : rows) for (size_t i = 0; i < r.size(); i += 3) r[i] = -r[i];
        L1Part2 bad;
        CHECK(!decodeL1(coded.header, rows, n0, bad).ok, "a damaged L1 is refused");
    }
}

// ---------------------------------------------------------------- clause 6: FEC blocks
static void testFec() {
    std::mt19937 rng(3);
    struct Case { int fecType, mod, cod; double snr; } const cases[] = {
        // Es/N0 about 2.5 dB above the AWGN thresholds of the DVB-C2 implementation guidelines (TS 102 991 table 23)
        {1, 2, 1, 16.0}, {1, 3, 2, 21.5}, {1, 1, 3, 13.5}, {1, 3, 4, 24.0}, {1, 3, 5, 27.0}, {1, 1, 5, 16.0},
        {0, 2, 1, 16.5}, {0, 3, 2, 22.0}, {0, 1, 3, 14.0}, {0, 2, 3, 19.0}, {0, 3, 4, 24.5}, {0, 2, 5, 22.5}};
    for (const Case& c : cases) {
        Fec f; f.fecType = c.fecType; f.mod = c.mod; f.cod = c.cod;
        const Dims d = dims(f);
        CHECK(d.ok && modCodValid(f), "dims %s %s", modName(f.mod), codName(f.fecType, f.cod));
        CHECK(d.cells == xfecFrameCells(f.fecType, f.mod), "cells %d vs table 16(b) %d", d.cells, xfecFrameCells(f.fecType, f.mod));
        std::vector<uint8_t> bb((size_t)d.kBch);
        for (auto& b : bb) b = rng() & 1;
        std::vector<cf32> cells;
        encodeBlock(f, bb, cells);
        CHECK((int)cells.size() == d.cells, "encoded cells");
        double p = 0;
        for (auto& v : cells) p += std::norm(v);
        CHECK(std::fabs(p / cells.size() - 1) < 0.05, "unit cell power %.3f", p / cells.size());
        addNoise(cells, c.snr, rng);
        std::vector<float> n0(cells.size(), (float)(std::pow(10.0, -c.snr / 10) / 2));
        std::vector<uint8_t> out;
        BlockResult r = decodeBlock(f, cells.data(), n0.data(), out);
        CHECK(r.ok && out == bb, "%s %s %s block at %.1f dB: ok %d iters %d preBER %.4f", c.fecType ? "64K" : "16K", modName(f.mod), codName(f.fecType, f.cod), c.snr, r.ok, r.ldpcIters, r.preBer);
    }
    CHECK(!modCodValid(Fec{1, 1, 1}) && !modCodValid(Fec{1, 3, 1}) && modCodValid(Fec{1, 2, 1}), "table 11(a)");
    // the bit map is a permutation
    Fec f; f.fecType = 1; f.mod = 3; f.cod = 5;
    const auto& m = bitMap(f);
    std::vector<int> seen(64800, 0);
    for (int v : m) seen[(size_t)v]++;
    for (int v : seen) if (v != 1) { CHECK(false, "bit map is not a permutation"); break; }
}

// ---------------------------------------------------------------- the receiver against the generator
struct RxCheck {
    std::map<int, uint64_t> next;   // tag -> next expected counter
    uint64_t packets = 0, bad = 0, gaps = 0;
    void feed(const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; i++, p += 188) {
            const int tag = (p[1] - 1) << 8 | p[2];
            const uint64_t ctr = (uint64_t)p[4] << 24 | (uint64_t)p[5] << 16 | (uint64_t)p[6] << 8 | p[7];
            uint8_t ref[188];
            testPacket(tag, ctr, ref);
            packets++;
            if (memcmp(ref, p, 188) != 0) { bad++; continue; }
            auto it = next.find(tag);
            if (it != next.end() && it->second != ctr) gaps++;
            next[tag] = ctr + 1;
        }
    }
};

struct RoundTrip {
    const char* name;
    int gi, type, hem, tiDepth, mod, cod, fecType, plps, notch;
    double snr, cfoHz, ppm, echoDb, rate;
    bool knownCentre;
    double seconds;
};

static bool roundTrip(const RoundTrip& rt) {
    const int64_t tune = (int64_t)std::llround(346e6 / kCarrierSpacingHz);
    GenConfig c = defaultSystem(rt.gi, tune);
    SliceConfig& s = c.slices[0];
    s.type = rt.type; s.fecHeaderType = rt.hem; s.tiDepth = rt.tiDepth;
    s.plps[0].mod = rt.mod; s.plps[0].cod = rt.cod; s.plps[0].fecType = rt.fecType; s.plps[0].ts = source(1);
    for (int p = 1; p < rt.plps; p++) { PlpConfig pc = s.plps[0]; pc.id = 1 + p; pc.tsId = 1 + p; pc.ts = source(1 + p); s.plps.push_back(pc); }
    if (rt.notch) { NotchConfig n; n.start = s.tunePos / 2; n.width = rt.gi == 1 ? 3 : 1; c.notches.push_back(n); }
    c.sampleRate = rt.rate; c.snrDb = rt.snr; c.cfoHz = rt.cfoHz; c.clockPpm = rt.ppm;
    if (rt.echoDb > 0) { c.echoDb = rt.echoDb; c.echoUs = 1.8; c.echoDeg = 40; }
    c.seed = 11;
    Generator gen(c);
    const double centre = c.tuneCarrier * kCarrierSpacingHz;
    Dvbc2Receiver rx;
    RxCheck chk;
    std::vector<std::string> logs;
    rx.setLogCallback([&](const std::string& m) { logs.push_back(m); });
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { chk.feed(p, n); });
    rx.setCenterHz(rt.knownCentre ? centre : 0.0);
    rx.configure(rt.rate);
    rx.setBlocking(true);
    if (rt.plps > 1) rx.selectPlp(-1, rt.plps);   // the last PLP
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<cf32> buf(65536);
    double fed = 0;
    while (fed < rt.seconds) { gen.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); fed += buf.size() / rt.rate; }
    rx.flush();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Dvbc2Telemetry t;
    rx.telemetry(t, 0);
    const double expectPackets = gen.netBitrate(0, (size_t)rt.plps - 1) * rt.seconds / (188 * 8);
    printf("  %-34s state %d L1 %llu/%llu blocks %llu/%llu packets %llu (bad %llu gaps %llu, about %.0f sent) Es/N0 %.1f dB MER %.1f dB CFO %+.0f Hz clock %+.1f ppm  %.0f ms\n",
           rt.name, t.state, (unsigned long long)t.l1Good, (unsigned long long)t.l1Bad, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
           (unsigned long long)chk.packets, (unsigned long long)chk.bad, (unsigned long long)chk.gaps, expectPackets, t.snrDb, t.merDb, t.cfoHz, t.clockPpm, ms);
    bool ok = true;
    if (!(t.l1Ok && t.state == 4)) { ok = false; printf("    no L1 / not decoding: stage '%s'\n", t.stage.c_str()); for (auto& l : logs) printf("    log: %s\n", l.c_str()); }
    if (chk.bad != 0 || chk.gaps != 0) ok = false;
    if (chk.packets < expectPackets * 0.35) { ok = false; printf("    too few packets\n"); }
    // the whole offset is known only with the centre frequency; without it the receiver reports the part within a carrier spacing
    const double cfoWant = rt.knownCentre ? rt.cfoHz : rt.cfoHz - std::round(rt.cfoHz / kCarrierSpacingHz) * kCarrierSpacingHz;
    if (std::fabs(t.cfoHz - cfoWant) > 400) { ok = false; printf("    carrier offset estimate %.0f vs %.0f\n", t.cfoHz, cfoWant); }
    if (std::fabs(t.clockPpm + rt.ppm) > 8) { ok = false; printf("    clock estimate %.1f vs %.1f ppm\n", t.clockPpm, -rt.ppm); }   // the receiver's clock against the transmitter's
    const int64_t wantWindow = c.tuneCarrier - (int64_t)std::llround(rt.cfoHz / kCarrierSpacingHz);
    if (t.windowCarrier != wantWindow) { ok = false; printf("    window carrier %lld vs %lld\n", (long long)t.windowCarrier, (long long)wantWindow); }
    CHECK(ok, "round trip %s", rt.name);
    return ok;
}

static void testRoundTrips(bool longRun) {
    const double r = 10e6, native = kSampleRate;
    const RoundTrip cases[] = {
        // name                               gi type hem ti mod cod fec plps notch  snr    cfo     ppm  echo  rate   known  seconds
        {"GI 1/64 Type 1 64-QAM 2/3",          1, 0, 0, 0, 2, 1, 1, 1, 0,  18.0,  18300,  25,  0,   r,      true,  0.55},
        {"GI 1/128 Type 2 256-QAM 5/6 TI 4",   0, 1, 0, 1, 3, 4, 1, 2, 0,  31.0,  -7000, -20,  15,  r,      true,  0.55},
        {"GI 1/64 Type 2 HEM 16K 16-QAM 4/5",  1, 1, 1, 3, 1, 3, 0, 1, 1,  16.0,   3100,  12,  0,   native, false, 0.70},
    };
    for (const RoundTrip& c : cases) roundTrip(c);
    if (!longRun) return;
    const RoundTrip more[] = {
        {"64K 256-QAM 9/10 TI 8",              1, 0, 0, 2, 3, 5, 1, 1, 0,  30.0,  -40000, 40, 20,  r,      true,  0.8},
        {"64K 256-QAM 3/4 Type 2 notch",       0, 1, 0, 0, 3, 2, 1, 1, 1,  22.0,  12000,  -5,  0,   r,      true,  0.8},
        {"16K 64-QAM 8/9 Type 2 HEM TI 16",    1, 1, 1, 3, 2, 5, 0, 2, 0,  26.0,  -2000,   0,  12,  r,      true,  0.8},
        {"64K 64-QAM 9/10 GI 1/128",           0, 0, 0, 1, 2, 5, 1, 1, 0,  24.0,  25000, -20,  0,   r,      true,  0.8},
        {"16K 256-QAM 5/6 12 Msps",            1, 0, 0, 0, 3, 4, 0, 1, 0,  23.0,   5000,  30,  0,   12e6,   true,  0.8},
    };
    for (const RoundTrip& c : more) roundTrip(c);
}

int main(int argc, char** argv) {
    const bool longRun = getenv("ONAIR_TEST_LONG") != nullptr || (argc > 1 && strcmp(argv[1], "--long") == 0);
    testSequences();
    testHeaders();
    testL1();
    testFec();
    printf("sequences, headers, L1, FEC: %d failures so far\n", g_fail);
    testRoundTrips(longRun);
    printf("%s (%d failures)\n", g_fail ? "FAILED" : "OK", g_fail);
    return g_fail ? 1 : 0;
}
