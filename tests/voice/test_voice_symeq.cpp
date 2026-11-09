// Symbol equaliser (core/src/voice/sym_eq.cpp) for the 4-level voice links: symbol error rate with and without it over echo channels of several
// shapes, noise levels, gain and offset errors; a clean channel must be left alone.
#include "dect2/voice/sym_eq.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace dect2::symeq;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static float sl(float y) { return y >= 2.f ? 3.f : y >= 0.f ? 1.f : y >= -2.f ? -1.f : -3.f; }

struct Chan { const char* name; std::vector<float> h; int main; };   // h[main] is the main path

int main() {
    setEnabled(true);   // off by default
    const Chan chans[] = {
        {"clean", {1.f}, 0},
        {"post-echo 0.5", {1.f, 0.5f}, 0},
        {"post-echo 0.8", {1.f, 0.8f}, 0},
        {"pre+post", {0.3f, 1.f, 0.5f}, 1},
        {"long tail", {1.f, 0.55f, -0.3f, 0.2f}, 0},
        {"strong 2nd path", {0.7f, 1.f, 0.9f, 0.35f}, 1},
        {"simulcast-like", {0.2f, 1.f, -0.6f, 0.4f, -0.15f}, 1},
    };
    const double sig[] = {0.15, 0.30, 0.45};
    std::mt19937 g(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    printf("channel            sigma  raw-SER%%  eq-SER%%  used%%\n");
    for (const Chan& ch : chans) {
        for (double s : sig) {
            const int bursts = 400;
            long errRaw = 0, errEq = 0, tot = 0; int used = 0;
            SymEq eq;
            for (int b = 0; b < bursts; b++) {
                const int n = 132, sync0 = 54, m = 24;
                float a[132], z[132];
                float ref[24];
                for (int i = 0; i < n; i++) a[i] = (float)(2 * (int)(g() % 4) - 3);
                for (int i = 0; i < m; i++) { a[sync0 + i] = (g() & 1) ? 3.f : -3.f; ref[i] = a[sync0 + i]; }
                const float gain = 1.f + 0.08f * nd(g), off = 0.1f * nd(g);   // calibration error of the link
                for (int i = 0; i < n; i++) {
                    float v = 0;
                    for (size_t k = 0; k < ch.h.size(); k++) { const int j = i - (int)k + ch.main; if (j >= 0 && j < n) v += ch.h[k] * a[j]; }
                    z[i] = gain * v + off + (float)s * nd(g);
                }
                float y[132];
                const bool u = eq.run(z, n, sync0, ref, m, y);
                used += u;
                for (int i = 0; i < n; i++) { errRaw += sl(z[i]) != a[i]; errEq += sl(y[i]) != a[i]; tot++; }
            }
            printf("%-18s %.2f   %7.2f   %7.2f   %5.1f\n", ch.name, s, 100.0 * errRaw / tot, 100.0 * errEq / tot, 100.0 * used / bursts);
            // never worse than the plain slicer (a little slack for the noise of the estimate), clearly better where the eye is closed
            CHECK(errEq <= errRaw + tot / 200);
            const bool hard = !strcmp(ch.name, "long tail") || !strcmp(ch.name, "strong 2nd path");   // long tail, strong 2nd path
            if (errRaw > tot / 10 && s <= 0.30 && ch.h.size() > 1) CHECK(hard ? errEq * 100 <= errRaw * 90 : errEq * 10 <= errRaw * 6);
        }
    }
    // the eye metric
    {
        float z[8] = {3, -3, 1, -1, 3, 1, -1, -3};
        CHECK(eyeError(z, 8) == 0.f);
        float w[4] = {2.f, -2.f, 0.f, 0.f};
        CHECK(eyeError(w, 4) > 0.5f);
    }
    // off switch
    {
        setEnabled(false);
        float z[40], y[40], ref[24];
        for (int i = 0; i < 40; i++) z[i] = (i & 1) ? 0.1f : -0.2f;
        for (int i = 0; i < 24; i++) ref[i] = 3.f;
        SymEq eq;
        CHECK(!eq.run(z, 40, 8, ref, 24, y));
        setEnabled(true);
    }
    printf("%s\n", fails ? "FAILED" : "voice_symeq OK");
    return fails ? 1 : 0;
}
