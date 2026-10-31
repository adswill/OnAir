// The voice monitor (voice_monitor.h) on its own, with a test clock: the ID lists, one call at a time with hang time, the mute and allow
// lists, hold on a talkgroup, priority calls taking the sound, encrypted and squelched frames, a talkgroup found late, a new talker,
// the recording of a played call (WAV + JSON, never an encrypted one), the history search and the settings text.
// NOT covered here: a receiver (test_voice_loopback.cpp runs DMR through the stub plug-in into the monitor).
#include "dect2/voice/voice_monitor.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace dect2;
using namespace dect2::voice;
#include "../common/test_util.h"

namespace fs = std::filesystem;

static double gT = 0;
static int64_t gWall = 1790000000;   // 2026-09-21

static Monitor& fresh(const std::string& name) {
    Monitor& m = monitor(name);
    m.setClock([] { return gT; }, [] { return gWall; });
    m.setSettings(Settings());
    m.clearHistory();
    return m;
}

static CallInfo tg(uint32_t dst, uint32_t src, int slot = 1) { CallInfo c; c.dst = dst; c.src = src; c.slot = slot; c.group = true; c.access = "CC 1"; return c; }

static void testIdList() {
    IdList l("TG 91, 3100-3199; LY*\nm0abc tg2620");
    CHECK(l.matchId(91) && l.matchId(3150) && l.matchId(2620) && !l.matchId(92) && !l.matchId(0), "numbers and ranges");
    CHECK(l.matchText("LY1ABC") && l.matchText("m0abc") && !l.matchText("M0ABD"), "callsigns and prefixes");
    CallInfo c; c.srcText = "LY2XYZ"; c.dstText = "CQCQCQ";
    CHECK(l.match(c) && !l.matchTarget(c), "a source callsign matches as a source");
    CHECK(IdList("*").matchTarget(tg(5, 1)), "* matches every target");
    CHECK(IdList("").empty(), "empty list");
}

static void testOneAtATime() {
    Monitor& m = fresh("t-one");
    gT = 100;
    CHECK(m.offer(tg(91, 1001, 1)) == Verdict::Play, "first call plays");
    gT += 0.06;
    CHECK(m.offer(tg(92, 1002, 2)) == Verdict::Busy, "second call waits");
    CHECK(m.offer(tg(91, 1001, 1)) == Verdict::Play, "first keeps playing");
    CallEntry now;
    CHECK(m.nowTalking(now) && now.info.dst == 91 && now.info.src == 1001, "now talking is TG 91");
    CHECK(m.calls().size() == 2 && m.calls()[0].info.dst == 91, "two active calls, the playing first");
    gT += 1.0;   // TG 91 stops; within the hang time the sound stays with it
    CHECK(m.offer(tg(92, 1002, 2)) == Verdict::Busy, "hang time keeps the sound for TG 91");
    gT += 1.5;
    CHECK(m.offer(tg(92, 1002, 2)) == Verdict::Play, "after the hang time TG 92 plays");
    // the same talkgroup on another carrier (simulcast in a wide capture) is not played twice
    CallInfo sim = tg(92, 1002, 2); sim.carrierHz = 446.1e6;
    CHECK(m.offer(sim) == Verdict::Busy, "a simulcast copy waits");
    // an explicit end frees the sound at once
    m.endCall(tg(92, 1002, 2));
    CHECK(m.offer(sim) == Verdict::Play, "after the terminator the other copy may play");
}

static void testMuteAllow() {
    Monitor& m = fresh("t-mute");
    gT = 200;
    Settings s; s.mute = "91, 5000-5999";
    m.setSettings(s);
    CHECK(m.offer(tg(91, 1)) == Verdict::Muted, "muted talkgroup");
    CHECK(m.offer(tg(7, 5005, 2)) == Verdict::Muted, "muted source range");
    CHECK(m.offer(tg(8, 1, 1)) == Verdict::Play, "another talkgroup plays");
    m.muteToken("8");
    gT += 0.1;
    CHECK(m.offer(tg(8, 1, 1)) == Verdict::Muted, "muteToken adds to the mute list");
    m.muteToken("8");
    CHECK(m.settings().mute == "91, 5000-5999, 8", "no duplicate: '%s'", m.settings().mute.c_str());
    s = Settings(); s.allowOnly = true; s.allow = "3100-3199";
    m.setSettings(s);
    gT += 5;
    CHECK(m.offer(tg(91, 1)) == Verdict::Filtered, "not on the allow list");
    CHECK(m.offer(tg(3120, 1, 2)) == Verdict::Play, "on the allow list");
}

