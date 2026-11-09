// ADS-C contract requests, acknowledgements and the contract state per aircraft. Layouts as libacars documents them (adsc.c); the messages are built by the
// test's builders, plus a real uplink quoted in JAERO's aerol.cpp through the same parser in test_aero_adsc. Simulated otherwise.
#include "dect2/aero/aero_adsc.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

static AeroAdscMessage round(const std::vector<uint8_t>& g, bool uplink) {
    AeroAdscMessage m;
    CHECK(aeroDecodeAdsc(aeroBuildAdscText("TLVCAYA", "HB-JHM", g), uplink, m), "found");
    CHECK(m.crcOk, "crc");
    return m;
}

int main() {
    auto& book = AdscContractBook::get();
    {   // every interval the scaling can code
        for (int sec : {1, 2, 32, 64, 72, 304, 512, 4096}) {
            auto m = round(aeroAdscPeriodicRequest(5, sec, {12, 14, 16}), true);
            CHECK(m.complete && m.requests.size() == 1, "%d complete", sec);
            CHECK(m.requests[0].intervalSec == sec, "interval %d got %d", sec, m.requests[0].intervalSec);
            CHECK(m.requests[0].groups.size() == 3 && m.requests[0].groups[2] == 16, "groups");
        }
    }
    {   // a message with two requests, and truncation
        auto a = aeroAdscPeriodicRequest(1, 304, {13});
        auto b = aeroAdscEventRequest(2, 1.5, 41000, 33000, true);
        std::vector<uint8_t> g = a; g.insert(g.end(), b.begin(), b.end());
        auto m = round(g, true);
        CHECK(m.complete && m.requests.size() == 2, "two requests");
        const auto& e = m.requests[1];
        CHECK(e.kind == 8 && e.contract == 2 && e.latDevNm == 1.5 && e.hasAltRange && e.ceilingFt == 41000 && e.floorFt == 33000 && e.waypoint, "event fields");
        CHECK(aeroDescribeAdsc(m).find("altitude outside 33000 - 41000 ft") != std::string::npos, "described");
        for (size_t cut = 1; cut + 1 < b.size(); cut++) {
            std::vector<uint8_t> t(b.begin(), b.begin() + cut);
            AeroAdscMessage x; aeroParseAdscUplink(t.data(), t.size(), x);   // must not read past the end
        }
    }
    book.clear();
    {   // request, acknowledge, a report, cancel
        book.note(round(aeroAdscPeriodicRequest(3, 304, {14}), true), 1000);
        auto v = book.contractsOf("HB-JHM");
        CHECK(v.size() == 1 && v[0].state == 0 && v[0].intervalSec == 304, "requested");
        book.note(round(aeroAdscAck(3), false), 1001);
        CHECK(book.contractsOf("HBJHM")[0].state == 1, "acknowledged");
        book.note(round(aeroAdscBasicGroup(47.4, 8.5, 36000, 120), false), 1010);
        auto l = book.linksOf("..HB-JHM");
        CHECK(l.size() == 1 && l[0].reports == 1, "report counted");
        std::vector<uint8_t> cancel = {2, 3};
        book.note(round(cancel, true), 1100);
        CHECK(book.contractsOf("HB-JHM")[0].state == 3, "cancelled");
    }
    book.clear();
    {   // refused, and a damaged message is ignored
        book.note(round(aeroAdscEventRequest(9, 2, 0, 0, false), true), 1);
        book.note(round(aeroAdscNack(9, 6), false), 2);
        auto v = book.contractsOf("HB-JHM");
        CHECK(v.size() == 1 && v[0].state == 2 && v[0].nackReason == 6, "refused");
        CHECK(aeroAdscNackReason(6) == "contract already exists", "reason text");
        std::string t = aeroBuildAdscText("TLVCAYA", "HB-JHM", aeroAdscAck(1));
        t[t.size() - 6] = t[t.size() - 6] == '0' ? '1' : '0';
        AeroAdscMessage m; aeroDecodeAdsc(t, false, m);
        size_t before = book.contractsOf("HB-JHM").size();
        book.note(m, 3);
        CHECK(book.contractsOf("HB-JHM").size() == before && !m.crcOk, "bad CRC ignored");
    }
    return testSummary("aero_adsc_contract");
}
