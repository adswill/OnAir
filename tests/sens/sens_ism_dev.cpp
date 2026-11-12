// ISM sensors sensitivity per device: the share of a single test device's transmissions that give a reading with right values, against the
// carrier-to-noise ratio in 100 kHz (IsmGenConfig::snrDb), narrow-envelope retry on or off.
// Usage: sens_ism_dev <on|off> <snrFrom> <snrTo> <step> [cfoHz] [offsetHz]
// Simulated signals only: the generator of ism_gen.h (the layout and timing each rtl_433 decoder documents), complex white noise.
#include "dect2/ism/ism_gen.h"
#include "dect2/ism/ism_rx.h"
#include "dect2/ism/ism_proto.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace dect2;

static double run(IsmTx p, bool narrow, double snr, double cfo, double off, uint32_t seed, int* sentOut, int* wrong) {
    const double rate = 1e6, secs = 8.0;
    IsmGenConfig c;
    c.rate = rate; c.snrDb = snr; c.cfoHz = cfo; c.seed = seed;
    IsmGenDevice d; d.proto = p; d.offsetHz = off; d.period = 0.5; d.phase = 0.2;
    c.devs.push_back(d);
    IsmGenerator gen(c);
    IsmReceiver rx;
    rx.setNarrowRetry(narrow);
    rx.setSignalOffset(0);
    rx.configure(rate);
    int good = 0, bad = 0;
    rx.setReadingTap([&](const IsmReading& r, double) {
        IsmTx w; int k;
        if (ismTruthMatches(r, &w, &k) && w == p) good++; else bad++;
    });
    std::vector<cf32> buf(65536);
    for (size_t done = 0; done < (size_t)(secs * rate);) {
        const size_t n = std::min<size_t>(buf.size(), (size_t)(secs * rate) - done);
        gen.generate(buf.data(), n);
        rx.feed(buf.data(), n);
        done += n;
    }
    *sentOut = gen.sent(0);
    *wrong += bad;
    return good;
}

int main(int argc, char** argv) {
    if (argc < 5) { printf("usage: sens_ism_dev on|off from to step [cfoHz] [offsetHz]\n"); return 2; }
    const bool narrow = !strcmp(argv[1], "on");
    const double lo = atof(argv[2]), hi = atof(argv[3]), st = atof(argv[4]);
    const double cfo = argc > 5 ? atof(argv[5]) : 0, off = argc > 6 ? atof(argv[6]) : 0;
    const char* only = getenv("ISM_DEV");
    for (int pi = 0; pi < (int)IsmTx::Count; pi++) {
        const IsmTx p = (IsmTx)pi;
        if (only && !strstr(ismTxName(p), only)) continue;
        printf("%-22s", ismTxName(p));
        for (double s = hi; s >= lo - 1e-9; s -= st) {
            int sent = 0, wrong = 0;
            double good = 0, tot = 0;
            for (uint32_t seed = 1; seed <= 2; seed++) { int sn; good += run(p, narrow, s, cfo, off, seed, &sn, &wrong); tot += sn; }
            printf(" %5.0f%%%s", 100 * good / std::max(1.0, tot), wrong ? "!" : " ");
            (void)sent;
        }
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
