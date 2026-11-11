// HF weak-signal modes (FT8 FT4 FT2 WSPR JT65 JT9 Q65 FST4 JT4): the SNR (in 2500 Hz) at which half and 90 % of the stations are decoded, plain
// and with a receiving side's faults (dial +40 Hz, +100 ppm clock), and slots of noise alone (no decode allowed). docs/SENSITIVITY-4.0-B.md.
//   default: a short guard (FT8 and WSPR, a few dB above the measured 50 % point, one scenario)
//   ONAIR_TEST_LONG=1: the sweep of every mode, ONAIR_SENS40B_MODES=<names> to pick, ONAIR_SENS40B_SLOTS=<n> slots a point (default 6)
// Simulated signals only: no recording is involved.
#include "hfdig_wsjt_testutil.h"
#include "../common/test_util.h"
#include "../common/test_long.h"
#include <cstring>
using namespace dect2;

namespace {

struct M { int mode; const char* name; double hi; double lo; };
const M kModes[] = {{ftx::kFt8, "FT8", -12, -30}, {ftx::kFt4, "FT4", -8, -26}, {ftx::kFt2, "FT2", -5, -22}, {ftx::kWspr, "WSPR", -20, -36},
                    {ftx::kJt65, "JT65", -20, -34}, {ftx::kJt9, "JT9", -22, -36}, {ftx::kQ65, "Q65", -22, -38}, {ftx::kFst4, "FST4", -22, -38},
                    {ftx::kJt4, "JT4", -16, -32}};

// share of (stations x slots) decoded at snr; three stations a slot, spread over the passband
double share(const M& m, double snr, const wsjttest::Imp& imp, int slots, int* wrong) {
    const auto ts = ftxTestStations(m.mode);
    int ok = 0, all = 0;
    for (int t = 0; t < slots; t++) {
        std::vector<wsjttest::St> st;
        for (size_t i = 0; i < ts.size() && i < 3; i++) st.push_back({ts[i].msg, ts[i].hz + 37.0 * t, snr, ts[i].dt * 0.5});
        const auto d = wsjttest::decode(wsjttest::audio(m.mode, st, 1, imp, 9000 + (uint64_t)t * 17 + (uint64_t)(-snr * 10)), m.mode, imp);
        for (const auto& s : st) { all++; ok += wsjttest::find(d, s.msg) != nullptr; }
        for (const auto& x : d) {
            bool sent = false;
            for (const auto& s : st) sent |= x.msg == s.msg;
            if (!sent) { (*wrong)++; printf("  false \"%s\" (%.0f dB, slot %d)\n", x.msg.c_str(), snr, t); }
        }
    }
    return (double)ok / (double)all;
}

} // namespace

int main() {
    const bool full = testLongPick(0, 1) == 1;
    const char* only = std::getenv("ONAIR_SENS40B_MODES");
    const int slots = std::getenv("ONAIR_SENS40B_SLOTS") ? std::atoi(std::getenv("ONAIR_SENS40B_SLOTS")) : 6;
    for (const M& m : kModes) {
        if (only ? !strstr(only, m.name) : (!full && m.mode != ftx::kFt8 && m.mode != ftx::kWspr && m.mode != ftx::kJt4)) continue;
        if (!full && !only) {   // the guard: one point a few dB above the measured 90 % point, no wrong text
            wsjttest::Imp imp;
            int wrong = 0;
            const double snr = m.mode == ftx::kFt8 ? -16 : m.mode == ftx::kJt4 ? -18 : -24;
            const double sh = share(m, snr, imp, 3, &wrong);
            printf("%-5s %5.0f dB: %3.0f %%\n", m.name, snr, 100 * sh);
            CHECK(sh >= 0.8 && wrong == 0, "%s at %.0f dB: %.0f %% decoded, %d wrong", m.name, snr, 100 * sh, wrong);
        }
        if (!full && !only && m.mode == ftx::kJt4) {   // 4.0: the all-zero word of the Fano decoder (wrong text next to strong stations): a few slots at the SNR where it showed
            wsjttest::Imp f;
            f.dialHz = 40; f.ppm = 100;
            int wrong = 0;
            const double snr = -23;
            share(m, snr, f, 4, &wrong);
            CHECK(wrong == 0, "%s at %.0f dB with a dial and clock error: %d decodes of a text that was not sent", m.name, snr, wrong);
        }
        for (int sc = 0; sc < (full || only ? 2 : 0); sc++) {
            wsjttest::Imp imp;
            if (sc == 1) { imp.dialHz = 40; imp.ppm = 100; }
            double t50 = 99, t90 = 99;
            int wrong = 0, zeros = 0;
            for (double snr = m.hi; snr >= m.lo && zeros < 2; snr -= 1) {
                const double s = share(m, snr, imp, slots, &wrong);
                printf("%-5s %-6s %5.0f dB: %3.0f %%\n", m.name, sc ? "faults" : "awgn", snr, 100 * s);
                if (s >= 0.9) t90 = snr;
                if (s >= 0.5) t50 = snr;
                zeros = s == 0 ? zeros + 1 : 0;
            }
            printf("== %s %s: 90 %% at %.0f dB, 50 %% at %.0f dB (%d false)\n", m.name, sc ? "faults" : "awgn", t90, t50, wrong);
            if (wrong) printf("   note: %d decodes of a text that was not sent next to the stations (listed above; the sweep reports them, the guard run fails on them)\n", wrong);
        }
        int fd = 0;
        for (int t = 0; t < (full ? 6 : 2); t++) {
            wsjttest::Imp imp;
            for (const auto& x : wsjttest::decode(wsjttest::audio(m.mode, {}, 1, imp, 555 + 7919 * (uint64_t)t), m.mode, imp)) { fd++; printf("  noise: \"%s\"\n", x.msg.c_str()); }
        }
        printf("== %s noise only: %d false decodes\n", m.name, fd);
        CHECK(fd == 0, "%s: %d false decodes on noise", m.name, fd);
    }
    return testSummary("hfdig_sens40b");
}
