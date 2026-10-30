// The Session model and its ids (docs/MODE-EXPERIENCE.md, 2.1): collision-safe stable ids, begin / update / end through the store, NaN positions,
// UTF-8 keys that are never cut, and the encrypted verdict keeping metadata only.
#include "dect2/session/session_store.h"
#include "session_test_util.h"
#include "../common/test_util.h"
#include <cmath>
#include <set>

using namespace dect2::sess;
using sesstest::mk;

static void testIds() {
    // layout: day (15 bits) << 48 | mode (16 bits) << 32 | sequence (32 bits), always positive as a signed 64 bit number
    const uint64_t id = makeId(20400, 9, 77);
    CHECK(idDay(id) == 20400 && idMode(id) == 9 && idSeq(id) == 77, "id fields round trip");
    CHECK((int64_t)id > 0 && (int64_t)makeId(0x7FFF, 0xFFFF, 0xFFFFFFFFu) > 0, "ids are positive");
    CHECK(dayOfWall(86400000LL * 3 + 5) == 3 && dayOfWall(0) == 0, "day of a wall time");

    IdAllocator a;
    const int64_t t = 1793000000123LL;
    std::set<uint64_t> seen;
    uint64_t prev = 0;
    for (int i = 0; i < 100000; i++) {
        const uint64_t x = a.next(t, 9);   // the same millisecond, the same mode, 100,000 times
        CHECK(seen.insert(x).second, "duplicate id");
        CHECK(x > prev, "ids grow");
        prev = x;
    }
    CHECK(a.next(t, 9) != a.next(t, 38) && idMode(a.next(t, 38)) == 38, "another mode never collides");
    CHECK(idDay(a.next(t + 86400000LL, 9)) == idDay(a.next(t, 9)) + 1, "the next day has its own sequence");
    CHECK(idSeq(a.next(t + 86400000LL * 5, 9)) == 1, "a new day starts at 1");
    // the sequence is raised above what the database already holds
    IdAllocator b;
    b.raise(dayOfWall(t), 9, 5000);
    CHECK(idSeq(b.next(t, 9)) == 5001, "raised above the stored maximum");
    b.raise(dayOfWall(t), 9, 10);
    CHECK(idSeq(b.next(t, 9)) == 5002, "raise never lowers");
}

static void testDefaultsAndKinds() {
    Session s;
    CHECK(!s.hasPosition() && std::isnan(s.lat) && std::isnan(s.lon) && std::isnan(s.altM), "no position by default");
    CHECK(s.active() && s.endWall == 0, "active until ended");
    s.lat = 54.68; s.lon = 25.27;
    CHECK(s.hasPosition(), "position");
    s.lon = kNaN;
    CHECK(!s.hasPosition(), "half a position is none");
    CHECK(std::string(kindName(Kind::Call)) == "call" && std::string(kindEventType(Kind::Thread)) == "session.thread" && std::string(kindTitle(Kind::Track)) == "Tracks", "kind names");
    CHECK(std::string(verdictName(Verdict::Encrypted)) == "encrypted" && std::string(linkTypeName(LinkType::Iq)) == "iq", "verdict and link names");
}

static void testStoreLifecycle() {
    SessionStore st;
    StoreConfig cfg;
    cfg.retentionEverySec = 0;
    std::string err;
    CHECK(st.openAt(":memory:", cfg, &err), "open memory store %s", err.c_str());

    // two sessions with the same key starting in the same millisecond
    const int64_t t0 = sesstest::nowMs();
    Session a = mk(9, "TG 91/slot1", t0), b = mk(9, "TG 91/slot1", t0);
    const uint64_t ia = st.begin(a), ib = st.begin(b);
    CHECK(ia != 0 && ib != 0 && ia != ib, "same key, same millisecond: different ids");
    st.flush();
    CHECK(st.window(9).size() == 2, "both in the window, got %zu", st.window(9).size());

    // update with a revised start (late entry): the id does not change
    a.id = ia;
    a.startWall = t0 - 5000;
    a.flags |= FlagLateEntry;
    a.eventCount = 12;
    st.update(a);
    st.flush();
    Session got;
    CHECK(st.get(ia, got) && got.id == ia && got.startWall == t0 - 5000 && got.eventCount == 12 && (got.flags & FlagLateEntry), "revised start, same id");
    CHECK(st.query([] { Query q; q.mode = 9; return q; }()).total == 2, "still two rows (upsert, no duplicates)");

    // end
    a.endWall = t0 + 3000;
    st.end(a);
    st.flush();
    CHECK(st.get(ia, got) && !got.active() && got.endWall == t0 + 3000 && got.durationMs == 8000, "ended: duration %u", got.durationMs);

    // an update arriving after the end does not reopen it
    Session late = a;
    late.endWall = 0;
    st.update(late);
    st.flush();
    CHECK(st.get(ia, got) && !got.active(), "a late update does not reopen");

    // positions: NaN stays NaN, a real one round trips
    Session p = mk(9, "pos", t0);
    p.lat = 54.6872; p.lon = 25.2797; p.altM = 112.5f;
    const uint64_t ip = st.begin(p);
    st.flush();
    Session gp;
    CHECK(st.get(ia, got) && !got.hasPosition() && std::isnan(got.lat), "NaN position round trips as NaN");
    CHECK(st.get(ip, gp) && std::fabs(gp.lat - 54.6872) < 1e-9 && std::fabs(gp.lon - 25.2797) < 1e-9 && std::fabs(gp.altM - 112.5f) < 1e-3, "position round trips");
    st.close();
}

