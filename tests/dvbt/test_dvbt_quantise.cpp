// #365: the soft values handed to the 8-bit Viterbi are limited to +-limit (fecp::kViterbi8Max) on every SIMD path (NEON, SSE2, plain C++), also for
// strong carriers; an unclipped +-127 wraps the decoder's int8 branch metrics. Runs on lengths that use the vector loop and the scalar tail.
#include "dect2/dvbt/dvbt.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2::dvbt;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    for (int limit : {31, 63, 127}) {
        for (size_t n : {64, 67, 200}) {
            std::vector<float> llr(n);
            for (size_t i = 0; i < n; i++) llr[i] = (i % 5 == 0 ? 1.f : -1.f) * (float)(i * 3 % 400);   // up to +-400 at scale 1: well past any limit
            std::vector<int8_t> soft;
            Viterbi::depuncture(llr.data(), n, 0, 0, soft, 1.0f, limit);   // rate 1/2: every input keeps its place
            int worst = 0, wrong = 0;
            for (size_t i = 0; i < soft.size() && i < n; i++) {
                worst = std::max(worst, std::abs((int)soft[i]));
                const float v = std::max(-(float)limit, std::min((float)limit, llr[i]));
                if (std::abs((int)soft[i] - (int)std::lround(v)) > 1) wrong++;
            }
            CHECK(!soft.empty() && (limit < 127 ? worst == limit : worst <= 128), "limit %d n %zu: largest value %d", limit, n, worst);
            CHECK(wrong == 0, "limit %d n %zu: %d values differ from the clipped input", limit, n, wrong);
        }
    }
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
