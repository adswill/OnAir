// Airport table: the built-in data, frequency normalisation, search, near, the airband channels of an airport, grouping into captures,
// and the OurAirports CSV import with damaged input.
#include "dect2/airports/airports.h"
#include <cmath>
#include <cstdio>
#include <set>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    // ---- frequency normalisation
    CHECK(airportKhz(118.5) == 118500, "118.5");
    CHECK(airportKhz(118.505) == 118505, "8.33 name 118.505");
    CHECK(airportKhz(118.510) == 118510 && airportKhz(118.515) == 118515, "8.33 names");
    CHECK(airportKhz(121.9) == 121900 && airportKhz(121.5) == 121500, "25 kHz");
    CHECK(airportKhz(118.52) == 118525 && airportKhz(118.998) == 119000 && airportKhz(118.502) == 118500, "snap to 25 kHz: %d", airportKhz(118.52));
    CHECK(airportKhz(118.512) == 0 && airportKhz(118.513) == 0, "off raster rejected: %d", airportKhz(118.512));
    CHECK(airportKhz(243.0) == 243000 && airportKhz(123.45) == 123450, "outside the airband kept");
    CHECK(airportKhz(0) == 0 && airportKhz(-5) == 0 && airportKhz(NAN) == 0 && airportKhz(INFINITY) == 0 && airportKhz(9000) == 0, "garbage");
    // the channel naming agrees with the airband mode
    {
        double f; bool n;
        CHECK(airbandParse("118.505", f, n) && n && std::fabs(f - 118.5e6) < 1, "118.505 -> 118.500 8.33");
        CHECK(airbandParse("118.510", f, n) && n && std::fabs(f - 118.508333e6) < 1, "118.510 -> 118.50833");
    }
    // ---- built-in table
    const AirportDb& db = AirportDb::builtin();
    CHECK(db.size() > 10000, "airports %zu", db.size());
    size_t withFreq = 0, nfreq = 0, bad = 0;
    for (const auto& a : db.all()) {
        withFreq += a.airbandCount() > 0;
        for (const auto& f : a.freqs) {
            nfreq++;
            if (f.airband()) { double hz; bool n; char b[24]; snprintf(b, sizeof b, "%d.%03d", f.khz / 1000, f.khz % 1000); if (!airbandParse(b, hz, n) || !airbandOnRaster(hz, n)) bad++; }
        }
    }
    printf("  %zu airports, %zu with airband frequencies, %zu frequencies\n", db.size(), withFreq, nfreq);
    CHECK(withFreq > 9000 && nfreq > 25000 && bad == 0, "airband frequencies off raster: %zu", bad);
    const Airport* lhr = db.find("EGLL");
    CHECK(lhr && lhr == db.find("lhr") && lhr->name.find("Heathrow") != std::string::npos && lhr->country == "GB", "Heathrow by ICAO and IATA");
    CHECK(lhr && std::fabs(lhr->latDeg - 51.47) < 0.05 && std::fabs(lhr->lonDeg + 0.46) < 0.05, "Heathrow position");
    CHECK(lhr && lhr->airbandCount() >= 8, "Heathrow frequencies %zu", lhr ? lhr->airbandCount() : 0);
    CHECK(!db.find("") && !db.find("ZZZZZZ"), "unknown codes");
    // ---- search
    {
        auto h = db.search("heathrow");
        CHECK(!h.empty() && db.all()[h[0].index].icao == "EGLL", "heathrow first");
        h = db.search("EGLL");
        CHECK(h.size() >= 1 && db.all()[h[0].index].icao == "EGLL", "ICAO");
        h = db.search("lhr");
        CHECK(h.size() >= 1 && db.all()[h[0].index].icao == "EGLL", "IATA");
        h = db.search("london");
        bool gatwick = false, heath = false;
        for (const auto& x : h) { gatwick |= db.all()[x.index].icao == "EGKK"; heath |= db.all()[x.index].icao == "EGLL"; }
        CHECK(gatwick && heath, "london finds Gatwick and Heathrow (%zu hits)", h.size());
        h = db.search("vilnius");
        CHECK(!h.empty() && db.all()[h[0].index].icao == "EYVI", "Vilnius: %s", h.empty() ? "-" : db.all()[h[0].index].icao.c_str());
        h = db.search("  VILNIUS  int ");
        CHECK(!h.empty() && db.all()[h[0].index].icao == "EYVI", "several words, any case");
        CHECK(db.search("").empty() && db.search("   ").empty() && db.search("qqqqzzzz").empty(), "nothing");
        const double me[2] = {54.9, 23.9};   // Kaunas: nearer than the other Lithuanian airports with the same name word
        h = db.search("kaunas", 5, me);
        CHECK(!h.empty() && h[0].km >= 0 && h[0].km < 30, "Kaunas from Kaunas");
        h = db.search("a", 10);
        CHECK(h.size() == 10, "short query is capped");
        h = db.search("paris", 100, nullptr, true);
        for (const auto& x : h) CHECK(db.all()[x.index].airbandCount() > 0, "withFreqOnly");
    }
    // ---- near
    {
        const auto n = db.nearby(54.64, 25.28, 60);
        CHECK(!n.empty() && db.all()[n[0].index].icao == "EYVI" && n[0].km < 10, "nearest to Vilnius");
        for (size_t i = 1; i < n.size(); i++) CHECK(n[i].km >= n[i - 1].km && n[i].km <= 60, "sorted and inside");
        CHECK(db.nearby(54.64, 25.28, 0.1, 5, true).size() <= 1, "tiny radius");
        CHECK(db.nearby(-60, 0, 10).empty(), "the sea");
        CHECK(db.nearby(54.64, 25.28, 5000, 7).size() == 7, "capped");
        CHECK(std::fabs(airportDistanceKm(0, 0, 0, 1) - 111.19) < 0.1 && airportDistanceKm(10, 10, 10, 10) == 0, "distance");
    }
    // ---- the channels of an airport, and their grouping
    {
        int skipped = 0;
        const auto ch = airportAirbandChannels(*lhr, &skipped);
        CHECK(ch.size() >= 8 && ch.size() <= lhr->airbandCount(), "channels %zu of %zu", ch.size(), lhr->airbandCount());
        CHECK(skipped >= 1, "Heathrow lists VOR-ATIS frequencies outside the airband: %d", skipped);
        std::set<long> seen;
        std::set<std::string> labels;
        bool sorted = true;
        for (size_t i = 0; i < ch.size(); i++) {
            CHECK(ch[i].label.compare(0, 5, "EGLL ") == 0, "label %s", ch[i].label.c_str());
            CHECK(airbandOnRaster(ch[i].freqHz, ch[i].is833), "on raster %.1f", ch[i].freqHz);
            CHECK(seen.insert(std::lround(ch[i].freqHz)).second && labels.insert(ch[i].label).second, "duplicate %s", ch[i].label.c_str());
            if (i && ch[i].freqHz < ch[i - 1].freqHz) sorted = false;
        }
        CHECK(sorted, "sorted by frequency");
        bool tower = false;
        for (const auto& c : ch) tower |= c.label.find("Tower") != std::string::npos;
        CHECK(tower, "a Tower channel");
        std::vector<double> f;
        for (const auto& c : ch) f.push_back(c.freqHz);
        const double span = airbandUsableSpanHz(2.4e6);
        CHECK(span > 1.5e6 && span < 2.2e6, "usable span at 2.4 Msps: %.0f", span);
        CHECK(airbandUsableSpanHz(8e6) > 7e6 && airbandUsableSpanHz(100e3) == 0, "usable span scales");
        const auto g = airbandPlanGroups(f, span);
        size_t total = 0;
        for (const auto& x : g) { total += x.members.size(); CHECK(x.hiHz - x.loHz <= span + 1, "group fits"); }
        CHECK(total == f.size() && !g.empty(), "all channels in a group");
        const auto g8 = airbandPlanGroups(f, airbandUsableSpanHz(20e6));
        CHECK(g8.size() == 1, "one group at 20 Msps: %zu", g8.size());
        // a small airport: the 8.33 kHz channels keep their flags
        Airport t;
        t.icao = "XXXX";
        t.freqs = {{"TWR", "", 118505}, {"GND", "", 121900}, {"CTAF", "", 121900}, {"ATIS", "", 118510}, {"TWR", "", 119700}, {"TWR", "second", 120700}, {"MISC", "", 243000}};
        int sk = 0;
        const auto c2 = airportAirbandChannels(t, &sk);
        CHECK(c2.size() == 5 && sk == 1, "synthetic: %zu channels, %d skipped", c2.size(), sk);
        CHECK(c2.size() == 5 && c2[0].is833 && c2[1].is833 && !c2[2].is833 && !c2[4].is833 && std::fabs(c2[0].freqHz - 118.5e6) < 1 && std::fabs(c2[1].freqHz - 118.508333e6) < 1, "8.33 flags");
        bool both = false;
        for (const auto& c : c2) both |= c.label == "XXXX Ground/CTAF";
        CHECK(both, "one frequency, two roles");
        CHECK(c2.size() == 5 && c2[0].label == "XXXX Tower 1" && c2[2].label == "XXXX Tower 2" && c2[3].label == "XXXX Tower 3", "numbered");
        const Airport none;
        CHECK(airportAirbandChannels(none).empty(), "no frequencies");
    }
    // ---- text round trip and damaged text
    {
        AirportDb d;
        const auto r = d.parseTsv(db.toTsv());
        CHECK(r.airports == db.size() && r.freqs == nfreq && r.skipped == 0, "round trip: %zu/%zu, %zu skipped", r.airports, db.size(), r.skipped);
        AirportDb e;
        const auto r2 = e.parseTsv("# c\nA\tAAAA\tAAA\tName\tCity\tXX\t10.0\t20.0\t5\tM\t1\nF\tTWR\t118500\tdesc\nF\tbad\n"
                                   "A\tBBBB\t\tNo position\tC\tXX\tnan\t20\t0\tS\t0\nF\tTWR\t121500\t\nF\tTWR\t999999999\t\nX\tnonsense\n\n"
                                   "A\tCCCC\n"
                                   "A\tDDDD\t\tD\tDC\tXX\t95\t20\t0\tS\t0\nA\tEEEE\t\tE\tEC\tXX\t-10.5\t179.9\t-20\tS\t0\nF\tATIS\t118505\r\n");
        CHECK(e.size() == 2 && r2.airports == 2 && r2.freqs == 2 && r2.skipped >= 6, "damaged: %zu airports, %zu freqs, %zu skipped", r2.airports, r2.freqs, r2.skipped);
        CHECK(e.size() == 2 && e.all()[0].freqs.size() == 1 && e.all()[1].freqs.size() == 1 && e.all()[1].elevFt == -20, "frequencies stay with their airport");
        AirportDb z;
        CHECK(z.parseTsv("").airports == 0 && z.empty() && z.search("x").empty() && z.nearby(0, 0, 10).empty(), "empty table");
    }
    // ---- OurAirports CSV import
    {
        const std::string ap =
            "\"id\",\"ident\",\"type\",\"name\",\"latitude_deg\",\"longitude_deg\",\"elevation_ft\",\"continent\",\"iso_country\",\"iso_region\",\"municipality\",\"scheduled_service\",\"icao_code\",\"iata_code\",\"gps_code\",\"local_code\",\"home_link\",\"wikipedia_link\",\"keywords\"\r\n"
            "1,\"EGXX\",\"large_airport\",\"Test \"\"Field\"\", North\",51.5,-0.5,83,\"EU\",\"GB\",\"GB-ENG\",\"London, Greater\",\"yes\",\"EGXX\",\"XXX\",\"EGXX\",,,\r\n"
            "2,\"00AA\",\"small_airport\",\"Ranch\",38.7,-101.4,3435,\"NA\",\"US\",\"US-KS\",\"Leoti\",\"no\",,,\"00AA\",\"00AA\",,,\n"
            "3,\"00AB\",\"small_airport\",\"Strip with radio\",39.0,-100.0,100,\"NA\",\"US\",\"US-KS\",\"X\",\"no\",,,\"00AB\",\"00AB\",,,\n"
            "4,\"00H1\",\"heliport\",\"Pad\",40,-70,11,\"NA\",\"US\",\"US-PA\",\"Y\",\"no\",,,\"00H1\",\"00H1\",,,\n"
            "5,\"00CL\",\"closed\",\"Gone\",40,-70,11,\"NA\",\"US\",\"US-PA\",\"Y\",\"no\",,,\"00CL\",\"00CL\",,,\n"
            "6,\"BADP\",\"medium_airport\",\"No position\",,,11,\"NA\",\"US\",\"US-PA\",\"Y\",\"no\",,,\"BADP\",\"BADP\",,,\n"
            "7,\"EYSA\",\"medium_airport\",\"Siauliai\",55.9,23.4,420,\"EU\",\"LT\",\"LT-SA\",\"Siauliai\",\"no\",\"EYSA\",\"SQQ\",\"EYSA\",,,,\n"
            "short\n";
        const std::string fr =
            "\"id\",\"airport_ref\",\"airport_ident\",\"type\",\"description\",\"frequency_mhz\"\n"
            "10,1,\"EGXX\",\"TWR\",\"Test Tower\",118.505\n"
            "11,1,\"EGXX\",\"ATIS\",\"ATIS\",113.75\n"
            "12,1,\"EGXX\",\"GND\",,121.9\n"
            "13,1,\"EGXX\",\"GND\",,not a number\n"
            "14,1,\"EGXX\",\"UNIC\",\"Unicom\",118.512\n"
            "15,3,\"00AB\",\"CTAF\",\"CTAF\",122.9\n"
            "16,99,\"NONE\",\"TWR\",,118.0\n"
            "17\n";
        AirportDb d;
        const auto r = d.importOurAirports(ap, fr);
        CHECK(d.size() == 3 && r.airports == 3, "imported %zu airports", d.size());
        const Airport* a = d.find("EGXX");
        CHECK(a && a->name == "Test \"Field\", North" && a->city == "London, Greater" && a->scheduled && a->kind == 'L' && a->iata == "XXX", "quoted fields");
        CHECK(a && a->freqs.size() == 3, "frequencies of EGXX: %zu (bad number and off-raster dropped)", a ? a->freqs.size() : 0);
        CHECK(a && a->freqs.size() == 3 && a->freqs[0].type == "ATIS" && a->freqs[0].khz == 113750 && a->freqs[2].khz == 118505 && a->freqs[2].desc == "Test Tower", "values");
        CHECK(d.find("00AB") && d.find("00AB")->freqs.size() == 1 && d.find("00AB")->freqs[0].desc.empty(), "airport without a code uses ident; description equal to type dropped");
        CHECK(!d.find("00AA"), "a small airport without service or frequency is left out");
        CHECK(!d.find("00H1") && !d.find("00CL") && !d.find("BADP"), "heliport, closed and positionless left out");
        CHECK(d.find("SQQ") && d.find("SQQ")->icao == "EYSA", "IATA lookup of an airport with no frequencies (medium)");
        AirportDb e;
        CHECK(e.importOurAirports("garbage,garbage\nx,y\n", "").airports == 0 && e.empty(), "not an OurAirports file");
        CHECK(e.importOurAirports("", "").airports == 0, "empty files");
        // the exported text of an import loads again unchanged
        AirportDb f;
        const auto r3 = f.parseTsv(d.toTsv());
        CHECK(r3.airports == 3 && r3.freqs == r.freqs, "import -> text -> load");
    }
    {   // #411: a row found by the IATA code says so; the ICAO label does not contain it
        Airport t; t.icao = "HHAS"; t.iata = "ASM";
        CHECK(airportRowName(t, "HHAS Tower", "asm") == "HHAS Tower (ASM)", "IATA search names the code");
        CHECK(airportRowName(t, "HHAS Tower", "HHAS") == "HHAS Tower", "ICAO search keeps the label");
        Airport n; n.icao = "EYXX";
        CHECK(airportRowName(n, "EYXX Tower", "ASM") == "EYXX Tower", "no IATA code: the label");
    }
    return testSummary("airports");
}
