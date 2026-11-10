// NDB census: three beacons at different offsets inside one capture, 400 Hz, 1020 Hz and a keyed carrier, with offsets and noise; no beacon on an empty band. Simulated signals only.
#include "dect2/ndb/ndb_census.h"
#include "dect2/ndb/ndb_gen.h"
#include "dect2/ndb/ndb_tel.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// the generator repeats the ident with no pause between copies, so the decoder may report it rotated
static bool isRot(const std::string& s, const char* id) { const std::string w(id); return s.size() == w.size() && (w + w).find(s) != std::string::npos; }

static std::vector<NdbCensusRow> run(double snr, double offs) {
    const double rate = 256000;
    const struct { const char* id; double f, tone; } b[3] = {{"NDB", -31000.0 + offs, 400}, {"KLP", 8000.0 + offs, 1020}, {"VIL", 45000.0 + offs, 0}};
    std::vector<std::unique_ptr<NdbGenerator>> g;
    for (int i = 0; i < 3; i++) {
        NdbGenConfig c; c.rate = rate; c.snrDb = snr; c.dialOffsetHz = b[i].f; c.toneHz = b[i].tone; c.ident = b[i].id; c.wpm = 8; c.amp = 0.08f; c.seed = (uint32_t)i + 1;
        g.emplace_back(std::make_unique<NdbGenerator>(c));
    }
    NdbCensus cen;
    cen.configure(rate, 0.0);
    std::vector<cf32> sum(25600), one(25600);
    for (int s = 0; s < 450; s++) {   // 45 s
        std::fill(sum.begin(), sum.end(), cf32(0, 0));
        for (auto& gen : g) { gen->generate(one.data(), one.size()); for (size_t i = 0; i < sum.size(); i++) sum[i] += one[i]; }
        cen.feed(sum.data(), sum.size());
    }
    std::vector<NdbCensusRow> out;
    cen.snapshot(out);
    return out;
}

int main() {
    for (double offs : {0.0, 137.0}) for (double snr : {35.0, 22.0}) {
        auto r = run(snr, offs);
        int ok = 0;
        for (const auto& x : r) printf("  %+.1f  %s tone %d  snr %.0f\n", x.offsetHz, x.ident.c_str(), x.toneHz, x.snrDb);
        const struct { const char* id; double f; int tone; } want[3] = {{"NDB", -31000.0 + offs, 400}, {"KLP", 8000.0 + offs, 1020}, {"VIL", 45000.0 + offs, -1}};
        for (const auto& w : want) for (const auto& x : r) if (std::fabs(x.offsetHz - w.f) < 40.0 && isRot(x.ident, w.id) && x.toneHz == w.tone) ok++;
        printf("snr %.0f offs %.0f: %d of 3 (rows %zu)\n", snr, offs, ok, r.size());
        CHECK(ok == 3);
        CHECK(r.size() <= 6);
    }
    // nothing but noise: no channels
    { NdbCensus c; c.configure(256000, 0); std::vector<cf32> n(25600); std::vector<NdbCensusRow> r; std::srand(3);
      for (int s = 0; s < 100; s++) { for (auto& v : n) v = cf32((float)(std::rand() / (double)RAND_MAX - 0.5) * 0.1f, (float)(std::rand() / (double)RAND_MAX - 0.5) * 0.1f); c.feed(n.data(), n.size()); } c.snapshot(r); CHECK(r.empty()); }
    return testSummary("ndb_census");
}
