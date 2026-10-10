// HF digital, FT4 and FT2: the generator's stations, then several stations per slot at the threshold (FT4 -12 dB, FT2 -8 dB in
// 2500 Hz) with the receiving side's faults: dial error +-100 Hz, sample clock +-100 ppm, DT +-1.5 s (FT2 +-0.8 s), upside down,
// noise alone (no false decodes), a start in the middle of a slot.
#include "hfdig_ftx_testutil.h"
#include "dect2/hfdig_gen.h"
#include "dect2/mode_synth.h"
using namespace ftxtest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int count(const std::vector<FtxDecode>& d, const std::vector<Station>& st, int slots, double hzShift, double hzTol, double dtTol,
                 bool mirrored, const char* name) {
    int ok = 0;
    for (const auto& s : st) {
        int n = 0;
        for (const auto& x : d)
            if (x.msg == s.msg && std::fabs(x.hz - (s.hz + hzShift)) < hzTol && std::fabs(x.dt - s.dt) < dtTol && x.mirrored == mirrored) n++;
        ok += std::min(n, slots);
    }
    int wrong = 0;
    for (const auto& x : d) {
        bool known = false;
        for (const auto& s : st) known |= x.msg == s.msg;
        if (!known) { wrong++; printf("  %s: unexpected \"%s\"\n", name, x.msg.c_str()); }
    }
    CHECK(wrong == 0, "%s: %d false decodes", name, wrong);
    printf("%-34s %d of %d\n", name, ok, (int)st.size() * slots);
    return ok;
}

static void runMode(int M) {
    const char* nm = ftxModeName(M);
    const bool ft2 = M == 2;
    const double P = ftxPeriod(M), tol = ft2 ? 3.0 : 2.0, dtTol = ft2 ? 0.02 : 0.03;
    {   // the generator
        SynthConfig sc;
        sc.modeOpt[0] = ft2 ? 6 : 4;
        auto gen = makeHfdigTestAudio(sc.modeOpt[0], sc);
        std::vector<float> a((size_t)((3 * P + 4) * 8000));
        gen->generate(a.data(), a.size());
        Gauss g(9);
        for (auto& v : a) v += (float)(0.02 * g.g());
        const auto d = decode(a, M, kT0);
        print(d);
        const auto st = ftxTestStations(M);
        for (const auto& s : st) {
            const FtxDecode* x = find(d, s.msg);
            CHECK(x, "%s generator: \"%s\" not decoded", nm, s.msg);
            if (x) CHECK(std::fabs(x->hz - s.hz) < tol && std::fabs(x->dt - s.dt) < dtTol, "%s generator: %s at %.1f Hz %.3f s", nm, s.msg, x->hz, x->dt);
        }
        CHECK(d.size() == 3 * st.size(), "%s generator: %d decodes, wanted %d", nm, (int)d.size(), 3 * (int)st.size());
    }
    const double snr = ft2 ? -8 : -12;
    const std::vector<Station> weak = {{"CQ K1ABC FN42", 500, snr, 0.0}, {"K1ABC W9XYZ -12", 1100, snr, 0.3},
                                       {"W9XYZ K1ABC R-05", 1700, snr, -0.2}, {"CQ TEST DL1XYZ JO62", 2300, snr, 0.1},
                                       {"VK2ABC ZL1XYZ 73", 2850, snr, -0.1}};
    const int slots = 3;
    char name[80];
    {
        const auto d = decode(audio(M, weak, slots, Faults()), M, kT0);
        snprintf(name, sizeof name, "%s %.0f dB", nm, snr);
        CHECK(count(d, weak, slots, 0, tol, dtTol, false, name) >= (int)weak.size() * slots - 1, "%s", name);
        double err = 0;
        for (const auto& x : d) err += std::fabs(x.snrDb - snr);
        CHECK(!d.empty() && err / (double)d.size() < 2.5, "%s SNR estimate off by %.1f dB", nm, d.empty() ? 99.0 : err / (double)d.size());
    }
    for (double dial : {100.0, -100.0}) {
        Faults f; f.dialHz = dial;
        snprintf(name, sizeof name, "%s %.0f dB, dial %+.0f Hz", nm, snr, dial);
        CHECK(count(decode(audio(M, weak, slots, f), M, kT0), weak, slots, dial, tol, dtTol, false, name) >= (int)weak.size() * slots - 1, "%s", name);
    }
    for (double ppm : {100.0, -100.0}) {
        Faults f; f.ppm = ppm;
        snprintf(name, sizeof name, "%s %.0f dB, clock %+.0f ppm", nm, snr, ppm);
        CHECK(count(decode(audio(M, weak, slots, f), M, kT0), weak, slots, 0, tol, dtTol + 0.01, false, name) >= (int)weak.size() * slots - 1, "%s", name);
    }
    {
        const double e = ft2 ? 0.8 : 1.5;
        std::vector<Station> st = {{"CQ K1ABC FN42", 700, snr + 3, e}, {"K1ABC W9XYZ -12", 1600, snr + 3, -e}};
        snprintf(name, sizeof name, "%s DT +-%.1f s", nm, e);
        CHECK(count(decode(audio(M, st, slots, Faults()), M, kT0), st, slots, 0, tol, dtTol, false, name) == (int)st.size() * slots, "%s", name);
    }
    {
        Faults f; f.mirrored = true;
        snprintf(name, sizeof name, "%s %.0f dB, mirrored", nm, snr);
        CHECK(count(decode(audio(M, weak, 2, f), M, kT0), weak, 2, 0, tol, dtTol, true, name) >= (int)weak.size() * 2 - 1, "%s", name);
    }
    {
        const auto d = decode(audio(M, {}, 8, Faults()), M, kT0);
        print(d);
        CHECK(d.empty(), "%s noise alone: %d decodes", nm, (int)d.size());
    }
    {
        Faults f; f.startSec = P / 2;
        std::vector<Station> st = {{"CQ K1ABC FN42", 800, snr + 3, 0.0}};
        const auto d = decode(audio(M, st, 3, f), M, kT0 + P / 2);
        CHECK(d.size() == 2, "%s mid-slot start: %d decodes, wanted 2", nm, (int)d.size());
    }
}

int main() {
    runMode(1);
    runMode(2);
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
