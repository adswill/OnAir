// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The memory bank (core/src/channels/memory_*.cpp): add / update / duplicates / sort / search, JSON and CSV round trips with quotes, commas, line
// breaks and unicode, imports of simple lists and of an SDR++ export, damaged files, the atomic save, the cap, and the recall order.
#include "dect2/channels/memory_bank.h"
#include "dect2/common/u8file.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static Memory mk(const char* name, uint64_t hz, const char* mode = "fm") { Memory m; m.name = name; m.freqHz = hz; m.mode = mode; return m; }

static std::string slurp(const std::string& p) { std::ifstream f(u8file(p), std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static void spit(const std::string& p, const std::string& t) { std::ofstream f(u8file(p), std::ios::binary); f << t; }

// a Memory that uses every field, with the awkward characters
static Memory full() {
    Memory m = mk("Caf\xC3\xA9 \"Radio\", 93,9\nline2 \xE6\x97\xA5\xE6\x9C\xAC \xF0\x9F\x93\xBB", 93900000, "fm");
    m.subId = "pocsag"; m.family = 5; m.stdMode = 7; m.sampleRateHz = 2400000; m.bandwidthHz = 180000; m.radio = "HackRF; \"One\"";
    m.hasGain = true; m.lna = 24; m.vga = 20; m.amp = true; m.gainDb = 33.5;
    m.options["fmDeemph"] = "75"; m.options["we=ird;k%"] = "va=l;ue%41"; m.options["demod"] = "nfm";
    m.group = "Dub, ai"; m.tags = "news, music"; m.notes = "tab\there, back\\slash, \"q\"";
    m.colour = 0xFF8800; m.lastUsed = 1760000000; m.created = 1750000000; m.favourite = true; m.scan = true; m.dwellSec = 3.5;
    return m;
}
static bool same(const Memory& a, const Memory& b) {
    return a.name == b.name && a.freqHz == b.freqHz && a.mode == b.mode && a.subId == b.subId && a.family == b.family && a.stdMode == b.stdMode &&
           a.sampleRateHz == b.sampleRateHz && a.bandwidthHz == b.bandwidthHz && a.radio == b.radio && a.hasGain == b.hasGain && a.lna == b.lna && a.vga == b.vga && a.amp == b.amp &&
           a.gainDb == b.gainDb && a.options == b.options && a.group == b.group && a.tags == b.tags && a.notes == b.notes && a.colour == b.colour && a.lastUsed == b.lastUsed &&
           a.created == b.created && a.favourite == b.favourite && a.scan == b.scan && a.dwellSec == b.dwellSec;
}

struct FakeHost : MemoryHost {
    std::vector<std::string> calls;
    bool have = true, accept = true;
    bool hasMode(const std::string& id) override { calls.push_back("has:" + id); return have; }
    bool setMode(const Memory& m, std::string& n) override { calls.push_back("mode:" + m.mode); if (!accept) n = "stop first"; return accept; }
    void setOption(const std::string& k, const std::string& v) override { calls.push_back("opt:" + k + "=" + v); }
    void setGains(const Memory& m) override { calls.push_back("gain:" + std::to_string(m.lna)); }
    void setSampleRate(double hz) override { calls.push_back("rate:" + std::to_string((long)hz)); }
    void setFrequencyHz(uint64_t hz) override { calls.push_back("freq:" + std::to_string((unsigned long long)hz)); }
};

int main() {
    // add / update / duplicate / remove / undo / move
    {
        MemoryBank b;
        const auto a1 = b.add(mk("A", 100000000)), a2 = b.add(mk("B", 101000000)), a3 = b.add(mk("C", 99000000, "airband"));
        CHECK(b.size() == 3 && a1.id != a2.id && a2.id != a3.id && a1.what == MemoryAdd::Added, "add");
        CHECK(b.findDuplicate(100000000, "fm") && !b.findDuplicate(100000000, "airband") && !b.findDuplicate(100000001, "fm"), "duplicate needs frequency and mode");
        auto d = b.add(mk("A again", 100000000), DupPolicy::Skip);
        CHECK(d.what == MemoryAdd::Duplicate && b.size() == 3 && b.find(a1.id)->name == "A", "skip duplicate");
        d = b.add(mk("A new", 100000000), DupPolicy::Update);
        CHECK(d.what == MemoryAdd::Duplicate && b.size() == 3 && b.find(a1.id)->name == "A new" && b.find(a1.id)->id == a1.id, "update duplicate");
        d = b.add(mk("A third", 100000000), DupPolicy::Keep);
        CHECK(d.what == MemoryAdd::Added && b.size() == 4, "keep duplicate");
        Memory u = *b.find(a2.id); u.name = "B2"; u.favourite = true;
        CHECK(b.update(a2.id, u) && b.find(a2.id)->name == "B2" && b.find(a2.id)->created == u.created && !b.update(9999, u), "update");
        Memory gone; size_t pos = 99;
        CHECK(b.remove(a2.id, &gone, &pos) && pos == 1 && b.size() == 3 && !b.find(a2.id), "remove");
        b.restore(gone, pos);
        CHECK(b.size() == 4 && b.all()[1].id == a2.id && b.all()[1].name == "B2", "undo puts it back in place");
        const auto n4 = b.add(mk("N", 1));
        CHECK(n4.id > a2.id, "ids are not reused");
        CHECK(b.move(0, 3) && b.all()[3].id == a1.id && b.all()[0].id == a2.id && !b.move(0, 10), "reorder");
    }
    // sort, search, groups, recent
    {
        MemoryBank b;
        Memory m1 = mk("zulu", 300000000, "fm"); m1.group = "G2"; m1.tags = "Air, Test"; m1.lastUsed = 50;
        Memory m2 = mk("Alpha", 100000000, "airband"); m2.group = "G1"; m2.favourite = true; m2.lastUsed = 200; m2.notes = "tower";
        Memory m3 = mk("mike", 162400000, "fm"); m3.group = "G1"; m3.tags = "weather"; m3.lastUsed = 100;
        Memory m4 = mk("echo", 162412500, "nfm");
        b.add(m1); b.add(m2); b.add(m3); b.add(m4);
        auto names = [&](const std::vector<size_t>& v) { std::string s; for (size_t i : v) s += b.all()[i].name + ","; return s; };
        CHECK(names(b.view({}, MemorySort::Name)) == "Alpha,echo,mike,zulu,", "sort by name, case-insensitive: %s", names(b.view({}, MemorySort::Name)).c_str());
        CHECK(names(b.view({}, MemorySort::Frequency)) == "Alpha,mike,echo,zulu,", "sort by frequency");
        CHECK(names(b.view({}, MemorySort::Frequency, true)) == "zulu,echo,mike,Alpha,", "sort by frequency descending");
        CHECK(names(b.view({}, MemorySort::LastUsed)) == "Alpha,mike,zulu,echo,", "sort by last used, never used last");
        CHECK(names(b.view({}, MemorySort::Group)).substr(0, 5) == "echo,", "sort by group (no group first)");
        CHECK(names(b.view({}, MemorySort::Frequency, false, true)) == "Alpha,mike,echo,zulu,", "favourites first keeps the order inside");
        CHECK(names(b.view({}, MemorySort::Name, false, true)).substr(0, 6) == "Alpha,", "favourite first");
        MemoryQuery q; q.text = "TOWER";
        CHECK(names(b.search(q)) == "Alpha,", "search notes, case-insensitive");
        q = {}; q.text = "162.4"; CHECK(names(b.search(q)) == "mike,echo,", "frequency text finds both 162.4 channels: %s", names(b.search(q)).c_str());
        q = {}; q.text = "162,4125"; CHECK(names(b.search(q)) == "echo,", "decimal comma");
        q = {}; q.text = "162"; CHECK(names(b.search(q)) == "mike,echo,", "a whole MHz finds that MHz only");
        q = {}; q.text = "16"; CHECK(names(b.search(q)).empty(), "a prefix of the MHz digits without a point is not a match: %s", names(b.search(q)).c_str());
        q = {}; q.loHz = 150000000; q.hiHz = 200000000; CHECK(names(b.search(q)) == "mike,echo,", "range");
        q = {}; q.tag = "air"; CHECK(names(b.search(q)) == "zulu,", "tag is a whole word");
        q = {}; q.tag = "ai"; CHECK(names(b.search(q)).empty(), "tag substring is not a tag");
        q = {}; q.mode = "NFM"; CHECK(names(b.search(q)) == "echo,", "mode");
        q = {}; q.group = "g1"; CHECK(names(b.search(q)) == "Alpha,mike,", "group");
        q = {}; q.favouritesOnly = true; CHECK(names(b.search(q)) == "Alpha,", "favourites");
        const auto g = b.groups();
        CHECK(g.size() == 2 && g[0] == "G2" && g[1] == "G1", "groups in order of first use");
        const auto r = b.recent(2);
        CHECK(r.size() == 2 && b.all()[r[0]].name == "Alpha" && b.all()[r[1]].name == "mike", "recent");
        b.touch(b.all()[3].id, 500);
        CHECK(b.all()[b.recent(1)[0]].name == "echo", "touch");
        b.sortInPlace(MemorySort::Name);
        CHECK(b.all()[0].name == "Alpha" && b.all()[3].name == "zulu", "sortInPlace");
    }
    // frequency text
    {
        CHECK(memoryFreqMhzText(93900000) == "93.900" && memoryFreqMhzText(162412500) == "162.4125" && memoryFreqMhzText(7074123) == "7.074123" && memoryFreqMhzText(500) == "0.0005", "freq text");
        uint64_t h = 0;
        CHECK(memoryParseFreq("162,4125", h) && h == 162412500, "comma");
        CHECK(memoryParseFreq(" 146520 kHz", h) && h == 146520000, "kHz");
        CHECK(memoryParseFreq("7074000", h) && h == 7074000, "bare big number is Hz");
        CHECK(memoryParseFreq("93.9 MHz", h) && h == 93900000, "MHz unit");
        CHECK(memoryParseFreq("1.2 GHz", h) && h == 1200000000, "GHz");
        CHECK(!memoryParseFreq("abc", h) && !memoryParseFreq("", h) && !memoryParseFreq("-5", h) && !memoryParseFreq("5 furlongs", h), "garbage");
    }
    // JSON and CSV round trips
    {
        MemoryBank b;
        b.add(full()); b.add(mk("plain", 7074000, "ft8"));
        MemoryBank j;
        const auto rj = j.fromJson(b.toJson());
        CHECK(rj.ok && rj.loaded == 2 && rj.skipped == 0 && j.size() == 2, "json load");
        CHECK(same(j.all()[0], b.all()[0]) && same(j.all()[1], b.all()[1]), "json round trip keeps every field, quotes, newline, unicode, emoji");
        MemoryBank c;
        const auto rc = c.importText(b.toCsv());
        CHECK(rc.ok && rc.loaded == 2 && c.size() == 2, "csv load %zu / %s", rc.loaded, rc.error.c_str());
        CHECK(same(c.all()[0], b.all()[0]) && same(c.all()[1], b.all()[1]), "csv round trip keeps every field (comma, quote, newline, ; = %% in options)");
        MemoryBank cj;
        const auto ri = cj.importText(b.toJson());
        CHECK(ri.loaded == 2 && same(cj.all()[0], b.all()[0]), "json import");
        const auto csv = b.toCsv();
        CHECK(csv.compare(0, 4, "name") == 0 && csv.find("\r\n") != std::string::npos, "csv has a header and CRLF");
        // a duplicate import is skipped or updates
        const auto r2 = c.importText(b.toCsv(), DupPolicy::Skip);
        CHECK(c.size() == 2 && r2.loaded == 0 && r2.skipped == 2, "import duplicates skipped");
        const auto r3 = c.importText(b.toCsv(), DupPolicy::Keep);
        CHECK(c.size() == 4 && r3.loaded == 2, "import duplicates kept");
    }
    // simple lists
    {
        MemoryBank b;
        auto r = b.importText("\xEF\xBB\xBF" "162.4,Weather\r\n7074000,FT8\r\n146.52 , \"Calling, simplex\"\nnot a line\n\n446.00625,PMR 1\n", DupPolicy::Keep, "Imported");
        CHECK(r.ok && r.loaded == 4, "simple import loaded %zu", r.loaded);
        CHECK(b.size() >= 3 && b.all()[0].freqHz == 162400000 && b.all()[0].name == "Weather" && b.all()[0].group == "Imported", "first line");
        bool call = false, pmr = false;
        for (const Memory& m : b.all()) { call |= m.freqHz == 146520000 && m.name == "Calling, simplex"; pmr |= m.freqHz == 446006250; }
        CHECK(call && pmr, "quoted name with a comma, a decimal MHz");
        CHECK(r.skipped >= 1 && !r.log.empty(), "bad line logged");
        MemoryBank s;
        auto rs = s.importText("Name;Frequency;Mode\nTower;118,1;am\nGround;121.9;am\n");
        CHECK(rs.loaded == 2 && s.all()[0].freqHz == 118100000 && s.all()[0].mode == "am" && s.all()[1].name == "Ground", "semicolon CSV with a header and a decimal comma");
        MemoryBank hz;
        hz.importText("name,frequency_hz\nA,7074000\n");
        CHECK(hz.size() == 1 && hz.all()[0].freqHz == 7074000, "frequency_hz column");
        MemoryBank e;
        CHECK(!e.importText("").ok && !e.importText("hello world").ok && !e.importText("{broken").ok, "empty / junk / broken refused");
    }
    // SDR++ frequency manager export
    {
        MemoryBank b;
        const char* sdrpp = "{\"bookmarkDisplayMode\":0,\"lists\":{\"General\":{\"showOnWaterfall\":true,\"bookmarks\":{\"Radio Test\":{\"bandwidth\":15000.0,\"frequency\":100100000.0,\"mode\":0},\"Bad\":{\"mode\":1}}},\"Air\":{\"bookmarks\":{\"Tower\":{\"bandwidth\":10000,\"frequency\":118100000,\"mode\":2}}}}}";
        auto r = b.importText(sdrpp);
        CHECK(r.ok && r.loaded == 2 && r.skipped == 1 && b.all()[0].name == "Radio Test" && b.all()[0].freqHz == 100100000 && b.all()[0].bandwidthHz == 15000 && b.all()[0].group == "General" && b.all()[1].group == "Air", "sdr++ import");
    }
    // damaged files
    {
        MemoryBank b;
        b.add(mk("one", 100000000)); b.add(mk("two", 101000000)); b.add(mk("three", 102000000));
        const std::string good = b.toJson();
        MemoryBank t;
        auto r = t.fromJson(good.substr(0, good.size() - 60));   // cut in the third entry
        CHECK(r.ok && t.size() == 2 && !r.log.empty(), "truncated file keeps the entries before the damage (%zu)", t.size());
        MemoryBank g;
        r = g.fromJson("{\"version\":99,\"memories\":[{\"freqHz\":5e6,\"name\":\"x\",\"future\":{\"a\":[1,2]}},{\"name\":\"no freq\"},{\"freqHz\":\"abc\"},7,{\"freqHz\":-3},{\"freqHz\":90100000,\"options\":{\"k\":5},\"favourite\":\"true\",\"colour\":99999999999,\"family\":\"x\"}],\"extra\":1}");
        CHECK(r.ok && g.size() == 2 && r.skipped == 4 && g.all()[0].freqHz == 5000000 && g.all()[1].options.at("k") == "5" && g.all()[1].favourite && g.all()[1].colour == 0xFFFFFF, "unknown fields ignored, bad entries skipped (%zu loaded, %zu skipped)", r.loaded, r.skipped);
        CHECK(r.log.size() >= 4, "one log line per skipped entry plus the version note");
        MemoryBank n;
        CHECK(!n.fromJson("garbage").ok && !n.fromJson("").ok && !n.fromJson("[1,2").ok, "nothing readable");
        r = n.fromJson("[{\"freqHz\":1000000,\"name\":\"bare array\"}]");
        CHECK(r.ok && n.size() == 1, "a bare array works");
        std::string e = "{\"memories\":[{\"name\":\"\\ud83d\\udcfb \\u00e9 \\ud800 bad\\q\",\"freqHz\":1000000}]}";
        MemoryBank u;
        r = u.fromJson(e);   // bad escape: whole file is damaged at the bad escape; the entry is the last and is dropped
        CHECK(u.size() == 0, "bad escape drops the entry it is in");
        MemoryBank u2;
        u2.fromJson("{\"memories\":[{\"name\":\"\\ud83d\\udcfb \\u00e9 \\ud800\",\"freqHz\":1000000}]}");
        CHECK(u2.size() == 1 && u2.all()[0].name == "\xF0\x9F\x93\xBB \xC3\xA9 \xEF\xBF\xBD", "unicode escapes and a lone surrogate");
    }
    // files: atomic save, load, corrupt, missing
    {
        const std::string dir = (std::filesystem::temp_directory_path() / "onair_memtest_\xC3\xA9").string();
        std::filesystem::remove_all(u8file(dir));
        const std::string path = dir + "/sub/memories.json";
        MemoryBank b;
        b.add(full()); b.add(mk("x", 100000000));
        std::string err;
        CHECK(b.save(path, &err), "save: %s", err.c_str());
        CHECK(!std::filesystem::exists(u8file(path + ".tmp")), "the temp file is gone after a save");
        MemoryBank l;
        auto r = l.load(path);
        CHECK(r.ok && l.size() == 2 && same(l.all()[0], b.all()[0]), "load what was saved (unicode folder)");
        // a failed rename leaves the old file whole: make the target a directory
        const std::string odd = dir + "/dir.json";
        std::filesystem::create_directories(u8file(odd));
        CHECK(!b.save(odd, &err) && std::filesystem::is_directory(u8file(odd)) && !std::filesystem::exists(u8file(odd + ".tmp")), "failed save cleans up its temp file");
        // saving over an existing file replaces it and never shows half a file
        b.add(mk("y", 101000000));
        CHECK(b.save(path) && MemoryBank().load(path).loaded == 3, "overwrite");
        MemoryBank m;
        r = m.load(dir + "/none.json");
        CHECK(r.ok && m.empty(), "a missing file is an empty bank");
        spit(dir + "/bad.json", "this is not json at all");
        r = m.load(dir + "/bad.json");
        CHECK(!r.ok && m.empty() && std::filesystem::exists(u8file(dir + "/bad.json.corrupt")) && slurp(dir + "/bad.json.corrupt") == "this is not json at all", "unreadable file is kept as .corrupt");
        CHECK(b.exportFile(dir + "/e.csv", true) && b.exportFile(dir + "/e.json", false), "export");
        MemoryBank ic; CHECK(ic.importFile(dir + "/e.csv").loaded == 3 && ic.all()[0].name == b.all()[0].name, "import from a csv file");
        MemoryBank ij; CHECK(ij.importFile(dir + "/e.json").loaded == 3, "import from a json file");
        CHECK(!ij.importFile(dir + "/nope.csv").ok, "import of a missing file");
        std::filesystem::remove_all(u8file(dir));
    }
    // cap
    {
        MemoryBank b;
        for (size_t i = 0; i < kMemoryCap; i++) b.add(mk("m", 1000000 + i));
        const auto r = b.add(mk("over", 5));
        CHECK(b.size() == kMemoryCap && r.what == MemoryAdd::Full, "cap on add");
        MemoryBank c;
        const auto ri = c.importText(b.toCsv());
        CHECK(c.size() == kMemoryCap, "import round trip at the cap");
        const auto r2 = c.importText("1,extra\n2,extra2\n");
        CHECK(c.size() == kMemoryCap && r2.loaded == 0 && !r2.log.empty(), "import stops at the cap");
        MemoryBank j;
        std::string big = "{\"memories\":[";
        for (size_t i = 0; i < kMemoryCap + 50; i++) big += std::string(i ? "," : "") + "{\"freqHz\":" + std::to_string(1000000 + i) + "}";
        j.fromJson(big + "]}");
        CHECK(j.size() == kMemoryCap, "load stops at the cap");
        (void)ri;
    }
    // recall order
    {
        FakeHost h;
        Memory m = full();
        auto r = memoryRecall(h, m);
        CHECK(r.ok && r.modeChanged && h.calls.size() >= 7 && h.calls[0] == "has:fm" && h.calls[1] == "mode:fm" && h.calls.back() == "freq:93900000", "mode first, frequency last");
        size_t optI = 99, gainI = 99, rateI = 99, freqI = 99;
        for (size_t i = 0; i < h.calls.size(); i++) { if (h.calls[i].rfind("opt:", 0) == 0) optI = std::min(optI, i); if (h.calls[i].rfind("gain:", 0) == 0) gainI = i; if (h.calls[i].rfind("rate:", 0) == 0) rateI = i; if (h.calls[i].rfind("freq:", 0) == 0) freqI = i; }
        CHECK(optI == 2 && gainI > optI && rateI > gainI && freqI > rateI, "options, gains, rate, frequency in that order");
        h.calls.clear();
        r = memoryRecall(h, m, false);
        CHECK(r.ok && !r.modeChanged && h.calls.size() == 1 && h.calls[0] == "freq:93900000", "frequency only");
        h.calls.clear(); h.have = false;
        r = memoryRecall(h, m);
        CHECK(!r.ok && r.note.find("not in this build") != std::string::npos && h.calls.size() == 1, "an unavailable mode changes nothing");
        h.calls.clear(); h.have = true; h.accept = false;
        r = memoryRecall(h, m);
        CHECK(!r.ok && r.note == "stop first" && h.calls.back() == "mode:fm", "a refused mode says why and tunes nothing");
        h.calls.clear(); h.accept = true;
        Memory bare = mk("bare", 5000000, "");
        r = memoryRecall(h, bare);
        CHECK(r.ok && h.calls.size() == 1, "a memory without a mode is a frequency only");
    }
    if (!fails) printf("memory bank: all passed\n");
    return fails ? 1 : 0;
}
