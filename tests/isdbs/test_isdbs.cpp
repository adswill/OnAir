// ISDB-S: the numbers of ITU-R BO.1408-1 / ARIB TR-B15, the TMCC bits and their RS(64,48) code, the frame coding loop, and the whole
// receiver against the generator (clean, with noise, with carrier and symbol clock offsets, hierarchical, a TMCC change, data on the
// bursts). Synthetic signals only: nothing here has met a real BS transponder.
#include "dect2/isdbs/isdbs.h"
#include "dect2/isdbs/isdbs_rx.h"
#include "../../core/src/isdbs/isdbs_fec.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>

using namespace dect2;
using namespace dect2::isdbs;

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
static StreamCfg stream(int mod, int slots, uint16_t tsId, int tag) { StreamCfg s; s.mod = mod; s.slots = slots; s.tsId = tsId; if (tag >= 0) s.src = source(tag); return s; }

// numbers that the documents state and the code must reproduce
static void testNumbers() {
    // BO.1408 Table 4, row 34.5 MHz / 28.9 MBd: TS rates with one modulation in all slots
    struct R { int mod; double mbit; } const rows[] = {{kBpsk12, 13.0}, {kQpsk12, 26.1}, {kQpsk23, 34.8}, {kQpsk34, 39.1}, {kQpsk56, 43.5}, {kQpsk78, 45.6}, {kTc8psk23, 52.2}};
    for (const R& r : rows) {
        const double v = slotTsBitrate(r.mod) * kSlots / 1e6;
        CHECK(std::fabs(v - r.mbit) < 0.06, "%s in 48 slots: %.2f Mbit/s, Table 4 says %.1f", modName(r.mod), v, r.mbit);
        // a unit of slots fills its air time exactly: steps -> coded bits -> symbols
        const int u = slotUnit(r.mod), steps = groupSteps(r.mod, u);
        int syms = 0;
        if (r.mod == kTc8psk23) syms = steps;
        else if (r.mod == kBpsk12) syms = steps * 2;
        else { static const int k[] = {0, 0, 1, 2, 3, 5, 7}, n[] = {0, 0, 2, 3, 4, 6, 8}; syms = steps / k[r.mod] * n[r.mod] / 2; }
        CHECK(syms == u * kSlotSyms, "%s: a unit of %d slots is %d symbols, not %d", modName(r.mod), u, syms, u * kSlotSyms);
    }
    // TR-B15 Table 7.1.5: QPSK 1/2 in 2 slots and BPSK 1/2 in 4 slots carry one slot's worth ("effective 1 slot"), QPSK 1/2 in 4 two
    CHECK(slotActive(kQpsk12) * 2 / slotUnit(kQpsk12) == 1 && slotActive(kBpsk12) * 4 / slotUnit(kBpsk12) == 1 && slotActive(kQpsk12) * 4 / slotUnit(kQpsk12) == 2, "effective slots");
    CHECK(kHeaderSyms + kDataSyms + kBlocks * kBurstSyms == 39936 && kDataSyms == 38976, "Table 3 frame sizes");
    // the frame lasts 39936 / 28.86e6 s; a super-frame about 11 ms (TR-B15 Fig. 6.4.1: "1SF = 11mS")
    CHECK(std::fabs(kFrameSeconds * kFramesPerSuper - 0.01107) < 0.0001, "super-frame %.5f s", kFrameSeconds * kFramesPerSuper);
}

