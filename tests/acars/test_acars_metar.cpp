// Airport weather from datalink text (acars_metar.h): METAR / SPECI / TAF found inside message text, European and US forms.
#include "dect2/acars/acars_metar.h"
#include <cmath>
#include "../common/test_util.h"
using namespace dect2;

int main() {
    {   auto v = parseWxReports("/WXRQ EYVI\nMETAR EYVI 101350Z 24012G22KT 9999 FEW030 BKN045 08/03 Q1012 NOSIG=");
        CHECK(v.size() == 1, "one report %zu", v.size());
        if (!v.empty()) { const auto& r = v[0];
            CHECK(r.station == "EYVI" && r.kind == "METAR" && r.day == 10 && r.hour == 13 && r.minute == 50, "head");
            CHECK(r.hasWind && r.windDir == 240 && r.windKt == 12 && r.gustKt == 22, "wind %d %d %d", r.windDir, r.windKt, r.gustKt);
            CHECK(r.hasVis && r.visM == 10000, "vis %.0f", r.visM);
            CHECK(r.hasCeiling && r.ceilingFt == 4500 && r.clouds == "FEW030 BKN045", "ceiling %d '%s'", r.ceilingFt, r.clouds.c_str());
            CHECK(r.hasTemp && r.tempC == 8 && r.dewC == 3, "temp");
            CHECK(r.hasQnh && r.qnhHpa == 1012, "qnh");
            CHECK(r.category == "VFR", "cat %s", r.category.c_str()); } }
    {   // US form, a fraction visibility, minus temperatures, inHg
        auto v = parseWxReports("KJFK 101351Z 31015KT 1 1/2SM BR OVC008 M02/M04 A2992 RMK AO2");
        CHECK(v.size() == 1 && v[0].hasVis && std::fabs(v[0].visM - 1.5 * 1609.344) < 1, "1 1/2SM %.0f", v.empty() ? 0 : v[0].visM);
        if (!v.empty()) {
            CHECK(v[0].tempC == -2 && v[0].dewC == -4, "M02/M04");
            CHECK(std::fabs(v[0].qnhHpa - 1013.2) < 0.2, "A2992 %.1f", v[0].qnhHpa);
            CHECK(v[0].category == "IFR" && v[0].ceilingFt == 800, "IFR %s %d", v[0].category.c_str(), v[0].ceilingFt);
        } }
    {   // several reports in one D-ATIS / printer message, CAVOK, VRB, MPS, VV, a SPECI and a TAF with change groups
        const char* txt = "WX FOR YOUR ROUTE\r\nMETAR EGLL 101350Z AUTO VRB03KT CAVOK 12/06 Q1020=\r\n"
                          "SPECI UUEE 101342Z 18005MPS 0300 FG VV001 M01/M01 Q1003=\r\n"
                          "TAF AMD EDDF 101100Z 1012/1118 27015KT 6000 SCT020 BKN035 TEMPO 1014/1018 3000 SHRA BKN012=";
        auto v = parseWxReports(txt);
        CHECK(v.size() == 3, "three %zu", v.size());
        if (v.size() == 3) {
            CHECK(v[0].station == "EGLL" && v[0].cavok && v[0].variable && v[0].windKt == 3 && v[0].category == "VFR", "EGLL");
            CHECK(v[1].kind == "SPECI" && v[1].windKt == 10 && v[1].visM == 300 && v[1].ceilingFt == 100 && v[1].category == "LIFR", "UUEE %d %s", v[1].windKt, v[1].category.c_str());
            CHECK(v[2].kind == "TAF" && v[2].station == "EDDF" && v[2].windKt == 15 && v[2].visM == 6000 && v[2].ceilingFt == 3500 && v[2].category.empty(), "TAF first group only: ceiling %d", v[2].ceilingFt);
        } }
    {   // not weather: a station-like word and time with nothing after it, an OOOI report, free text
        CHECK(parseWxReports("EYVI 101350Z").empty(), "bare head");
        CHECK(parseWxReports("OUT 1342 OFF 1350 ETA 1530 FUEL 123").empty(), "OOOI");
        CHECK(parseWxReports("").empty(), "empty");
        CHECK(parseWxReports("METAR").empty(), "keyword only");
        CHECK(parseWxReports("ABCD 991350Z 24012KT").empty(), "bad day");
    }
    {   // the book keeps the newest per airport, counts repeats, METAR and TAF separately
        WxBook b;
        CHECK(b.addText("METAR EYVI 101320Z 24010KT 9999 SCT040 08/03 Q1012=", 100, "ACARS") == 1, "add");
        b.addText("METAR EYVI 101350Z 25014KT 9999 BKN012 07/04 Q1011=", 200, "VDL2");
        b.addText("METAR EYVI 101320Z 24010KT 9999 SCT040 08/03 Q1012=", 300, "HFDL");   // an older one heard later
        b.addText("TAF EYVI 101100Z 1012/1112 24012KT 9999 BKN030=", 300, "ACARS");
        CHECK(b.rows().size() == 2, "rows %zu", b.rows().size());
        const auto& m = b.rows().at("EYVI|METAR");
        CHECK(m.rep.minute == 50 && m.source == "VDL2" && m.count == 3 && m.rep.category == "MVFR", "newest kept %d %s %d %s", m.rep.minute, m.source.c_str(), m.count, m.rep.category.c_str());
    }
    {   // flight category edges
        CHECK(wxCategory(true, 3000, true, 9000) == "MVFR", "3000 ft is MVFR");
        CHECK(wxCategory(true, 3100, true, 10000) == "VFR", "3100 VFR");
        CHECK(wxCategory(false, 0, true, 1500) == "LIFR", "under 1 SM");
        CHECK(wxCategory(true, 999, false, 0) == "IFR", "999 ft");
        CHECK(wxCategory(false, 0, false, 0).empty(), "unknown");
    }
    {   // noise: random characters around a report do not break it; corrupted digits drop single groups only
        std::string junk = "#$%^ 8812 A1B2 ";
        auto v = parseWxReports(junk + "METAR LYBE 101400Z 3401OKT 8000 BKN025 10/06 Q1015=" + junk);
        CHECK(v.size() == 1 && !v[0].hasWind && v[0].hasVis && v[0].hasQnh && v[0].category == "MVFR", "corrupted wind only");
    }
    return testSummary("acars_metar");
}
