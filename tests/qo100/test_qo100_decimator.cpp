// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The QO-100 decimator computes four outputs per pass over a linear buffer; the result must equal the one-output-at-a-time circular-buffer filter
// to float rounding (the compiler may fuse the multiply-adds differently in the two loops), whatever the block sizes (the filter state and the output phase carry across calls).
#include "dect2/qo100/qo100_dsp.h"
#include "dect2/common/stable_random.h"
#include "dect2/dmr/dmr_dsp.h"
#include <cstdio>
#include <cstring>
#include <random>

using namespace dect2;

struct RefStage { std::vector<float> h; std::vector<cf32> hist; int pos = 0, ph = 0; };

static void refProcess(std::vector<RefStage>& st, const cf32* x, size_t n, std::vector<cf32>& out) {
    std::vector<cf32> a(x, x + n), b;
    for (size_t si = 0; si < st.size(); si++) {
        RefStage& s = st[si];
        const int T = (int)s.h.size();
        std::vector<cf32>& dst = si + 1 == st.size() ? out : b;
        if (&dst == &b) b.clear();
        for (const cf32 v : a) {
            s.hist[(size_t)s.pos] = v; s.hist[(size_t)(s.pos + T)] = v;
            if (++s.pos >= T) s.pos = 0;
            if (++s.ph < 2) continue;
            s.ph = 0;
            const cf32* w = &s.hist[(size_t)s.pos];
            float ar = 0, ai = 0;
            for (int k = 0; k < T; k++) { ar += s.h[(size_t)k] * w[k].real(); ai += s.h[(size_t)k] * w[k].imag(); }
            dst.push_back(cf32(ar, ai));
        }
        if (&dst == &b) std::swap(a, b);
    }
}

int main() {
    int fails = 0;
    struct Case { double in, pass, minOut; } cases[] = {{20e6, 1e6, 1e6}, {10e6, 0.4e6, 0.5e6}, {4e6, 0.9e6, 1e6}};
    for (const Case& c : cases) {
        qo100::Decimator d;
        d.configure(c.in, c.pass, c.minOut);
        std::vector<RefStage> ref;
        double r = c.in;
        while (r / 2 >= c.minOut && r / 2 > 2.2 * c.pass) {
            RefStage s;
            s.h = dmr::lowpassTaps(c.pass, r / 2 - c.pass, r, 70, 9, 255);
            s.hist.assign(2 * s.h.size(), cf32(0, 0));
            ref.push_back(std::move(s));
            r /= 2;
        }
        std::mt19937 rng(5);
        dect2::normal_distribution<float> nd(0.f, 1.f);
        std::vector<cf32> got, want;
        for (int call = 0; call < 40; call++) {
            const size_t n = call == 0 ? 1 : (call % 5 == 0 ? 7 : 1 + rng() % 6000);
            std::vector<cf32> x(n);
            for (auto& v : x) v = cf32{nd(rng), nd(rng)};
            d.process(x.data(), n, got);
            refProcess(ref, x.data(), n, want);
        }
        double md = 0;
        for (size_t i = 0; i < std::min(got.size(), want.size()); i++) md = std::max(md, (double)std::abs(got[i] - want[i]));
        if (got.size() != want.size() || md > 2e-6) { printf("FAIL: %.0f Hz, %zu vs %zu outputs, max difference %g\n", c.in, got.size(), want.size(), md); fails++; }
    }
    if (!fails) printf("qo100 decimator: ok\n");
    return fails ? 1 : 0;
}
