#include "dect2/resampler.h"
#include <chrono>
#include <cstdio>
using namespace dect2;
int main() {
    RationalResampler r;
    double err = 0;
    r.configure(10e6, 64e6 / 7, &err);
    printf("L/M %d/%d err %.2e\n", r.L(), r.M(), err);
    std::vector<cf32> in(1 << 20), out;
    for (size_t i = 0; i < in.size(); i++) in[i] = cf32(std::sin(0.01f * i), std::cos(0.013f * i));
    const auto t0 = std::chrono::steady_clock::now();
    size_t tot = 0;
    for (int k = 0; k < 40; k++) { out.clear(); r.process(in.data(), in.size(), out); tot += in.size(); }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    printf("%.1f Msps in -> %.0f%% of one core at 10 Msps real time\n", tot / s / 1e6, 100.0 * 10e6 / (tot / s));
}
