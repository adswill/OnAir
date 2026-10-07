// DTMB receiver against noise: for six profiles the numbered test packets must come out clean 1 dB above the C/N where this receiver starts to lose
// codewords (measured with `dtmbtool sweep`, see docs/modes/dtmb.md) and no wrong packet may ever come out, also below that point. The thresholds
// are this receiver's, not the standard's: they sit 1 to 3 dB below the C/N values that planning documents give (3 / 5 / 7 dB for 4QAM at rate
// 0.4 / 0.6 / 0.8, 9 / 12 / 14 dB for 16QAM, 15 / 17 / 20 dB for 64QAM, Gaussian channel, PN945), which include an implementation margin.
#include "dect2/dtmb_testkit.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace dect2;
using namespace dect2::dtmb;
using namespace dect2::dtmb::kit;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL line %d: %s  ", __LINE__, #c); printf(__VA_ARGS__); printf("\n"); } } while (0)

struct Case { const char* name; Header h; Mapping m; Rate r; bool mode2; double clean; double cliff; };   // clean: C/N with no lost codeword; cliff: about half of them are lost

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    const Case cases[] = {
        {"4QAM 0.4 PN945", Header::Pn945, Mapping::Qam4, Rate::R04, false, 2.0, 1.0},
        {"4QAM-NR 0.8 PN420", Header::Pn420, Mapping::Qam4Nr, Rate::R08, false, 2.0, 1.0},
        {"16QAM 0.6 PN420 mode 2", Header::Pn420, Mapping::Qam16, Rate::R06, true, 10.0, 8.5},
        {"32QAM 0.8 PN945", Header::Pn945, Mapping::Qam32, Rate::R08, false, 16.0, 14.0},
        {"64QAM 0.4 PN595", Header::Pn595, Mapping::Qam64, Rate::R04, false, 12.0, 10.0},
        {"64QAM 0.8 PN945", Header::Pn945, Mapping::Qam64, Rate::R08, false, 18.0, 17.0},
    };
    for (const Case& c : cases) {
        if (only && !strstr(c.name, only)) continue;
        for (int pass = 0; pass < 2; pass++) {
            Scenario o;
            const double snr = pass == 0 ? c.clean + 1.0 : c.cliff - 1.0;
            o.sc = makeSignal(c.h, c.m, c.r, c.mode2, 10e6, snr);
            o.seconds = pass == 0 ? 1.4 : 1.0;
            o.threads = 0;   // decode inside feed(): no codeword is dropped for lack of time
            const Result r = run(o);
            const double perSec = netBitrate(c.h, o.sc.tx.profile) / kTsBits;
            printf("  %-24s C/N %5.1f dB: %6llu good, %llu wrong, %llu missing; codewords ok %llu bad %llu; C/N(PN) %.1f MER %.1f LDPC %.1f it\n", c.name, snr, (unsigned long long)r.good, (unsigned long long)r.wrong,
                   (unsigned long long)r.missing, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.blocksBad, r.tel.snrPnDb, r.tel.merDb, r.tel.ldpcIter);
            CHECK(r.wrong == 0, "%s at %.1f dB: %llu wrong packets", c.name, snr, (unsigned long long)r.wrong);
            CHECK(r.backwards == 0 && r.seqBack == 0, "%s: order", c.name);
            if (pass == 0) {
                CHECK(r.firstGoodSec >= 0 && r.firstGoodSec < 1.0, "%s: first packet at %.2f s", c.name, r.firstGoodSec);
                CHECK(r.missing <= 2 * (uint64_t)packetsPerFrame(o.sc.tx.profile) + 4, "%s at %.1f dB: %llu packets missing in the stream", c.name, snr, (unsigned long long)r.missing);
                CHECK((double)r.good > 0.97 * perSec * (o.seconds - 0.6), "%s at %.1f dB: %llu packets, expected about %.0f", c.name, snr, (unsigned long long)r.good, perSec * (o.seconds - 0.4));
                CHECK(std::fabs(r.tel.snrPnDb - snr) < 1.0, "%s: C/N from the header fit %.1f dB, set %.1f", c.name, r.tel.snrPnDb, snr);
            } else {
                // below the cliff most codewords fail, and what is delivered is still right (checked above by `wrong`)
                CHECK(r.tel.blocksBad > r.tel.blocksOk / 4 || r.tel.state < 2, "%s at %.1f dB: expected losses below the threshold", c.name, snr);
            }
        }
    }
    printf(failures ? "dtmb_snr: %d FAILED\n" : "dtmb_snr: all passed\n", failures);
    return failures ? 1 : 0;
}
