// The FT8 / WSJT decoder's report before any Auto search has run: the found flags, sub-modes and periods start cleared. They were left
// uninitialised, and telemetry() copied them, so the first report carried garbage bools (UB; found by fuzz_engine_modes on a switch to HF
// digital). Every heap block of this program is filled with 0xBE first, as ASan does, so an uninitialised member shows in any build.
#include "dect2/hfdig/hfdig_ftx.h"
#include <cstdlib>
#include <cstring>
#include <new>
#include "../common/test_util.h"

void* operator new(std::size_t n) {
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    std::memset(p, 0xBE, n);
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main() {
    dect2::HfdigFtx ftx;
    dect2::HfdigFtxTelemetry t;
    ftx.telemetry(t);
    for (int m = 0; m < dect2::kFtxModes; m++) {
        unsigned char raw;
        std::memcpy(&raw, &t.autoFound[m], 1);
        CHECK(raw == 0, "mode %d: autoFound byte %u before any search", m, raw);
        CHECK(t.autoSub[m] == 0 && t.autoPeriod[m] == 0, "mode %d: auto sub %d period %g before any search", m, t.autoSub[m], t.autoPeriod[m]);
    }
    return testSummary("hfdig_ftx_init");
}