static void testTmcc() {
    std::vector<StreamCfg> s = {stream(kTc8psk23, 30, 0x4010, -1), stream(kQpsk34, 12, 0x4011, -1), stream(kBpsk12, 4, 0x4012, -1), stream(kQpsk12, 2, 0x4013, -1)};
    Tmcc t, u;
    std::string err;
    std::vector<int> order;
    CHECK(tmccFromStreams(s, 5, t, &err, &order), "tmccFromStreams: %s", err.c_str());
    // most efficient first: TC8PSK, QPSK 3/4, QPSK 1/2, BPSK
    CHECK(t.mode[0] == kTc8psk23 && t.mode[1] == kQpsk34 && t.mode[2] == kQpsk12 && t.mode[3] == kBpsk12 && order[2] == 3 && order[3] == 2, "group order %d %d %d %d", t.mode[0], t.mode[1], t.mode[2], t.mode[3]);
    CHECK(t.slotMod(29) == kTc8psk23 && t.slotMod(30) == kQpsk34 && t.slotMod(42) == kQpsk12 && t.slotMod(47) == kBpsk12, "slot modulation");
    // dummy slots: the last of each unit of QPSK 3/4 (4 slots), the second of QPSK 1/2 (2), the last three of BPSK (4)
    CHECK(t.slotCarries(30) && t.slotCarries(32) && !t.slotCarries(33) && t.slotCarries(42) && !t.slotCarries(43) && t.slotCarries(44) && !t.slotCarries(45) && !t.slotCarries(47), "dummy slots");
    CHECK(t.packetsOf(0) == 30 && t.packetsOf(1) == 9 && t.packetsOf(2) == 1 && t.packetsOf(3) == 1, "packets of the streams");
    CHECK(t.tsId[4] == 0xFFFF && t.tsId[7] == 0xFFFF, "unused relative numbers are 0xFFFF (TR-B15 6.2.4)");
    t.ews = true; t.uplink = 9;
    const auto bits = tmccPack(t);
    CHECK(tmccUnpack(bits.data(), u) && u == t, "TMCC pack / unpack");
    {   // fields where BO.1408 puts them: change counter first; 4 + 6 bits per combination
        int c = 0; for (int i = 0; i < 5; i++) c = (c << 1) | bits[(size_t)i];
        int w = 0; for (int i = 5; i < 9; i++) w = (w << 1) | bits[(size_t)i];
        CHECK(c == 5 && w == kTc8psk23, "change %d, first word %d", c, w);
    }
    {   // an unused combination is 1111 with count 0
        std::vector<StreamCfg> one = {stream(kTc8psk23, 48, 0x4010, -1)};
        Tmcc a;
        CHECK(tmccFromStreams(one, 0, a), "one stream");
        const auto b = tmccPack(a);
        int w = 0, n = 0; for (int i = 15; i < 19; i++) w = (w << 1) | b[(size_t)i];
        for (int i = 19; i < 25; i++) n = (n << 1) | b[(size_t)i];
        CHECK(w == 15 && n == 0, "second combination: word %d count %d", w, n);
    }
    // RS(64,48)
    uint8_t c512[kTmccCoded], back[kTmccBits];
    tmccEncode(bits.data(), c512);
    CHECK(memcmp(c512, bits.data(), kTmccBits) == 0, "the code is systematic");
    CHECK(tmccDecode(c512, back) == 0 && memcmp(back, bits.data(), kTmccBits) == 0, "clean decode");
    std::mt19937 rng(5);
    for (int e = 0; e < 8; e++) c512[(rng() % 64) * 8 + rng() % 8] ^= 1;   // up to 8 byte errors
    CHECK(tmccDecode(c512, back) >= 0 && memcmp(back, bits.data(), kTmccBits) == 0, "8 errors corrected");
    tmccEncode(bits.data(), c512);
    for (int i = 0; i < 12; i++) c512[i * 40 + 3] ^= 1;   // 12 bytes wrong: beyond the code
    uint8_t junk[kTmccBits];
    CHECK(tmccDecode(c512, junk) < 0, "12 byte errors are reported, not passed on");
    // refused configurations
    std::vector<StreamCfg> bad = {stream(kTc8psk23, 30, 1, -1), stream(kQpsk12, 30, 2, -1)};
    CHECK(!tmccFromStreams(bad, 0, t), "more than 48 slots is refused");
    bad = {stream(kTc8psk23, 40, 1, -1)};
    CHECK(!tmccFromStreams(bad, 0, t), "fewer than 48 slots is refused");
    bad = {stream(kTc8psk23, 45, 1, -1), stream(kQpsk34, 3, 2, -1)};
    CHECK(!tmccFromStreams(bad, 0, t), "QPSK 3/4 in 3 slots (not a unit of 4) is refused");
    bad = {stream(kTc8psk23, 30, 1, -1), stream(kQpsk78, 8, 2, -1), stream(kQpsk34, 4, 3, -1), stream(kQpsk12, 2, 4, -1), stream(kBpsk12, 4, 5, -1)};
    CHECK(!tmccFromStreams(bad, 0, t), "five modulations are refused");
}

