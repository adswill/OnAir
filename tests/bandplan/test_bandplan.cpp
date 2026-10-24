// Band plans (core/src/bandplan): the built-in channel tables against known values, validation messages, JSON / CSV round trips,
// the user's registry (import, replace, delete, reload) and damaged input (truncated, mutated and nonsense files must not crash).
#include "dect2/bandplan/bandplan.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace dect2::bandplan;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static const Plan* P(const Registry& r, const char* id) { const Plan* p = r.find(id); CHECK(p, "plan %s exists", id); return p; }
static const Band* B(const Plan* p, const char* service, const char* name, int nth = 0) {
    if (!p) return nullptr;
    int n = 0;
    for (const Band& b : p->bands) if (b.service == service && b.name == name && n++ == nth) return &b;
    std::printf("FAIL: %s has no band %s %s #%d\n", p->id.c_str(), service, name, nth); failures++;
    return nullptr;
}
static bool nearBy(double a, double b, double tol = 1.0) { return std::fabs(a - b) <= tol; }
// channel `n` of a numbered raster band is at `hz`
static void ch(const Band* b, int n, double hz, const char* what) {
    double c = 0;
    if (!b || !b->channelCentre(n, c) || !nearBy(c, hz)) { failures++; std::printf("FAIL: %s ch %d expected %.3f MHz, got %.3f\n", what, n, hz / 1e6, c / 1e6); }
}
static uint32_t rng = 12345;
static uint32_t rnd() { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main() {
    Registry reg;
    // ---- the built-in plans
    const char* ids[] = {"de", "gb", "fr", "it", "es", "nl", "pl", "se", "fi", "no", "dk", "lt", "lv", "ee", "ie", "at", "ch", "us", "ca", "au", "nz", "jp", "in", "cn", "br", "ae", "za", "eu"};
    for (const char* id : ids) {
        const Plan* p = P(reg, id);
        if (!p) continue;
        CHECK(p->builtin && !p->bands.empty() && !p->name.empty(), "%s is complete", id);
        for (const Band& b : p->bands) CHECK(!b.source.empty(), "%s / %s has a source note", id, b.name.c_str());
    }
    CHECK(builtinPlans().size() >= 28, "all built-in plans parse (%zu)", builtinPlans().size());

    // ---- TV rasters
    for (const char* id : {"de", "gb", "fr", "it", "es", "nl", "pl", "se", "fi", "dk", "lt", "lv", "ee", "ie", "at"}) {
        const Plan* p = P(reg, id);
        const Band* u = B(p, "tv", "UHF TV");
        ch(u, 21, 474e6, id); ch(u, 48, 690e6, id);
        CHECK(u && u->count() == 28 && u->bandwidthHz == 8e6, "%s UHF is ch 21-48 at 8 MHz", id);
        const Band* v = B(p, "tv", "VHF TV");
        ch(v, 5, 177.5e6, id); ch(v, 12, 226.5e6, id);
        CHECK(v && v->count() == 8 && v->bandwidthHz == 7e6, "%s VHF is ch 5-12 at 7 MHz", id);
    }
    { // the nominal UK channel 21 is 474.000 MHz, 8 MHz wide: 470-478
        const Band* u = B(P(reg, "gb"), "tv", "UHF TV");
        Channel c;
        CHECK(u && u->channelAt(498e6, c) && c.number == 24, "498 MHz is UK ch 24");
        CHECK(u && !u->channelAt(700e6, c), "700 MHz is no UK TV channel (cleared)");
        CHECK(u && u->channelAt(481e6, c, 4e6) && (c.number == 21 || c.number == 22), "a loose lookup finds a neighbour");
    }
    { const Plan* p = P(reg, "us");
      ch(B(p, "tv", "UHF TV"), 14, 473e6, "us"); ch(B(p, "tv", "UHF TV"), 36, 605e6, "us");
      ch(B(p, "tv", "VHF TV"), 7, 177e6, "us"); ch(B(p, "tv", "VHF TV"), 13, 213e6, "us");
      ch(B(p, "tv", "VHF TV low", 0), 2, 57e6, "us"); ch(B(p, "tv", "VHF TV low", 0), 4, 69e6, "us"); ch(B(p, "tv", "VHF TV low", 1), 5, 79e6, "us"); ch(B(p, "tv", "VHF TV low", 1), 6, 85e6, "us");
      const Band* fm = B(p, "fm", "FM broadcast");
      ch(fm, 200, 87.9e6, "us fm"); ch(fm, 300, 107.9e6, "us fm");
      CHECK(fm && fm->count() == 101, "US FM has channels 200-300");
      const Band* mw = B(p, "am", "MW");
      CHECK(mw && mw->count() == 117 && nearBy(mw->list().front().centreHz, 540e3) && nearBy(mw->list().back().centreHz, 1700e3), "US MW 540-1700 kHz, 10 kHz");
      CHECK(!p->bandsFor("tv", "ATSC").empty() && p->bandsFor("tv", "DVB-T2").empty(), "US TV bands are ATSC, not DVB-T2"); }
    { const Plan* p = P(reg, "jp");
      ch(B(p, "tv", "UHF TV"), 13, 473142857, "jp"); ch(B(p, "tv", "UHF TV"), 52, 707142857, "jp");
      const Band* fm = B(p, "fm", "FM broadcast");
      CHECK(fm && nearBy(fm->startHz, 76e6) && nearBy(fm->endHz, 95e6), "Japan FM is 76-95 MHz"); }
    { const Plan* p = P(reg, "br");
      ch(B(p, "tv", "VHF TV"), 7, 177142857, "br"); ch(B(p, "tv", "UHF TV"), 14, 473142857, "br"); ch(B(p, "tv", "UHF TV"), 51, 695142857, "br");
      // amateur bands from Anatel Ato 926/2024 Tabela I, not the US 47 CFR 97 limits
      const Band* h70 = B(p, "ham", "70 cm");
      CHECK(h70 && nearBy(h70->startHz, 430e6) && nearBy(h70->endHz, 440e6), "Brazil 70 cm is 430-440 MHz");
      const Band* h60 = B(p, "ham", "60 m");
      CHECK(h60 && nearBy(h60->startHz, 5351.5e3) && nearBy(h60->endHz, 5366.5e3), "Brazil 60 m is 5351.5-5366.5 kHz");
      const Band* h33 = B(p, "ham", "33 cm"); const Band* h33u = B(p, "ham", "33 cm (upper)");
      CHECK(h33 && h33u && nearBy(h33->endHz, 907.5e6) && nearBy(h33u->startHz, 915e6) && !h33->contains(910e6) && !h33u->contains(910e6), "Brazil 33 cm skips 907.5-915 MHz");
      CHECK(B(p, "ham", "2200 m") && B(p, "ham", "630 m") && B(p, "ham", "1.25 m"), "Brazil has the 2200 m, 630 m and 1.25 m bands");
      // FM numbering of the Anatel PBFM: channel n at 87.9 + (n - 200) x 0.2 MHz, eFM 141-197, RadCom 198-200, FM 201-300
      const Band* fm = B(p, "fm", "FM broadcast"); const Band* rc = B(p, "fm", "FM community (RadCom)"); const Band* ef = B(p, "fm", "FM extended (eFM)");
      ch(fm, 201, 88.1e6, "br fm"); ch(fm, 300, 107.9e6, "br fm"); ch(rc, 198, 87.5e6, "br radcom"); ch(rc, 200, 87.9e6, "br radcom"); ch(ef, 141, 76.1e6, "br efm"); ch(ef, 196, 87.1e6, "br efm"); ch(ef, 197, 87.3e6, "br efm");
      CHECK(fm && rc && ef && fm->count() == 100 && rc->count() == 3 && ef->count() == 57, "Brazil FM has 100 + 3 + 57 channels");
      CHECK(p->firstBand("fm") == fm, "the first Brazilian FM band is 88-108 MHz (the listen wizard starts there)"); }
    { const Plan* p = P(reg, "au");
      const Band* v = B(p, "tv", "VHF TV");
      Channel c;
      CHECK(v && v->count() == 8 && v->channelAt(205.5e6, c) && c.label == "9A", "Australia has channel 9A at 205.5 MHz");
      ch(B(p, "tv", "UHF TV"), 28, 530e6, "au"); ch(B(p, "tv", "UHF TV"), 51, 691e6, "au");
      CHECK(B(p, "tv", "UHF TV")->bandwidthHz == 7e6, "Australia UHF is 7 MHz"); }
    { const Plan* p = P(reg, "cn");
      ch(B(p, "tv", "VHF TV"), 6, 171e6, "cn"); ch(B(p, "tv", "VHF TV"), 12, 219e6, "cn");
      ch(B(p, "tv", "UHF TV", 0), 13, 474e6, "cn"); ch(B(p, "tv", "UHF TV", 0), 24, 562e6, "cn"); ch(B(p, "tv", "UHF TV", 1), 25, 610e6, "cn"); ch(B(p, "tv", "UHF TV", 1), 48, 794e6, "cn");
      CHECK(!p->verified, "the China plan is marked as not fully verified"); }
    // every numbered raster band: channel <-> frequency both ways, for all plans
    for (const Plan& p : reg.plans()) for (const Band& b : p.bands) {
        if (b.channels.empty() && b.stepHz > 0 && b.count() > 0 && b.count() < 5000) {
            for (const Channel& c : b.list()) {
                Channel back;
                if (!b.channelAt(c.centreHz, back) || !nearBy(back.centreHz, c.centreHz, 0.01) || back.number != c.number) { failures++; std::printf("FAIL: %s / %s channel %d does not map back\n", p.id.c_str(), b.name.c_str(), c.number); break; }
                if (b.firstChannel >= 0) { double hz; if (!b.channelCentre(c.number, hz) || !nearBy(hz, c.centreHz, 0.01)) { failures++; std::printf("FAIL: %s / %s number %d\n", p.id.c_str(), b.name.c_str(), c.number); break; } }
                if (c.centreHz - b.bandwidthHz / 2 < b.startHz - 5 || c.centreHz + b.bandwidthHz / 2 > b.endHz + 5) { failures++; std::printf("FAIL: %s / %s channel %d sticks out of the band\n", p.id.c_str(), b.name.c_str(), c.number); break; }
            }
        }
    }

    // ---- other services
    { const Plan* p = P(reg, "de");
      const Band* dab = B(p, "dab", "DAB Band III");
      Channel c;
      CHECK(dab && dab->count() == 41 && dab->channelAt(174.928e6, c) && c.label == "5A" && dab->channelAt(239.2e6, c) && c.label == "13F" && dab->channelAt(224.096e6, c) && c.label == "12N", "DAB band III 5A to 13F and the N blocks");
      const Band* mw = B(p, "am", "MW");
      CHECK(mw && mw->count() == 120 && nearBy(mw->list().front().centreHz, 531e3) && nearBy(mw->list().back().centreHz, 1602e3), "MW 531-1602 kHz, 9 kHz");
      const Band* lw = B(p, "am", "LW");
      CHECK(lw && lw->channelAt(198e3, c) && lw->channelAt(234e3, c), "LW 198 and 234 kHz are on the 9 kHz raster");
      const Band* a25 = B(p, "air", "Airband 25 kHz");
      CHECK(a25 && a25->count() == 760 && nearBy(a25->list().back().centreHz, 136.975e6), "airband 25 kHz: 118.000-136.975");
      const Band* a833 = B(p, "air", "Airband 8.33 kHz");
      CHECK(a833 && a833->count() == 2278, "airband 8.33 kHz has 2278 channels (%d)", a833 ? a833->count() : 0);
      const Band* pm = B(p, "pmr", "PMR446 analogue");
      CHECK(pm && pm->count() == 8, "PMR446 analogue has 8 channels"); ch(pm, 8, 446.09375e6, "pmr");
      ch(B(p, "pmr", "PMR446 digital"), 16, 446.196875e6, "pmr digital");
      const Band* lpd = B(p, "pmr", "LPD433");
      CHECK(lpd && lpd->count() == 69, "LPD433 has 69 channels"); ch(lpd, 69, 434.775e6, "lpd");
      const Band* m1 = B(p, "marine", "Marine VHF ch 1-28");
      ch(m1, 16, 156.8e6, "marine"); ch(B(p, "marine", "Marine VHF ch 60-88"), 70, 156.525e6, "marine");
      const Band* ais = B(p, "marine", "AIS");
      CHECK(ais && ais->count() == 2 && ais->channelAt(162.025e6, c) && c.label.find("AIS 2") == 0, "AIS 2 is on 162.025 MHz");
      const Band* h20 = B(p, "ham", "20 m");
      CHECK(h20 && h20->count() == 0 && h20->contains(14.2e6) && nearBy(h20->startHz, 14e6) && nearBy(h20->endHz, 14.35e6), "20 m is a plain range 14.000-14.350");
      const Band* h2 = B(p, "ham", "2 m");
      CHECK(h2 && nearBy(h2->endHz, 146e6), "Region 1 2 m ends at 146 MHz");
      CHECK(nearBy(B(P(reg, "us"), "ham", "2 m")->endHz, 148e6) && nearBy(B(P(reg, "au"), "ham", "40 m")->endHz, 7.3e6), "Region 2 / Australia limits"); }

    // ---- defaults from the language and locale
    CHECK(reg.defaultFor("de", "") == "de" && reg.defaultFor("lt", "en_US.UTF-8") == "lt" && reg.defaultFor("ja", "") == "jp" && reg.defaultFor("zh", "") == "cn", "language picks a plan");
    CHECK(reg.defaultFor("en", "en_GB.UTF-8") == "gb" && reg.defaultFor("en", "en_US.UTF-8") == "us" && reg.defaultFor("en", "en_AU") == "au" && reg.defaultFor("en", "") == "" && reg.defaultFor("hu", "") == "eu", "locale region picks a plan");
    CHECK(reg.defaultFor("pt_PT", "") == "eu" && reg.defaultFor("pt-PT", "") == "eu" && reg.defaultFor("pt_PT.UTF-8", "") == "eu" && reg.defaultFor("pt", "") == "br" && reg.defaultFor("pt-BR", "") == "br",
          "Portuguese (Portugal) picks the CEPT plan, other Portuguese the Brazilian one");

    // ---- JSON and CSV round trips of every built-in plan
    for (const Plan& p : builtinPlans()) {
        Plan a, b;
        std::string err;
        const std::string js = toJson(p), cs = toCsv(p);
        CHECK(parseJson(js, a, err), "%s JSON reads back: %s", p.id.c_str(), err.c_str());
        CHECK(toJson(a) == js, "%s JSON round trip is exact", p.id.c_str());
        CHECK(parseCsv(cs, b, err), "%s CSV reads back: %s", p.id.c_str(), err.c_str());
        CHECK(toJson(b) == js, "%s CSV round trip gives the same plan", p.id.c_str());
    }

    // ---- validation messages
    auto bad = [&](const std::string& text, const char* needle, bool csv = false) {
        Plan p;
        std::string err;
        const bool ok = csv ? parseCsv(text, p, err) : parseJson(text, p, err);
        CHECK(!ok && !err.empty(), "rejected: %s", needle);
        CHECK(err.find(needle) != std::string::npos, "message for \"%s\" mentions \"%s\": got \"%s\"", text.substr(0, 40).c_str(), needle, err.c_str());
    };
    bad("", "not valid JSON"); bad("[]", "JSON object"); bad("{", "not valid JSON"); bad("{}", "no \"bands\"");
    bad(R"({"name":"x"})", "no \"bands\""); bad(R"({"name":"x","bands":[]})", "no bands");
    bad(R"({"name":"x","bands":[{"name":"a","start_hz":2e6,"end_hz":1e6}]})", "must be above");
    bad(R"({"name":"x","bands":[{"name":"a","start_hz":"1","end_hz":2}]})", "must be a number");
    bad(R"({"name":"x","bands":[{"name":"a","start_hz":1e6,"end_hz":2e6,"service":"radio"}]})", "unknown service");
    bad(R"({"name":"x","bands":[{"name":"a","start_hz":470,"end_hz":862,"step_hz":0.001}]})", "limit");
    bad(R"({"name":"x","bands":[{"name":"a","start_hz":470e6,"end_hz":862e6,"step_hz":8e6,"first_centre_hz":100e6}]})", "outside the band");
    bad(R"({"name":"x","bands":[{"name":"a","start_hz":1e6,"end_hz":2e6,"first_channel":3}]})", "need step_hz");
    bad(R"({"name":"x","bands":[{"name":"a","start_hz":1e6,"end_hz":9e12}]})", "300 GHz");
    bad(R"({"name":"x","bands":[{"start_hz":1e6,"end_hz":2e6}]})", "has no name");
    bad(R"({"name":"x","countries":["Germany"],"bands":[{"name":"a","start_hz":1e6,"end_hz":2e6}]})", "country");
    bad("band,start_hz\nA,1\n", "end_hz", true); bad("band,start_hz,end_hz\nA,1e6,abc\n", "not a number", true); bad("# name: x\nband,start_hz,end_hz\nA,5e6,1e6\n", "must be above", true);
    bad("# name: x\nband,start_hz,end_hz\n\"A,1e6,2e6\n", "not closed", true); bad("# name: x\n", "no header", true);
    bad("# name: x\nband,start_hz,end_hz,channels\nA,1e6,2e6,oops\n", "label=centre_hz", true);
    { Plan p; std::string err;   // a handwritten CSV in MHz / kHz with a quoted name and a comment
      const char* csv = "# name: My town\n# country: de; at\n# source: my notes\nband,service,start_mhz,end_mhz,step_khz,bw_khz,first_channel,first_centre_mhz,modes\n\"Local, FM\",fm,87.5,108,100,,,87.5,FM\nUHF,tv,470,694,8000,8000,21,474,DVB-T2;DVB-T\n";
      CHECK(parseCsv(csv, p, err), "handwritten CSV: %s", err.c_str());
      CHECK(p.name == "My town" && p.countries.size() == 2 && p.countries[1] == "AT" && p.bands.size() == 2 && p.bands[0].name == "Local, FM" && nearBy(p.bands[1].stepHz, 8e6) && p.bands[1].modes.size() == 2, "handwritten CSV fields");
      ch(&p.bands[1], 22, 482e6, "csv"); }

    // ---- the user's registry
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "onair-bandplan-test";
    fs::remove_all(dir);
    {
        Registry r;
        r.setUserDir(dir.string());
        std::string id, err;
        bool replaced = true;
        const char* csv = "# name: My town\nband,service,start_hz,end_hz,step_hz,first_centre_hz\nFM,fm,87.5e6,108e6,100e3,87.5e6\n";
        CHECK(r.importText(csv, id, err, &replaced), "import: %s", err.c_str());
        CHECK(id == "user-my-town" && !replaced && r.find(id) && !r.find(id)->builtin, "user plan id");
        CHECK(fs::exists(dir / "user-my-town.json") && !fs::exists(dir / "user-my-town.json.tmp"), "the plan is written atomically (no .tmp left)");
        CHECK(r.importText(csv, id, err, &replaced) && replaced && id == "user-my-town", "the same name replaces the plan");
        std::ofstream(dir / "user-broken.json") << "{\"name\": \"b\", \"bands\": [{\"name\": \"x\", \"start_hz\": 5, \"end_hz\": 1}]}";
        std::ofstream(dir / "notaplan.json") << "{}";
        std::ofstream(dir / "readme.txt") << "x";
        Registry r2;
        r2.setUserDir(dir.string());
        CHECK(r2.find("user-my-town") && !r2.find("user-broken") && !r2.find("notaplan") && r2.log().size() == 2, "reload keeps the good file and skips the damaged ones (%zu logged)", r2.log().size());
        CHECK(r2.plans().size() == builtinPlans().size() + 1, "built-in plans plus one user plan");
        const fs::path out = dir / "export.json", outc = dir / "export.csv";
        CHECK(r2.exportFile("user-my-town", out.string(), false, err) && r2.exportFile("de", outc.string(), true, err), "export: %s", err.c_str());
        Registry r3;
        r3.setUserDir((dir / "other").string());
        CHECK(r3.importFile(out.string(), id, err) && id == "user-my-town", "re-import of an export: %s", err.c_str());
        CHECK(r3.importFile(outc.string(), id, err) && r3.find(id) && r3.find(id)->bands.size() == r3.find("de")->bands.size(), "a built-in plan exported as CSV imports as a user plan: %s", err.c_str());
        CHECK(!r3.importFile((dir / "nope.csv").string(), id, err) && err.find("cannot open") == 0, "missing file: %s", err.c_str());
        CHECK(!r3.remove("de", err) && err.find("built-in") != std::string::npos, "built-in plans cannot be deleted");
        CHECK(!r3.remove("user-nothing", err), "unknown plan");
        CHECK(r2.remove("user-my-town", err) && !fs::exists(dir / "user-my-town.json") && !r2.find("user-my-town"), "delete: %s", err.c_str());
        Registry nodir;
        CHECK(!nodir.importText(csv, id, err), "no folder, no import (nothing stored)");
        CHECK(slug("My Plan (DE) \xC3\xA4") == "my-plan-de" && slug("***") == "plan", "slug");
    }
    fs::remove_all(dir);

    // ---- damaged input: truncated at every length, bytes mutated, nonsense; nothing may crash or hang
    {
        const Plan* de = reg.find("de");
        const std::string js = toJson(*de), cs = toCsv(*de);
        int accepted = 0;
        for (size_t n = 0; n < js.size(); n += 7) { Plan p; std::string e; accepted += parseJson(js.substr(0, n), p, e); }
        for (size_t n = 0; n < cs.size(); n += 7) { Plan p; std::string e; accepted += parseCsv(cs.substr(0, n), p, e); }
        for (int i = 0; i < 3000; i++) {
            std::string m = (i & 1) ? js : cs;
            const int hits = 1 + (int)(rnd() % 6);
            for (int k = 0; k < hits; k++) {
                const size_t at = rnd() % m.size();
                switch (rnd() % 4) { case 0: m[at] = (char)rnd(); break; case 1: m.erase(at, 1 + rnd() % 8); break; case 2: m.insert(at, 1, "\",{}[]:\n-e.0"[rnd() % 12]); break; default: m[at] = "0123456789"[rnd() % 10]; }
                if (m.empty()) m = "x";
            }
            Plan p; std::string e;
            if ((i & 1) ? parseJson(m, p, e) : parseCsv(m, p, e)) { accepted++; std::string chk; Plan q = p; CHECK(validate(q, chk), "an accepted plan validates again"); for (const Band& b : p.bands) (void)b.list(10); }
            else CHECK(!e.empty(), "a rejection has a message");
        }
        const char* junk[] = {"\xFF\xFE\x00\x01", "{\"name\":\"x\",\"bands\":[{\"name\":\"a\",\"start_hz\":1e300,\"end_hz\":1e308}]}", "{\"name\":\"x\",\"bands\":[null,1,\"a\",[]]}",
                              "{\"name\":1,\"bands\":{}}", "{\"name\":\"x\",\"bands\":[{\"name\":\"a\",\"start_hz\":1e6,\"end_hz\":2e6,\"channels\":[1,2]}]}", "{\"name\":\"x\",\"bands\":[{\"name\":\"a\",\"start_hz\":-5,\"end_hz\":2e6}]}",
                              "{\"name\":\"x\",\"bands\":[{\"name\":\"a\",\"start_hz\":1e6,\"end_hz\":2e6,\"step_hz\":NaN}]}", ",,,,\n,,,\n", "\"\"\"\"", "#\n#:\n"};
        for (const char* j : junk) { Plan p; std::string e; const bool a = parseJson(j, p, e), b = parseCsv(j, p, e); (void)a; (void)b; }
        std::string big(5u << 20, 'x'); Plan p; std::string e;
        CHECK(!parseJson(big, p, e) && !parseCsv(big, p, e), "a file over 4 MB is refused");
        std::string deep(100000, '['); CHECK(!parseJson(deep, p, e), "deeply nested JSON is refused");
        std::printf("fuzz: %d accepted\n", accepted);
    }

    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("bandplan: all checks passed\n");
    return 0;
}
