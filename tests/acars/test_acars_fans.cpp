// FANS-1/A over VHF ACARS: a real CPDLC downlink (libacars examples, also in test_aero_cpdlc) sent as the text of every downlink, and the
// generator's ADS-C reports, through the AM-MSK test signal and the ACARS receiver, with carrier offsets and noise. Every decoded message that
// carries the CPDLC text must come out with the CRC good and the message decoded; ADS-C reports must show as ADS-C, not CPDLC.
// Simulated RF only (the CPDLC text itself is a real downlink).
#include "dect2/acars/acars_gen.h"
#include "dect2/acars/acars_rx.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)

static const char* kCpdlc = "/SOUCAYA.AT1.HL8251243F880C3D903BB412903604FE326C2479F4A64F7F62528B1A9CF8382738186AC28B16668E013DF464D8A7F0";

int main() {
    struct Case { double cfo, snr; bool positions; };
    const Case cases[] = {{0, 30, false}, {2500, 16, false}, {-1800, 25, true}};
    for (const auto& c : cases) {
        const double rate = 1e6, secs = 7;
        AcarsGenOptions o;
        o.aircraft = 3;
        o.rateFactor = 3;
        o.uplinks = false;
        o.positions = c.positions;
        o.text = kCpdlc;
        SynthConfig sc;
        sc.snrDb = c.snr;
        sc.cfoHz = c.cfo;
        auto syn = makeAcarsSynthEx(o, sc, rate);
        AcarsReceiver rx;
        rx.configure(rate);
        rx.setCenterHz(o.centerHz);
        rx.setSignalOffset(0);
        std::vector<cf32> buf(65536);
        for (size_t done = 0, total = (size_t)(rate * secs); done < total;) {
            const size_t n = std::min(buf.size(), total - done);
            syn->generate(buf.data(), n);
            rx.feed(buf.data(), n);
            done += n;
        }
        AcarsTelemetry t;
        rx.telemetry(t, ~0ull);
        int cp = 0, cpGood = 0, adsc = 0, adscAsCpdlc = 0;
        for (const auto& m : t.messages) {
            if (m.text.find("AT1.HL8251") != std::string::npos) {
                cp++;
                if (m.adsc.rfind("FANS-1/A CPDLC", 0) == 0 && m.adsc.find("CRC FAILED") == std::string::npos && m.adsc.find("did not decode") == std::string::npos) cpGood++;
            } else if (!m.adsc.empty()) {
                adsc++;
                if (m.adsc.rfind("FANS-1/A CPDLC", 0) == 0) adscAsCpdlc++;
            }
        }
        printf("  cfo %+5.0f Hz, C/N %2.0f dB%s: %zu messages, CPDLC %d (%d decoded), ADS-C %d\n", c.cfo, c.snr, c.positions ? ", positions" : "", t.messages.size(), cp, cpGood, adsc);
        CHECK(cp >= 2, "cfo %.0f snr %.0f: only %d CPDLC messages received", c.cfo, c.snr, cp);
        CHECK(cpGood == cp, "cfo %.0f snr %.0f: %d of %d CPDLC messages decoded", c.cfo, c.snr, cpGood, cp);
        if (c.positions) CHECK(adsc >= 1 && adscAsCpdlc == 0, "ADS-C: %d reports, %d labelled CPDLC", adsc, adscAsCpdlc);
    }
    if (fails) printf("acars fans: %d FAILED\n", fails);
    else printf("acars fans: all passed\n");
    return fails ? 1 : 0;
}
