// The receiver's result must not depend on how the samples are cut into chunks: the same 5 s of the simulated sky fed in chunks of 1, 7, 4096 and
// 65536 samples (and a ragged mixture) gives the same channel table, the same states, the same search work and the same report numbers. The Doppler,
// code phase and C/N0 values agree to the last digits but not always bit for bit: the FFT of the platform (Accelerate) rounds differently depending on
// the alignment of its arrays, and two runs of the program put them at different addresses. The tolerances below are far under what a loop's noise is.
#include "dect2/gnss_testkit.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace dect2;
using namespace dect2::gnsstest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Snap { std::vector<GnssChannel> ch; uint64_t units = 0; double level = 0; uint64_t seq = 0; };

static Snap runChunks(size_t chunk, bool ragged) {
    GnssSimConfig cfg;
    GnssSim sim(cfg, 4e6);
    GnssReceiver rx;
    rx.configure(4e6);
    const size_t total = (size_t)(5.0 * 4e6);
    std::vector<cf32> buf(65536);
    // the same stream, 8 bit rounded; cut into chunks as asked
    std::vector<cf32> pending;
    uint64_t seq = 0;
    GnssTelemetry t, last;
    size_t done = 0, raggedPos = 0;
    static const size_t pattern[] = {1, 7, 4096, 33, 65536, 500, 2, 16383, 16385};
    while (done < total) {
        const size_t n = std::min<size_t>(65536, total - done);
        sim.generate(buf.data(), n);
        for (size_t i = 0; i < n; i++) buf[i] = cf32(std::round(buf[i].real() * 127.f) / 127.f, std::round(buf[i].imag() * 127.f) / 127.f);
        size_t off = 0;
        while (off < n) {
            const size_t c = ragged ? pattern[raggedPos++ % 9] : chunk;
            const size_t m = std::min(c, n - off);
            rx.feed(buf.data() + off, m);
            off += m;
        }
        done += n;
        if (rx.telemetry(t, seq)) { seq = t.seq; last = t; }
    }
    Snap s;
    s.ch = last.channels; s.units = rx.workUnitsUsed(); s.level = last.levelDbfs; s.seq = last.seq;
    return s;
}

int main() {
    const Snap ref = runChunks(65536, false);
    printf("reference (65536 sample chunks): %zu channels, %llu search FFTs, report %llu\n", ref.ch.size(), (unsigned long long)ref.units, (unsigned long long)ref.seq);
    CHECK(ref.ch.size() >= 4, "only %zu channels after 5 s", ref.ch.size());
    struct Case { size_t chunk; bool ragged; const char* name; } cases[] = {{65536, false, "65536 again"}, {1, false, "1 sample"}, {7, false, "7 samples"}, {4096, false, "4096 samples"}, {0, true, "ragged mixture"}};
    for (auto& c : cases) {
        const Snap s = runChunks(c.chunk, c.ragged);
        bool same = s.ch.size() == ref.ch.size() && s.units == ref.units && s.seq == ref.seq && std::fabs(s.level - ref.level) < 1e-3;
        double worstCn = 0, worstDop = 0, worstCode = 0;
        for (size_t i = 0; same && i < s.ch.size(); i++) {
            same = s.ch[i].prn == ref.ch[i].prn && s.ch[i].state == ref.ch[i].state;
            worstCn = std::fmax(worstCn, std::fabs(s.ch[i].cn0 - ref.ch[i].cn0));
            worstDop = std::fmax(worstDop, std::fabs(s.ch[i].dopplerHz - ref.ch[i].dopplerHz));
            worstCode = std::fmax(worstCode, std::fabs(s.ch[i].codePhase - ref.ch[i].codePhase));
        }
        same = same && worstCn < 0.3 && worstDop < 1.5 && worstCode < 0.05;
        (void)worstCn; (void)worstDop; (void)worstCode;
        if (!same) for (size_t i = 0; i < std::min(s.ch.size(), ref.ch.size()); i++) printf("   ch %zu: prn %d/%d cn0 %.4f/%.4f dopp %.4f/%.4f code %.5f/%.5f state %d/%d\n", i, s.ch[i].prn, ref.ch[i].prn, s.ch[i].cn0, ref.ch[i].cn0, s.ch[i].dopplerHz, ref.ch[i].dopplerHz, s.ch[i].codePhase, ref.ch[i].codePhase, s.ch[i].state, ref.ch[i].state);
        if (!same) printf("   units %llu/%llu level %.5f/%.5f seq %llu/%llu\n", (unsigned long long)s.units, (unsigned long long)ref.units, s.level, ref.level, (unsigned long long)s.seq, (unsigned long long)ref.seq);
        printf("%-16s %zu channels, %llu search FFTs: %s\n", c.name, s.ch.size(), (unsigned long long)s.units, same ? "same" : "DIFFERENT");
        CHECK(same, "%s gives a different result", c.name);
    }
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
