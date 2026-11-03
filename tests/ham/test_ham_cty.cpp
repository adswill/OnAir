// Country file (cty.dat / wl_cty.dat / cty_wt_mod.dat) reader: longest prefix, exact calls, portable forms, zone/position/continent
// overrides, WAE-only entities, the version stamp, CRLF and hostile input. The sample below is written from the file format (marked
// SAMPLE), not copied from a release of the file.
#include "dect2/ham/ham_dxcc.h"
#include <cstdio>
#include <string>
using namespace dect2::ham;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

// SAMPLE in the cty.dat layout (west-positive longitude)
static const char* kSample =
    "Sov Mil Order of Malta:   15:  28:  EU:   41.90:   -12.43:    -1.0:  1A:\n"
    "    1A,VER20240301;\n"
    "United States:            05:  08:  NA:   37.53:    91.67:     5.0:  K:\n"
    "    AA,AB,K,N,W,=W1AW(4)[7]<10.5/20.25>{SA}~3.0~,=K1TTT,K6(3)[6],KL7(1)[1];\n"
    "Canada:                   05:  09:  NA:   44.35:    78.75:     5.0:  VE:\n"
    "    CF,VA,VE,VO,VY9(2),VE8(1)[2];\n"
    "Germany:                  14:  28:  EU:   51.00:   -10.00:    -1.0:  DL:\n"
    "    DA,DB,DC,DD,DF,DG,DH,DJ,DK,DL,DM,DN,DO,DP,DQ,DR,Y2,Y3,Y4,Y5,Y6,Y7,Y8,Y9;\n"
    "European Russia:          16:  29:  EU:   55.00:   -37.00:    -3.0:  UA:\n"
    "    R?1,R?3(17),UA,UB,UC,UD,UE,UF,UG,UH,UI;\n"
    "*Test WAE Entity:         14:  27:  EU:   50.00:     0.00:     0.0:  *TT:\n"
    "    TT;\n";

