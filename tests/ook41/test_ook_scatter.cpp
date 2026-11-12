#include "dect2/ism/ook_scatter.h"
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
using namespace dect2::ookscatter;
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { std::printf("FAIL: %s\n", w); fails++; } }
static std::mt19937 rng(7);
static float jit(float v, float j) { std::normal_distribution<float> d(0.f, j); return v * (1.f + std::max(-0.35f, std::min(0.35f, d(rng)))); }
static bool near2(float a, float b, float tol) { return std::fabs(a - b) <= tol * b; }
static Burst pwm(int pulses, float shortUs, float longUs, float period, float j, bool fixedGap) {
    Burst b;
    for (int i = 0; i < pulses; i++) {
        const float on = (rng() & 1) ? longUs : shortUs;
        const float off = fixedGap ? 400.f : period - on;
        b.push_back({jit(on, j), jit(off, j)});
    }
    b.push_back({jit(shortUs, j), 9000.f});
    return b;
}
static Burst ppm(int pulses, float on, float g0, float g1, float j) {
    Burst b;
    for (int i = 0; i < pulses; i++) b.push_back({jit(on, j), jit((rng() & 1) ? g1 : g0, j)});
    b.push_back({jit(on, j), 9000.f});
    return b;
}
static Burst manchester(int bits, float T, float j) {
    std::vector<int> half;
    for (int i = 0; i < bits; i++) { const int v = rng() & 1; half.push_back(v ? 1 : 0); half.push_back(v ? 0 : 1); }
    Burst b;
    size_t i = 0;
    while (i < half.size()) {
        while (i < half.size() && half[i] == 0) i++;
        if (i >= half.size()) break;
        int on = 0, off = 0;
        while (i < half.size() && half[i] == 1) { on++; i++; }
        while (i < half.size() && half[i] == 0) { off++; i++; }
        if (off == 0) off = 40;
        b.push_back({jit(T * (float)on, j), jit(T * (float)off, j)});
    }
    return b;
}
static Result run(const std::vector<Burst>& v) { return analyse(collect(v, Sel::All, 0, 0)); }

int main() {
    for (float j : {0.02f, 0.05f, 0.08f}) {
        std::vector<Burst> v;
        for (int i = 0; i < 6; i++) v.push_back(pwm(24, 350, 1050, 1400, j, false));
        Result r = run(v);
        check(r.clusters.size() == 2, "pwm fixed period: two clusters");
        check(r.guess == "PWM", "pwm fixed period: guess");
        if (r.clusters.size() == 2) { check(near2(r.clusters[0].onUs, 350, 0.1f) && near2(r.clusters[1].onUs, 1050, 0.1f), "pwm centres on"); check(near2(r.clusters[0].offUs, 1050, 0.1f) && near2(r.clusters[1].offUs, 350, 0.1f), "pwm centres off"); }
        std::vector<Burst> w;
        for (int i = 0; i < 6; i++) w.push_back(pwm(24, 300, 700, 0, j, true));
        r = run(w);
        check(r.clusters.size() == 2 && r.guess == "PWM", "pwm fixed gap");
        std::vector<Burst> q;
        for (int i = 0; i < 6; i++) q.push_back(ppm(24, 300, 500, 1000, j));
        r = run(q);
        if (r.clusters.size() != 2 || r.guess != "PPM") { std::printf("ppm j=%.2f clusters=%zu guess=%s\n", j, r.clusters.size(), r.guess.c_str()); for (const Cluster& c : r.clusters) std::printf("  %s\n", clusterText(c).c_str()); }
        check(r.clusters.size() == 2, "ppm: two clusters");
        check(r.guess == "PPM", "ppm: guess");
        if (r.clusters.size() == 2) check(near2(r.clusters[0].onUs, 300, 0.1f) && near2(r.clusters[0].offUs + r.clusters[1].offUs, 1500, 0.15f), "ppm centres");
        std::vector<Burst> m;
        for (int i = 0; i < 8; i++) m.push_back(manchester(32, 250, j));
        r = run(m);
        check(r.clusters.size() >= 3 && r.clusters.size() <= 4, "manchester: 3 or 4 clusters");
        check(r.guess == "Manchester", "manchester: guess");
    }
    for (int rep = 0; rep < 20; rep++) {
        std::vector<Burst> v;
        std::uniform_real_distribution<float> d(0.f, 1.f);
        const int nb = 1 + rep % 10;
        for (int i = 0; i < nb; i++) {
            Burst b;
            for (int k = 0; k < 8 + (int)(rng() % 40); k++) b.push_back({std::exp(std::log(20.f) + d(rng) * std::log(300.f)), std::exp(std::log(20.f) + d(rng) * std::log(300.f))});
            v.push_back(b);
        }
        Result r = run(v);
        check(r.clusters.empty() && r.guess == "unknown", "noise only: no clusters");
    }
    {
        std::vector<Burst> v;
        std::uniform_real_distribution<float> d(0.f, 1.f);
        for (int i = 0; i < 50; i++) { Burst b; for (int k = 0; k < 60; k++) b.push_back({std::exp(std::log(20.f) + d(rng) * std::log(300.f)), std::exp(std::log(20.f) + d(rng) * std::log(300.f))}); v.push_back(b); }
        Result r = run(v);
        check(r.clusters.empty(), "heavy noise: no clusters");
    }
    {
        std::vector<Burst> v;
        for (int i = 0; i < 6; i++) v.push_back(pwm(24, 350, 1050, 1400, 0.05f, false));
        std::uniform_real_distribution<float> d(0.f, 1.f);
        for (int i = 0; i < 4; i++) { Burst b; for (int k = 0; k < 30; k++) b.push_back({std::exp(std::log(20.f) + d(rng) * std::log(300.f)), std::exp(std::log(20.f) + d(rng) * std::log(300.f))}); v.push_back(b); }
        Result r = run(v);
        check(r.clusters.size() == 2 && r.guess == "PWM", "signal plus noise bursts");
    }
    {
        std::vector<Burst> v;
        for (int i = 0; i < 5; i++) v.push_back(ppm(10, 300, 500, 1000, 0.03f));
        check(collect(v, Sel::Current, 0, 2).size() == 10, "current selects one burst, drops the trailing gap");
        check(collect(v, Sel::Current, 0, 9).empty(), "current out of range");
        check(collect(v, Sel::LastN, 2, 0).size() == 20, "last N");
        check(collect(v, Sel::All, 0, 0).size() == 50, "all");
        const std::string s = csv(collect(v, Sel::Current, 0, 1));
        int lines = 0;
        for (char c : s) if (c == '\n') lines++;
        check(lines == 11 && s.rfind("burst,pulse,on_us,off_us\n", 0) == 0, "csv rows");
        check(axis(100, 10, 1000, true) > 0.49f && axis(100, 10, 1000, true) < 0.51f, "log axis");
        check(axis(505, 10, 1000, false) > 0.49f && axis(505, 10, 1000, false) < 0.51f, "linear axis");
    }
    check(analyse({}).clusters.empty(), "empty input");
    std::printf(fails ? "ook_scatter: %d FAILED\n" : "ook_scatter: ok\n", fails);
    return fails ? 1 : 0;
}
