// Message database: insert/search/filter, FTS safety, voice/encrypted refusal, retention limits, exports, the writer thread, the engine tap, persistence.
#include "dect2/automation/engine.h"
#include "dect2/msgdb/msgdb.h"
#include "../common/test_util.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace dect2::msgdb;
namespace au = dect2::automation;
namespace fs = std::filesystem;

namespace {
constexpr double kT0 = 1792232100.0;

au::Event acars(const char* tail, const char* text, double t) {
    au::Event e("acars.message");
    e.at(t, "ACARS", 131.725e6);
    e.str("tail", tail).str("text", text).str("label", "H1").num("lat", 54.7).num("lon", 25.3);
    return e;
}
au::Event pager(double addr, const char* text, double t) {
    au::Event e("pager.message");
    e.at(t, "POCSAG", 152e6);
    e.num("address", addr).str("text", text).str("type", "alpha");
    return e;
}

void testBasic() {
    Db db;
    CHECK(db.open(":memory:"), "open");
    Record r;
    CHECK(fromEvent(acars("A6-EDA", "REQUEST PUSHBACK GATE 12", kT0), r), "from event");
    CHECK(r.ident == "A6-EDA" && r.mode == "ACARS" && r.hasPos, "mapping");
    CHECK(db.add(r), "add");
    CHECK(fromEvent(pager(1234567, "FIRE ALARM MAIN ST 12", kT0 + 10), r) && db.add(r), "pager");
    au::Event ads("adsb.aircraft");
    ads.at(kT0 + 20, "ADS-B", 1090e6);
    ads.str("hex", "4840D6").str("callsign", "KLM1023").num("alt_ft", 36000);
    CHECK(fromEvent(ads, r) && db.add(r), "adsb");
    CHECK(db.count(Filter()) == 3, "count 3");
    Filter f;
    f.text = "pushback";
    auto v = db.query(f);
    CHECK(v.size() == 1 && v[0].ident == "A6-EDA", "text search");
    f.text = "push*";
    CHECK(db.query(f).size() == 1, "prefix");
    f.text = "fire main";
    CHECK(db.query(f).size() == 1, "AND of words");
    f.text = "klm1023";
    CHECK(db.query(f).size() == 1, "search in the fields");
    f.text = "\"; DROP TABLE msgs; -- OR NEAR(";
    db.query(f);
    CHECK(db.count(Filter()) == 3, "hostile query is harmless");
    f = Filter();
    f.mode = "POCSAG";
    CHECK(db.query(f).size() == 1, "mode filter");
    f = Filter();
    f.ident = "a6-";
    CHECK(db.query(f).size() == 1, "ident substring");
    f = Filter();
    f.since = kT0 + 5; f.until = kT0 + 15;
    v = db.query(f);
    CHECK(v.size() == 1 && v[0].mode == "POCSAG", "time range");
    f = Filter();
    f.posOnly = true;
    CHECK(db.query(f).size() == 1, "position filter");
    f = Filter();
    v = db.query(f);
    CHECK(v.size() == 3 && v[0].time >= v[1].time, "newest first");
    f.oldestFirst = true;
    f.limit = 2; f.offset = 1;
    v = db.query(f);
    CHECK(v.size() == 2 && v[0].mode == "POCSAG", "paging");
    CHECK(db.modes().size() == 3, "modes");
    Stats s = db.stats();
    CHECK(s.rows == 3 && s.oldest == kT0 && s.bytes > 0, "stats");
}

void testRefusals() {
    Db db;
    db.open(":memory:");
    au::Event e = pager(1, "x", kT0);
    e.flag("encrypted", true);
    Record r;
    CHECK(!fromEvent(e, r), "encrypted event refused");
    au::Event v("voice.call");
    v.at(kT0, "DMR", 1);
    CHECK(!fromEvent(v, r), "voice refused");
    au::Event sig("signal.new");
    sig.at(kT0, "", 1);
    CHECK(!fromEvent(sig, r), "signal events are not messages");
    Record raw;
    raw.type = "voice.audio"; raw.time = kT0;
    CHECK(!db.add(raw), "voice record refused by add()");
    raw.type = "pager.message"; raw.extra = "encrypted=true\n";
    CHECK(!db.add(raw), "encrypted record refused by add()");
    CHECK(db.count(Filter()) == 0, "nothing stored");
}

void testLimits() {
    Db db;
    db.open(":memory:");
    for (int i = 0; i < 100; i++) { Record r; fromEvent(pager(i, "msg", kT0 + i), r); db.add(r); }
    Limits l; l.maxRows = 40; l.maxBytes = 0;
    db.setLimits(l);
    CHECK(db.prune() == 60, "row cap prunes 60");
    Filter f; f.oldestFirst = true; f.limit = 1;
    CHECK(db.query(f)[0].time == kT0 + 60, "oldest rows went first");
    f.text = "msg";
    CHECK(db.count(f) == 40, "index follows deletes");
    l.maxRows = 0; l.maxAgeDays = 1.0 / 86400 * 1e9;   // everything is younger than ~31 years
    db.setLimits(l);
    CHECK(db.prune() == 0, "age cap keeps young rows");
    l.maxAgeDays = 1;   // test rows are from 2026-10-17, now is later in the real clock or earlier: just check no crash
    db.setLimits(l);
    db.prune();
    db.clear();
    CHECK(db.count(Filter()) == 0, "clear");
    f.text = "msg";
    CHECK(db.count(f) == 0, "clear empties the index");
}

void testSizeCap() {
    const std::string path = (fs::temp_directory_path() / "onair_msgdb_cap.db").string();
    for (const char* sfx : {"", "-wal", "-shm"}) fs::remove(path + sfx);
    Db db;
    CHECK(db.open(path), "open file");
    std::string big(800, 'x');
    for (int i = 0; i < 3000; i++) { Record r; fromEvent(pager(i, ("word" + std::to_string(i) + " " + big).c_str(), kT0 + i), r); db.add(r); }
    const int64_t before = db.stats().bytes;
    Limits l; l.maxRows = 0; l.maxBytes = before / 3;
    db.setLimits(l);
    db.prune();
    Stats s = db.stats();
    CHECK(s.bytes <= l.maxBytes, "size cap %lld <= %lld", (long long)s.bytes, (long long)l.maxBytes);
    CHECK(s.rows > 0 && s.rows < 3000 && s.newest == kT0 + 2999, "newest kept (%lld rows)", (long long)s.rows);
    db.close();
    // persistence
    Db d2;
    CHECK(d2.open(path), "reopen");
    CHECK(d2.stats().rows == s.rows, "rows survive");
    d2.close();
    for (const char* sfx : {"", "-wal", "-shm"}) fs::remove(path + sfx);
}

void testExport() {
    Db db;
    db.open(":memory:");
    Record r;
    fromEvent(acars("A6-EDA", "HELLO, \"WORLD\"\nline2", kT0), r); db.add(r);
    au::Event ap("aprs.packet");
    ap.at(kT0 + 5, "APRS", 144.8e6);
    ap.str("from", "LY1ABC-9").str("info", "!5441.00N/02517.00E>test").num("lat", 54.68).num("lon", 25.28);
    fromEvent(ap, r); db.add(r);
    std::string csv = db.exportAs(Filter(), "csv");
    CHECK(csv.rfind("time,type,mode,freq_hz,ident,text,lat,lon\r\n", 0) == 0, "csv header");
    CHECK(csv.find("\"HELLO, \"\"WORLD\"\"\nline2\"") != std::string::npos, "csv quoting");
    CHECK(csv.find("2026-10-17T10:15:00Z") != std::string::npos, "csv time");
    std::string js = db.exportAs(Filter(), "json");
    CHECK(js.find("\"ident\":\"LY1ABC-9\"") != std::string::npos && js.find("\"lat\":54.68") != std::string::npos, "json");
    CHECK(js.find("\"tail\":\"A6-EDA\"") != std::string::npos, "json fields");
    std::string adi = db.exportAs(Filter(), "adif");
    CHECK(adi.find("<CALL:8>LY1ABC-9") != std::string::npos && adi.find("<QSO_DATE:8>20261017") != std::string::npos, "adif");
    CHECK(adi.find("A6-EDA") == std::string::npos, "adif: only call signs");
    CHECK(db.exportAs(Filter(), "json").size() > 10 && Db().exportAs(Filter(), "csv").find("time,") == 0, "closed db exports header only");
}

void testWriterThreadAndTap() {
    Db db;
    db.open(":memory:");
    db.start();
    au::Engine eng;
    eng.setTap([&](const au::Event& e) { db.enqueue(e); }, au::familyBit(au::FAcars) | au::familyBit(au::FPager) | au::familyBit(au::FAdsb));
    CHECK(eng.wants(au::FAcars) && !eng.wants(au::FAis), "tap makes the decoders emit");
    eng.submit(acars("A6-EDA", "ONE", kT0));
    eng.submit(pager(5, "TWO", kT0 + 1));
    for (int i = 0; i < 5; i++) {   // chatty aircraft updates: stored once a minute per aircraft
        au::Event a("adsb.aircraft");
        a.at(kT0 + i * 2, "ADS-B", 1090e6);
        a.str("hex", "4840D6").num("alt_ft", 1000 + i);
        eng.submit(std::move(a));
    }
    au::Event s("signal.new");
    s.at(kT0, "", 1e6);
    eng.submit(std::move(s));
    db.stop();   // flushes
    CHECK(db.count(Filter()) == 3, "tap -> queue -> store, throttled (%lld)", (long long)db.count(Filter()));
    eng.setTap(nullptr, 0);
    CHECK(!eng.wants(au::FAcars), "tap removed");
}
}  // namespace

