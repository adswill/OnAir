#include "dect2/lbj/lbj_proto.h"
#include "dect2/lbj/lbj_rx.h"
#include "dect2/lbj/lbj_gen.h"
#include "dect2/railway/railway_proto.h"
#include "../common/test_util.h"
#include <cmath>
using namespace dect2;
static lbj::Page page(uint32_t address, const std::string& text) {
    lbj::Page p; p.ric = address; p.digits = text; p.function = 3; return p;
}
int main() {
    const std::string sample = "204U2390093130U-(2 9U- (-(202011720927939053465000";
    const auto basic = page(lbj::kAddrBasic, "123 23 123"), detail = page(lbj::kAddrDetail, sample);
    lbj::LbjSessionState st; LbjMessage m;
    CHECK(lbj::parsePage(basic, 1, st, m) && m.train == "123" && m.direction == "down" && m.kmMarker == "12.3", "basic fields");
    CHECK(lbj::parsePage(detail, 2, st, m) && m.paired && m.train == "K123", "pair");
    CHECK(m.loco == "HXD3C-0931" && m.locoCode == "2390" && m.locoSerial == "0931" && m.cabEnd == "30", "fixed locomotive fields");
    CHECK(m.route == "津山线" && m.unknownTail == "000", "GBK route on all platforms: %s", m.route.c_str());
    CHECK(m.positionValid && std::fabs(m.longitude - (117 + 20.9279 / 60)) < 1e-8 && std::fabs(m.latitude - (39 + 5.3465 / 60)) < 1e-8, "degrees and minutes");
    CHECK(lbj::parsePage(basic, 3, st, m) && !m.detailed && m.loco.empty() && !m.positionValid, "no stale extension");
    st = {};
    CHECK(lbj::parsePage(detail, 5, st, m) && !m.paired && m.train.empty() && m.speedKmh.empty(), "orphan detail is visible");
    CHECK(lbj::parsePage(basic, 6, st, m) && m.paired && m.train == "K123", "reverse order pair");
    CHECK(lbj::parsePage(basic, 7, st, m) && !m.detailed, "consume pair once");
    st = {}; lbj::parsePage(basic, 10, st, m);
    CHECK(lbj::parsePage(detail, 16, st, m) && !m.paired && m.train.empty(), "pair expires");
    st = {}; lbj::parsePage(detail, 20, st, m);
    CHECK(!lbj::parsePage(page(lbj::kAddrBasic, "broken"), 21, st, m), "reject malformed basic");
    CHECK(lbj::parsePage(basic, 22, st, m) && !m.detailed, "malformed breaks pairing");
    auto invalid = detail; invalid.digits.replace(33, 2, "60"); st = {};
    CHECK(lbj::parsePage(invalid, 30, st, m) && !m.positionValid, "invalid coordinate minutes");
    CHECK(lbj::parsePage(page(lbj::kAddrTime, "01234"), 31, st, m) && m.timeSync && m.networkTime == "12:34" && m.train.empty() && !m.positionValid, "isolated time sync");
    CHECK(lbj::parsePage(page(lbj::kAddrTime, "02561"), 32, st, m) && m.networkTime.empty(), "invalid clock");
    st = {};
    CHECK(lbj::parsePage(page(lbj::kAddrBasic, "00124 118 -123"), 33, st, m) && m.train == "124" && m.direction == "up" && m.kmMarker == "-12.3", "negative mileage and parity");
    CHECK(railway::lbjLocomotive("30730093") == "CRH380BL-0093" && railway::lbjLocomotive("23990093") == "2399-0093", "subtype and unknown codes");
    LbjReceiver rx;
    CHECK(rx.loadResults("lbj2\nK123\t12.3\tHXD3C-0931\t23\tdown\t1000\n"), "legacy results");
    const auto saved = rx.saveResults(); LbjReceiver restored;
    CHECK(saved.starts_with("lbj3\n") && restored.loadResults(saved), "v3 round trip");
    LbjTelemetry t; CHECK(restored.telemetry(t, 0) && t.messages.size() == 1 && t.messages[0].train == "K123", "restored row");
    // The fixed fields travel through POCSAG, the IQ modem and the results file.
    const auto bits = lbj::burstBits({{lbj::kAddrBasic, 3, lbj::formatBasic("123", 23, 123)}, {lbj::kAddrDetail, 3, sample}});
    LbjReceiver live; live.configure(960000); std::vector<cf32> iq(32768); double phase = 0;
    for (size_t start = 0; start < (bits.size() * 800 + 100000); start += iq.size()) {
        for (size_t i = 0; i < iq.size(); i++) {
            const size_t bit = (start + i) / 800;
            if (bit < bits.size()) { phase += 2 * 3.14159265358979323846 * (bits[bit] ? -4200 : 4200) / 960000; iq[i] = cf32(float(0.4 * std::cos(phase)), float(0.4 * std::sin(phase))); }
            else iq[i] = {};
        }
        live.feed(iq.data(), iq.size());
    }
    CHECK(live.telemetry(t, 0) && !t.messages.empty() && t.messages[0].paired && t.messages[0].route == "津山线" && t.messages[0].positionValid, "extended IQ reception");
    CHECK(restored.loadResults(live.saveResults()), "extended results file");
    restored.telemetry(t, 0);
    CHECK(!t.messages.empty() && t.messages[0].loco == "HXD3C-0931" && t.messages[0].paired && t.messages[0].positionValid && t.messages[0].route == "津山线", "all extended fields persist");
    return testSummary("test_lbj_fields");
}
