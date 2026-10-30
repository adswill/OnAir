// Privacy of the session data (docs/MODE-EXPERIENCE.md, 2.7): the per-install salt and salted short tags, the "Keep real ids" switch per mode,
// the single "Redact message text" switch, escaping of exports (formula injection, control characters, invalid UTF-8), the Anonymise option,
// owner-only files, and "Delete all data for this mode" / "Delete everything".
#include "dect2/session/privacy.h"
#include "dect2/session/session_store.h"
#include "session_test_util.h"
#include "../common/test_util.h"
#include <fstream>
#include <sstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace dect2::sess;
using sesstest::mk;

static StoreConfig quiet() {
    StoreConfig c;
    c.retentionEverySec = 0;
    c.loadSalt = false;
    return c;
}

static std::array<uint8_t, 16> saltOf(uint8_t b) { std::array<uint8_t, 16> a; a.fill(b); return a; }

static void testSaltAndHash() {
    Privacy a, b;
    a.setSalt(saltOf(1));
    b.setSalt(saltOf(2));
    const std::string h1 = a.hash(51, "AA:BB:CC:DD:EE:FF");
    CHECK(h1.size() == 16 && h1.find_first_not_of("0123456789abcdef") == std::string::npos, "16 hex digits: %s", h1.c_str());
    CHECK(h1 == a.hash(51, "AA:BB:CC:DD:EE:FF"), "stable within an install");
    CHECK(h1 != b.hash(51, "AA:BB:CC:DD:EE:FF"), "different across installs");
    CHECK(h1 != a.hash(54, "AA:BB:CC:DD:EE:FF") && h1 != a.hash(51, "AA:BB:CC:DD:EE:00"), "different per mode and per id");
    const std::string tag = a.shortTag(51, "AA:BB:CC:DD:EE:FF", "dev");
    CHECK(tag.size() == 8 && tag.rfind("dev ", 0) == 0 && tag.substr(4) == [&] { std::string x = h1.substr(0, 4); for (char& c : x) c = (char)toupper((unsigned char)c); return x; }(), "short tag 'dev 7F3A' form: %s", tag.c_str());
    CHECK(a.shortTag(51, "x") != b.shortTag(51, "x") || a.shortTag(51, "y") != b.shortTag(51, "y"), "tags differ across installs");

    // the salt file: created once, 16 bytes, owner only, the same on the next start
    sesstest::TempDir td;
    Privacy p, q;
    CHECK(p.loadSalt(td.file("keys")), "salt created");
    const std::string file = td.file("keys/session_salt.bin");
    CHECK(sesstest::fs::exists(sesstest::fs::path(file)) && sesstest::fs::file_size(sesstest::fs::path(file)) == 16, "16 bytes on disk");
    CHECK(q.loadSalt(td.file("keys")) && p.hash(9, "abc") == q.hash(9, "abc"), "the same salt on the next start");
    Privacy other;
    sesstest::TempDir td2;
    other.loadSalt(td2.file("keys"));
    CHECK(other.hash(9, "abc") != p.hash(9, "abc"), "another install has another salt");
#ifndef _WIN32
    struct stat st;
    CHECK(stat(file.c_str(), &st) == 0 && (st.st_mode & 077) == 0, "salt file is owner only (%o)", (unsigned)(st.st_mode & 0777));
    CHECK(stat(td.file("keys").c_str(), &st) == 0 && (st.st_mode & 077) == 0, "salt folder is owner only (%o)", (unsigned)(st.st_mode & 0777));
#endif
    const auto before = p.salt();
    CHECK(p.regenerateSalt() && p.salt() != before && p.hash(9, "abc") != q.hash(9, "abc"), "a new salt on request");
    // the salt never goes into the settings text
    p.setKeepRealIds(51, true);
    p.setRedactText(true);
    const std::string saved = p.save();
    std::string saltHex;
    for (uint8_t c : p.salt()) { char h[4]; snprintf(h, sizeof h, "%02x", c); saltHex += h; }
    CHECK(saved.find(saltHex) == std::string::npos && saved.find("redact_text=1") != std::string::npos && saved.find("keep_real_ids=51") != std::string::npos, "settings text: %s", saved.c_str());
}

