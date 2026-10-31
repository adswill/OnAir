// Pagers: the capcode alias CSV (#5): import with real-world spreadsheet quirks, lookup precedence per protocol, export and a round trip.
#include "../../app/pager/pager_alias.h"
#include <cstdio>
using namespace dect2;
#include "../common/test_util.h"

int main() {
    {   // plain file with a header
        PagerAliases a;
        const auto r = a.importCsv("capcode,name,category,protocol\n1234567,Fire Station 3,Fire,POCSAG\n1000-1099,Hospital pagers,Medical,\n77,Both,Misc,any\n");
        CHECK(r.added == 3 && r.skipped == 0, "added %d skipped %d", r.added, r.skipped);
        CHECK(a.name(1234567, false) == "Fire Station 3", "POCSAG entry");
        CHECK(a.name(1234567, true).empty(), "the same number on FLEX has no name: entries are per protocol");
        CHECK(a.name(1050, false) == "Hospital pagers" && a.name(1050, true) == "Hospital pagers", "a range for both protocols");
        CHECK(a.name(1100, false).empty() && a.name(999, false).empty(), "range edges");
        CHECK(a.find(77, true) && a.find(77, true)->category == "Misc", "category");
    }
    {   // precedence: protocol-specific > any, single > range, narrow > wide
        PagerAliases a;
        a.importCsv("1000-1999,wide,,\n1200-1299,narrow,,\n1250,single-any,,\n1250,single-flex,,FLEX\n1500,any-only,,\n1500-1600,flex-range,,FLEX\n");
        CHECK(a.name(1100, false) == "wide", "wide");
        CHECK(a.name(1210, false) == "narrow", "narrower range wins");
        CHECK(a.name(1250, false) == "single-any" && a.name(1250, true) == "single-flex", "protocol-specific single wins on its protocol");
        CHECK(a.name(1500, true) == "any-only", "a single capcode beats a range, even one for the protocol");
        CHECK(a.name(1550, true) == "flex-range" && a.name(1550, false) == "wide", "FLEX-only range does not apply to POCSAG");
    }
    {   // spreadsheet quirks: BOM, CRLF, semicolons, quoted fields with commas / quotes / newlines, comments, blank lines, odd header names
        PagerAliases a;
        const std::string csv = "\xEF\xBB\xBF" "RIC;Label;Group;Mode\r\n# a comment\r\n\r\n2000;\"Smith, John\";\"Doc \"\"on call\"\"\";pocsag\r\n2001;\"two\nlines\";;FLEX\r\n";
        const auto r = a.importCsv(csv);
        CHECK(r.added == 2 && r.skipped == 0, "added %d skipped %d", r.added, r.skipped);
        CHECK(a.name(2000, false) == "Smith, John" && a.find(2000, false)->category == "Doc \"on call\"", "quoted fields");
        CHECK(a.name(2001, true) == "two\nlines", "newline inside quotes");
        PagerAliases t; t.importCsv("3000\tTabbed\tCat\tFLEX\n");
        CHECK(t.name(3000, true) == "Tabbed", "tab separated, no header");
        PagerAliases c; c.importCsv("Name,Capcode\nAlpha,4000\nBeta,4001\n");
        CHECK(c.name(4000, false) == "Alpha" && c.name(4001, true) == "Beta", "columns by header name in any order");
    }
    {   // bad input is skipped, never crashes
        PagerAliases a;
        const auto r = a.importCsv("capcode,name\nabc,Bad\n12x,Bad2\n99999999999,Huge\n,Empty\n500,\n600,ok\n700-,half\n");
        CHECK(a.rows().size() == 1 && a.name(600, false) == "ok", "only the valid row: %zu", a.rows().size());
        CHECK(r.skipped == 6, "skipped %d", r.skipped);
        PagerAliases g; g.importCsv(std::string("capcode,name\n1,\"unterminated\n2,x\n")); g.importCsv("\xFF\xFE\x00\x01"); g.importCsv("");
        PagerAliases n; CHECK(n.importCsv("foo,bar\n1,2\n").skipped > 0 && n.empty(), "no capcode column");
        PagerAliases rev; rev.importCsv("90-80,rev\n"); CHECK(rev.name(85, false) == "rev", "reversed range");
        PagerAliases lng; lng.importCsv("1," + std::string(500, 'x') + "\n"); CHECK(lng.find(1, false)->name.size() == 120, "long name clipped");
    }
    {   // merge on import: same range+protocol replaces, others stay
        PagerAliases a;
        a.importCsv("10,old,A,\n20,keep,,\n");
        const auto r = a.importCsv("10,new,B,\n30,third,,\n");
        CHECK(r.replaced == 1 && r.added == 1, "replaced %d added %d", r.replaced, r.added);
        CHECK(a.name(10, false) == "new" && a.find(10, false)->category == "B" && a.name(20, false) == "keep" && a.rows().size() == 3, "merged");
        a.set({10, 10, kAliasAny, "", ""});
        CHECK(a.name(10, false).empty() && a.rows().size() == 2, "empty name removes");
    }
    {   // export and round trip, with characters that need quoting and a formula-looking name
        PagerAliases a;
        a.set({1234567, 1234567, kAliasPocsag, "Fire, Station \"3\"", "Fire"});
        a.set({1000, 1099, kAliasAny, "=SUM(A1)", ""});
        a.set({5, 5, kAliasFlex, "Ä Ö ü ß", "Text"});
        const std::string csv = a.exportCsv();
        CHECK(csv.find("'=SUM(A1)") != std::string::npos, "a formula-looking name is guarded in the export");
        PagerAliases b;
        const auto r = b.importCsv(csv);
        CHECK(r.added == 3 && r.skipped == 0, "round trip rows %d/%d", r.added, r.skipped);
        CHECK(b.name(1234567, false) == "Fire, Station \"3\"" && b.name(1050, true) == "=SUM(A1)" && b.name(5, true) == "Ä Ö ü ß", "round trip text");
        CHECK(b.exportCsv() == csv, "export is stable");
    }
    {   // big table: 50000 single entries plus a range
        PagerAliases a;
        std::string csv = "capcode,name\n";
        for (int i = 0; i < 50000; i++) csv += std::to_string(100000 + i * 3) + ",n" + std::to_string(i) + "\n";
        csv += "1-99,low\n";
        a.importCsv(csv);
        long hit = 0;
        for (uint32_t i = 0; i < 200000; i++) if (a.find(100000 + i, false)) hit++;
        CHECK(hit == 50000, "hits %ld", hit);
        CHECK(a.name(50, false) == "low", "range with a big table");
    }
    return testSummary("pager_alias");
}
