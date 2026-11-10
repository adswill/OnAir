// #335: a Motorola LRRP position behind a compressed IP/UDP header (an unconfirmed SAP 9 packet whose second header is proprietary), through
// the real receiver, octet for octet as in the reporter's log, with the packet CRC as received. The positions must reach the map list.
// Simulated signal (dmr_gen) carrying the real octets; carrier offset, clock offset and noise.
#include "dect2/dmr/dmr_gen.h"
#include "dect2/dmr/dmr_rx.h"
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;

static int runCase(const char* name, double snr, double cfo, double ppm, uint32_t seed) {
    DmrGenConfig g;
    g.rate = 1.2e6; g.snrDb = snr; g.cfoHz = cfo; g.sroPpm = ppm; g.seed = seed; g.traffic = 2;
    g.textMessages = true; g.motoLrrp = true; g.talkerAlias = false;
    DmrSignal sig(g);
    DmrReceiver rx;
    rx.setSilent(true);
    rx.configure(g.rate);
    const double secs = 40;
    const size_t chunk = 4800, total = (size_t)(secs * g.rate);
    std::vector<cf32> buf(chunk);
    uint64_t seq = 0;
    DmrTelemetry tel;
    for (size_t done = 0; done < total; done += chunk) {
        sig.generate(buf.data(), chunk);
        rx.feed(buf.data(), chunk);
        DmrTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; tel = t; }
    }
    int sent = 0, good = 0, wrong = 0;
    for (const DmrTruth& tr : sig.truth()) if (tr.kind == 3 && tr.finished && tr.endSec < secs - 1.0) sent++;
    for (const DmrLocation& l : tel.locations) {
        if (l.hasFix && std::fabs(l.lat - 45.32213) < 1e-4 && std::fabs(l.lon - 8.88696) < 1e-4 && l.src == 204 && l.dst == 1) good++;
        else wrong++;
    }
    printf("%-22s sent %d, positions %d, wrong %d\n", name, sent, good, wrong);
    return (sent > 0 && good >= (sent + 1) / 2 && wrong == 0) ? 0 : 1;
}

int main() {
    int f = 0;
    f += runCase("clean", 40, 0, 0, 1);
    f += runCase("offset 400 Hz, 8 ppm", 25, 400, 8, 5);
    f += runCase("noisy (18 dB)", 18, -250, -5, 3);
    printf(f ? "FAIL\n" : "PASS\n");
    return f ? 1 : 0;
}