static void testClassesAndSwitch() {
    Privacy p;
    p.setSalt(saltOf(7));
    p.applyDefaultClasses();
    // the defaults of 2.7: BLE, Wi-Fi, ANT+, NFC, DECT, meters, Remote ID, pagers, satellite terminals are personal; digital voice only with a position
    for (uint16_t m : {(uint16_t)51, (uint16_t)54, (uint16_t)77, (uint16_t)148, (uint16_t)78, (uint16_t)86, (uint16_t)56, (uint16_t)23, (uint16_t)19, (uint16_t)18})
        CHECK(p.classOf(m) == IdClass::Personal && p.masks(m, false), "mode %u is personal", m);
    for (uint16_t m : {(uint16_t)9, (uint16_t)38, (uint16_t)50, (uint16_t)45, (uint16_t)31, (uint16_t)33})
        CHECK(p.classOf(m) == IdClass::WithPosition && !p.masks(m, false) && p.masks(m, true), "mode %u: masked only with a position", m);
    CHECK(p.classOf(11) == IdClass::Open && !p.masks(11, true), "ADS-B is open");
    CHECK(p.protect(51, "AA:BB", false, "dev").rfind("dev ", 0) == 0 && p.protect(11, "4CA7B1") == "4CA7B1" && p.protect(51, "") == "", "protect");
    CHECK(p.protect(9, "2321001", false) == "2321001" && p.protect(9, "2321001", true) != "2321001", "a radio id is hidden only when a position goes with it");
    // "Keep real ids" per mode
    p.setKeepRealIds(51, true);
    CHECK(p.keepRealIds(51) && !p.masks(51, false) && p.protect(51, "AA:BB") == "AA:BB", "keep real ids shows the real id");
    CHECK(!p.keepRealIds(54) && p.masks(54, false), "another mode is not affected");
    // settings round trip
    Privacy q;
    q.load(p.save());
    CHECK(q.keepRealIds(51) && !q.keepRealIds(54), "settings round trip");
    // text redaction
    CHECK(p.text("hello") == "hello", "off: text as it is");
    p.setRedactText(true);
    const std::string red = p.text("h\xC3\xA9llo wor\xC3\xB6ld");
    CHECK(red == "[redacted, 12 characters]" && p.text("") == "", "on: placeholder with the length in characters: %s", red.c_str());
}