static void testUtf8() {
    SessionStore st;
    StoreConfig cfg;
    cfg.retentionEverySec = 0;
    CHECK(st.openAt(":memory:", cfg), "open");
    // a long DevEUI + DevAddr pair, a 500 character callsign and non-ASCII names: nothing is cut
    std::string longKey = "0123456789ABCDEF/26011BFF";
    for (int i = 0; i < 20; i++) longKey += "-segment" + std::to_string(i);
    std::string utf = "Šiauliai \xE2\x80\x94 \xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E \xF0\x9F\x93\xA1";   // Lithuanian, dash, Japanese, antenna emoji
    std::string big;
    while (big.size() < 500) big += "\xC4\x85";   // 2-byte characters
    Session s = mk(20, longKey, sesstest::nowMs(), utf + " " + big);
    s.who[0] = big; s.alias[1] = utf;
    const uint64_t id = st.begin(s);
    st.flush();
    Session got;
    CHECK(st.get(id, got), "stored");
    CHECK(got.key == longKey && got.title == utf + " " + big && got.who[0] == big && got.alias[1] == utf, "long and UTF-8 text comes back byte for byte");
    CHECK(validUtf8(got.title) && validUtf8(got.who[0]), "still valid UTF-8");
    // a search for a non-ASCII word finds it
    Query q;
    q.text = "Šiauliai";
    CHECK(st.query(q).total == 1, "full text finds a non-ASCII word");
    // the cut helper never splits a character
    const std::string c = cutUtf8(big, 11);
    CHECK(c.size() == 10 && validUtf8(c), "cut at 11 bytes gives 10 (5 characters), got %zu", c.size());
    CHECK(cutUtf8("abc", 10) == "abc" && cutUtf8(utf, 1000) == utf, "short text untouched");
    st.close();
}

static void testEncryptedMetadataOnly() {
    SessionStore st;
    StoreConfig cfg;
    cfg.retentionEverySec = 0;
    CHECK(st.openAt(":memory:", cfg), "open");
    Session s = mk(9, "TG 4000/slot1", sesstest::nowMs());
    s.verdict = (uint8_t)Verdict::Encrypted;
    const uint64_t id = st.begin(s);
    st.link(id, LinkType::Audio, "/tmp/x.wav");
    st.link(id, LinkType::Iq, "/tmp/x.sigmf-data");
    st.link(id, LinkType::Transcript, "/tmp/x.txt");
    st.link(id, LinkType::Message, "17");
    st.link(id, LinkType::Telemetry, "level=-80");   // metadata is fine
    st.flush();
    const auto links = st.links(id);
    CHECK(links.size() == 1 && links[0].type == (uint8_t)LinkType::Telemetry, "only metadata linked to an encrypted session, got %zu links", links.size());
    CHECK(st.stats().refusedLinks == 4, "four content links refused, got %llu", (unsigned long long)st.stats().refusedLinks);

    // decided late: the session was clear when audio was linked, then turned out encrypted
    Session c = mk(9, "TG 4001/slot2", sesstest::nowMs());
    const uint64_t ic = st.begin(c);
    st.link(ic, LinkType::Audio, "/tmp/y.wav");
    st.flush();
    CHECK(st.links(ic).size() == 1, "clear: audio linked");
    c.id = ic;
    c.verdict = (uint8_t)Verdict::Encrypted;
    st.update(c);
    st.flush();
    CHECK(st.links(ic).empty(), "turned encrypted: the audio link is dropped");
    // a clear session keeps its audio and message links
    Session d = mk(9, "TG 4002/slot1", sesstest::nowMs());
    const uint64_t id2 = st.begin(d);
    st.link(id2, LinkType::Audio, "/tmp/z.wav");
    st.link(id2, LinkType::Message, "5");
    st.flush();
    CHECK(st.links(id2).size() == 2, "clear session keeps its links");
    st.close();
}

int main() {
    testIds();
    testDefaultsAndKinds();
    testStoreLifecycle();
    testUtf8();
    testEncryptedMetadataOnly();
    return testSummary("session_model");
}