static void testWords() {
    // W3 is W2 inverted, and the code sends the inverse of everything for an inverted input (171 and 133 have odd weight)
    unsigned s2 = 0, s3 = 0;
    for (int i = 0; i < 16; i++) {
        const int b2 = (kW2 >> (15 - i)) & 1, b3 = (kW3 >> (15 - i)) & 1;
        CHECK(b2 != b3, "W3 = not W2");
        const unsigned o2 = convOut(s2, b2), o3 = convOut(s3, b3);
        if (i >= 6) CHECK((o2 ^ o3) == 3, "coded W3 tail is the negative of W2's at bit %d", i);
        s2 = convNext(s2, b2); s3 = convNext(s3, b3);
    }
    // the PRBS: period 2^15 - 1, the load value as BO.1408 Fig. 5
    std::vector<uint8_t> pr(70000);
    prbs15(pr.data(), pr.size());
    CHECK(memcmp(pr.data(), pr.data() + 32767, 1000) == 0 && memcmp(pr.data(), pr.data() + 16000, 64) != 0, "PRBS period");
    CHECK(burstSequence().size() == 768, "burst length");
    // 8PSK labels: neighbours differ in one of (Y, X) or are the opposite pair (W)
    for (int l = 0; l < 8; l++) {
        const cf32 a = psk8Point(l), b = psk8Point(l ^ 4);
        CHECK(std::abs(a + b) < 1e-5f, "label %d and %d opposite", l, l ^ 4);
    }
    CHECK(std::abs(psk8Point(0) - cf32(1, 0)) < 1e-5f && std::abs(psk8Point(3) - cf32(0, 1)) < 1e-5f && std::abs(psk8Point(7) - cf32(0, -1)) < 1e-5f, "8PSK map (Fig. 15 a)");
}

// one super-frame's worth of frames through FrameEncoder -> symbols (+ noise) -> branch metrics -> Viterbi over the whole frame
static void testFrameCode(double snrDb) {
    std::vector<StreamCfg> s = {stream(kTc8psk23, 28, 1, -1), stream(kQpsk78, 8, 2, -1), stream(kQpsk56, 6, 3, -1), stream(kQpsk23, 6, 4, -1)};
    Tmcc t;
    std::string err;
    CHECK(tmccFromStreams(s, 0, t, &err), "%s", err.c_str());
    Layout L;
    L.build(t);
    CHECK(L.ok, "layout");
    FrameEncoder enc;
    std::mt19937 rng(11);
    std::normal_distribution<float> nd(0.f, 1.f);
    const float sigma = (float)std::sqrt(std::pow(10.0, -snrDb / 10.0) / 2.0);
    std::vector<cf32> fr((size_t)kFrameSyms * 3);
    std::vector<std::vector<std::vector<uint8_t>>> sent(3);
    uint8_t hb[3][kHeaderBits];
    for (int f = 0; f < 3; f++) {
        sent[(size_t)f].resize(L.groups.size());
        for (size_t g = 0; g < L.groups.size(); g++) {
            const auto& G = L.groups[g];
            const int pk = G.count / slotUnit(G.mod) * slotActive(G.mod);
            sent[(size_t)f][g].resize((size_t)pk * kSlotBytes);
            for (auto& b : sent[(size_t)f][g]) b = (uint8_t)rng();
        }
        uint8_t tm[kTmccCoded];
        for (auto& b : tm) b = rng() & 1;
        headerBits(tm, f, hb[f]);
        enc.encode(L, hb[f], sent[(size_t)f], &fr[(size_t)f * kFrameSyms]);
    }
    if (snrDb < 100) for (auto& v : fr) v += cf32(nd(rng) * sigma, nd(rng) * sigma);
    // frame 1 (its predecessor ends at a known state only through W1; start from the state after the header)
    const int f = 1;
    const cf32* F = &fr[(size_t)f * kFrameSyms];
    const int total = L.totalSteps + 6;
    std::vector<Branch> br((size_t)total);
    headerBranches(F, br.data());
    for (size_t g = 0; g < L.groups.size(); g++) groupBranches(L, (int)g, F, &br[(size_t)L.groups[g].step0]);
    Branch nb[kHeaderBits];
    headerBranches(F + kFrameSyms, nb);
    for (int i = 0; i < 6; i++) br[(size_t)L.totalSteps + (size_t)i] = nb[i];
    std::vector<int8_t> forced((size_t)total, -1);
    for (int i = 0; i < 6; i++) forced[(size_t)L.totalSteps + (size_t)i] = (int8_t)((kW1 >> (15 - i)) & 1);
    std::vector<uint8_t> bits((size_t)total), states((size_t)total + 1);
    viterbi(br.data() + 6, total - 6, (int)w1State(), (int)w1State(), forced.data() + 6, bits.data() + 6, states.data() + 6);
    int hdrErr = 0;
    for (int i = 6; i < kHeaderBits; i++) hdrErr += bits[(size_t)i] != hb[f][i];
    size_t byteErr = 0, nBytes = 0;
    for (size_t g = 0; g < L.groups.size(); g++) {
        const auto& G = L.groups[g];
        const auto& ref = sent[(size_t)f][g];
        std::vector<uint8_t> got(ref.size());
        if (G.mod == kTc8psk23) {
            std::vector<uint8_t> w((size_t)G.steps);
            tc8Uncoded(L, (int)g, F, &states[(size_t)G.step0], &bits[(size_t)G.step0], 0, G.steps, w.data());
            for (size_t k = 0; k < got.size(); k++) { unsigned v = 0; for (int q = 0; q < 4; q++) v = (v << 2) | ((unsigned)w[k * 4 + (size_t)q] << 1) | bits[(size_t)G.step0 + k * 4 + (size_t)q]; got[k] = (uint8_t)v; }
        } else {
            for (size_t k = 0; k < got.size(); k++) { unsigned v = 0; for (int q = 0; q < 8; q++) v = (v << 1) | bits[(size_t)G.step0 + k * 8 + (size_t)q]; got[k] = (uint8_t)v; }
        }
        size_t e = 0;
        for (size_t k = 0; k < got.size(); k++) e += got[k] != ref[k];
        printf("  frame code %4.1f dB, %-10s %2d slots: %zu of %zu bytes wrong\n", snrDb, modName(G.mod), G.count, e, got.size());
        byteErr += e; nBytes += got.size();
    }
    if (snrDb > 100) CHECK(byteErr == 0 && hdrErr == 0, "noise-free frame: %zu byte errors, %d header bit errors", byteErr, hdrErr);
    else CHECK(byteErr * 200 < nBytes, "frame at %.1f dB: %zu of %zu bytes wrong", snrDb, byteErr, nBytes);
}

