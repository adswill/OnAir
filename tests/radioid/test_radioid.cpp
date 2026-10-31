// Radio ID list: the radioid.net CSV layouts, separators, quotes, BOM, damaged rows, duplicates, the global slots.
#include "dect2/common/radioid_db.h"
#include <cstdio>
#include <string>
using namespace dect2;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main() {
    {   // the radioid.net user.csv layout
        RadioIdDb d;
        const size_t n = d.parse("RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY\r\n"
                                 "1234567,W1ABC,Joe,Smith,Boston,Massachusetts,United States\r\n"
                                 "2345678,LY1XYZ,Jonas,\"Petraitis, Jr\",Vilnius,,Lithuania\r\n"
                                 "abc,BAD,Row,x,,,\r\n"
                                 "0,ZERO,Row,x,,,\r\n"
                                 "3456789,,Only Name,,,,\r\n");
        CHECK(n == 3); CHECK(d.size() == 3); CHECK(d.skipped() == 2);
        CHECK(d.label(1234567) == "W1ABC Joe Smith");
        CHECK(d.label(2345678) == "LY1XYZ Jonas Petraitis, Jr");
        CHECK(d.label(3456789) == "Only Name");
        CHECK(d.label(42).empty());
        RadioIdDb::Entry e; CHECK(d.find(2345678, e)); CHECK(e.place == "Vilnius, Lithuania"); CHECK(e.callsign == "LY1XYZ");
    }
    {   // semicolons, a BOM, shuffled and differently named columns, lower case
        RadioIdDb d;
        d.parse("\xEF\xBB\xBF" "Callsign;Name;DMR ID;Country\n" "OH2XX;Matti;2441234;Finland\n" "DL1AA;Hans;2629999;Germany\n");
        CHECK(d.size() == 2); CHECK(d.label(2441234) == "OH2XX Matti"); CHECK(d.label(2629999) == "DL1AA Hans");
    }
    {   // no header: id, callsign, first name, last name; tabs
        RadioIdDb d;
        d.parse("1000001\tK1AAA\tAnn\tLee\n1000002\tK1BBB\tBob\tRay\n");
        CHECK(d.size() == 2); CHECK(d.label(1000002) == "K1BBB Bob Ray");
    }
    {   // a repeated id: the later row wins; unsorted input; a huge file stays fast
        RadioIdDb d;
        d.parse("id,callsign\n5,A\n3,B\n5,C\n");
        CHECK(d.size() == 2); CHECK(d.label(5) == "C"); CHECK(d.label(3) == "B");
        std::string big = "RADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME\n";
        for (int i = 1; i <= 300000; i++) big += std::to_string(7000000 - i * 7) + ",CALL" + std::to_string(i) + ",N" + std::to_string(i) + ",L\n";
        RadioIdDb b; CHECK(b.parse(big) == 300000);
        CHECK(b.label(7000000 - 150000 * 7) == "CALL150000 N150000 L"); CHECK(b.label(7000000 - 7).substr(0, 5) == "CALL1");
    }
    {   // not a radio id list, empty, binary rubbish: nothing loaded, no crash
        RadioIdDb d;
        CHECK(d.parse("name,city\nJoe,Boston\n") == 0);
        CHECK(d.parse("") == 0);
        CHECK(d.parse(std::string("\x00\x01\xff\x02,,\n\"unterminated", 20)) == 0);
        std::string err; CHECK(!d.loadFile("/nonexistent/radioid.csv", err)); CHECK(!err.empty());
    }
    {   // the two global slots are separate (DMR and NXDN ids overlap) and unload cleanly
        auto a = std::make_shared<RadioIdDb>(), b = std::make_shared<RadioIdDb>();
        a->parse("id,callsign\n77,DMRCALL\n"); b->parse("id,callsign\n77,NXDNCALL\n");
        radioIdInstall(RadioIdKind::Dmr, a); radioIdInstall(RadioIdKind::Nxdn, b);
        CHECK(radioIdLabel(RadioIdKind::Dmr, 77) == "DMRCALL"); CHECK(radioIdLabel(RadioIdKind::Nxdn, 77) == "NXDNCALL");
        radioIdInstall(RadioIdKind::Dmr, nullptr);
        CHECK(radioIdLabel(RadioIdKind::Dmr, 77).empty()); CHECK(radioIdCount(RadioIdKind::Dmr) == 0); CHECK(radioIdCount(RadioIdKind::Nxdn) == 1);
    }
    printf(fails ? "radioid: %d FAILED\n" : "radioid: all ok\n", fails);
    return fails ? 1 : 0;
}
