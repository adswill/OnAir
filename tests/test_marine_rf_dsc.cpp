// DSC through the radio layer: MF/HF (100 bd FSK) and VHF channel 70 (FM, 1200 bd AFSK).
#include "data/marine/rf_util.h"
using namespace mt;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static bool isDistress(const DscCall& c) { return c.format == 112 && c.eccOk && c.fromMmsi == "232123456" && c.nature == 103 && c.hasPos && std::fabs(c.lat - 50.85) < 1e-6 && std::fabs(c.lon + 1.3) < 1e-6 && c.hasTime && c.utcHour == 12 && c.utcMin == 34; }

static void want(const char* what, Scenario s, int minDistress = 1, int minOk = 0) {
    Outcome o = run(s);
    int d = 0, ok = 0;
    for (const auto& c : o.tel.dsc) { if (isDistress(c)) d++; if (c.eccOk) ok++; }
    printf("%-34s %zu calls, %d with a good ECC, %d distress alerts, %.0fx real time\n", what, o.tel.dsc.size(), ok, d, o.rtf);
    CHECK(d >= minDistress, "%s: %d distress alerts decoded", what, d);
    CHECK(ok >= minOk, "%s: %d calls with a good ECC, want %d", what, ok, minOk);
    CHECK(o.seqOk && o.reports >= (uint64_t)(s.secs * 3), "%s: %llu reports", what, (unsigned long long)o.reports);
    CHECK(o.tel.serviceActive == 2 || o.tel.serviceActive == 4, "%s: service %d", what, o.tel.serviceActive);
}

int main() {
    for (int vhf = 0; vhf < 2; vhf++) {
        Scenario base; base.service = vhf ? 4 : 2; base.secs = vhf ? 12 : 16; base.snr = 25;
        const char* tag = vhf ? "VHF" : "MF/HF";
        char n[64];
        snprintf(n, sizeof n, "%s 250 ksps", tag); want(n, base);
        for (double r : {2e6, 2.4e6, 8e6, 10e6}) { if (kSanitized && r > 2.5e6) continue; Scenario s = base; s.rate = r; snprintf(n, sizeof n, "%s %.1f Msps", tag, r / 1e6); want(n, s); }
        for (size_t c : {(size_t)1, (size_t)7, (size_t)4096, (size_t)65536}) { if (kSanitized && c < 100) continue; Scenario s = base; s.chunk = c; snprintf(n, sizeof n, "%s chunks of %zu", tag, c); want(n, s); }
        { Scenario s = base; s.rate = 2e6; s.quant8 = true; snprintf(n, sizeof n, "%s 8 bit, 2 Msps", tag); want(n, s); }
        { Scenario s = base; s.dc = 0.05f; s.quant8 = true; snprintf(n, sizeof n, "%s DC offset", tag); want(n, s); }
        { Scenario s = base; s.gapAt = vhf ? 0.25 : 4.0; s.gapMs = 20; s.secs = base.secs + 12; snprintf(n, sizeof n, "%s 20 ms gap in a call", tag); want(n, s, 0, 2); }   // the call hit by the gap is lost, the next ones are not
        { Scenario s = base; s.resetAt = 3; s.secs = vhf ? base.secs + 8 : base.secs + 14; snprintf(n, sizeof n, "%s reset mid stream", tag); want(n, s, 0, 1); }
        const double offs[] = {-50, 50, vhf ? 2000.0 : 150.0, vhf ? -2000.0 : -300.0};
        for (double f : offs) { Scenario s = base; s.cfo = f; snprintf(n, sizeof n, "%s carrier offset %+.0f Hz", tag, f); want(n, s); }
        { Scenario s = base; s.sro = 50; snprintf(n, sizeof n, "%s clock +50 ppm", tag); want(n, s); }
        { Scenario s = base; s.sro = -50; snprintf(n, sizeof n, "%s clock -50 ppm", tag); want(n, s); }
    }
    // the frequency picks MF/HF or VHF
    {
        Scenario s; s.service = 2; s.secs = 16; s.setting = 0; s.freqHz = 8414.5e3; want("auto, 8414.5 kHz", s);
        Scenario v; v.service = 4; v.secs = 12; v.setting = 0; v.freqHz = 156.525e6; want("auto, 156.525 MHz", v);
    }
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
