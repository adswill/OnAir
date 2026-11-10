// HF digital, FT8 / FT4 / FT2 decoding depth: the share decoded at the WSJT-X thresholds (FT8 -21 dB, FT4 -16 dB, FT2 -13 dB in 2500 Hz;
// eight stations a slot, random DT), a crowded FT8 slot (100 stations at -20 .. 0 dB, overlapping), and slots of noise alone (no false
// decodes). Before the coherent metrics, OSD and the CQ a-priori pass: FT8 -21 dB 4 %, FT4 -16 dB 4 %, the crowded slot 83 of 100.
#include "hfdig_wsjt_testutil.h"
#include "../common/test_util.h"
#include "../common/test_long.h"
using namespace dect2;

static std::string callOf(int i) {
    static const char* pre[] = {"K", "W", "N", "DL", "G", "JA", "VK", "F", "OH", "SM"};
    char b[16];
    snprintf(b, sizeof b, "%s%d%c%c", pre[i % 10], (i / 10) % 10, 'A' + (i / 100) % 26, 'A' + (i * 7) % 26);
    return b;
}

// share of the stations decoded, with no false decodes allowed
static double share(int mode, double snr, int slots, int k) {
    int ok = 0, all = 0, wrong = 0;
    for (int t = 0; t < slots; t++) {
        ftxtest::Gauss g(1000 + t * 31 + (int)(snr * 7 + 500));
        std::vector<wsjttest::St> st;
        for (int i = 0; i < k; i++) {
            const std::string a = callOf(i + 13 * t), b = callOf(i + 13 * t + 37);
            st.push_back({i % 2 ? "CQ " + a + " FN42" : a + " " + b + " -" + std::to_string(10 + i % 9), 300 + 2400 * (i + 0.2 + 0.6 * g.u()) / k, snr,
                          2 * g.u() - 1});
        }
        wsjttest::Imp imp;
        const auto d = wsjttest::decode(wsjttest::audio(mode, st, 1, imp, 5000 + t * 13 + (int)(snr * 3 + 300)), mode, imp);
        for (const auto& s : st) { all++; ok += wsjttest::find(d, s.msg) != nullptr; }
        for (const auto& x : d) {
            bool sent = false;
            for (const auto& s : st) sent |= x.msg == s.msg;
            if (!sent) { wrong++; printf("  false \"%s\"\n", x.msg.c_str()); }
        }
    }
    CHECK(wrong == 0, "%s %.0f dB: %d false decodes", ftxModeName(mode), snr, wrong);
    printf("%-4s %.0f dB: %d of %d\n", ftxModeName(mode), snr, ok, all);
    return (double)ok / all;
}

int main() {
    CHECK(share(ftx::kFt8, -21, 3, 8) >= 0.35, "FT8 at -21 dB");
    CHECK(share(ftx::kFt4, -16, 3, 8) >= 0.45, "FT4 at -16 dB");
    CHECK(share(ftx::kFt2, -13, 3, 8) >= 0.30, "FT2 at -13 dB");
    {   // the crowded slot
        std::vector<wsjttest::St> st;
        ftxtest::Gauss g(7);
        for (int i = 0; i < 100; i++) {
            const std::string a = callOf(i), b = callOf(i + 37);
            const int r = 5 + i % 15;
            const std::string msg = i % 3 == 0 ? "CQ " + a + " FN42" : i % 3 == 1 ? a + " " + b + " -" + (r < 10 ? "0" : "") + std::to_string(r) : a + " " + b + " RR73";
            st.push_back({msg, 300 + 26.0 * i + 3.0 * (i % 4), -20 + 20 * g.u(), -0.3 + 0.6 * g.u()});
        }
        wsjttest::Imp imp;
        const auto d = wsjttest::decode(wsjttest::audio(ftx::kFt8, st, 1, imp), ftx::kFt8, imp);
        int ok = 0;
        for (const auto& s : st) ok += wsjttest::find(d, s.msg) != nullptr;
        printf("crowded FT8 slot: %d of 100, %d decodes\n", ok, (int)d.size());
        CHECK(ok >= 93 && (int)d.size() == ok, "crowded FT8 slot: %d of 100 (%d decodes)", ok, (int)d.size());
    }
    for (int mode : {ftx::kFt8, ftx::kFt4, ftx::kFt2}) {   // noise alone
        int wrong = 0;
        // quick run: a third of the noise slots (each must give no decode); ONAIR_TEST_LONG=1 runs all of them
        const int slots = (mode == ftx::kFt8 ? 12 : 24) / testLongPick(3, 1);
        for (int t = 0; t < slots; t++) {
            wsjttest::Imp imp;
            for (const auto& x : wsjttest::decode(wsjttest::audio(mode, {}, 1, imp, 777 + 7919 * t), mode, imp)) { wrong++; printf("  noise: \"%s\"\n", x.msg.c_str()); }
        }
        CHECK(wrong == 0, "%s: %d false decodes on noise", ftxModeName(mode), wrong);
    }
    return testSummary("hfdig_ftxsens");
}