static void testHold() {
    Monitor& m = fresh("t-hold");
    gT = 300;
    CHECK(m.offer(tg(91, 1)) == Verdict::Play, "TG 91 plays");
    m.holdCurrent();
    CHECK(m.settings().hold == "91", "hold the playing talkgroup: '%s'", m.settings().hold.c_str());
    gT += 10;   // TG 91 is long over: another talkgroup still is not played
    CHECK(m.offer(tg(92, 2, 2)) == Verdict::NotHeld, "held on TG 91");
    CHECK(m.offer(tg(91, 3, 1)) == Verdict::Play, "TG 91 plays again");
    m.releaseHold();
    gT += 10;
    CHECK(m.offer(tg(92, 2, 2)) == Verdict::Play, "released: scan again");
}

static void testPriority() {
    Monitor& m = fresh("t-prio");
    gT = 400;
    Settings s; s.priority = "9";
    m.setSettings(s);
    CHECK(m.offer(tg(91, 1, 1)) == Verdict::Play, "an ordinary call plays");
    gT += 0.05;
    CHECK(m.offer(tg(9, 2, 2)) == Verdict::Play, "a priority call takes the sound");
    CHECK(m.offer(tg(91, 1, 1)) == Verdict::LowerPriority, "the ordinary call waits");
    CallEntry now;
    CHECK(m.nowTalking(now) && now.info.dst == 9 && now.priority, "now talking is the priority call");
    CallInfo other = tg(8, 3, 1); other.carrierHz = 1e6;
    CHECK(m.offer(other) == Verdict::LowerPriority, "a third call waits too");
}

static void testEncryptedSquelchLate() {
    Monitor& m = fresh("t-enc");
    gT = 500;
    CallInfo e = tg(91, 1); e.encrypted = true;
    CHECK(m.offer(e) == Verdict::Encrypted, "encrypted is never played");
    CallInfo nc = tg(92, 2, 2); nc.codecMissing = true;
    CHECK(m.offer(nc) == Verdict::NoCodec, "no codec");
    Settings s; s.squelchPct = 5;
    m.setSettings(s);
    gT += 5;
    CallInfo bad = tg(93, 3); bad.errorRate = 0.12f;
    CHECK(m.offer(bad) == Verdict::Squelched, "12 %% errors over a 5 %% squelch");
    bad.errorRate = 0.01f;
    CHECK(m.offer(bad) == Verdict::Play, "1 %% errors pass");
    // a late entry: the first frames have no target, then it is found: one call
    m.setSettings(Settings());
    gT += 5;
    CallInfo late; late.slot = 2;
    CHECK(m.offer(late) == Verdict::Play, "a frame of an unknown call plays");
    late.dst = 3100; late.src = 77;
    CHECK(m.offer(late) == Verdict::Play, "the same call, target now known");
    int n = 0;
    for (const auto& c : m.calls()) if (c.info.slot == 2) { n++; CHECK(c.info.dst == 3100 && c.frames == 2, "merged entry dst %u frames %llu", c.info.dst, (unsigned long long)c.frames); }
    CHECK(n == 1, "one entry for the late call, %d", n);
    // another talker on the same talkgroup: a new call in the history, the sound goes on
    gT += 0.1;
    CallInfo next = late; next.src = 78;
    CHECK(m.offer(next) == Verdict::Play, "the next talker plays");
    CHECK(m.history("3100").size() == 2, "two calls of TG 3100 in the history");
}

