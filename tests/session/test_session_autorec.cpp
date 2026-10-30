// Auto-record through the voice monitor (docs/MODE-EXPERIENCE.md, 3): a call is recorded only when a rule matches (the master switch is off by
// default), never when encrypted, only up to the rule's longest call, and the file counts against the rule's disk cap. Also the paged sessions JSON.
#include "dect2/results/sessions_json.h"
#include "dect2/session/record_rules.h"
#include "dect2/session/session_store.h"
#include "dect2/session/voice_bridge.h"
#include "dect2/voice/voice_monitor.h"
#include "session_test_util.h"
#include <cstdio>
#include <cstdlib>
using namespace dect2;
#include "../common/test_util.h"

static double gT = 0;
static int64_t gWall = 1790000000;
static voice::CallInfo tg(uint32_t dst, uint32_t src, bool enc = false) { voice::CallInfo c; c.dst = dst; c.src = src; c.slot = 1; c.group = true; c.access = "CC 1"; c.encrypted = enc; return c; }

static size_t wavCount(const std::string& dir) {
    size_t n = 0;
    std::error_code ec;
    if (!sesstest::fs::exists(dir, ec)) return 0;
    for (auto& e : sesstest::fs::recursive_directory_iterator(dir, ec)) if (e.path().extension() == ".wav") n++;
    return n;
}

static void callWithAudio(voice::Monitor& m, const voice::CallInfo& c, int seconds) {
    gT += 1; m.offer(c);
    std::vector<float> pcm(8000, 0.1f);
    for (int i = 0; i < seconds; i++) { m.audio(c, pcm.data(), pcm.size(), 1, 8000); gT += 1; m.offer(c); }
    gT += 10; m.housekeep();
}

int main() {
    sesstest::TempDir td;
    sess::SessionStore& st = sess::global();
    std::string err;
    CHECK(st.openAt(td.file("s.db"), sess::StoreConfig(), &err), "store opens");
    sess::rules().clear();
    voice::Monitor& m = voice::monitor("dmr");
    m.setClock([] { return gT; }, [] { return gWall; });
    voice::Settings s;
    s.recordDir = td.file("rec");
    m.setSettings(s);
    m.clearHistory();
    m.setSessionStore(&st);

    callWithAudio(m, tg(91, 1001), 3);
    CHECK(wavCount(td.file("rec")) == 0, "nothing is recorded by default");

    sess::RecordRule r;
    r.mode = (uint16_t)sess::voicebridge::modeNumber("dmr");
    r.field = sess::RecField::Target; r.match = "91"; r.maxSeconds = 2; r.capMB = 1;
    const uint32_t rid = sess::rules().add(r);
    callWithAudio(m, tg(91, 1001), 3);
    CHECK(wavCount(td.file("rec")) == 0, "rules are ignored while the master switch is off");

    sess::rules().setEnabled(true);
    callWithAudio(m, tg(92, 1001), 3);
    CHECK(wavCount(td.file("rec")) == 0, "another talkgroup is not recorded");
    callWithAudio(m, tg(91, 1001, true), 3);
    CHECK(wavCount(td.file("rec")) == 0, "an encrypted call is never recorded");
    callWithAudio(m, tg(91, 1001), 5);
    CHECK(wavCount(td.file("rec")) == 1, "a matching call is recorded (%zu files)", wavCount(td.file("rec")));
    int rate = 0, ch = 0;
    std::vector<int16_t> pcm;
    for (auto& e : sesstest::fs::recursive_directory_iterator(td.file("rec"))) if (e.path().extension() == ".wav") voice::readWav(e.path().string(), rate, ch, pcm);
    CHECK(rate == 8000 && pcm.size() <= 2 * 8000 + 8 && pcm.size() >= 8000, "the rule's longest call (2 s) is honoured: %zu samples", pcm.size());
    uint64_t used = 0;
    for (const auto& x : sess::rules().list()) if (x.id == rid) used = x.usedBytes;
    CHECK(used > 0, "the file counts against the rule's cap");

    // the paged JSON
    st.flush();
    const std::string j = sessionsJson("mode=9&limit=2&offset=0");
    CHECK(j.find("\"sessions\":[") != std::string::npos && j.find("\"limit\":2") != std::string::npos, "paged sessions JSON: %s", j.substr(0, 120).c_str());
    CHECK(j.find("alias") == std::string::npos && j.find("note") == std::string::npos, "no alias or note in the payload");
    CHECK(sessionsJson("limit=99999").find("\"limit\":200") != std::string::npos, "limit is capped at 200");
    CHECK(sessionsJson("offset=abc").find("error") != std::string::npos, "a bad number is an error");
    CHECK(sessionStatusJson().find("\"gen\":") != std::string::npos, "status json");

    m.setSessionStore(nullptr);
    st.close();
    sess::rules().clear();
    return testSummary("session_autorec");
}
