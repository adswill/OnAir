// HF digital, FT8: several stations per slot through the decoder, exact messages, at -15 dB (2500 Hz) and with the receiving side's
// faults: dial error +-100 Hz, sample clock +-100 ppm, DT +-1.5 s, a strong neighbour on top of a weak one (subtraction), upside down
// (lower sideband), noise alone (no false decodes), a start in the middle of a slot, the clock error shown, a slot search without a
// start time; and the generator's audio through the whole receiver (IQ at 1 MHz, tests/impair.h: frequency offset, clock, noise).
#include "hfdig_ftx_testutil.h"
#include "impair.h"
#include "dect2/hfdig_gen.h"
#include "dect2/hfdig_rx.h"
#include "dect2/hfdig_tel.h"
#include "dect2/mode_synth.h"
#include <chrono>
#include <limits>
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

int main() {
    const int M = 0;   // FT8
    const auto t0 = std::chrono::steady_clock::now();
    // 1. the generator's stations (8 messages of 5 kinds), clean enough
    {
        SynthConfig sc;
        sc.modeOpt[0] = 3;
        auto gen = makeHfdigTestAudio(3, sc);
        std::vector<float> a((size_t)(2 * 15 + 4) * 8000);
        gen->generate(a.data(), a.size());
        Gauss g(5);
        for (auto& v : a) v += (float)(0.02 * g.g());
        const auto d = decode(a, M, kT0);
        print(d);
        for (const auto& s : ftxTestStations(M)) {
            std::string want = s.msg;
            if (want == "7123456789ABCDEF01") want = "7123456789ABCDEF01";
            const FtxDecode* x = find(d, want);
            CHECK(x, "generator: \"%s\" not decoded", s.msg);
            if (x) CHECK(std::fabs(x->hz - s.hz) < 1.0 && std::fabs(x->dt - s.dt) < 0.05, "generator: %s at %.1f Hz %.2f s", s.msg, x->hz, x->dt);
        }
        CHECK(d.size() == 2 * ftxTestStations(M).size(), "generator: %d decodes, wanted %d", (int)d.size(), 2 * (int)ftxTestStations(M).size());
        for (const auto& x : d) if (x.msg == "CQ K1ABC FN42") CHECK(x.cq && x.call == "K1ABC" && x.grid == "FN42", "call/grid %s %s", x.call.c_str(), x.grid.c_str());
    }
    // the stations of the threshold and fault cases: -15 dB, apart in frequency, DT inside +-1.5 s
    const std::vector<Station> weak = {{"CQ K1ABC FN42", 500, -15, 0.0},   {"K1ABC W9XYZ -12", 900, -15, 0.4},
                                       {"W9XYZ K1ABC R-05", 1400, -15, -0.3}, {"CQ PJ4/K1ABC", 1900, -15, 0.8},
                                       {"TNX BOB 73 GL", 2400, -15, -0.6},  {"G4ABC DL1XYZ RR73", 2800, -15, 0.2}};
    const int slots = 3;
    {
        const auto d = decode(audio(M, weak, slots, Faults()), M, kT0);
        const int ok = count(d, weak, slots, 0, 1.0, 0.06, false, "-15 dB");
        CHECK(ok >= (int)weak.size() * slots - 1, "-15 dB: %d", ok);
        double err = 0;
        int n = 0;
        for (const auto& x : d) { err += std::fabs(x.snrDb + 15); n++; }
        CHECK(n && err / n < 2.5, "SNR estimate off by %.1f dB on average", n ? err / n : 99.0);
    }
    for (double dial : {100.0, -100.0}) {
        Faults f; f.dialHz = dial;
        char nm[64]; snprintf(nm, sizeof nm, "-15 dB, dial %+.0f Hz", dial);
        const auto d = decode(audio(M, weak, slots, f), M, kT0);
        CHECK(count(d, weak, slots, dial, 1.0, 0.06, false, nm) >= (int)weak.size() * slots - 1, "%s", nm);
    }
    for (double ppm : {100.0, -100.0}) {
        Faults f; f.ppm = ppm;
        char nm[64]; snprintf(nm, sizeof nm, "-15 dB, clock %+.0f ppm", ppm);
        const auto d = decode(audio(M, weak, slots, f), M, kT0);
        CHECK(count(d, weak, slots, 0, 1.0, 0.08, false, nm) >= (int)weak.size() * slots - 1, "%s", nm);
    }
    {   // DT +-1.5 s, and the clock error shown
        std::vector<Station> st = {{"CQ K1ABC FN42", 700, -12, 1.5}, {"K1ABC W9XYZ -12", 1500, -12, -1.5}, {"CQ JA1XYZ PM95", 2300, -12, 1.4}};
        HfdigFtxTelemetry t;
        const auto d = decode(audio(M, st, slots, Faults()), M, kT0, false, &t);
        CHECK(count(d, st, slots, 0, 1.0, 0.06, false, "DT +-1.5 s") == (int)st.size() * slots, "DT +-1.5 s");
        printf("  clock error %.2f s, spread %.2f s, warning %d\n", t.clockErr, t.dtSpread, (int)t.clockWarn);
        CHECK(t.clockWarn && std::fabs(t.clockErr - 1.4f) < 0.1f, "clock error %.2f, warning %d", t.clockErr, (int)t.clockWarn);
    }
    {   // a strong neighbour 15 Hz from a weak station (their tones overlap): the weak one decodes after the strong one is taken out
        std::vector<Station> st = {{"CQ K1ABC FN42", 1000, 5, 0.0}, {"K1ABC W9XYZ -12", 1015, -10, 0.2}, {"CQ DL1XYZ JO62", 1700, 10, 0.1},
                                   {"DL1XYZ G4ABC IO91", 1730, -12, -0.2}};
        const auto d = decode(audio(M, st, 2, Faults()), M, kT0);
        print(d);
        CHECK(count(d, st, 2, 0, 1.0, 0.06, false, "strong neighbour") == (int)st.size() * 2, "strong neighbour");
        int later = 0;
        for (const auto& x : d) later += x.pass > 0;
        CHECK(later >= 2, "the weak ones should need the subtraction (pass > 0): %d", later);
    }
    {   // upside down: a lower sideband transmission on the upper sideband
        Faults f; f.mirrored = true;
        const auto d = decode(audio(M, weak, 2, f), M, kT0);
        CHECK(count(d, weak, 2, 0, 1.0, 0.06, true, "-15 dB, mirrored") >= (int)weak.size() * 2 - 1, "mirrored");
    }
    {   // noise alone: nothing
        // with a steady carrier in the band (interference) and a burst of NaN samples (bad input)
        auto a = audio(M, {}, 6, Faults());
        for (size_t i = 0; i < a.size(); i++) a[i] += (float)(0.2 * std::cos(2 * 3.14159265358979 * 1234.5 * (double)i / 8000));
        for (size_t i = 40000; i < 40100; i++) a[i] = std::numeric_limits<float>::quiet_NaN();
        const auto d = decode(a, M, kT0);
        CHECK(d.empty(), "noise alone: %d decodes", (int)d.size());
        print(d);
    }
    {   // started 7 s into a slot: that slot is skipped (its transmissions began 6.5 s before), the next ones decode
        Faults f; f.startSec = 7;
        std::vector<Station> st = {{"CQ K1ABC FN42", 800, -10, 0.0}};
        const auto d = decode(audio(M, st, 3, f), M, kT0 + 7);
        print(d);
        CHECK(d.size() == 2, "mid-slot start: %d decodes, wanted 2", (int)d.size());
        for (const auto& x : d) CHECK(std::fabs(x.dt) < 0.06 && x.slotUtc >= kT0 + 15, "mid-slot start: slot %.0f dt %.2f", x.slotUtc - kT0, x.dt);
    }
    {   // a recording whose start is unknown: started 4.2 s into a slot, found from the signals, the later slots at DT 0
        Faults f; f.startSec = 4.2;
        std::vector<Station> st = {{"CQ K1ABC FN42", 800, -10, 0.0}, {"K1ABC W9XYZ -12", 1600, -10, 0.1}};
        HfdigFtxTelemetry t;
        const auto d = decode(audio(M, st, 5, f), M, 0, true, &t);
        print(d);
        int late = 0;
        // the clock is set from the median DT of the first slot: afterwards both stations sit within 0.15 s of DT 0
        for (const auto& x : d) if (std::fabs(x.dt) < 0.15) late++;
        CHECK(t.timeMode == 3 && late >= 6, "slot search: mode %d, %d decodes near DT 0", t.timeMode, late);
    }
    // the whole receiver: the generator's audio as an upper sideband at 1 MHz with a frequency offset, a clock error and noise
    // c == 2: I and Q swapped: the upper sideband lands on the far side of the radio's centre, outside the channel; nothing may be
    // decoded from it (no false decodes from what is left)
    for (int c = 0; c < 3; c++) {
        SynthConfig sc;
        sc.modeOpt[0] = 3;
        sc.snrDb = 20;
        const double rate = 1e6;
        auto syn = makeHfdigSynth(sc, rate);
        std::vector<cf32> iq((size_t)(rate * 19));
        syn->generate(iq.data(), iq.size());
        const double off = c == 0 ? 100.0 : -60.0, ppm = c == 0 ? 50.0 : -50.0;
        impair::shift(iq, off, rate);
        iq = impair::clock(iq, ppm);
        if (c == 2) impair::swapIq(iq);
        impair::noise(iq, 40, 3);
        HfdigReceiver rx;
        rx.configure(rate);
        rx.setSignalOffset(-hfdigTuning().tuneOffsetHz);
        rx.setSilent(true);
        for (int m = 0; m < kFtxModes; m++) rx.ftx().setEnabled(m, m == M);
        rx.ftx().setStartTime(kT0);
        rx.reset();
        for (size_t i = 0; i < iq.size(); i += 65536) rx.feed(&iq[i], std::min<size_t>(65536, iq.size() - i));
        rx.ftx().waitIdle();
        HfdigTelemetry t;
        rx.feed(iq.data(), 0);
        rx.telemetry(t, 0);
        HfdigFtxTelemetry ft;
        rx.ftx().telemetry(ft);
        print(ft.decodes);
        int ok = 0;
        for (const auto& s : ftxTestStations(M)) {
            const FtxDecode* x = find(ft.decodes, s.msg);
            if (x && std::fabs(x->hz - (s.hz + off)) < 2.0) ok++;
        }
        printf("receiver, %+.0f Hz %+.0f ppm%s: %d of %d\n", off, ppm, c == 2 ? ", I/Q swapped" : "", ok, (int)ftxTestStations(M).size());
        if (c < 2) CHECK(ok == (int)ftxTestStations(M).size(), "receiver %+.0f Hz: %d", off, ok);
        else CHECK(ft.decodes.empty(), "I/Q swapped: %d decodes", (int)ft.decodes.size());
    }
    printf("%.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
