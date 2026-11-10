// DVB-S symbol timing loop pulled hard (full-scale impulsive noise, a wide loop): the step between symbols falls to almost one sample, so
// one call can make more symbols than the 1.5 samples a symbol the output used to be sized for, and wrote past its end (ASan
// container-overflow, found by fuzz_engine_modes). Checks that this case is reached and that every symbol fits; under DECT2_SANITIZE=ON
// the old sizing fails here.
#include "../../core/src/dvbs/dvbs_dsp.h"
#include <algorithm>
#include <random>
#include "../common/test_util.h"
using namespace dect2;

int main() {
    std::mt19937 g(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    double worst = 0;   // most symbols one call made, per input sample
    // symbols much faster than the nominal two samples (a wrong symbol rate, or a strong neighbour), mixed with full-scale impulses
    for (int run = 0; run < 8; run++) {
        dvbs::SymbolTimer st;
        st.configure(0.35);
        st.setLoop(1e-5 * (run + 1), 0.707);   // a narrow loop, so the offset stays while the block runs
        st.shiftIntegrator(-(0.30 + 0.02 * run));   // a rate offset the loop holds (symbols 15 - 25 % fast): the state the fuzzer reached
        const double period = 1.15 + 0.05 * run;   // samples per symbol of the input
        std::vector<cf32> out;
        double ph = 0;
        float sym = 1;
        for (int blk = 0; blk < 300; blk++) {
            dvbs::SplitBuf in;
            const size_t n = 256 + g() % 4096;
            in.resize(n);
            for (size_t i = 0; i < n; i++) {
                ph += 1.0;
                if (ph >= period) { ph -= period; sym = g() % 2 ? 0.7f : -0.7f; }
                const bool imp = g() % 50 == 0;
                in.re[i] = imp ? 1.f : sym + 0.01f * nd(g);
                in.im[i] = imp ? -1.f : 0.01f * nd(g);
            }
            out.clear();
            st.process(in, out);
            worst = std::max(worst, (double)out.size() / (double)n);
            CHECK(out.size() <= n + 64, "run %d block %d: %zu symbols from %zu samples", run, blk, out.size(), n);
        }
    }
    printf("  most symbols per input sample in one call: %.3f (the old output sizing held %.3f)\n", worst, 1 / 1.5);
    CHECK(worst > 1 / 1.5, "the loop was not pulled past 1.5 samples a symbol (%.3f): the case is not reached", worst);
    return testSummary("dvbs_timer_bound");
}