void testSearchFeatures() {
    // tokenising: case, separators, prefix marker, non-ASCII letters, hostile characters
    auto tk = tokenize("  A6-EDA gate* \"x\"  Caf\xC3\xA9 ");
    CHECK(tk.size() == 5 && tk[0].word == "a6" && tk[1].word == "eda" && tk[2].word == "gate" && tk[2].prefix && tk[3].word == "x" && tk[4].word == "caf\xC3\xA9" && !tk[4].prefix, "tokenize");
    CHECK(tokenize("*** ...").empty(), "no tokens from punctuation");

    Db db;
    CHECK(db.open(":memory:"), "open");
    Record a, b, c;
    a.type = "acars.message"; a.mode = "ACARS"; a.time = kT0; a.freqHz = 131.725e6; a.ident = "FIRE"; a.text = "routine"; a.source = "/rec/morning.sigmf-data";
    b.type = "pager.message"; b.mode = "POCSAG"; b.time = kT0 + 10; b.freqHz = 152e6; b.ident = "1234"; b.text = "fire fire fire at the depot"; b.source = "";
    c.type = "acars.message"; c.mode = "ACARS"; c.time = kT0 + 20; c.freqHz = 131.55e6; c.ident = "X"; c.text = "a fire was reported once"; c.source = "/rec/evening.sigmf-data";
    CHECK(db.add(a) && db.add(b) && db.add(c), "add three");
    Filter f;
    f.text = "fire"; f.relevance = true;
    auto v = db.query(f);
    CHECK(v.size() == 3 && v[0].ident == "FIRE", "relevance: a hit in the identity ranks first");
    f.relevance = false;
    v = db.query(f);
    CHECK(v.size() == 3 && v[0].ident == "X", "default order is newest first");
    f = Filter(); f.freqLo = 130e6; f.freqHi = 140e6;
    CHECK(db.count(f) == 2, "frequency range");
    f.freqLo = 131.6e6;
    CHECK(db.count(f) == 1 && db.query(f)[0].ident == "FIRE", "narrower frequency range");
    f = Filter(); f.source = "MORNING";
    v = db.query(f);
    CHECK(v.size() == 1 && v[0].source == "/rec/morning.sigmf-data", "source filter, case-insensitive, and the source comes back");
    f = Filter(); f.text = "fire"; f.mode = "ACARS"; f.since = kT0 + 5; f.freqHi = 140e6;
    CHECK(db.count(f) == 1, "text + mode + time + frequency together");
    f = Filter(); f.text = "fir*"; f.relevance = true;
    CHECK(db.count(f) == 3, "prefix with relevance");
}

int main() {
    testSearchFeatures();
    testBasic();
    testRefusals();
    testLimits();
    testSizeCap();
    testExport();
    testWriterThreadAndTap();
    return testSummary("msgdb");
}