static std::string readAll(const fs::path& p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

static void testRecording() {
    Monitor& m = fresh("t-rec");
    const fs::path dir = fs::temp_directory_path() / "onair-test-voice-rec";
    std::error_code ec;
    fs::remove_all(dir, ec);
    Settings s; s.record = true; s.recordDir = dir.string(); s.labels = "91=Worldwide\nTG 3100 = Lithuania";
    m.setSettings(s);
    gT = 600;
    CallInfo c = tg(91, 2620001); c.alias = "Kris";
    std::vector<float> st(160 * 2);
    for (int f = 0; f < 50; f++) {   // 50 frames of 20 ms at 8 kHz, stereo
        for (int i = 0; i < 160; i++) st[2 * i] = st[2 * i + 1] = (float)((f * 160 + i) % 100) / 200.f;
        CHECK(m.offer(c) == Verdict::Play, "recorded call plays");
        m.audio(c, st.data(), 160, 2, 8000);
        gT += 0.02;
    }
    // an encrypted call at the same time is not recorded
    CallInfo e = tg(92, 5, 2); e.encrypted = true;
    m.offer(e);
    m.audio(e, st.data(), 160, 2, 8000);
    gT += 5;
    m.housekeep();
    std::vector<fs::path> wavs, jsons;
    for (auto& p : fs::recursive_directory_iterator(dir, ec)) {
        if (p.path().extension() == ".wav") wavs.push_back(p.path());
        if (p.path().extension() == ".json") jsons.push_back(p.path());
    }
    CHECK(wavs.size() == 1 && jsons.size() == 1, "one WAV and one JSON: %zu %zu", wavs.size(), jsons.size());
    if (wavs.size() == 1) {
        int rate = 0, ch = 0; std::vector<int16_t> pcm;
        CHECK(readWav(wavs[0].string(), rate, ch, pcm), "WAV reads");
        CHECK(rate == 8000 && ch == 1 && pcm.size() == 8000, "8 kHz mono, 1 s: %d %d %zu", rate, ch, pcm.size());
        if (pcm.size() > 150) CHECK(std::abs(pcm[150] - (int)(0.25f * 32767)) <= 1 && pcm[100] == 0, "samples kept: %d %d", pcm[150], pcm[100]);
        CHECK(wavs[0].filename().string().find("TG_91_from_2620001") != std::string::npos && wavs[0].parent_path().filename() == "2026-09-21",
              "file name %s", wavs[0].string().c_str());
    }
    if (jsons.size() == 1) {
        const std::string js = readAll(jsons[0]);
        CHECK(js.find("\"mode\": \"t-rec\"") != std::string::npos && js.find("\"target\": \"TG 91\"") != std::string::npos &&
              js.find("\"source_id\": 2620001") != std::string::npos && js.find("\"label\": \"Worldwide\"") != std::string::npos &&
              js.find("\"alias\": \"Kris\"") != std::string::npos && js.find("\"audio_seconds\": 1.00") != std::string::npos &&
              js.find("\"access\": \"CC 1\"") != std::string::npos, "JSON details:\n%s", js.c_str());
    }
    const auto h = m.history("world");
    CHECK(h.size() == 1 && h[0].info.dst == 91 && !h[0].file.empty() && !h[0].active, "history search by label");
    CHECK(m.history("encrypted").size() == 1, "history search by state");
    CHECK(m.history("lithuania").empty() && m.history().size() == 2, "search misses; all calls listed");
    fs::remove_all(dir, ec);
}

static void testSettingsText() {
    Monitor& m = fresh("t-set");
    Settings s;
    s.volume = 0.4f; s.squelchPct = 7; s.device = "USB Audio"; s.record = true; s.recordDir = "/tmp/x"; s.allowOnly = true;
    s.allow = "1, 2"; s.mute = "3"; s.priority = "9"; s.hold = "91"; s.labels = "91=World\n92=Europe"; s.hangSec = 3;
    m.setSettings(s);
    const std::string all = saveAllSettings();
    m.setSettings(Settings());
    loadAllSettings(all);
    const Settings r = m.settings();
    CHECK(std::abs(r.volume - 0.4f) < 1e-3 && r.squelchPct == 7 && r.device == "USB Audio" && r.record && r.recordDir == "/tmp/x" && r.allowOnly &&
          r.allow == "1, 2" && r.mute == "3" && r.priority == "9" && r.hold == "91" && r.labels == "91=World\n92=Europe" && r.hangSec == 3, "settings round trip");
    CHECK(m.label(tg(92, 1)) == "Europe", "labels after loading");
}


// ---- 3.5: source names, name-list import, hold that lets go, last heard ----

static void testSourceNames() {
    Monitor& m = fresh("t-rid");
    Settings s;
    s.labels = "91=Worldwide\nRID 3120123=LY1ABC Jonas\nRID 91=Radio ninety-one\nLY2XYZ=Kaunas relay";
    m.setSettings(s);
    gT = 10;
    CallInfo c = tg(91, 3120123);
    m.offer(c);
    CallEntry e = m.calls()[0];
    CHECK(e.label == "Worldwide" && e.srcLabel == "LY1ABC Jonas", "talkgroup and source radio each get their own name");
    // a radio ID that equals a talkgroup number does not take the talkgroup's name
    CHECK(m.sourceLabel(tg(5, 91)) == "Radio ninety-one" && m.label(tg(91, 7)) == "Worldwide", "RID names only sources");
    CallInfo t; t.srcText = "ly2xyz"; t.dstText = "CQCQCQ";
    CHECK(m.sourceLabel(t) == "Kaunas relay", "callsign source name");
    CHECK(!m.history("jonas").empty() && m.history("nobody").empty(), "history search finds a source name");
    const std::string saved = m.saveSettings();
    m.setSettings(Settings());
    m.loadSettings(saved);
    CHECK(m.sourceLabel(tg(1, 3120123)) == "LY1ABC Jonas", "source names survive save and load");
}

static void testImportLabels() {
    size_t n = 0;
    // RadioID.net user.csv, with quotes, a BOM, CRLF and a name that tries to break the label syntax
    const std::string ru = "\xEF\xBB\xBFRADIO_ID,CALLSIGN,FIRST_NAME,LAST_NAME,CITY,STATE,COUNTRY\r\n"
                           "3120123,LY1ABC,Jonas,\"Petr, aitis\",Kaunas,,Lithuania\r\n"
                           "2341001,M0XYZ,Tom;x=y,Smith,London,,UK\r\n"
                           "abc,BAD,No,Id,,,\r\n"
                           "5,,,,,,\r\n";
    std::string r = importLabels(ru, ListKind::Auto, &n);
    CHECK(n == 2 && r == "RID 3120123=LY1ABC Jonas Petr, aitis\nRID 2341001=M0XYZ Tom x y Smith\n", "radio list: callsign and name, bad lines skipped");
    // talkgroup list, semicolon separated, header, "TG" prefix
    n = 0;
    r = importLabels("ID;Name;Country\n91;Worldwide;\nTG 2620;Poland;PL\n9;Local;\n", ListKind::Auto, &n);
    CHECK(n == 3 && r == "91=Worldwide\n2620=Poland\n9=Local\n", "talkgroup list");
    // headerless tab list forced to radios
    r = importLabels("3100001\tAlpha\n3100002\tBravo\n", ListKind::Radios, &n);
    CHECK(n == 2 && r == "RID 3100001=Alpha\nRID 3100002=Bravo\n", "headerless radios");
    r = importLabels("", ListKind::Auto, &n);
    CHECK(n == 0 && r.empty(), "empty input");
    // a long name is cut, the result is valid UTF-8
    r = importLabels("7," + std::string(40, '\xC3') + std::string(40, '\xA4') + "\n", ListKind::Auto, &n);
    CHECK(n == 1 && r.size() < 80, "long name cut");
    // merge: the new line replaces the old one with the same key, others stay
    const std::string mg = mergeLabels("91=Old\nrid 5=Five\n7=Seven", "91=New\nRID 6=Six\n");
    CHECK(mg == "rid 5=Five\n7=Seven\n91=New\nRID 6=Six\n", "merge replaces by key");
    // imported names reach the monitor
    Monitor& m = fresh("t-imp");
    Settings s; s.labels = mergeLabels("", importLabels(ru));
    m.setSettings(s);
    CHECK(m.sourceLabel(tg(1, 2341001)) == "M0XYZ Tom x y Smith", "imported source name used");
}

static void testHoldRelease() {
    Monitor& m = fresh("t-holdsec");
    Settings s; s.hold = "91"; s.holdSec = 30;
    gT = 1000;
    m.setSettings(s);
    CHECK(m.offer(tg(92, 5)) == Verdict::NotHeld, "another talkgroup is not played during the hold");
    gT += 20;
    CHECK(m.offer(tg(91, 6)) == Verdict::Play, "the held talkgroup plays and renews the hold");
    gT += 20;   // 40 s since the hold was set but 20 s since the held talkgroup
    CHECK(m.offer(tg(92, 5, 2)) == Verdict::NotHeld && m.settings().hold == "91", "hold still on");
    gT += 31;   // quiet for longer than holdSec
    const uint64_t g = m.settingsGen();
    CHECK(m.offer(tg(92, 5, 2)) == Verdict::Play, "the hold let go by itself, the call plays");
    CHECK(m.settings().hold.empty() && m.settingsGen() != g, "hold cleared and the settings generation changed");
    // no calls at all: housekeeping lets go
    s.hold = "93"; gT += 1; m.setSettings(s);
    gT += 40; m.housekeep();
    CHECK(m.settings().hold.empty(), "housekeeping releases an idle hold");
    // holdSec 0: held until released
    s.hold = "93"; s.holdSec = 0; m.setSettings(s);
    gT += 5000; m.housekeep();
    CHECK(m.settings().hold == "93", "no timeout when holdSec is 0");
    s.holdSec = 12.5; m.setSettings(s);
    const std::string t = m.saveSettings();
    m.setSettings(Settings()); m.loadSettings(t);
    CHECK(std::abs(m.settings().holdSec - 12.5) < 1e-6, "holdSec saved");
}

static void testHeard() {
    Monitor& m = fresh("t-heard");
    Settings s; s.labels = "RID 1001=Alice";
    m.setSettings(s);
    gT = 50; gWall = 1790000100;
    CallInfo a = tg(91, 1001); a.errorRate = 0.02f;
    m.offer(a); gT += 1.0; m.offer(a); gT += 4; gWall += 4;   // first call, over
    CallInfo b = tg(91, 1002); b.emergency = true; b.errorRate = 0.04f;
    m.offer(b); gT += 0.5; m.offer(b);                         // Bob, new talker on the talkgroup
    gT += 5; gWall += 5; m.housekeep();
    gT += 1; gWall += 1;
    a.errorRate = 0.06f; m.offer(a); gT += 0.5; m.offer(a); gT += 5; gWall += 5; m.housekeep();   // Alice again, newest
    CallInfo none; none.dst = 9; m.offer(none); gT += 5; m.housekeep();   // a call with no source is not listed
    const auto h = m.heard();
    CHECK(h.size() == 2, "two radios heard, the unknown source left out");
    CHECK(h.size() == 2 && h[0].last.src == 1001 && h[0].calls == 2 && h[0].name == "Alice", "newest first, two calls, named");
    CHECK(h.size() == 2 && h[0].errorRate > 0.039f && h[0].errorRate < 0.041f && !h[0].emergency, "mean error rate");
    CHECK(h.size() == 2 && h[1].last.src == 1002 && h[1].emergency && h[1].calls == 1 && h[1].seconds > 0.4, "second radio with emergency flag and airtime");
    CHECK(m.heard(1).size() == 1, "max");
    CallInfo t; t.srcText = "M0XYZ"; t.dstText = "CQCQCQ"; t.group = true;
    gT += 10; m.offer(t);
    bool found = false;
    for (auto& x : m.heard()) if (x.key == "txt:M0XYZ") found = true;
    CHECK(found, "callsign sources are listed too");
}

int main() {
    testIdList();
    testOneAtATime();
    testMuteAllow();
    testHold();
    testPriority();
    testEncryptedSquelchLate();
    testRecording();
    testSettingsText();
    testSourceNames();
    testImportLabels();
    testHoldRelease();
    testHeard();
    return testSummary("voice_monitor");
}
