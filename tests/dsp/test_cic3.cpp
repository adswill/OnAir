// The exact CIC decimator core (dsp/cic3.h): it matches the plain moving sums, and integrators that have wrapped many times (hours at 30 Msps)
// give the same output as fresh ones. The double integrators it replaced decayed: at D = 307 the output was below the error within a minute.
#include "dect2/dsp/cic3.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    std::mt19937_64 rng(3);
    std::normal_distribution<double> nd(0, 0.3);
    for (int D : {4, 307, 614}) {
        const int n = D * 40;
        std::vector<std::complex<double>> x((size_t)n);
        for (auto& v : x) v = {nd(rng), nd(rng)};
        // reference: the cascade of three length-D moving sums from zero, in O(n) (the input quantised to the decimator's 2^-20 step first, so
        // the two agree to rounding); s3[i] is the decimator's output after sample i
        std::vector<std::complex<double>> s1((size_t)n), s2((size_t)n), s3((size_t)n);
        auto mov = [&](const std::vector<std::complex<double>>& in, std::vector<std::complex<double>>& out) {
            std::complex<double> acc = 0;
            for (int i = 0; i < n; i++) { acc += in[(size_t)i]; if (i >= D) acc -= in[(size_t)(i - D)]; out[(size_t)i] = acc; }
        };
        std::vector<std::complex<double>> xq((size_t)n);
        for (int i = 0; i < n; i++) xq[(size_t)i] = {std::trunc(x[(size_t)i].real() * Cic3::kScale) / Cic3::kScale, std::trunc(x[(size_t)i].imag() * Cic3::kScale) / Cic3::kScale};
        mov(xq, s1); mov(s1, s2); mov(s2, s3);
        auto ref = [&](int end) { return s3[(size_t)(end - 1)]; };
        Cic3 fresh, aged;
        for (int k = 0; k < 3; k++) {   // integrators far past 64 bits of history: arbitrary wrapped states
            aged.ir[k] = rng(); aged.ii[k] = rng(); aged.cr[k] = rng(); aged.ci[k] = rng();
        }
        double worst = 0, worstRef = 0;
        int outs = 0;
        for (int i = 0; i < n; i++) {
            fresh.integrate(x[(size_t)i]);
            aged.integrate(x[(size_t)i]);
            if ((i + 1) % D) continue;
            const std::complex<double> a = fresh.comb(), b = aged.comb();
            if (++outs > 3) worst = std::max(worst, std::abs(a - b));   // after the combs have flushed the old states
            worstRef = std::max(worstRef, std::abs(a - ref(i + 1)) / ((double)D * D * D));   // from the first output: both start from zero
        }
        CHECK(worst == 0, "D %d: aged integrators differ from fresh ones by %g", D, worst);
        CHECK(worstRef < 1e-9, "D %d: differs from the moving sums by %g (of the mean)", D, worstRef);
        printf("D %d: aged vs fresh %g, vs reference %g\n", D, worst, worstRef);
    }
    // NaN and huge inputs do not break it
    Cic3 c;
    c.integrate({NAN, 1e30});
    for (int i = 0; i < 10; i++) c.integrate({0, 0});
    const std::complex<double> o = c.comb();
    CHECK(std::isfinite(o.real()) && std::isfinite(o.imag()), "NaN / huge input");
    return testSummary("cic3");
}
