// PTT and duty-cycle logger: the start and length of transmissions against what the generator keyed, with fades inside a transmission (must not split it) and longer gaps
// (must), kerchunks, a stuck key, an offset / drifting carrier, a noise sweep and the false transmissions on noise alone. Simulated signals only.
#include "x40_gen3.h"
#include <cstdlib>
using namespace x40gen; using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Iv { double a, b; };
// a keyed carrier: intervals on, with raised-cosine edges of 5 ms; dropouts (fades) are intervals off inside
struct Keyed {
    double fs, hz, drift, noise; std::vector<Iv> on, drop; Rng rng; double ph = 0;
    Keyed(double f, double h, double d, double n, unsigned s) : fs(f), hz(h), drift(d), noise(n), rng(s) {}
    static double edge(double t, double a, double b) {          // 1 inside [a, b], 5 ms ramps
        if (t < a - 0.005 || t > b + 0.005) return 0; if (t >= a && t <= b) return 1;
        return t < a ? 0.5 - 0.5 * std::cos(kPi * (t - (a - 0.005)) / 0.005) : 0.5 + 0.5 * std::cos(kPi * (t - b) / 0.005);
    }
    std::vector<cf32> operator()(double t0, size_t n) {
        std::vector<cf32> x(n); const float sg = (float)std::sqrt(noise / 2);
        for (size_t i = 0; i < n; i++) {
            const double t = t0 + i / fs; double a = 0;
            for (auto& v : on) a = std::max(a, edge(t, v.a, v.b));
            for (auto& v : drop) a = std::min(a, 1.0 - edge(t, v.a, v.b));
            ph += 2 * kPi * (hz + drift * t) / fs;
            x[i] = (float)a * cf32((float)std::cos(ph), (float)std::sin(ph)) + cf32(sg * rng.g(), sg * rng.g());
        }
        return x;
    }
};
struct Tx { double start, len; int kind; };
static std::vector<Tx> parse(const X40Telemetry& t) {
    std::vector<Tx> v; for (auto& r : t.rows) { Tx x{}; if (r.kind != "Transmission" && r.kind != "Kerchunk") continue; if (sscanf(r.text.c_str(), "start %lf s, length %lf s", &x.start, &x.len) == 2) { x.kind = r.kind == "Kerchunk"; v.push_back(x); } }
    std::sort(v.begin(), v.end(), [](const Tx& a, const Tx& b) { return a.start < b.start; }); return v;
}

int main() {
    // a scripted session of 10 minutes: 12 overs, 2 kerchunks, fades of 0.12 s inside overs (no split), a gap of 0.7 s inside one over (split into two)
    std::vector<Iv> on = {{10, 14.2}, {20, 22.5}, {30, 31.0}, {40.3, 40.6}, {50, 60.5}, {75, 76.2}, {90, 98}, {100, 101.5}, {110.0, 111.0}, {111.7, 113.0}, {130, 135}, {150, 150.35}, {170, 171.3}, {200, 215}, {300, 303}};
    std::vector<Iv> drop = {{12.0, 12.12}, {55, 55.12}, {94, 94.15}, {132, 132.2}};
    for (double fs : {48000.0, 96000.0}) for (int vi = 0; vi < 3; vi++) {
        const double hz[3] = {0, 2300, -4100}, dr[3] = {0, 0, 3.0};
        Keyed k(fs, hz[vi], dr[vi], noiseFor(24, 12500, fs), 7 + vi); k.on = on; k.drop = drop;
        auto t = runChunks(x40::PttLog, fs, 330.0, std::ref(k), 0, 146.52e6, 1.0);
        auto got = parse(t);
        // expected: the dropout inside 110-113 is the 0.7 s gap (two overs); all others are whole
        const size_t want = on.size();
        CHECK(got.size() == want, "fs %.0f off %.0f: %zu transmissions, want %zu", fs, hz[vi], got.size(), want);
        for (size_t i = 0; i < std::min(got.size(), want); i++) {
            CHECK(std::fabs(got[i].start - on[i].a) < 0.1 && std::fabs(got[i].len - (on[i].b - on[i].a)) < 0.15, "fs %.0f: #%zu start %.2f len %.2f want %.2f / %.2f", fs, i, got[i].start, got[i].len, on[i].a, on[i].b - on[i].a);
            CHECK(got[i].kind == ((on[i].b - on[i].a) < 0.5 ? 1 : 0), "kind of #%zu", i);
        }
        CHECK(factStr(t, "transmissions").rfind("15 (2 under", 0) == 0, "count fact: %s", factStr(t, "transmissions").c_str());
        double busy = 0; for (auto& v : on) busy += v.b - v.a;
        const double duty = factNum(t, "duty cycle"); CHECK(std::fabs(duty - 100 * busy / (330.0 - 5.0)) < 1.0, "duty %.2f want %.2f", duty, 100 * busy / 325.0);
    }
    // the stuck key: a 250 s transmission is flagged once at 180 s
    { Keyed k(48000, 500, 0, noiseFor(24, 12500, 48000), 4); k.on = {{20, 270}}; auto t = runChunks(x40::PttLog, 48000, 300.0, std::ref(k), 0, 146e6, 1.0);
      CHECK(countKind(t, "Stuck key") == 1 && parse(t).size() == 1 && std::fabs(parse(t)[0].len - 250) < 0.2, "stuck key: %d flags, %zu tx", countKind(t, "Stuck key"), parse(t).size()); }
    // noise sweep: carrier to noise in 12.5 kHz; transmissions found of 10 (4 s each) and starts within 0.1 s
    printf("noise sweep (carrier to noise in 12.5 kHz): transmissions found of 10, mean error of the length (s), false\n");
    for (double cnr : {20.0, 14.0, 10.0, 8.0, 6.0, 4.0}) {
        std::vector<Iv> iv; for (int i = 0; i < 10; i++) iv.push_back({15.0 + 12 * i, 19.0 + 12 * i});
        Keyed k(48000, 1000, 0, noiseFor(cnr, 12500, 48000), 21); k.on = iv;
        auto t = runChunks(x40::PttLog, 48000, 140.0, std::ref(k), 0, 146e6, 1.0);
        auto got = parse(t); int ok = 0, extra = 0; double err = 0;
        for (auto& g : got) { bool m = false; for (auto& v : iv) if (std::fabs(g.start - v.a) < 0.3) { m = true; ok++; err += std::fabs(g.len - 4.0); } if (!m) extra++; }
        printf("  %.0f dB: %d, %.2f, %d\n", cnr, ok, ok ? err / ok : 0.0, extra);
        if (cnr >= 8) CHECK(ok == 10 && extra == 0, "%.0f dB: found %d, extra %d", cnr, ok, extra);
    }
    // noise only, 30 minutes: no transmission
    { Keyed k(48000, 0, 0, 1.0, 2); auto t = runChunks(x40::PttLog, 48000, 1800.0, std::ref(k), 0, 146e6, 5.0); CHECK(parse(t).empty(), "false transmissions %zu", parse(t).size()); }
    printf(fails ? "x40 pttlog: %d FAILED\n" : "x40 pttlog: ok\n", fails);
    return fails ? 1 : 0;
}
