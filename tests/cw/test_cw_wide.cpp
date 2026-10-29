// CW skimmer, whole span: many carriers across +-100 kHz of a 2.4 Msps capture (different speeds and levels), each decoded by its own
// decoder with the right offset from the dial; the CPU time of the receiver; with the switch off nothing outside +-7 kHz is heard.
#include "dect2/cw/cw_gen.h"
#include "dect2/cw/cw_rx.h"
#include "dect2/cw/cw_tel.h"
#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/cpu_time.h"
#include "../common/test_util.h"

int main() {
    const double rate = 2400000, off = -20000;
    const double offs[] = {-97300, -61000, -33300, -12100, -4000, 2500, 6100, 15800, 31400, 58700, 83000, 99200};
    const double wpms[] = {18, 25, 14, 30, 20, 22, 16, 28, 12, 24, 19, 26};
    const double snr[] = {30, 22, 26, 18, 28, 24, 20, 25, 27, 16, 23, 30};
    const char* txt[] = {"CQ DE LY1AA K", "TEST LY2BB LY2BB", "CQ CQ DE OH3CC", "73 DE SM4DD", "QRZ DE DL5EE", "CQ DE G6FF K",
                         "DE F7GG GG", "CQ TEST OK1HH", "RRR DE ES2II", "UR 599 TU", "CQ DE PA3JJ K", "TEST DE I4KK"};
    for (int mode = 0; mode < 2; mode++) {
        CwGenConfig c;
        c.rate = rate; c.dialOffsetHz = off; c.seed = 5;
        for (int i = 0; i < 12; i++) {
            CwGenSignal s;
            s.offsetHz = offs[i]; s.wpm = wpms[i]; s.snrDb = snr[i]; s.text = txt[i]; s.pauseSec = 1.2; s.startSec = 0.3 + 0.17 * i;
            c.signals.push_back(s);
        }
        CwGenerator g(c);
        CwReceiver r;
        r.setSilent(true);
        r.setWide(mode == 1);
        r.configure(rate);
        r.setSignalOffset(off);
        std::vector<cf32> x(240000);
        CwTelemetry t; uint64_t seq = 0;
        double cpu = 0;
        for (double tm = 0; tm < 40; tm += 0.1) {
            g.generate(x.data(), x.size());
            const double t0 = processCpuSecs();
            r.feed(x.data(), x.size());
            cpu += (processCpuSecs() - t0);
            CwTelemetry tt;
            if (r.telemetry(tt, seq)) { seq = tt.seq; t = std::move(tt); }
        }
        int found = 0, textOk = 0;
        for (int i = 0; i < 12; i++) {
            const CwSignal* b = nullptr;
            for (const auto& s : t.signals) if (std::fabs(s.offsetHz - offs[i]) < 40 && (!b || s.chars > b->chars)) b = &s;
            if (b) { found++; if (b->text.find(txt[i]) != std::string::npos) textOk++; }
            if (mode == 1) CHECK(b && b->text.find(txt[i]) != std::string::npos, "wide: %+.0f Hz %s", offs[i], b ? b->text.c_str() : "(none)");
        }
        printf("wide=%d signals found %d/12, text ok %d/12, listed %zu, cpu %.2f s for 40 s (%.0f %% of one core)\n", mode, found, textOk, t.signals.size(), cpu, cpu / 40 * 100);
        if (mode == 0) CHECK(found <= 5 && textOk <= 5, "off: heard signals outside +-7 kHz (%d)", found);
    }
    printf(fails ? "FAILED\n" : "OK\n");
    return fails ? 1 : 0;
}
