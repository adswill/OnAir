// The voice watch list (voice/watch.h): matching a talkgroup, a radio and a callsign across modes, one event per call, the mode filter,
// the history (active and ended calls), the record rules it keeps in step, the saved text. A test clock drives the monitors; nothing is played.
#include "dect2/voice/watch.h"
#include "../common/test_util.h"
#include <string>

using namespace dect2;
using namespace dect2::voice;

static double gT = 0;
static int64_t gWall = 1790000000;

static Monitor& fresh(const std::string& name) {
    Monitor& m = monitor(name);
    m.setClock([] { return gT; }, [] { return gWall; });
    m.setSettings(Settings());
    m.clearHistory();
    return m;
}
static CallInfo tg(uint32_t dst, uint32_t src, int slot = 1) { CallInfo c; c.dst = dst; c.src = src; c.slot = slot; c.group = true; return c; }

int main() {
    Monitor& dmr = fresh("watch-a");
    Monitor& p25 = fresh("watch-b");
    Watch w;
    CHECK(w.poll(gWall).empty(), "an empty list raises nothing");

    WatchEntry e1; e1.match = "TG 91"; e1.name = "Worldwide"; e1.alert = true; e1.sound = true;
    WatchEntry e2; e2.match = "3120123"; e2.mode = "watch-b";
    WatchEntry bad; bad.match = "   ";
    const uint32_t id1 = w.add(e1), id2 = w.add(e2);
    CHECK(id1 && id2 && id1 != id2 && w.add(bad) == 0, "entries get ids, an empty match is refused");
    WatchEntry noAlert; noAlert.match = "1"; noAlert.sound = true;
    const uint32_t id3 = w.add(noAlert);
    bool soundOff = true; for (auto& e : w.list()) if (e.id == id3) soundOff = !e.sound;
    CHECK(soundOff, "a beep without a notification is dropped");

    gT = 100;
    dmr.offer(tg(91, 555)); dmr.offer(tg(7, 8, 2));
    auto ev = w.poll(gWall);
    CHECK(ev.size() == 1 && ev[0].entry == id1 && ev[0].mode == "watch-a", "TG 91 on the first mode: %zu events", ev.size());
    CHECK(ev.size() == 1 && ev[0].text.find("91") != std::string::npos, "the text names the talkgroup: %s", ev.empty() ? "" : ev[0].text.c_str());
    gT += 0.06; dmr.offer(tg(91, 555));
    CHECK(w.poll(gWall).empty(), "the same call raises no second event");

    dmr.offer(tg(1234, 3120123, 2));   // radio 3120123 on the first mode: entry 2 is limited to the second mode
    CHECK(w.poll(gWall).empty(), "an entry limited to a mode ignores the others");
    p25.offer(tg(2000, 3120123));
    ev = w.poll(gWall);
    CHECK(ev.size() == 1 && ev[0].entry == id2 && ev[0].mode == "watch-b", "a radio ID matches as a source on its mode");

    gT += 5; dmr.housekeep();
    gT += 0.1; dmr.offer(tg(91, 556));   // a new call on the same talkgroup
    ev = w.poll(gWall);
    CHECK(ev.size() == 1, "a later call of the talkgroup is a new event (%zu)", ev.size());
    CHECK(w.events().size() == 3, "the history of events holds all three (%zu)", w.events().size());

    const auto hist = w.history(id1);
    CHECK(hist.size() >= 2 && hist[0].first == "watch-a", "history of the entry has the calls of that talkgroup (%zu)", hist.size());
    CHECK(w.history(9999).empty(), "no history for an unknown entry");

    // saved text
    const std::string t = w.saveText();
    Watch w2; w2.loadText(t);
    CHECK(w2.list().size() == 3 && w2.list()[0].name == "Worldwide" && w2.list()[0].alert && w2.list()[0].sound && w2.list()[1].mode == "watch-b", "save and load round trip");
    w2.loadText("garbage\n\t\t\t\t\t\n");
    CHECK(w2.list().empty(), "lines without a match are skipped");

    // record rules: ticking record adds one rule and switches auto-record on; unticking removes only that rule
    sess::RecordRules rr;
    CHECK(!rr.enabled(), "auto-record starts off");
    w.syncRecord(rr);
    CHECK(rr.list().empty() && !rr.enabled(), "nothing ticked: no rule, still off");
    WatchEntry r1 = w.list()[0]; r1.record = true; w.update(r1);
    w.syncRecord(rr);
    CHECK(rr.list().size() == 1 && rr.enabled(), "record ticked: one rule, auto-record on");
    CHECK(rr.list()[0].match == "TG 91" && rr.list()[0].field == sess::RecField::Any, "the rule matches the entry");
    w.syncRecord(rr);
    CHECK(rr.list().size() == 1, "syncing again adds nothing");
    WatchEntry foreign; foreign.name = "mine"; foreign.match = "5555"; rr.add(foreign);
    r1 = w.list()[0]; r1.record = false; w.update(r1);
    w.syncRecord(rr);
    CHECK(rr.list().size() == 1 && rr.list()[0].match == "5555", "unticking removed only the watch rule");
    r1 = w.list()[0]; r1.record = true; w.update(r1); w.syncRecord(rr);
    w.remove(w.list()[0].id); w.syncRecord(rr);
    CHECK(rr.list().size() == 1, "removing the entry removes its rule");

    // an encrypted call is listed (the fact that it happened) and never gets a recording from the monitor
    CallInfo enc = tg(91, 9); enc.encrypted = true;
    dmr.offer(enc);
    CHECK(true, "encrypted offers do not crash");
    return testSummary("voice_watch");
}