int main() {
    DxccDb d;
    std::string err;
    CHECK(d.loadCty(kSample, err) && err.empty());
    CHECK(d.entities().size() == 6);
    CHECK(d.version() == "20240301");
    // longest prefix and the entity's own zones
    DxccHit h = d.lookup("DL1ABC");
    CHECK(h.e && h.e->name == "Germany" && h.cqz == 14 && h.ituz == 28 && h.cont == "EU" && h.prefix == "DL" && h.e->lon > 9.9);
    h = d.lookup("K1ABC");
    CHECK(h.e && h.e->name == "United States" && h.cqz == 5 && h.prefix == "K" && h.e->lon < -91);
    h = d.lookup("KL7ABC");   CHECK(h.e && h.cqz == 1 && h.ituz == 1 && h.prefix == "KL7");   // KL7 beats K
    h = d.lookup("K6XYZ");    CHECK(h.cqz == 3 && h.ituz == 6);
    h = d.lookup("VE8AB");    CHECK(h.e && h.e->name == "Canada" && h.cqz == 1 && h.ituz == 2);
    h = d.lookup("VY9AB");    CHECK(h.cqz == 2 && h.ituz == 9);
    // exact calls with every override
    h = d.lookup("W1AW");
    CHECK(h.prefix == "=W1AW" && h.cqz == 4 && h.ituz == 7 && h.cont == "SA" && h.lat > 10.4 && h.lat < 10.6 && h.lon < -20.2);
    CHECK(d.lookup("K1TTT").prefix == "=K1TTT" && d.lookup("K1TTTA").prefix == "K");
    CHECK(d.lookup("w1aw").prefix == "=W1AW");   // case
    h = d.lookup("W2ABC"); CHECK(h.cont == "NA" && h.lat > 37 && h.lat < 38);   // no override: the entity's
    // portable forms
    CHECK(d.lookup("W1AW/VE3").e->name == "Canada");
    CHECK(d.lookup("VE3/W1AW").e->name == "Canada");
    CHECK(d.lookup("DL/W1AW").e->name == "Germany");
    CHECK(d.lookup("W1AW/DL").e->name == "Germany");
    CHECK(d.lookup("DL1ABC/P").e->name == "Germany" && d.lookup("DL1ABC/M").e->name == "Germany" && d.lookup("DL1ABC/QRP").e->name == "Germany");
    CHECK(d.lookup("W1AW/P").prefix == "=W1AW");   // the exact entry survives /P
    CHECK(d.lookup("DL1ABC/MM").e == nullptr && d.lookup("W1AW/MM").e == nullptr && d.lookup("W1AW/AM").e == nullptr);
    CHECK(d.lookup("K1ABC/4").prefix == "K");
    // '?' as a letter
    CHECK(d.lookup("RA1AA").e && d.lookup("RA1AA").e->name == "European Russia" && d.lookup("RZ3AA").cqz == 17);
    CHECK(d.lookup("RA2AA").e == nullptr);
    // WAE-only: found, but not a DXCC key
    h = d.lookup("TT1X");
    CHECK(h.e && h.e->wae && h.key().empty() && d.lookup("DL1ABC").key() == "Germany");
    // nothing
    CHECK(d.lookup("").e == nullptr && d.lookup("   ").e == nullptr && d.lookup("/").e == nullptr && d.lookup("ZZ9ZZ").e == nullptr);

    // CRLF line ends, a BOM and blank lines
    {
        std::string crlf;
        for (const char* p = kSample; *p; p++) { if (*p == '\n') crlf += "\r\n\r\n"; else crlf += *p; }
        DxccDb c;
        CHECK(c.loadCty("\xEF\xBB\xBF" + crlf, err) && c.entities().size() == 6 && c.lookup("W1AW").prefix == "=W1AW" && c.version() == "20240301");
    }
    // wl_cty.dat / cty_wt_mod.dat: the same layout, a name with spaces and a one-line prefix list
    {
        DxccDb w;
        CHECK(w.loadCty("Fed. Rep. of Germany: 14: 28: EU: 51.00: -10.00: -1.0: DL: DL,DA,Y2;\n", err) && w.lookup("Y21A").e && w.lookup("Y21A").cont == "EU");
    }
    // broken and hostile input: refused or ignored, never a crash
    {
        DxccDb b;
        CHECK(!b.loadCty("", err) && !err.empty());
        CHECK(!b.loadCty("garbage garbage garbage", err));
        CHECK(!b.loadCty("Name: 1: 2: EU: 0: 0: 0: XX:\n XX", err));          // no ';'
        CHECK(!b.loadCty("Name: 1: 2: EU: 0:", err));                         // header cut short
        CHECK(!b.loadCty("X: 1: 2: EU: 0: 0: 0: : ;", err));                  // no primary prefix
        CHECK(!b.loadCty(std::string("\0\0\0\0", 4), err));
        std::string bin;
        for (int i = 0; i < 200000; i++) bin += (char)(i * 131 + 7);
        b.loadCty(bin, err);                                                   // whatever it says, it returns
        DxccDb q;
        CHECK(q.loadCty("A: 1: 2: EU: 0: 0: 0: AA: ????????????,(((((,[[[[,<<<,{{{,~~~,=,;\n", err));   // 12 '?' would be 26^12 prefixes
        CHECK(q.prefixCount() < 100);
        CHECK(q.lookup("AA1AA").e != nullptr);
        std::string big = "Big: 1: 2: EU: 0: 0: 0: BB:\n";
        for (int i = 0; i < 30000; i++) big += "B" + std::to_string(i) + ",";
        big += "BX;\n";
        DxccDb g;
        CHECK(g.loadCty(big, err) && g.lookup("B777XYZ").e != nullptr);
        std::string huge((size_t)33 << 20, 'x');
        CHECK(!g.loadCty(huge, err) && !err.empty());
        CHECK(g.lookup("B777XYZ").e != nullptr);                               // a refused load keeps the old table
    }
    // the built-in table still works as the fallback
    CHECK(DxccDb::builtin().lookup("DL1ABC").e != nullptr);
    printf(fails ? "ham cty: %d FAILED\n" : "ham cty: ok\n", fails);
    return fails ? 1 : 0;
}