struct RunResult {
    std::map<int, std::vector<uint8_t>> byTs;   // by stream tag (PID low byte)
    IsdbsTelemetry tel;
    double cpuSecs = 0;
    std::vector<std::string> log;
};

// a signal through the receiver
static RunResult run(const GenConfig& gc, int frames, int select, double rxRate = 0, bool quiet = false) {
    RunResult r;
    Generator gen(gc);
    CHECK(gen.ok(), "generator: %s", gen.error().c_str());
    IsdbsReceiver rx;
    rx.configure(rxRate > 0 ? rxRate : gc.sampleRate);
    CHECK(rx.ready(), "receiver not ready");
    rx.selectStream(select);
    std::vector<uint8_t> all;
    rx.setPacketCallback([&](const uint8_t* p, size_t n, double) { all.insert(all.end(), p, p + n * 188); });
    rx.setLogCallback([&](const std::string& s) { r.log.push_back(s); });
    const size_t total = (size_t)((double)frames * kFrameSyms / gc.symbolRate * gc.sampleRate);
    std::vector<cf32> buf(65536);
    const auto t0 = std::chrono::steady_clock::now();
    for (size_t done = 0; done < total;) {
        const size_t n = std::min(buf.size(), total - done);
        gen.generate(buf.data(), n);
        rx.feed(buf.data(), n);
        done += n;
    }
    r.cpuSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    rx.telemetry(r.tel, 0);
    for (size_t i = 0; i + 188 <= all.size(); i += 188) r.byTs[all[i + 2]].insert(r.byTs[all[i + 2]].end(), all.begin() + (long)i, all.begin() + (long)i + 188);
    if (!quiet) printf("  %zu packets out, state %d, Es/N0 %.1f dB, carrier %.0f Hz, RS clean %llu corrected %llu failed %llu, %.1f s\n", all.size() / 188, r.tel.state, r.tel.snrDb, r.tel.cfoHz,
                       (unsigned long long)r.tel.rsClean, (unsigned long long)r.tel.rsCorrected, (unsigned long long)r.tel.rsFailed, r.cpuSecs);
    return r;
}

