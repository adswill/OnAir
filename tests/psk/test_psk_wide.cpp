// PSK31 receiver, whole span: BPSK31 signals across +-100 kHz of a 2.4 Msps capture (dial + 100 Hz .. 3.9 kHz are what it hears with the
// switch off), one text per signal; CPU time of the receiver; PSK31 clusters (signals 50 Hz apart).
#include "dect2/psk/psk_gen.h"
#include "dect2/psk/psk_rx.h"
#include "dect2/psk/psk_tel.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
using namespace dect2;
#include "../common/cpu_time.h"
#include "../common/test_util.h"

int main(int argc, char** argv) {
    const double rate = 2400000, off = -20000;
    const double span = argc > 1 ? atof(argv[1]) : 250000;   // the receiver limits itself to 100 kHz
    const double lim = std::min(span, 100000.0);
    const double offs[] = {-48000, -41000.3, -12345, 800, 1500, 1570, 3710, 3790, 3860, 7777, 31000, 44000.5, 49000};   // 1500/1570 and 3790/3860: clusters (70 Hz apart, the second across a slice edge)
    const char* txt[] = {"CQ DE LY1AA", "TEST LY2BB", "QRZ DE OH3CC", "73 SM4DD", "CQ DE DL5EE", "DE G6FF K", "CQ F7GG", "TEST OK1HH", "RRR ES2II", "UR 599", "CQ PA3JJ", "TEST I4KK", "CQ SP5LL"};
    const int N = 13;
    for (int mode = 0; mode < 2; mode++) {
        PskGenConfig c;
        c.rate = rate; c.dialOffsetHz = off; c.seed = 5;
        for (int i = 0; i < N; i++) {
            PskGenSignal s;
            s.offsetHz = offs[i]; s.text = std::string(txt[i]) + "\n"; s.snrDb = 22 - (i % 3) * 3; s.repeat = true; s.startSec = 0.3 + 0.2 * i; s.pauseSec = 1.0;
            c.signals.push_back(s);
        }
        PskGenerator g(c);
        PskReceiver r;
        r.setSilent(true);
        r.setWide(mode == 1, span);
        r.configure(rate);
        r.setSignalOffset(off);
        std::vector<cf32> x(240000);
        PskTelemetry t; uint64_t seq = 0;
        double cpu = 0;
        for (double tm = 0; tm < 40; tm += 0.1) {
            g.generate(x.data(), x.size());
            const double t0 = processCpuSecs();
            r.feed(x.data(), x.size());
            cpu += (processCpuSecs() - t0);
            PskTelemetry tt;
            if (r.telemetry(tt, seq)) { seq = tt.seq; t = std::move(tt); }
        }
        int found = 0, textOk = 0, inSpan = 0;
        for (int i = 0; i < N; i++) {
            if (std::fabs(offs[i]) > lim / 2 - 500) continue;
            inSpan++;
            const PskSignal* b = nullptr;
            for (const auto& s : t.signals) if (std::fabs(s.offsetHz - offs[i]) < 25 && (!b || s.chars > b->chars)) b = &s;
            if (b) { found++; if (b->text.find(txt[i]) != std::string::npos) textOk++; }
            if (mode == 1) CHECK(b && b->text.find(txt[i]) != std::string::npos, "wide: %+.1f Hz %s", offs[i], b ? b->text.c_str() : "(none)");
        }
        printf("wide=%d signals found %d/%d, text ok %d/%d, listed %zu, cpu %.2f s for 40 s (%.0f %% of one core)\n", mode, found, inSpan, textOk, inSpan, t.signals.size(), cpu, cpu / 40 * 100);
        if (mode == 0) CHECK(textOk <= 6, "off: heard signals outside the passband (%d)", textOk);
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
