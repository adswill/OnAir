// The conversation tracker (voice/conversation.h) on made-up call lists: grouping by talkgroup, carrier and slot, the quiet gap that ends a
// conversation, private calls in both directions as one thread, callsign modes (D-STAR), unordered input, the filters (search, length,
// error rate, played, recording, encrypted, slot, since), the talker and error statistics, the exports (CSV formula guard, JSON escaping)
// and the real-world cases: calls with unknown error rate, zero-length calls, a wall clock that ends before it starts, many parallel talkgroups.
// NOT covered: the screen (app/voice/voice_monitor_ui.cpp).
#include "dect2/voice/conversation.h"
#include <string>
#include <vector>

using namespace dect2::voice;
#include "../common/test_util.h"

static uint64_t gId = 1;
static CallEntry call(uint32_t dst, uint32_t src, int64_t start, double sec, int slot = 1, bool group = true, float err = 0.01f) {
    CallEntry c;
    c.id = gId++;
    c.info.dst = dst; c.info.src = src; c.info.slot = slot; c.info.group = group; c.info.access = "CC 1";
    c.wallStart = start; c.wallEnd = start + (int64_t)sec;
    c.firstSec = 0; c.lastSec = sec; c.errorRate = err; c.active = false; c.played = true; c.verdict = Verdict::Play;
    return c;
}

static void testGrouping() {
    std::vector<CallEntry> v = {call(91, 100, 1000, 5), call(91, 101, 1010, 4), call(91, 100, 1200, 3), call(92, 100, 1005, 2)};
    auto g = groupConversations(v, 60);
    CHECK(g.size() == 3, "TG 91 splits at the 3 minute gap, TG 92 is its own: %zu", g.size());
    CHECK(g[0].title == "TG 91" && g[0].calls.size() == 1 && g[0].firstWall == 1200, "newest conversation first");
    const Conversation* a = nullptr;
    for (const auto& c : g) if (c.title == "TG 91" && c.calls.size() == 2) a = &c;
    CHECK(a && a->talkers.size() == 2 && a->talkSeconds == 9.0, "two talkers, 9 s");
    CHECK(a && a->talkers[0].who == "100" && a->talkers[0].calls == 1, "busiest talker first");
    CHECK(a && a->calls[0].wallStart == 1000, "calls oldest first");
    CHECK(groupConversations(v, 1000).size() == 2, "a long gap merges TG 91 into one conversation");
}

static void testSlotsCarriers() {
    CallEntry a = call(91, 1, 1000, 3, 1), b = call(91, 2, 1001, 3, 2), c = call(91, 3, 1002, 3, 1);
    c.info.carrierHz = 450.1e6;
    auto g = groupConversations({a, b, c}, 60);
    CHECK(g.size() == 3, "same talkgroup on another slot or carrier is another conversation: %zu", g.size());
}

static void testPrivate() {
    auto g = groupConversations({call(5678, 1234, 1000, 4, 1, false), call(1234, 5678, 1010, 3, 1, false), call(9999, 1234, 1012, 2, 1, false)}, 60);
    CHECK(g.size() == 2, "A->B and B->A are one thread, A->C another: %zu", g.size());
    bool both = false;
    for (const auto& c : g) if (c.title == "1234 <-> 5678" && c.calls.size() == 2 && !c.group) both = true;
    CHECK(both, "private thread title");
}

static void testCallsigns() {
    CallEntry a = call(0, 0, 1000, 3), b = call(0, 0, 1005, 3);
    a.info.dstText = b.info.dstText = "CQCQCQ"; a.info.srcText = "LY2ABC"; b.info.srcText = "LY1XYZ";
    auto g = groupConversations({a, b}, 60);
    CHECK(g.size() == 1 && g[0].title == "CQCQCQ" && g[0].talkers.size() == 2, "D-STAR style callsign calls");
}