// counts the packets of stream `tag` that match the reference and checks they come in order without gaps after the first
static size_t countGood(const std::vector<uint8_t>& v, int tag, const char* what, size_t* bad = nullptr, size_t* gaps = nullptr) {
    size_t good = 0, badn = 0, gapn = 0, flagged = 0;
    bool have = false;
    uint64_t expect = 0;
    for (size_t i = 0; i + 188 <= v.size(); i += 188) {
        const uint8_t* p = &v[i];
        const uint64_t n = ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) | ((uint64_t)p[6] << 8) | p[7];
        uint8_t ref[188];
        testPacket(tag, n, ref);
        if (memcmp(p, ref, 188) != 0) { if (p[1] & 0x80) flagged++; else badn++; continue; }   // a packet Reed-Solomon gave up on says so (transport error indicator)
        if (have && n != expect) gapn++;
        have = true; expect = n + 1; good++;
    }
    if (gapn) {
        printf("    counters:");
        for (size_t i = 0; i + 188 <= v.size() && i < 24 * 188; i += 188) printf(" %u", ((unsigned)v[i + 6] << 8) | v[i + 7]);
        printf("\n");
    }
    if (bad) *bad = badn;
    if (gaps) *gaps = gapn;
    printf("  %s: %zu good, %zu flagged as errors, %zu bad, %zu gaps\n", what, good, flagged, badn, gapn);
    return good;
}

static GenConfig hierarchical(double snrDb, double cfo, double ppm) {
    GenConfig g;
    g.sampleRate = 2 * kSymbolRate;
    g.snrDb = snrDb; g.cfoHz = cfo; g.clockPpm = ppm;
    g.streams = {stream(kTc8psk23, 32, 0x4010, 0x10), stream(kQpsk34, 8, 0x4011, 0x11), stream(kBpsk12, 8, 0x4012, 0x12)};
    return g;
}

static GenConfig single(int mod, double snr, double cfo, double ppm) {
    GenConfig g;
    g.sampleRate = 2 * kSymbolRate; g.snrDb = snr; g.cfoHz = cfo; g.clockPpm = ppm;
    g.streams = {stream(mod, 48, 0x4040, 0x40)};
    return g;
}

