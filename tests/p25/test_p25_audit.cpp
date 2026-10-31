// Audit fixes of the Phase 1 trunking model (one check per defect). M11: the explicit group update follows channel (T), the downlink, not channel (R).
#include "dect2/p25/trunk.h"
#include <cmath>
#include <cstdio>
using namespace dect2;
using namespace dect2::trunk;
#include "../common/test_util.h"

static double chHz(int ch) { return 851006250 + (ch & 0xFFF) * 12500.0; }

int main() {
    // ---- M11: GRP_V_CH_GRANT_UPDT_EXP (0x03): service 8, reserved 8, channel (T) 16, channel (R) 16, group 16
    {
        SystemModel m;
        std::vector<Grant> gr;
        auto feed = [&](const p25::Tsbk& t) { uint8_t b[96]; p25::tsbkBits(t, b); p25::Tsbk r; CHECK(p25::tsbkParse(b, r), "crc"); m.onTsbk(r, 0x293, 1.0, &gr); };
        feed(gen::idenUp(1, 851006250, 12500, -45000000, 12500));
        feed(gen::grpVGrantUpdExp(0x04, 0x1010, 0x1020, 1201));   // T = 851.20625 MHz, R = 851.50625 MHz
        CHECK(gr.size() == 1 && gr[0].tg == 1201, "one explicit update (%zu)", gr.size());
        CHECK(!gr.empty() && gr[0].channel == 0x1010 && std::fabs(gr[0].freqHz - chHz(0x1010)) < 1, "follows channel (T): %.5f MHz", gr.empty() ? 0.0 : gr[0].freqHz / 1e6);
    }
    return fails ? 1 : 0;
}