static void testEscaping() {
    CHECK(csvCell("plain") == "plain" && csvCell("a,b") == "\"a,b\"" && csvCell("say \"hi\"") == "\"say \"\"hi\"\"\"", "quoting");
    CHECK(csvCell("=1+1") == "'=1+1" && csvCell("+49 170") == "'+49 170" && csvCell("-5") == "'-5" && csvCell("@SUM(A1)") == "'@SUM(A1)", "formula starts get an apostrophe");
    CHECK(csvCell("  =cmd|' /C calc'!A0") == "'  =cmd|' /C calc'!A0", "also after spaces");
    CHECK(csvCell("=HYPERLINK(\"http://x\",\"y\")") == "\"'=HYPERLINK(\"\"http://x\"\",\"\"y\"\")\"", "formula with quotes: both treatments");
    CHECK(csvCell("a\x01" "b\r\nc\x7F" "d\te") == "abcd e", "control characters stripped, tab becomes a space: '%s'", csvCell("a\x01" "b\r\nc\x7F" "d\te").c_str());
    CHECK(csvCell("A\xFF" "B") == "A\xEF\xBF\xBD" "B" && validUtf8(csvCell("A\xFF" "B")), "invalid UTF-8 replaced");
    CHECK(csvCell("") == "" && csvCell("\xC5\xA0iauliai") == "\xC5\xA0iauliai", "empty and non-ASCII");
    CHECK(jsonEscape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001" && jsonEscape("x\xFF") == "x\xEF\xBF\xBD", "json");
    CHECK(srtText("one\r\n\r\n\r\ntwo\x01\n") == "one\ntwo", "srt: no empty line inside a cue: '%s'", srtText("one\r\n\r\n\r\ntwo\x01\n").c_str());
    CHECK(stripControl("a\x1b[31mb") == "a[31mb", "escape character removed");
    CHECK(!validUtf8("\xC0\xAF") && !validUtf8("\xED\xA0\x80") && !validUtf8("\xF4\x90\x80\x80") && validUtf8("\xF0\x9F\x93\xA1") && validUtf8(""), "UTF-8 validator (overlong, surrogate, too large)");
}

static void testStorePrivacy() {
    Privacy& pv = privacy();
    pv.setSalt(saltOf(9));
    pv.applyDefaultClasses();
    pv.setKeepRealIds(51, false);
    SessionStore st;
    CHECK(st.openAt(":memory:", quiet()), "open");
    // BLE: the address is hashed in key, who, title; the alias is dropped; the list stays distinguishable
    Session a = mk(51, "AA:BB:CC:DD:EE:01", sesstest::nowMs(), "BLE AA:BB:CC:DD:EE:01 seen 12 times");
    a.kind = (uint8_t)Kind::Thread;
    a.who[0] = "AA:BB:CC:DD:EE:01"; a.alias[0] = "Kristijonas iPhone";
    Session b = a; b.key = b.who[0] = "AA:BB:CC:DD:EE:02"; b.title = "BLE AA:BB:CC:DD:EE:02 seen 3 times";
    a.id = st.begin(a);
    b.id = st.begin(b);
    st.flush();
    Session ga, gb;
    CHECK(st.get(a.id, ga) && st.get(b.id, gb), "stored");
    CHECK(ga.key.rfind("id ", 0) == 0 && ga.who[0] == ga.key && ga.alias[0].empty(), "key and source are short tags, the alias is gone: key '%s' who '%s'", ga.key.c_str(), ga.who[0].c_str());
    CHECK(ga.title.find("AA:BB") == std::string::npos && ga.title.find(ga.key) != std::string::npos, "the address in the title became the tag: '%s'", ga.title.c_str());
    CHECK(ga.key != gb.key, "two devices stay distinguishable");
    auto win = st.window(51);
    bool leak = false;
    for (const Session& s : win) if (s.key.find("AA:BB") != std::string::npos || s.title.find("AA:BB") != std::string::npos || s.who[0].find("AA:BB") != std::string::npos || !s.alias[0].empty()) leak = true;
    CHECK(!leak, "nothing real in the window either");
    CHECK(st.query([] { Query q; q.text = "iPhone"; return q; }()).total == 0 && st.query([] { Query q; q.text = "AA:BB:CC:DD:EE:01"; return q; }()).total == 0, "not searchable by the real address or alias");
    std::string csv = st.exportAs(Query(), "csv");
    CHECK(csv.find("AA:BB") == std::string::npos && csv.find("iPhone") == std::string::npos, "exports hold no real id");
    // the same device gets the same tag next time (stable)
    Session a2 = a; a2.id = 0;
    a2.id = st.begin(a2);
    st.flush();
    Session g2;
    st.get(a2.id, g2);
    CHECK(g2.key == ga.key, "stable tag for the same device");

    // keep real ids for the mode
    pv.setKeepRealIds(51, true);
    Session c = mk(51, "AA:BB:CC:DD:EE:03", sesstest::nowMs(), "BLE AA:BB:CC:DD:EE:03");
    c.who[0] = c.key; c.alias[0] = "Watch";
    c.id = st.begin(c);
    st.flush();
    Session gc;
    CHECK(st.get(c.id, gc) && gc.key == "AA:BB:CC:DD:EE:03" && gc.alias[0] == "Watch", "'Keep real ids' stores the real ones");
    pv.setKeepRealIds(51, false);

    // digital voice: the radio id is hidden only when a position is attached
    Session v = mk(9, "TG 91/slot1", sesstest::nowMs(), "DMR TG 91 from 2321001");
    v.who[0] = "2321001"; v.who[1] = "TG 91";
    Session vp = v; vp.key = "TG 92/slot1"; vp.title = "DMR TG 92 from 2321001"; vp.lat = 54.68; vp.lon = 25.27;
    v.id = st.begin(v);
    vp.id = st.begin(vp);
    st.flush();
    Session gv, gvp;
    st.get(v.id, gv); st.get(vp.id, gvp);
    CHECK(gv.who[0] == "2321001" && gv.title == "DMR TG 91 from 2321001", "a call without a position keeps its ids");
    CHECK(gvp.who[0] != "2321001" && gvp.title.find("2321001") == std::string::npos && gvp.hasPosition(), "a call with a position (LRRP) hides the radio id: '%s'", gvp.title.c_str());
    // ADS-B is open
    Session ad = mk(11, "ICAO 4CA7B1", sesstest::nowMs(), "Flight 4CA7B1");
    ad.who[0] = "4CA7B1";
    ad.id = st.begin(ad);
    st.flush();
    Session gad;
    CHECK(st.get(ad.id, gad) && gad.who[0] == "4CA7B1" && gad.key == "ICAO 4CA7B1", "open modes are as they are");
    st.close();
}

static void testExportSafety() {
    privacy().setSalt(saltOf(3));
    SessionStore st;
    st.setModeName(26, "Airband");
    CHECK(st.openAt(":memory:", quiet()), "open");
    Session s = mk(26, "118.300", sesstest::nowMs(), "=HYPERLINK(\"http://evil\",\"x\"),\x01 injected");
    s.who[0] = "@cmd"; s.alias[0] = "Tower"; s.lat = 54.5; s.lon = 25.1;
    s.id = st.begin(s);
    Session s2 = mk(26, "118.400", sesstest::nowMs(), "second call");
    s2.id = st.begin(s2);
    st.flush();
    const std::string csv = st.exportAs(Query(), "csv");
    CHECK(csv.find(",'=HYPERLINK") != std::string::npos || csv.find("\"'=HYPERLINK") != std::string::npos, "formula neutralised in CSV:\n%s", csv.c_str());
    CHECK(csv.find('\x01') == std::string::npos && csv.find(",'@cmd,") != std::string::npos, "control character gone, '@cmd' neutralised");
    CHECK(csv.find("Tower") != std::string::npos && csv.find("54.5") != std::string::npos, "a plain export keeps what the user asked for");
    const std::string an = st.exportAs(Query(), "csv", true);
    CHECK(an.find("Tower") == std::string::npos && an.find("@cmd") == std::string::npos && an.find("118.300") == std::string::npos && an.find("HYPERLINK") == std::string::npos && an.find("54.5") == std::string::npos, "Anonymise: no ids, aliases, keys, titles or positions:\n%s", an.c_str());
    CHECK(an.find("Airband call") != std::string::npos, "anonymised title is mode and kind");
    const std::string js = st.exportAs(Query(), "json");
    CHECK(js.front() == '[' && js.find("\\u0001") != std::string::npos && js.find("\"mode_name\":\"Airband\"") != std::string::npos, "json export, escaped");
    const std::string jl = st.exportAs(Query(), "jsonl");
    size_t lines = 0;
    for (char c : jl) if (c == '\n') lines++;
    CHECK(lines == 2 && jl.front() == '{', "jsonl: one object per line (export only)");
    CHECK(st.exportAs(Query(), "json").size() > 10 && st.exportAs([] { Query q; q.text = "nomatch"; return q; }(), "json") == "[]\n", "empty json export");
    st.close();
}

static void testDeleteAllData() {
    Privacy& pv = privacy();
    pv.setSalt(saltOf(5));
    sesstest::TempDir td;
    const std::string audio = td.file("audio"), outside = td.file("outside");
    sesstest::fs::create_directories(sesstest::fs::path(audio + "/2026-10-28"));
    sesstest::fs::create_directories(sesstest::fs::path(outside));
    auto touch = [](const std::string& p, size_t n = 100) { std::ofstream f(p, std::ios::binary); f << std::string(n, 'a'); };
    SessionStore st;
    StoreConfig cfg = quiet();
    cfg.audioRoots = {audio};
    CHECK(st.openAt(":memory:", cfg), "open");
    uint64_t ids[3];
    for (int i = 0; i < 3; i++) {
        Session s = mk(i == 2 ? 38 : 9, "k" + std::to_string(i), sesstest::nowMs());
        ids[i] = st.begin(s);
        const std::string wav = audio + "/2026-10-28/call" + std::to_string(i) + ".wav";
        touch(wav);
        touch(audio + "/2026-10-28/call" + std::to_string(i) + ".json");
        st.link(ids[i], LinkType::Audio, wav);
    }
    touch(outside + "/precious.wav");
    st.link(ids[0], LinkType::Audio, outside + "/precious.wav");   // a link that points outside the library: never deleted
    st.setNote(ids[1], "note", "tag");
    st.flush();
    Query q9; q9.mode = 9;
    CHECK(st.query(q9).total == 2, "two sessions in mode 9");
    CHECK(st.deleteMode(9) == 2, "deleteMode returns the sessions removed");
    CHECK(st.query(q9).total == 0 && st.window(9).empty(), "rows and window of the mode are gone");
    CHECK(!sesstest::fs::exists(sesstest::fs::path(audio + "/2026-10-28/call0.wav")) && !sesstest::fs::exists(sesstest::fs::path(audio + "/2026-10-28/call1.wav")), "its audio files are gone");
    CHECK(!sesstest::fs::exists(sesstest::fs::path(audio + "/2026-10-28/call0.json")), "and the sidecar next to them");
    CHECK(sesstest::fs::exists(sesstest::fs::path(outside + "/precious.wav")), "a file outside the library is never touched");
    CHECK(sesstest::fs::exists(sesstest::fs::path(audio + "/2026-10-28/call2.wav")) && st.query([] { Query q; q.mode = 38; return q; }()).total == 1, "another mode is untouched");
    Note n;
    CHECK(!st.note(ids[1], n), "the note went with the session");
    const std::string tagBefore = pv.shortTag(9, "x");
    CHECK(st.deleteEverything() == 1, "deleteEverything");
    CHECK(st.query(Query()).total == 0 && st.window(38).empty(), "nothing left");
    CHECK(!sesstest::fs::exists(sesstest::fs::path(audio + "/2026-10-28/call2.wav")) && !sesstest::fs::exists(sesstest::fs::path(audio + "/2026-10-28/call2.json")), "all library files are gone");
    CHECK(pv.shortTag(9, "x") != tagBefore, "a new salt: old tags cannot be matched to new ones");
    CHECK(st.dbStats().links == 0, "no links left");
    st.close();
}

int main() {
    testSaltAndHash();
    testClassesAndSwitch();
    testEscaping();
    testStorePrivacy();
    testExportSafety();
    testDeleteAllData();
    return testSummary("privacy");
}