static void testFilters() {
    CallEntry a = call(91, 100, 1000, 5, 1, true, 0.01f), b = call(92, 101, 1000, 0.5, 2, true, 0.2f), c = call(93, 102, 1000, 5, 1, true, -1.f);
    b.played = false; b.verdict = Verdict::Muted; c.file = "/x/call.wav"; c.info.encrypted = true; c.verdict = Verdict::Encrypted; c.played = false;
    std::vector<CallEntry> v = {a, b, c};
    ConvFilter f;
    f.query = "tg 92";    CHECK(groupConversations(v, 60, f).size() == 1, "search target");
    f = ConvFilter(); f.query = "MUTED";  CHECK(groupConversations(v, 60, f).size() == 1, "search state, case-insensitive");
    f = ConvFilter(); f.query = "102";    CHECK(groupConversations(v, 60, f).size() == 1, "search source");
    f = ConvFilter(); f.minSeconds = 1;   CHECK(groupConversations(v, 60, f).size() == 2, "min length");
    f = ConvFilter(); f.maxErrorRate = 0.05f; CHECK(groupConversations(v, 60, f).size() == 2, "bad error rate out, unknown error rate stays");
    f = ConvFilter(); f.playedOnly = true; CHECK(groupConversations(v, 60, f).size() == 1, "played only");
    f = ConvFilter(); f.withAudioOnly = true; CHECK(groupConversations(v, 60, f).size() == 1, "with recording");
    f = ConvFilter(); f.hideEncrypted = true; CHECK(groupConversations(v, 60, f).size() == 2, "hide encrypted");
    f = ConvFilter(); f.slot = 2; CHECK(groupConversations(v, 60, f).size() == 1, "slot");
    f = ConvFilter(); f.sinceWall = 2000; CHECK(groupConversations(v, 60, f).empty(), "since");
}

static void testStats() {
    CallEntry a = call(91, 100, 1000, 5, 1, true, 0.01f), b = call(91, 101, 1010, 4, 1, true, 0.09f), c = call(91, 100, 1020, 3, 1, true, -1.f);
    a.file = "a.wav"; b.info.emergency = true; c.info.encrypted = true; c.active = true;
    auto g = groupConversations({c, a, b}, 60);
    CHECK(g.size() == 1, "one conversation");
    const auto& v = g[0];
    CHECK(v.withAudio == 1 && v.encrypted == 1 && v.emergency == 1 && v.active, "counters");
    CHECK(v.worstError > 0.089f && v.worstError < 0.091f && v.avgError > 0.049f && v.avgError < 0.051f, "error stats ignore unknown: avg %f", (double)v.avgError);
    CHECK(v.spanSeconds() == 23.0, "span %f", v.spanSeconds());
    CHECK(std::string(signalLabel(0.01f)) == "good" && std::string(signalLabel(0.05f)) == "fair" && std::string(signalLabel(0.2f)) == "poor" && std::string(signalLabel(-1)) == "-", "signal labels");
}

static void testEdges() {
    CHECK(groupConversations({}, 60).empty(), "empty");
    CallEntry z = call(91, 1, 1000, 0); z.wallEnd = 900; z.lastSec = 0;   // zero length, end before start
    auto g = groupConversations({z}, 60);
    CHECK(g.size() == 1 && g[0].lastWall == 1000 && g[0].talkSeconds == 0, "zero-length call with a bad end time");
    std::vector<CallEntry> many;
    for (int i = 0; i < 400; ++i) many.push_back(call(1 + i % 40, 1000 + i, 1000 + i, 1, 1 + (i / 40) % 2));
    CHECK(groupConversations(many, 120).size() == 80, "40 talkgroups x 2 slots in parallel");
}

static void testExport() {
    CallEntry a = call(91, 100, 1790000000, 5); a.info.alias = "=cmd|' /C calc'!A1"; a.file = "a,b\"c.wav";
    CallEntry b = call(91, 101, 1790000010, 4); b.info.alias = "Tab\there \"q\"\n";
    auto g = groupConversations({a, b}, 60);
    const std::string csv = conversationsCsv(g);
    CHECK(csv.find("'=cmd") != std::string::npos, "formula guard");
    CHECK(csv.find("\"a,b\"\"c.wav\"") != std::string::npos, "csv quoting");
    CHECK(csv.find("2026-") != std::string::npos && csv.find("TG 91") != std::string::npos, "csv fields");
    size_t lines = 0; for (char ch : csv) if (ch == '\n') ++lines;
    CHECK(lines == 1 + 2 + 1 || lines == 5, "header + 2 calls + the newline inside the quoted alias: %zu", lines);
    const std::string js = conversationsJson(g, "dmr");
    CHECK(js.find("\\\"q\\\"") != std::string::npos && js.find("\\n") != std::string::npos && js.find("\\t") != std::string::npos, "json escaping");
    CHECK(js.find("\"mode\": \"dmr\"") != std::string::npos && js.find("\"calls\": 2") != std::string::npos, "json fields");
    CHECK(utcText(0) == "-" && utcText(1790000000) == "2026-09-21 14:13:20", "utc text %s", utcText(1790000000).c_str());
}

int main() {
    testGrouping();
    testSlotsCarriers();
    testPrivate();
    testCallsigns();
    testFilters();
    testStats();
    testEdges();
    testExport();
    return testSummary("voice_conversation");
}
