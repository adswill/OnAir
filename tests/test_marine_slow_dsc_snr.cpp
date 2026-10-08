// DSC through the radio layer: how many of the calls sent are decoded with a good ECC, against signal to noise ratio (noise in 3 kHz; in 12.5 kHz on VHF).
#include "data/marine/rf_util.h"
#include "dect2/marine_dsc.h"
#include <iterator>
using namespace mt;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

// calls per second of signal in the generator's loop (the same list as marine_gen.cpp)
static double callsPerSecond(bool vhf) {
    using namespace dect2::marine;
    const double baud = vhf ? 1200 : 100, gap = vhf ? 1.2 : 2.5;
    const int dotsDist = vhf ? 20 : 200;
    const double secs =
        (dscFrameBits(dscBuildDistress("232123456", 103, 50.85, -1.3, 12, 34, 109), 127, dotsDist).size() +
         dscFrameBits(dscBuildAllShips("235012345", 108, 109, 126, 82910, -1), 127, dotsDist).size() +
         dscFrameBits(dscBuildIndividual("002320064", 100, "232123456", 109, 126, 82910, 82910), 117, 20).size() +
         dscFrameBits(dscBuildDistressAck("002320064", "232123456", 103, 50.85, -1.3, 12, 34, 109), 127, dotsDist).size()) / baud + 4 * gap;
    return 4.0 / secs;
}

int main() {
    for (int vhf = 0; vhf < 2; vhf++) {
        const double perSec = callsPerSecond(vhf);
        const double snrsHf[] = {20, 10, 6, 3, 0, -3, -6}, snrsVhf[] = {20, 14, 12, 10, 8, 6, 4, 0};
        printf("%s: %.3f calls per second sent\n  SNR(%s)  sent  decoded (ECC ok)  with wrong fields\n", vhf ? "VHF channel 70" : "MF/HF 100 bd", perSec, vhf ? "12.5 kHz" : "3 kHz");
        double last = 99;
        for (double snr : vhf ? std::vector<double>(std::begin(snrsVhf), std::end(snrsVhf)) : std::vector<double>(std::begin(snrsHf), std::end(snrsHf))) {
            Scenario s; s.service = vhf ? 4 : 2; s.snr = snr; s.secs = vhf ? 40 : 120;
            Outcome o = run(s);
            int ok = 0, wrong = 0;
            for (const auto& c : o.tel.dsc) {
                if (!c.eccOk) continue;
                ok++;
                const bool good = (c.fromMmsi == "232123456" || c.fromMmsi == "235012345" || c.fromMmsi == "002320064") && (c.format == 112 || c.format == 116 || c.format == 120);
                if (!good) wrong++;
            }
            const double sent = perSec * s.secs - 0.5;      // the last one may be cut by the end
            printf("  %6.0f      %4.0f  %4d              %d\n", snr, sent, ok, wrong);
            if (ok >= 0.85 * sent) last = snr;
            if (snr >= (vhf ? 14 : 3)) CHECK(ok >= 0.9 * sent, "%s %.0f dB: %d of %.0f calls", vhf ? "VHF" : "MF/HF", snr, ok, sent);
            CHECK(wrong == 0, "%s %.0f dB: %d calls with a good ECC but wrong fields", vhf ? "VHF" : "MF/HF", snr, wrong);
        }
        printf("at least 85 %% of the calls decoded down to %.0f dB\n", last);
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
