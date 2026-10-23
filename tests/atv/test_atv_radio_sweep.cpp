// Analog TV receiver against the radios and transmitters of issue #239 (an Airspy R2 at 10 Msps looking at a HackRF transmitting analogue TV):
// the sample rates and tuning errors such a pair gives. The signal is a PAL/SECAM/NTSC test card for the usual systems, the channel is shifted
// by the tuning error, the spectrum may be mirrored (I and Q swapped), and the level is low. Every line printed is a result of the report.
#include "dect2/atv/atv_testkit.h"
#include "../common/jobs.h"
#include <atomic>
#include <cmath>
#include <cstdio>

using namespace dect2;
using namespace dect2::atvkit;
using testjobs::jprintf;
static std::atomic<int> fails{0};
#define CHECK(c, ...) do { if (!(c)) { jprintf("FAIL: " __VA_ARGS__); jprintf("\n"); fails++; } } while (0)

struct Case { const char* name; int sys, col; double rate, cfo; bool mirror; double scale; bool mustLock; };

int main() {
    const Case cases[] = {
        {"G 10 Msps centred", kAtvG, kAtvPal, 10e6, 0, false, 1.0, true},
        {"G 10 Msps mirrored", kAtvG, kAtvPal, 10e6, 0, true, 1.0, true},
        {"G 10 Msps +-0.5 MHz", kAtvG, kAtvPal, 10e6, 5e5, false, 1.0, true},
        {"G 10 Msps -0.5 MHz", kAtvG, kAtvPal, 10e6, -5e5, false, 1.0, true},
        {"G 10 Msps +1 MHz", kAtvG, kAtvPal, 10e6, 1e6, false, 1.0, true},
        {"G 10 Msps -1 MHz", kAtvG, kAtvPal, 10e6, -1e6, false, 1.0, true},
        {"G 10 Msps tuned to the vision carrier", kAtvG, kAtvPal, 10e6, 2.75e6, false, 1.0, false},
        {"G 10 Msps tuned to the vision carrier, mirrored", kAtvG, kAtvPal, 10e6, 2.75e6, true, 1.0, false},
        {"G 10 Msps low level", kAtvG, kAtvPal, 10e6, 0, false, 0.05, true},
        {"I 10 Msps centred", kAtvI, kAtvPal, 10e6, 0, false, 1.0, true},
        {"I 10 Msps -0.5 MHz", kAtvI, kAtvPal, 10e6, -5e5, false, 1.0, true},
        {"D/K 10 Msps centred", kAtvDK, kAtvPal, 10e6, 0, false, 1.0, true},
        {"D/K 10 Msps SECAM", kAtvDK, kAtvSecam, 10e6, 0, false, 1.0, true},
        {"B 10 Msps centred", kAtvB, kAtvPal, 10e6, 0, false, 1.0, true},
        {"M 10 Msps NTSC", kAtvM, kAtvNtsc, 10e6, 0, false, 1.0, true},
        {"G 8 Msps centred", kAtvG, kAtvPal, 8e6, 0, false, 1.0, true},
        {"G 16 Msps centred", kAtvG, kAtvPal, 16e6, 0, false, 1.0, true},
        {"G 20 Msps centred", kAtvG, kAtvPal, 20e6, 0, false, 1.0, true},
        {"G 2.5 Msps centred", kAtvG, kAtvPal, 2.5e6, 0, false, 1.0, false},
    };
    for (const Case& c : cases) {
        AtvGenConfig g; g.sys = c.sys; g.colour = c.col; g.cnrDb = 35; g.rate = std::max(c.rate, 10e6); g.cfoHz = c.cfo; g.sound = 4;
        if (c.rate < 7.9e6) continue;       // below the 8 Msps the receiver needs: the engine refuses it with a message (engine_start.cpp)
        g.rate = c.rate;
        Options o; o.scale = c.scale * 0.5;
        if (c.mirror) o.impair = [](cf32* x, size_t n, uint64_t) { for (size_t i = 0; i < n; i++) x[i] = std::conj(x[i]); };
        const Run r = run(g, 4.0, o);
        jprintf("%-50s state %d %-16s carrier %+.0f Hz  lock after %d ms  fields ok %llu\n", c.name, r.tel.state, r.tel.system.empty() ? "-" : r.tel.system.c_str(),
                r.tel.visionHz, r.lockedAfterMs, (unsigned long long)r.tel.blocksOk);
        if (c.mustLock) CHECK(r.tel.state == 2, "%s: no lock (state %d)", c.name, r.tel.state);
    }
    return fails ? 1 : 0;
}
