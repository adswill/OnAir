// "Use GPU where available": override storage, the global tick, the reasons, and that the policy only ever forces the CPU path (same compute mode otherwise).
#include "../../app/ui/gpu_logic.h"
#include "dect2/dsp/gpu_policy.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
using namespace gpulogic;
using namespace dect2::gpupol;
int main() {
    check(modeAllowed(true, "", "DVB"), "default on: no behaviour change");
    check(!modeAllowed(false, "", "DVB"), "global off");
    check(modeAllowed(false, "DVB=1", "DVB"), "override on beats global off");
    check(!modeAllowed(true, "DTMB=1;DVB=0", "DVB"), "override off beats global on");
    check(modeAllowed(true, "DTMB=1;DVB=0", "DTMB"), "other entry");
    std::string l = setOverride("", "DVB", 0); check(l == "DVB=0", "set");
    l = setOverride(l, "DTMB", 1); check(l == "DVB=0;DTMB=1", "append");
    l = setOverride(l, "DVB", 1); check(l == "DTMB=1;DVB=1", "replace");
    l = setOverride(l, "DTMB", -1); check(l == "DVB=1", "clear");
    check(overrideOf("DVB-S/S2=0", "DVB") == -1, "no prefix match");
    check(overrideOf("DVB-S/S2=0", "DVB-S/S2") == 0, "name with slash");
    check(noGpuReason("DVB") == nullptr && noGpuReason("DTMB") == nullptr && noGpuReason("DVB-S/S2") == nullptr && noGpuReason("ATSC 3.0") == nullptr && noGpuReason("ISDB-T") == nullptr, "GPU modes have a tick");
    check(noGpuReason("ATSC") && noGpuReason("FM") && noGpuReason("CW"), "other modes show a reason");
    for (int c = 0; c < 3; c++) { check(effectiveMode(c, true) == c, "allowed keeps the choice"); check(effectiveMode(c, false) == 0, "off forces CPU"); }
    check(diagLine(T2, false, true, 2, 1) == "DVB-T2 LDPC: GPU (GPU switched off)" || diagLine(T2, false, true, 2, 0) == "DVB-T2 LDPC: CPU (GPU switched off)", "diag line");
    Policy p; for (int d = 0; d < kDomains; d++) check(p.allowed[d].load(), "policy default on");
    printf(fails ? "gpu_policy: %d FAILED\n" : "gpu_policy: all passed\n", fails);
    return fails != 0;
}
