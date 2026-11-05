// Whole-span pagers + five-tone selective calls: a CCIR call on an FM carrier inside a 2 Msps capture is read by that carrier's receiver
// (setPagerOptions with selcall on), stamped with the carrier's frequency, and not decoded at all with the option off.
#include "dect2/engine/modes.h"
#include "dect2/engine/wide_modes.h"
#include "dect2/pager/pager_rx.h"
#include "dect2/tones/tones_gen.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static const double kFs = 2.0e6;

static size_t run(bool selcall, const std::string& digits, double cfo, double* carrierHz) {
    const double off = -pagerTuning().tuneOffsetHz;
    ToneScene s(48000, 9);
    s.pad(2.0);                                  // the carrier is up long enough to be found before the call starts
    s.sequential("ccir", digits, 0.3, 100, 0.3);
    s.pad(s.seconds() + 0.6);
    const std::vector<cf32> sig = toneSceneFm(s, kFs, off + cfo, 30, 5);
    WideModes wm;
    wm.configure(24, kFs, off);
    wm.setCenterHz(466.0e6);
    wm.setPagerOptions(3, 7, selcall);
    RxTelemetry t; uint64_t seq = 0;
    const size_t blk = (size_t)(kFs * 0.1);
    for (size_t i = 0; i + blk <= sig.size(); i += blk) {
        wm.feed(24, sig.data() + i, blk);
        wm.telemetry(24, t, seq);
    }
    size_t n = 0;
    if (t.pager.messages) for (const PagerMessage& m : *t.pager.messages) if (m.selcall) {
        n++;
        CHECK(m.text == digits && m.carrierId > 0, "call read '%s' carrier %d", m.text.c_str(), m.carrierId);
        if (carrierHz) *carrierHz = m.carrierHz;
    }
    return n;
}

int main() {
    double hz = 0;
    CHECK(run(true, "12345", 50000, &hz) == 1, "one CCIR call from a whole-span carrier");
    CHECK(std::fabs(hz - (-pagerTuning().tuneOffsetHz + 50000)) < 4000, "stamped with the carrier frequency (%.0f)", hz);
    CHECK(run(false, "12345", 50000, nullptr) == 0, "option off: no call");
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("pager_wide_selcall OK\n");
    return 0;
}
