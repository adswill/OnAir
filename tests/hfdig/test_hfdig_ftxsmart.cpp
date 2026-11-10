// FT8 smart decoding (docs/SMART-FT8-3.7.md): soft bits of a repeated signal added across slots, a-priori calls from the conversations
// heard on the band, the early decode a few seconds before the slot ends. Synthetic signals at known SNRs (2500 Hz), noise seeds, DT and
// frequency offsets; the gains are measured against the same decoder with the feature off (HFDIG_FTX_XSLOT=0, HFDIG_FTX_APQSO=0,
// HFDIG_FTX_EARLY=0 in the environment), see the document. With "--sweep" the program prints the counts only.
#include "hfdig_ftx_testutil.h"
#include "../common/test_util.h"
#include "../common/test_long.h"
#include <cstring>
#include <cstdlib>
using namespace ftxtest;

namespace {

struct Tx { std::string msg; double hz; double snr; double dt; };

// slots of audio: tx[k] is what is sent in slot k
std::vector<float> slotsAudio(const std::vector<std::vector<Tx>>& tx, const Faults& f, uint64_t seed) {
    const int n = (int)tx.size();
    std::vector<float> a((size_t)((n * 15.0 + 4.0) * 8000), 0.f);
    for (int k = 0; k < n; k++)
        for (const auto& t : tx[(size_t)k]) {
            if (!ftxAddTransmission(0, t.msg, t.hz + f.dialHz, ampFor(t.snr), 8000.0 * (1 + f.ppm * 1e-6), k * 15.0 + 0.5 + t.dt, false, a))
                printf("cannot send \"%s\"\n", t.msg.c_str());
        }
    Gauss g(seed);
    for (auto& v : a) v += (float)(kSigma * f.noise * g.g());
    return a;
}

int countMsg(const std::vector<FtxDecode>& d, const std::string& msg, int* how = nullptr) {
    int n = 0;
    for (const auto& x : d) if (x.msg == msg) { n++; if (how) how[x.how]++; }
    return n;
}

}   // namespace

