// ATSC 3.0 AEAT (A/331 advanced emergency alerts): built XML through the LLS framing and the parser. Simulated; element names as recalled from A/331, not checked against a broadcast.
#include "dect2/atsc3/atsc3_ip.h"
#include <cstdio>
#include "../common/test_util.h"
using namespace dect2::atsc3;
int main() {
    const std::string xml = "<?xml version=\"1.0\"?><AEAT xmlns=\"tag:atsc.org,2016:XMLSchemas/ATSC3/AEA/1.0/\">"
        "<AEA AEAId=\"a1\" issuer=\"NWS\" audience=\"public\" AEAType=\"alert\" priority=\"4\"><Header effective=\"2026-11-09T12:00:00Z\" expires=\"2026-11-09T14:00:00Z\">"
        "<EventCode type=\"SAME\">TOR</EventCode><EventDesc lang=\"en\">Tornado warning</EventDesc><Location type=\"FIPS\">048201</Location><Location type=\"FIPS\">048157</Location></Header>"
        "<AEAText lang=\"en\">Take shelter now.</AEAText></AEA><AEA AEAId=\"a2\" AEAType=\"cancel\"/></AEAT>";
    auto pkt = makeLls(4, 0, 1, xml);
    auto t = parseLls(pkt.data(), pkt.size());
    CHECK(t.ok && t.tableId == 4, "table");
    auto a = parseAeat(t.xml);
    CHECK(a.size() == 2, "%zu alerts", a.size());
    CHECK(a[0].issuer == "NWS" && a[0].eventCode == "TOR" && a[0].eventDesc == "Tornado warning" && a[0].text == "Take shelter now." && a[0].lang == "en" && a[0].priority == "4", "fields");
    CHECK(a[0].location == "048201; 048157", "both areas: %s", a[0].location.c_str());
    CHECK(a[1].type == "cancel" && a[1].text.empty(), "no text");
    CHECK(parseAeat("<SLT/>").empty() && parseAeat("garbage <<").empty(), "other documents");
    return testSummary("atsc3_aeat");
}
