// Auto-record rules (docs/MODE-EXPERIENCE.md, 3.1 and 3.2): default OFF, matching, limits, encrypted never, persistence, and the store telling the
// handlers when a matching session starts and ends (with the raw ids, before they are replaced by tags).
#include "dect2/session/record_rules.h"
#include "dect2/session/session_store.h"
#include "session_test_util.h"
#include "../common/test_util.h"
#include <atomic>

using namespace dect2::sess;
using sesstest::mk;

static Session call(uint16_t mode, const std::string& tgt, const std::string& src = "1234", const std::string& alias = "") {
    Session s = mk(mode, "TG " + tgt, sesstest::nowMs());
    s.who[0] = src;
    s.who[1] = tgt;
    s.alias[0] = alias;
    return s;
}

static void testOffByDefault() {
    RecordRules r;
    CHECK(!r.enabled() && r.list().empty(), "off and empty by default");
    RecordRule x;
    r.add(x);
    CHECK(!r.decide(call(9, "91")).any(), "rules do nothing while the master switch is off");
    r.setEnabled(true);
    CHECK(r.decide(call(9, "91")).audio, "an empty rule records everything of every mode once on");
}

static void testMatching() {
    CHECK(RecordRules::matchList("91, 100-199, LY*", "91") && RecordRules::matchList("91, 100-199, LY*", "150"), "id and range");
    CHECK(RecordRules::matchList("91, 100-199, LY*", "LY2ABC") && !RecordRules::matchList("91, 100-199, LY*", "200"), "prefix and miss");
    CHECK(RecordRules::matchList("TG 91", "TG 91/slot1") && RecordRules::matchList("91", "TG 91"), "TG prefix ignored");
    CHECK(!RecordRules::matchList("9", "91"), "no partial number match");
    RecordRules r;
    r.setEnabled(true);
    RecordRule a;
    a.mode = 9; a.field = RecField::Target; a.match = "91";
    const uint32_t id = r.add(a);
    CHECK(r.add(a) == id && r.list().size() == 1, "the same rule is not added twice");
    CHECK(r.decide(call(9, "91")).audio && !r.decide(call(9, "92")).any(), "talkgroup rule");
    CHECK(!r.decide(call(38, "91")).any(), "other mode");
    Session e = call(9, "91"); e.verdict = (uint8_t)Verdict::Encrypted;
    CHECK(!r.decide(e).any(), "encrypted: never");
    e.verdict = (uint8_t)Verdict::Unreadable;
    CHECK(!r.decide(e).any(), "unreadable: never");
    RecordRule b;
    b.field = RecField::Callsign; b.match = "LY2*"; b.actions = RecAudio | RecIq;
    r.add(b);
    const RecordDecision d = r.decide(call(11, "x", "1", "LY2ABC"));
    CHECK(d.audio && d.iq, "callsign rule with audio and IQ");
    RecordRule f;
    f.field = RecField::Frequency; f.freqMinMHz = 156.8; f.mode = 31;
    r.add(f);
    Session m = mk(31, "ch16", sesstest::nowMs()); m.freqHz = 156800000;
    CHECK(r.decide(m).audio, "frequency rule");
    m.freqHz = 157000000;
    CHECK(!r.decide(m).any(), "frequency outside");
    RecordRule em;
    em.emergencyOnly = true; em.mode = 50;
    r.add(em);
    Session p = call(50, "7"); 
    CHECK(!r.decide(p).any(), "emergency rule needs the flag");
    p.flags |= FlagEmergency;
    CHECK(r.decide(p).audio, "emergency rule with the flag");
}

static void testLimitsAndPersistence() {
    RecordRules r;
    r.setEnabled(true);
    RecordRule a;
    a.mode = 9; a.field = RecField::Target; a.match = "91 | x\nname"; a.capMB = 1; a.maxSeconds = 30; a.name = "a|b%";
    const uint32_t id = r.add(a);
    CHECK(r.decide(call(9, "91")).maxSeconds == 30, "max length is passed on");
    r.addUsage(id, 2u << 20);
    CHECK(r.atCap(id) && !r.decide(call(9, "91")).any() && r.cappedCount() >= 1, "at the disk cap the rule stops asking");
    RecordRules q;
    q.loadText(r.saveText());
    CHECK(q.enabled() && q.list().size() == 1, "loaded");
    const RecordRule b = q.list()[0];
    CHECK(b.id == id && b.mode == 9 && b.match == a.match && b.name == a.name && b.capMB == 1 && b.maxSeconds == 30 && b.usedBytes == (2u << 20), "every field survives the file");
    CHECK(RecordRules::iqEstimateBytes(1e6, 10) == 80000000ull, "IQ estimate 8 bytes per sample");
    CHECK(!RecordRules::describe(b).empty(), "describe");
}

static void testStoreHandlers() {
    RecordRules& rr = rules();
    rr.clear();
    rr.setEnabled(true);
    RecordRule a;
    a.mode = 9; a.field = RecField::Source; a.match = "1234567";
    rr.add(a);
    std::atomic<int> starts{0}, ends{0};
    std::string seenWho;
    std::atomic<uint64_t> startId{0}, endId{0};
    rr.setHandlers([&](const Session& s, const RecordDecision& d) { seenWho = s.who[0]; startId = s.id; if (d.audio) starts++; }, [&](uint64_t id) { endId = id; ends++; });
    StoreConfig c;
    c.retentionEverySec = 0; c.loadSalt = false;
    SessionStore st;
    CHECK(st.openAt(":memory:", c), "open");
    Session s = call(9, "91", "1234567");
    const uint64_t id = st.begin(s);
    Session u = s; u.id = id;
    st.update(u);
    st.flush();
    CHECK(starts == 1 && startId == id && seenWho == "1234567", "start handler once, with the raw id (starts=%d who=%s)", starts.load(), seenWho.c_str());
    Session other = call(9, "92", "999");
    st.begin(other);
    st.flush();
    CHECK(starts == 1, "a session that matches no rule is not announced");
    u.endWall = 0;
    st.end(u);
    st.flush();
    CHECK(ends == 1 && endId == id, "end handler");
    st.close();
    rr.setHandlers(nullptr, nullptr);
    rr.clear();
}

int main() {
    testOffByDefault();
    testMatching();
    testLimitsAndPersistence();
    testStoreHandlers();
    return testSummary("record_rules");
}