int main(int argc, char** argv) {
    const bool sweep = argc > 1 && !strcmp(argv[1], "--sweep");
    const char* only = argc > 2 ? argv[2] : "";
    auto want = [&](const char* t) { return !*only || !strcmp(only, t); };
    // by default only the SNR points the checks use and a shorter noise run; --sweep and the long run (ONAIR_TEST_LONG=1) do every point
    const bool quick = !sweep && !testLong();
    // 1. a station repeating its message every slot, near the threshold: slots decoded of 8
    if (want("xslot")) {
        const std::string msg = "W9XYZ K1ABC RR73";   // not a CQ: the CQ a-priori does not help it
        int total[8] = {};
        const double snrs[] = {-20, -21, -22, -23, -24};
        for (int si = 0; si < 5; si++) {
            if (quick && (si < 2 || si > 3)) continue;   // -22 and -23 dB
            int ok = 0, how[3] = {}, runs = 0, falseN = 0;
            for (uint64_t seed = 1; seed <= 3; seed++) {
                std::vector<std::vector<Tx>> tx(8, std::vector<Tx>{{msg, 1100 + 37.0 * (double)seed, snrs[si], 0.15 * (double)seed - 0.2}});
                const auto d = decode(slotsAudio(tx, Faults(), seed), 0, kT0);
                ok += countMsg(d, msg, how);
                falseN += (int)d.size() - countMsg(d, msg);
                runs++;
            }
            printf("xslot %.0f dB: %d of %d slots (alone %d, cross-slot %d, apriori %d), %d other decodes\n", snrs[si], ok, runs * 8, how[0], how[1], how[2], falseN);
            CHECK(falseN == 0, "xslot %.0f dB: %d false decodes", snrs[si], falseN);
            total[si] = ok;
        }
        if (!sweep) CHECK(total[2] + total[3] >= 6, "xslot: only %d slots decoded at -22/-23 dB", total[2] + total[3]);
    }
    // 2. a conversation on the band: the pair of calls is heard strong, then the weak R-report of one of them
    if (want("apqso")) {
        const double snrs[] = {-20, -21, -22, -23};
        for (int si = 0; si < 4; si++) {
            if (quick && (si < 1 || si > 2)) continue;   // -21 and -22 dB
            int ok = 0, how[3] = {}, runs = 0, other = 0;
            for (uint64_t seed = 1; seed <= 4; seed++) {
                const std::string weak = "K1ABC W9XYZ R-" + std::string(seed % 2 ? "12" : "09");
                std::vector<std::vector<Tx>> tx(4);
                tx[0] = {{"CQ W9XYZ EN37", 1500, -8, 0.1}};
                tx[1] = {{"W9XYZ K1ABC FN42", 1900, -8, 0.0}, {"JA1QRZ K1ABC PM95", 900, -10, 0.2}};
                tx[2] = {{"K1ABC W9XYZ -10", 1500, -10, 0.1}};
                tx[3] = {{weak, 1700 + 20.0 * (double)seed, snrs[si], 0.1}};
                const auto d = decode(slotsAudio(tx, Faults(), 100 + seed), 0, kT0);
                const int n = countMsg(d, weak, how);
                ok += n;
                other += (int)d.size() - n - 5;
                runs++;
            }
            printf("apqso %.0f dB: %d of %d weak messages (alone %d, cross-slot %d, apriori %d)\n", snrs[si], ok, runs, how[0], how[1], how[2]);
            CHECK(other == 0, "apqso %.0f dB: %d false decodes", snrs[si], other);
            if (!sweep && si >= 1 && si <= 2) CHECK(ok >= 3, "apqso %.0f dB: only %d of %d", snrs[si], ok, runs);
        }
    }
    // 3. noise only after a conversation was heard: nothing may be decoded from it
    if (want("noise")) {
        int bad = 0;
        const int nSlots = quick ? 16 : 44;
        for (uint64_t seed = 1; seed <= (quick ? 1u : 2u); seed++) {
            std::vector<std::vector<Tx>> tx((size_t)nSlots);
            tx[0] = {{"CQ W9XYZ EN37", 1500, -6, 0.1}, {"CQ DL1ABC JO62", 800, -6, 0.0}};
            tx[1] = {{"W9XYZ K1ABC FN42", 1900, -6, 0.0}, {"DL1ABC G4XYZ IO91", 2300, -6, 0.2}};
            tx[2] = {{"K1ABC W9XYZ -10", 1500, -6, 0.1}};
            const auto d = decode(slotsAudio(tx, Faults(), 500 + seed), 0, kT0);
            for (const auto& x : d) {
                bool known = false;
                for (const auto& s : tx) for (const auto& t : s) known |= t.msg == x.msg;
                if (!known) { bad++; printf("  noise: false decode \"%s\" %.1f Hz (how %d)\n", x.msg.c_str(), x.hz, x.how); }
            }
        }
        printf("noise: %d false decodes in %d slots\n", bad, (quick ? 1 : 2) * nSlots);
        CHECK(bad == 0, "noise: %d false decodes", bad);
    }
    // 4. the early decode: the audio up to 11.3 s into the slot is enough for a strong station; the full decode then replaces it
    if (want("early") && !sweep) {
        const std::string msg = "CQ K1ABC FN42", msg2 = "W9XYZ DL1ABC JO62";
        std::vector<std::vector<Tx>> tx(1, std::vector<Tx>{{msg, 1200, -8, 0.1}, {msg2, 2000, -12, -0.3}});
        const auto a = slotsAudio(tx, Faults(), 7);
        auto d = makeFtxDecoder();
        for (int m = 0; m < kFtxModes; m++) d->setEnabled(m, m == 0);
        d->setStartTime(kT0);
        d->reset();
        const size_t early = (size_t)(11.5 * 8000);
        for (size_t i = 0; i < early; i += 800) {
            d->feedAudio(&a[i], std::min<size_t>(800, early - i));
            if (i % 8000 == 0) d->waitIdle();
        }
        d->waitIdle();
        HfdigFtxTelemetry t;
        d->telemetry(t);
        printf("early: %d decodes at 11.5 s\n", (int)t.decodes.size());
        for (const auto& x : t.decodes) printf("  %s %.0f dB %.2f s %.1f Hz early=%d\n", x.msg.c_str(), x.snrDb, x.dt, x.hz, (int)x.early);
        CHECK(countMsg(t.decodes, msg) == 1 && t.decodes[0].early, "early: strong station not decoded early");
        CHECK(t.slots[0] == 0, "early: counted as a slot");
        for (size_t i = early; i < a.size(); i += 800) d->feedAudio(&a[i], std::min<size_t>(800, a.size() - i));
        d->waitIdle();
        d->telemetry(t);
        printf("early: %d decodes at the end\n", (int)t.decodes.size());
        CHECK(countMsg(t.decodes, msg) == 1 && countMsg(t.decodes, msg2) == 1 && t.decodes.size() == 2, "early: %d decodes at the end", (int)t.decodes.size());
        for (const auto& x : t.decodes) CHECK(!x.early, "early: %s still marked early", x.msg.c_str());
        CHECK(t.slots[0] == 1 && t.total == 2, "early: slots %d total %d", (int)t.slots[0], (int)t.total);
    }
    return testSummary("hfdig_ftxsmart");
}