static void sweep() {
    struct C { int mod; double snr, cfo, ppm; };
    const C cs[] = {{kTc8psk23, 12, 0, 0}, {kTc8psk23, 11, 0, 0}, {kTc8psk23, 10, 0, 0}, {kTc8psk23, 9, 0, 0}, {kQpsk12, 4, 0, 0}, {kQpsk12, 3, 0, 0}, {kQpsk12, 2, 0, 0},
                    {kQpsk34, 7, 0, 0}, {kQpsk34, 6, 0, 0}, {kQpsk78, 10, 0, 0}, {kQpsk78, 9, 0, 0}, {kBpsk12, 3, 0, 0}, {kBpsk12, 2, 0, 0}, {kBpsk12, 1, 0, 0},
                    {kTc8psk23, 14, 1.5e6, 0}, {kTc8psk23, 14, -1.7e6, 0}, {kTc8psk23, 14, 0, 60}, {kTc8psk23, 14, 0, -60}, {kQpsk12, 3, 400e3, 20}, {kQpsk12, 3, 40e3, 0}};
    for (const C& c : cs) {
        RunResult r = run(single(c.mod, c.snr, c.cfo, c.ppm), 200, -1, 0, true);
        size_t bad = 0, gaps = 0;
        printf("sweep %s %.1f dB cfo %.0f ppm %.0f: ", modName(c.mod), c.snr, c.cfo, c.ppm);
        countGood(r.byTs[0x40], 0x40, "", &bad, &gaps);
        printf("   locked %d, RS clean %llu corrected %llu failed %llu, est %.1f dB, cfo %.0f, %.1f s\n", (int)r.tel.locked, (unsigned long long)r.tel.rsClean, (unsigned long long)r.tel.rsCorrected, (unsigned long long)r.tel.rsFailed, r.tel.snrDb, r.tel.cfoHz, r.cpuSecs);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: a crash on the Windows runner (segfault seen in CI) must not swallow the progress lines
    if (argc > 1 && !strcmp(argv[1], "sweep")) { sweep(); return 0; }
    if (argc > 4 && !strcmp(argv[1], "one")) {   // one case: test_isdbs one <mod 1..7> <Es/N0 dB> <carrier Hz>
        RunResult r = run(single(atoi(argv[2]), atof(argv[3]), atof(argv[4]), 0), 200, -1);
        countGood(r.byTs[0x40], 0x40, modName(atoi(argv[2])));
        return 0;
    }
    testNumbers();
    testTmcc();
    testWords();
    printf("-- frame coding loop (FrameEncoder -> Viterbi)\n");
    testFrameCode(200);
    testFrameCode(11);

    printf("-- receiver, hierarchical signal, clean\n");
    {
        GenConfig g = hierarchical(200, 0, 0);
        RunResult r = run(g, 160, 0);
        CHECK(r.tel.tmcc.valid && r.tel.streams.size() == 3, "TMCC: %zu streams", r.tel.streams.size());
        if (r.tel.tmcc.valid) CHECK(r.tel.tmcc.mode[0] == kTc8psk23 && r.tel.tmcc.count[0] == 32 && r.tel.tmcc.mode[2] == kBpsk12 && r.tel.tmcc.mode[3] == kUnused && r.tel.tmcc.tsId[2] == 0x4012 && r.tel.tmcc.tsId[3] == 0xFFFF, "TMCC contents");
        size_t bad = 0, gaps = 0;
        const size_t good = countGood(r.byTs[0x10], 0x10, "TC8PSK stream", &bad, &gaps);
        CHECK(good > 1500 && bad == 0 && gaps == 0, "TC8PSK stream: %zu good %zu bad %zu gaps", good, bad, gaps);
        RunResult q = run(g, 160, 1);
        const size_t g1 = countGood(q.byTs[0x11], 0x11, "QPSK 3/4 stream", &bad, &gaps);
        CHECK(g1 > 400 && bad == 0 && gaps == 0, "QPSK stream: %zu good %zu bad %zu gaps", g1, bad, gaps);
        RunResult b = run(g, 160, 2);
        const size_t g2 = countGood(b.byTs[0x12], 0x12, "BPSK stream", &bad, &gaps);
        CHECK(g2 > 150 && bad == 0 && gaps == 0, "BPSK stream: %zu good %zu bad %zu gaps", g2, bad, gaps);
        CHECK(b.byTs[0x10].empty() && b.byTs[0x11].empty(), "only the selected stream comes out");
        CHECK(r.tel.snrDb > 25, "Es/N0 of a clean signal: %.1f dB", r.tel.snrDb);
    }

    printf("-- receiver, noise, carrier offset 120 kHz, symbol clock +15 ppm\n");
    {
        GenConfig g = hierarchical(16, 120e3, 15);
        g.streams = {stream(kTc8psk23, 40, 0x4010, 0x10), stream(kQpsk34, 8, 0x4011, 0x11)};
        RunResult r = run(g, 140, 0);
        size_t bad = 0, gaps = 0;
        const size_t good = countGood(r.byTs[0x10], 0x10, "TC8PSK, 16 dB", &bad, &gaps);
        CHECK(good > 1500 && bad == 0 && gaps == 0, "noisy TC8PSK: %zu good %zu bad %zu gaps", good, bad, gaps);
        CHECK(std::fabs(r.tel.cfoHz - 120e3) < 3e3, "carrier estimate %.0f Hz", r.tel.cfoHz);
        CHECK(std::fabs(r.tel.snrDb - 16) < 3, "Es/N0 estimate %.1f dB for 16", r.tel.snrDb);
        RunResult q = run(g, 140, 1);
        const size_t g1 = countGood(q.byTs[0x11], 0x11, "QPSK 3/4, 16 dB", &bad, &gaps);
        CHECK(g1 > 300 && bad == 0 && gaps == 0, "noisy QPSK: %zu good %zu bad %zu gaps", g1, bad, gaps);
    }

    printf("-- receiver, 40 Msample/s input, 900 kHz offset, 9 dB, QPSK 2/3 only\n");
    {
        GenConfig g = single(kQpsk23, 9, 900e3, 0);
        g.sampleRate = 40e6;
        RunResult r = run(g, 140, -1);
        size_t bad = 0, gaps = 0;
        const size_t good = countGood(r.byTs[0x40], 0x40, "QPSK 2/3, 9 dB, 40 Msps", &bad, &gaps);
        CHECK(good > 1500 && bad == 0 && gaps == 0, "QPSK 2/3: %zu good %zu bad %zu gaps", good, bad, gaps);
    }

    printf("-- receiver, large carrier offsets (the LNB), low Es/N0\n");
    {
        struct C { int mod; double snr, cfo; const char* what; };
        const C cs[] = {{kTc8psk23, 12, -1.7e6, "TC8PSK 12 dB, -1.7 MHz"}, {kQpsk12, 5, 400e3, "QPSK 1/2 5 dB, +400 kHz"}, {kQpsk56, 8.5, -950e3, "QPSK 5/6 8.5 dB, -950 kHz"},
                        {kBpsk12, 3, 70e3, "BPSK 1/2 3 dB, +70 kHz"}};
        for (const C& c : cs) {
            RunResult r = run(single(c.mod, c.snr, c.cfo, 0), 200, -1, 0, true);
            size_t bad = 0, gaps = 0;
            const size_t good = countGood(r.byTs[0x40], 0x40, c.what, &bad, &gaps);
            CHECK(good > 300 && bad == 0 && gaps == 0 && r.tel.rsFailed == 0 && std::fabs(r.tel.cfoHz - c.cfo) < 3e3, "%s: %zu good %zu bad %zu gaps, %llu RS failures, carrier %.0f Hz", c.what, good, bad, gaps,
                  (unsigned long long)r.tel.rsFailed, r.tel.cfoHz);
        }
    }

    printf("-- receiver, data on the bursts instead of the PN sequence (TR-B15 6.5)\n");
    {
        GenConfig g = single(kQpsk12, 5, 30e3, 0);
        g.burstData = true;
        RunResult r = run(g, 120, -1);
        size_t bad = 0, gaps = 0;
        const size_t good = countGood(r.byTs[0x40], 0x40, "QPSK 1/2 5 dB, random bursts", &bad, &gaps);
        CHECK(good > 600 && bad == 0 && gaps == 0, "random bursts: %zu good %zu bad %zu gaps", good, bad, gaps);
    }

    printf("-- receiver, TMCC change at super-frame 12 (announced at 10)\n");
    {
        GenConfig g = single(kQpsk12, 200, 0, 0);
        g.streams[0].tsId = 0x4030; g.streams[0].src = source(0x30);
        g.changeAfterSuper = 12;
        g.streams2 = {stream(kTc8psk23, 48, 0x4031, 0x31)};
        RunResult r = run(g, 240, -1);
        size_t bad = 0, gaps = 0, bad2 = 0, gaps2 = 0;
        const size_t before = countGood(r.byTs[0x30], 0x30, "before the change (QPSK 1/2)", &bad, &gaps);
        const size_t after = countGood(r.byTs[0x31], 0x31, "after the change (TC8PSK)", &bad2, &gaps2);
        // the last super-frame before the change and the first after it must both decode: the receiver applies the TMCC two super-frames late
        CHECK(before > 1000 && after > 2500 && bad + bad2 == 0 && gaps + gaps2 == 0 && r.tel.rsFailed == 0, "TMCC change: %zu before, %zu after, %llu RS failures",
              before, after, (unsigned long long)r.tel.rsFailed);
        CHECK(r.tel.tmcc.valid && r.tel.tmcc.mode[0] == kTc8psk23 && r.tel.tmcc.change == 1, "TMCC after the change");
    }

    printf("-- receiver, no signal and too low a rate\n");
    {
        IsdbsReceiver rx;
        rx.configure(20e6);
        CHECK(!rx.ready(), "20 Msample/s is too low");
        rx.configure(57.72e6);
        CHECK(rx.ready(), "57.72 Msample/s works");
        std::mt19937 rng(9);
        std::normal_distribution<float> nd(0.f, 1.f);
        std::vector<cf32> n(65536);
        uint64_t pk = 0;
        rx.setPacketCallback([&](const uint8_t*, size_t c, double) { pk += c; });
        for (int i = 0; i < 20; i++) { for (auto& v : n) v = cf32(nd(rng), nd(rng)); rx.feed(n.data(), n.size()); }
        IsdbsTelemetry t;
        rx.telemetry(t, 0);
        CHECK(!t.locked && pk == 0, "noise must not lock (locked %d, state %d, %llu packets)", (int)t.locked, t.state, (unsigned long long)pk);
    }

    printf("%s\n", g_fail ? "FAILED" : "all ok");
    return g_fail ? 1 : 0;
}
