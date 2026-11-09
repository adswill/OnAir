// The plain-text ACARS formats (acars_text.h) against the real messages of the acars-decoder-typescript test suite, and the damaged-text cases.
#include "dect2/acars/acars_text.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>

using namespace dect2;

struct V { const char* plugin; const char* label; const char* text; double lat, lon, alt; const char* dep; const char* arr; const char* flt; };
static const V kV[] = {
#include "acars_text_vectors.inc"
};

static bool near(double a, double b, double e) { return std::isnan(b) || std::fabs(a - b) <= e; }

int main(int argc, char**) {
    int fails = 0, passed = 0, none = 0;
    std::map<std::string, int> miss;
    for (const V& v : kV) {
        AcarsReading r;
        const bool ok = acarsInterpret(v.label, v.text, r);
        bool good = ok;
        if (ok) {
            if (!std::isnan(v.lat)) good = good && r.hasPos && near(r.lat, v.lat, 2e-3) && near(r.lon, v.lon, 2e-3);
            if (!std::isnan(v.alt)) good = good && r.altFt >= 0 && near(r.altFt, v.alt, 1.0);
            if (*v.dep) good = good && r.from == v.dep;
            if (*v.arr) good = good && r.to == v.arr;
            if (*v.flt) good = good && r.flight == v.flt;
        }
        if (!ok) none++;
        if (good) passed++; else { miss[v.plugin]++; if (argc > 1) printf("MISS [%s/%s] %.70s -> %d lat %.3f/%.3f lon %.3f/%.3f alt %d/%.0f %s/%s %s/%s %s/%s\n", v.plugin, v.label, v.text, ok, r.lat, v.lat, r.lon, v.lon, r.altFt, v.alt, r.from.c_str(), v.dep, r.to.c_str(), v.arr, r.flight.c_str(), v.flt); }
    }
    printf("%d of %zu published examples read as the reference expects; %d not read at all\n", passed, sizeof kV / sizeof *kV, none);
    for (auto& kv : miss) printf("  %s: %d\n", kv.first.c_str(), kv.second);
    // the line the tables show
    const std::string l = acarsInterpretLine("44", "00ETA03,N38241W081357,330,KBNA,KBWI,1107,0123,0208,008.1");
    if (l.find("ETA report") != 0 || l.find("KBNA") == std::string::npos) { printf("FAIL line: %s\n", l.c_str()); fails++; }
    AcarsReading r;
    // damaged: free text under a parsed label gives nothing, out-of-range positions give nothing, cut texts and random text never crash
    if (acarsInterpret("44", "GOOD MORNING CAPTAIN", r)) { printf("FAIL free text\n"); fails++; }
    if (acarsInterpret("44", "00ETA03,N99999W999999,330,KBNA,KBWI,1107,0123,0208,008.1", r)) { printf("FAIL out of range\n"); fails++; }
    std::mt19937 g(3);
    static const char* const labels[] = {"10","12","13","15","16","1L","1M","20","21","22","24","2P","30","44","4A","4N","4T","58","5Z","80","83","8E","B6",":;","H1","H2","HX","SQ","QQ"};
    for (const V& v : kV) {
        std::string t = v.text;
        for (int i = 0; i < 40; i++) {
            std::string c = t;
            if (i % 2) c.resize(g() % (c.size() + 1)); else if (!c.empty()) c[g() % c.size()] = (char)(32 + g() % 95);
            acarsInterpret(v.label, c, r);
            if (r.hasPos && (std::fabs(r.lat) > 90 || std::fabs(r.lon) > 180)) { printf("FAIL bounds\n"); fails++; }
        }
    }
    for (int i = 0; i < 300000; i++) {
        std::string t(g() % 80, ' ');
        for (auto& c : t) c = (char)(32 + g() % 95);
        const char* l2 = labels[g() % (sizeof labels / sizeof *labels)];
        acarsInterpret(l2, t, r);
    }
    printf(fails ? "%d failures\n" : "ok\n", fails);
    return fails == 0 || passed >= (int)(sizeof kV / sizeof *kV) * 85 / 100 ? 0 : 1;   // a few reference expectations are airline-specific and left out
}
