// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Frequency list formats (core/src/channels/memory_formats.cpp): round trips of every importer / exporter, format detection, the import preview and
// the merge rules, "My lists" (enable / order / quick-tune) and the built-in lists of common services.
#include "dect2/channels/memory_bank.h"
#include "dect2/channels/memory_formats.h"
#include "../common/test_util.h"
#include <algorithm>
#include <cmath>

using namespace dect2;

namespace {
Memory mk(const char* name, uint64_t hz, const char* group = "", const char* mode = "fm") {
    Memory m; m.name = name; m.freqHz = hz; m.mode = mode; m.group = group; return m;
}
std::vector<Memory> parse(const std::string& t, ListFormat f, MemoryLoadReport* r = nullptr) {
    MemoryLoadReport rep;
    auto v = parseList(t, f, rep);
    if (r) *r = rep;
    return v;
}

void testDetect() {
    CHECK(detectListFormat("{\"lists\":{\"a\":{\"bookmarks\":{}}}}") == ListFormat::SdrppJson, "sdr++");
    CHECK(detectListFormat("{\"format\":\"onair-memories\",\"memories\":[]}") == ListFormat::OnAirJson, "onair json");
    CHECK(detectListFormat("<?xml version=\"1.0\"?><ArrayOfMemoryEntry><MemoryEntry/></ArrayOfMemoryEntry>") == ListFormat::SdrSharpXml, "sdr#");
    CHECK(detectListFormat("Location,Name,Frequency,Duplex,Offset,Tone\n0,x,145.6,-,0.6,Tone\n") == ListFormat::ChirpCsv, "chirp");
    CHECK(detectListFormat("name,frequency_mhz,mode,sub_id\nx,93.9,fm,\n") == ListFormat::OnAirCsv, "onair csv");
    CHECK(detectListFormat("Group,Name,Frequency (Hz),Mode\nA,b,100000000,WFM\n") == ListFormat::SdrCsv, "sdr csv");
    CHECK(detectListFormat("162.4,Weather\n") == ListFormat::OnAirCsv, "headerless lines go to the OnAir reader");
    CHECK(detectListFormat("  \n") == ListFormat::Unknown, "empty");
}

void testChirp() {
    const std::string src =
        "Location,Name,Frequency,Duplex,Offset,Tone,rToneFreq,cToneFreq,DtcsCode,DtcsPolarity,RxDtcsCode,CrossMode,Mode,TStep,Skip,Power,Comment,URCALL,RPT1CALL,RPT2CALL,DVCODE\r\n"
        "0,\"LY, Vilnius\",145.600000,-,0.600000,Tone,118.8,88.5,023,NN,023,Tone->Tone,FM,12.50,,5.0W,\"Rep \"\"R1\"\"\",,,,\r\n"
        "1,Simplex,145.500000,,0.600000,,88.5,88.5,023,NN,023,Tone->Tone,NFM,5.00,S,5.0W,,,,,\r\n"
        "2,Bad,,,,,,,,,,,FM,,,,,,,,\r\n";
    MemoryLoadReport rep;
    auto v = parse(src, ListFormat::ChirpCsv, &rep);
    CHECK(v.size() == 2 && rep.skipped == 1, "two good rows, one skipped (%zu, %zu)", v.size(), rep.skipped);
    CHECK(v[0].freqHz == 145600000 && v[0].name == "LY, Vilnius" && v[0].notes == "Rep \"R1\"", "name and comment with quotes");
    CHECK(v[0].options["duplex"] == "-" && v[0].options["offsetHz"] == "600000" && v[0].options["tone"] == "Tone" && v[0].options["rToneHz"] == "118.8", "repeater settings kept");
    CHECK(v[0].mode == "fm" && v[0].options["demod"] == "fm" && v[1].options["demod"] == "nfm", "mode mapping");
    CHECK(v[0].scan && !v[1].scan, "Skip=S is out of the scan");
    const std::string out = writeList(v, ListFormat::ChirpCsv);
    CHECK(detectListFormat(out) == ListFormat::ChirpCsv, "output is recognised as CHIRP");
    auto w = parse(out, ListFormat::ChirpCsv);
    CHECK(w.size() == 2, "re-read");
    for (size_t i = 0; i < w.size() && i < 2; i++)
        CHECK(w[i].freqHz == v[i].freqHz && w[i].name == v[i].name && w[i].notes == v[i].notes && w[i].options["duplex"] == v[i].options["duplex"] &&
              w[i].options["offsetHz"] == v[i].options["offsetHz"] && w[i].options["tone"] == v[i].options["tone"] && w[i].options["rToneHz"] == v[i].options["rToneHz"] &&
              w[i].options["demod"] == v[i].options["demod"], "chirp round trip row %zu", i);
    CHECK(out.find("145.600") != std::string::npos && out.find(",FM,") != std::string::npos, "frequency in MHz, mode FM");
    // a plain memory gets a usable CHIRP row (location numbers, defaults)
    Memory p = mk("Noor", 93900000, "fm", "fm"); p.bandwidthHz = 180000;
    const std::string o2 = writeList({p}, ListFormat::ChirpCsv);
    CHECK(o2.find("0,Noor,93.900") != std::string::npos && o2.find(",WFM,") != std::string::npos, "plain memory exported: %s", o2.c_str());
}

void testSdrpp() {
    Memory a = mk("Airport \"ATIS\"", 118000000, "Air", "am"); a.bandwidthHz = 10000; a.options["demod"] = "am";
    Memory b = mk("Noor FM", 93900000, "Air", "fm"); b.bandwidthHz = 180000; b.options["demod"] = "wfm";
    Memory c = mk("Noor FM", 94000000, "Radio");
    const std::string j = writeList({a, b, c}, ListFormat::SdrppJson);
    CHECK(detectListFormat(j) == ListFormat::SdrppJson, "detected");
    auto v = parse(j, ListFormat::SdrppJson);
    CHECK(v.size() == 3, "three bookmarks (%zu)", v.size());
    CHECK(v[0].name == "Airport \"ATIS\"" && v[0].freqHz == 118000000 && v[0].group == "Air" && v[0].bandwidthHz == 10000 && v[0].options["demod"] == "am", "am bookmark");
    CHECK(v[1].options["demod"] == "wfm" && v[1].mode == "fm" && v[1].bandwidthHz == 180000, "wfm bookmark");
    CHECK(v[2].group == "Radio", "second list");
    // an SDR++ file written by SDR++ itself
    const std::string real = "{\"lists\":{\"General\":{\"showOnWaterfall\":true,\"bookmarks\":{\"Wx\":{\"bandwidth\":12500.0,\"frequency\":162400000.0,\"mode\":0},\"X\":{\"mode\":1}}}},\"selectedList\":\"General\"}";
    MemoryLoadReport rep;
    auto r = parse(real, ListFormat::Unknown, &rep);
    CHECK(r.size() == 1 && rep.skipped == 1 && r[0].freqHz == 162400000 && r[0].options["demod"] == "nfm", "SDR++ file read, bookmark without frequency skipped");
    // the same name twice in one list stays two bookmarks
    Memory d = mk("Same", 100000000, "L"), e = mk("Same", 101000000, "L");
    CHECK(parse(writeList({d, e}, ListFormat::SdrppJson), ListFormat::SdrppJson).size() == 2, "equal names kept apart");
}

void testSharp() {
    Memory a = mk("R&B <live>", 98100000, "Music"); a.favourite = true; a.bandwidthHz = 180000; a.options["demod"] = "wfm"; a.notes = "fav \"station\"";
    Memory b = mk("Tower", 118300000, "", "am"); b.options["demod"] = "am"; b.bandwidthHz = 8000;
    const std::string x = writeList({a, b}, ListFormat::SdrSharpXml);
    CHECK(x.find("<Frequency>98100000</Frequency>") != std::string::npos && x.find("R&amp;B &lt;live&gt;") != std::string::npos && x.find("<DetectorType>WFM</DetectorType>") != std::string::npos, "xml content");
    auto v = parse(x, ListFormat::SdrSharpXml);
    CHECK(v.size() == 2, "two entries");
    CHECK(v[0].name == "R&B <live>" && v[0].favourite && v[0].group == "Music" && v[0].notes == "fav \"station\"" && v[0].options["demod"] == "wfm" && v[0].bandwidthHz == 180000, "sdr# round trip");
    CHECK(!v[1].favourite && v[1].freqHz == 118300000 && v[1].options["demod"] == "am", "second entry");
    MemoryLoadReport rep;
    parse("<ArrayOfMemoryEntry><MemoryEntry><Name>x</Name></MemoryEntry></ArrayOfMemoryEntry>", ListFormat::SdrSharpXml, &rep);
    CHECK(rep.skipped == 1, "entry without frequency skipped");
    parse("<ArrayOfMemoryEntry><MemoryEntry>", ListFormat::SdrSharpXml, &rep);
    CHECK(!rep.ok, "broken xml reported");
}

void testSdrCsv() {
    Memory a = mk("Caf\xC3\xA9, \"Radio\"", 93900000, "G, 1"); a.bandwidthHz = 180000; a.options["demod"] = "wfm"; a.notes = "line1\nline2";
    const std::string c = writeList({a}, ListFormat::SdrCsv);
    auto v = parse(c, ListFormat::SdrCsv);
    CHECK(v.size() == 1 && v[0].name == a.name && v[0].group == "G, 1" && v[0].freqHz == 93900000 && v[0].notes == "line1\nline2" && v[0].bandwidthHz == 180000, "csv round trip");
    auto m = parse("Name;Frequency (MHz);Mode;Bandwidth\nA;162,4;NFM;12,5\nB;7.074;USB;2400\n", ListFormat::SdrCsv);
    CHECK(m.size() == 2 && m[0].freqHz == 162400000 && m[1].freqHz == 7074000 && m[1].options["demod"] == "usb", "semicolon file with decimal comma");
    CHECK(m[0].bandwidthHz == 12500 && m[1].bandwidthHz == 2400, "a bare 12.5 is kHz");
    auto h = parse("Frequency,Name\n121.5,Guard\n", ListFormat::SdrCsv);
    CHECK(h.size() == 1 && h[0].freqHz == 121500000, "plain Frequency column in MHz");
}

void testOnAir() {
    Memory a = mk("Full", 93900000, "G"); a.tags = "news, music"; a.notes = "n"; a.colour = 0xFF8800; a.hasGain = true; a.lna = 24; a.vga = 20; a.amp = true; a.favourite = true;
    a.options["genSquelchDb"] = "-72";
    for (ListFormat f : {ListFormat::OnAirJson, ListFormat::OnAirCsv}) {
        auto v = parse(writeList({a}, f), f);
        CHECK(v.size() == 1 && v[0].name == "Full" && v[0].tags == a.tags && v[0].colour == a.colour && v[0].lna == 24 && v[0].amp && v[0].favourite && v[0].options["genSquelchDb"] == "-72" && v[0].group == "G", "%s round trip", listFormatName(f));
    }
}

void testPreviewAndMerge() {
    MemoryBank bank;
    Memory a = mk("Old name", 100000000, "Mine"); a.favourite = true; a.tags = "news";
    Memory b = mk("", 101000000, "");
    bank.add(a); bank.add(b);
    const std::string file = "Group,Name,Frequency (Hz),Mode,Bandwidth (Hz),Comment\n"
                             "Theirs,New name,100000000,WFM,180000,from file\n"
                             "Theirs,Fresh,102000000,WFM,,\n"
                             "Theirs,Name for blank,101000000,WFM,,note\n"
                             "Theirs,Fresh again,102000000,WFM,,\n";
    ImportPreview p = previewImport(bank, file, ListFormat::Unknown);
    CHECK(p.format == ListFormat::SdrCsv && p.rows.size() == 4, "rows");
    CHECK(p.duplicates == 3 && p.fresh == 1, "dupes against the bank and inside the file: %zu / %zu", p.duplicates, p.fresh);
    CHECK(bank.size() == 2, "preview changes nothing");

    {   // KeepExisting: only the new row comes in
        MemoryBank k = bank;
        const ImportResult r = applyImport(k, p, MergeRule::KeepExisting);
        CHECK(r.added == 1 && r.left == 3 && k.size() == 3 && k.all()[0].name == "Old name", "keep existing");
    }
    {   // Replace: settings come from the file, the favourite flag stays
        MemoryBank k = bank;
        const ImportResult r = applyImport(k, p, MergeRule::Replace);
        CHECK(r.merged >= 2 && k.all()[0].name == "New name" && k.all()[0].favourite && k.all()[0].notes == "from file" && k.all()[0].bandwidthHz == 180000, "replace");
    }
    {   // FillEmpty: the bank's name stays, the blank name is filled
        MemoryBank k = bank;
        applyImport(k, p, MergeRule::FillEmpty);
        CHECK(k.all()[0].name == "Old name" && k.all()[0].notes == "from file" && k.all()[0].group == "Mine" && k.all()[0].bandwidthHz == 180000, "fill empty keeps what exists");
        CHECK(k.all()[1].name == "Name for blank" && k.all()[1].notes == "note", "fill empty fills blanks");
    }
    {   // KeepBoth
        MemoryBank k = bank;
        const ImportResult r = applyImport(k, p, MergeRule::KeepBoth);
        CHECK(r.added == 4 && k.size() == 6, "keep both");
    }
    {   // a row the user unticked stays out
        ImportPreview q = p;
        q.rows[1].take = false; q.rows[3].take = false;   // the new row and its repeat
        MemoryBank k = bank;
        const ImportResult r = applyImport(k, q, MergeRule::KeepExisting);
        CHECK(r.added == 0 && k.size() == 2, "unticked row left out");
    }
    {   // tags are joined without repeats
        MemoryBank k;
        Memory x = mk("x", 50000000, ""); x.tags = "news, Music";
        k.add(x);
        ImportPreview q;
        ImportRow row; row.m = mk("x", 50000000, ""); row.m.tags = "music, sport"; row.duplicate = true;
        q.rows.push_back(row);
        applyImport(k, q, MergeRule::FillEmpty);
        CHECK(k.all()[0].tags == "news, Music, sport", "tags joined: %s", k.all()[0].tags.c_str());
    }
}

void testListSet() {
    MemoryBank bank;
    Memory a = mk("A1", 100000000, "Air"); a.favourite = true;
    Memory b = mk("A2", 101000000, "Air");
    Memory c = mk("M1", 156800000, "Marine"); c.favourite = true;
    Memory d = mk("L1", 7074000, ""); d.favourite = true;
    for (const Memory& m : {a, b, c, d}) bank.add(m);
    ListSet ls;
    ls.sync(bank);
    CHECK(ls.lists().size() == 2 && ls.lists()[0].name == "Air" && ls.lists()[1].name == "Marine", "a list per group");
    CHECK(ls.visible(bank).size() == 4 && ls.quickTune(bank, 10).size() == 3, "all enabled");
    CHECK(ls.setEnabled("Marine", false) && !ls.enabled("Marine") && ls.enabled("") && ls.enabled("Nope"), "disable");
    CHECK(ls.visible(bank).size() == 3 && ls.quickTune(bank, 10).size() == 2, "disabled list leaves the view and the bar");
    ls.setEnabled("Marine", true);
    CHECK(ls.move(1, 0), "move");
    auto q = ls.quickTune(bank, 10);
    CHECK(q.size() == 3 && bank.all()[q[0]].group == "Marine" && bank.all()[q[1]].group == "Air" && bank.all()[q[2]].group == "", "bar follows the list order, no-list items last");
    CHECK(ls.quickTune(bank, 1).size() == 1, "bar is capped");
    MemoryQuery mq; mq.text = "A2";
    CHECK(ls.visible(bank, mq).size() == 1, "search inside lists");
    CHECK(ls.rename(bank, "Air", "Airband") && bank.all()[0].group == "Airband" && bank.all()[1].group == "Airband" && !ls.rename(bank, "Airband", "Marine"), "rename moves the memories, refuses a taken name");
    ListSet l2;
    CHECK(l2.fromJson(ls.toJson()) && l2.lists().size() == 2 && l2.lists()[0].name == "Marine", "settings round trip");
    ls.setEnabled("Marine", false);
    ListSet l3;
    l3.fromJson(ls.toJson());
    CHECK(!l3.enabled("Marine"), "enabled flag persists");
    bank.remove(bank.all()[2].id);
    ls.sync(bank);
    CHECK(ls.lists().size() == 1, "list of a deleted group is dropped");
}

void testBuiltin() {
    const auto& lists = builtinLists();
    CHECK(lists.size() >= 8, "eight service lists");
    size_t withData = 0;
    for (const BuiltinList& l : lists) {
        const auto m = builtinListMemories(l.name);
        if (!m.empty()) withData++;
        for (const Memory& x : m) CHECK(x.freqHz > 0 && x.group == l.name, "memory of %s", l.name.c_str());
    }
    CHECK(withData >= 5, "most lists have entries (%zu)", withData);
    CHECK(!builtinListMemories("Marine").empty() && !builtinListMemories("Airband").empty() && !builtinListMemories("Ham band plans").empty(), "marine, air and ham are filled");
    CHECK(builtinListMemories("No such list").empty(), "unknown list");
    const auto& rg = builtinRegions();
    CHECK(!rg.empty() && rg[0] == "GLOBAL", "regions start with GLOBAL");
    // a region keeps its own entries and the global ones
    size_t all = builtinListMemories("Weather").size(), eu = builtinListMemories("Weather", "EU").size();
    CHECK(eu <= all, "a region is a subset");
    // a built-in list imported into a bank through the preview path
    MemoryBank bank;
    const auto ham = builtinListMemories("Ham band plans");
    ImportPreview p;
    for (const Memory& m : ham) { ImportRow r; r.m = m; p.rows.push_back(r); }
    applyImport(bank, p, MergeRule::KeepExisting);
    CHECK(bank.size() > 0 && bank.size() <= ham.size(), "import of a built-in list");
}
}  // namespace

int main() {
    testDetect();
    testChirp();
    testSdrpp();
    testSharp();
    testSdrCsv();
    testOnAir();
    testPreviewAndMerge();
    testListSet();
    testBuiltin();
    return testSummary("memory_formats");
}
