// Yaesu Fusion receiver sweep: sample rate, tuning error, deviation error, symbol clock error, noise, every data type, header and terminator. Generated signal
// only (no real transmission was available); the frame layout itself is checked against MMDVMHost in test_fusion_interop.
#include "fusion_test_util.h"
#include "dect2/fusion/fusion_gen.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
using namespace dect2::fusion;
#include "../common/test_util.h"

int main() {
    int cases = 0, bad = 0;   // symbol clock error +-0.06 %: real clocks are within tens of ppm
    for (int dt : {0, 1, 2, 3})
        for (double cfo : {-2500.0, 0.0, 2500.0})
            for (double dev : {0.85, 1.0, 1.15})
                for (double clk : {-0.0006, 0.0, 0.0006})
                    for (int swap = 0; swap < 2; swap++) {
                        GenParams p;
                        p.dt = dt; p.src = "LY2ABC"; p.dest = "CQCQCQ"; p.commFrames = 6; p.cm = (cases % 2) ? 3 : 0; p.mr = cases % 3; p.voip = cases % 2;
                        p.slow = "54.7N 25.3E";
                        std::vector<uint8_t> bits;
                        genTransmissionBits(p, bits);
                        std::vector<cf32> x = modulateC4fm(bits, fusiontest::kRate, swap ? -(fusiontest::kOff + cfo) : fusiontest::kOff + cfo, 0.5f, 400, dev, 1.0 + clk);
                        if (swap) for (auto& v : x) v = std::conj(v);
                        impair::noise(x, 22, 9 + cases);
                        FusionReceiver r;
                        r.setSilent(true);
                        fusiontest::run(r, x);
                        const FusionTelemetry t = fusiontest::last(r);
                        cases++;
                        const bool ok = t.headers == 1 && t.terminators == 1 && t.comms == 6 && t.src == "LY2ABC" && t.dest == "CQCQCQ" && t.dataType == dt;
                        if (!ok) { bad++; std::printf("sweep miss dt %d cfo %.0f dev %.2f clk %+.4f swap %d: headers %llu terminators %llu comms %llu src '%s'\n", dt, cfo, dev, clk, swap, (unsigned long long)t.headers, (unsigned long long)t.terminators, (unsigned long long)t.comms, t.src.c_str()); }
                    }
    CHECK(bad == 0, "sweep: %d of %d cases failed", bad, cases);
    std::printf("fusion sweep: %d cases, %d failed\n", cases, bad);
    return testSummary("test_fusion_sweep");
}
