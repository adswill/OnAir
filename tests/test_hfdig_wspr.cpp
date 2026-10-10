// HF digital, WSPR: the generator's five stations (types 1, 2 and 3), then four stations at -26 dB (2500 Hz) with DT +-1.5 s and the
// receiving side's faults (dial error +-100 Hz with clock +-100 ppm, upside down), and noise alone (no false decodes).
#include "hfdig_ftx_testutil.h"
#include "dect2/hfdig_gen.h"
#include "dect2/mode_synth.h"
#include <chrono>
using namespace ftxtest;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    const int M = 3;
    const auto t0 = std::chrono::steady_clock::now();
    {   // the generator: types 1, 2, 3 (the hashed call is known from the type 1 message of the same slot)
        SynthConfig sc;
        sc.modeOpt[0] = 5;
        auto gen = makeHfdigTestAudio(5, sc);
        std::vector<float> a((size_t)(124 * 8000));
        gen->generate(a.data(), a.size());
        Gauss g(3);
        for (auto& v : a) v += (float)(0.05 * g.g());
        const auto d = decode(a, M, kT0);
        print(d);
        for (const auto& s : ftxTestStations(M)) {
            const FtxDecode* x = find(d, s.msg);
            CHECK(x, "generator: \"%s\" not decoded", s.msg);
            if (x) CHECK(std::fabs(x->hz - s.hz) < 0.3 && std::fabs(x->dt - s.dt) < 0.1, "generator: %s at %.2f Hz %.2f s", s.msg, x->hz, x->dt);
        }
        CHECK(d.size() == ftxTestStations(M).size(), "generator: %d decodes", (int)d.size());
        const FtxDecode* k = find(d, "K1ABC FN42 37");
        CHECK(k && k->dbm == 37 && k->call == "K1ABC" && k->grid == "FN42", "WSPR fields");
    }
    const std::vector<Station> weak = {{"K1ABC FN42 37", 1420, -26, 0.0}, {"G4XYZ IO91 23", 1470, -26, 1.5},
                                       {"PJ4/K1ABC 30", 1530, -26, -1.5}, {"DL1XYZ JO62 10", 1580, -26, 0.7}};
    struct Case { double dial, ppm; bool mir; const char* name; };
    const Case cases[] = {{0, 0, false, "-26 dB"}, {100, 100, false, "-26 dB, dial +100 Hz, +100 ppm"},
                          {-100, -100, false, "-26 dB, dial -100 Hz, -100 ppm"}, {0, 0, true, "-26 dB, mirrored"}};
    for (const Case& c : cases) {
        Faults f; f.dialHz = c.dial; f.ppm = c.ppm; f.mirrored = c.mir;
        const auto d = decode(audio(M, weak, 1, f, 11), M, kT0);
        print(d);
        int ok = 0;
        for (const auto& s : weak) {
            const FtxDecode* x = find(d, s.msg);
            if (x && std::fabs(x->hz - (s.hz + c.dial) * (1 - c.ppm * 1e-6)) < 0.5 && std::fabs(x->dt - s.dt) < 0.15 && x->mirrored == c.mir) ok++;
        }
        printf("%-34s %d of %d\n", c.name, ok, (int)weak.size());
        CHECK(ok >= (int)weak.size() - (c.dial != 0 || c.mir ? 1 : 0), "%s: %d of %d", c.name, ok, (int)weak.size());
        CHECK(d.size() <= weak.size(), "%s: %d decodes (false ones?)", c.name, (int)d.size());
        double err = 0;
        for (const auto& x : d) err += std::fabs(x.snrDb + 26);
        if (!c.dial && !c.mir) CHECK(!d.empty() && err / (double)d.size() < 2.5, "SNR estimate off by %.1f dB", d.empty() ? 99.0 : err / (double)d.size());
    }
    {
        const auto d = decode(audio(M, {}, 2, Faults(), 21), M, kT0);
        print(d);
        CHECK(d.empty(), "noise alone: %d decodes", (int)d.size());
    }
    printf("%.1f s\n%s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
