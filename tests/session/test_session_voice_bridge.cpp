// voice::Monitor -> SessionStore bridge: a call becomes a Session when it opens, is updated while it runs and is ended when the hang time is over;
// an encrypted call keeps metadata only; two monitors with different receiver indexes do not mix.
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

static voice::CallInfo tg(uint32_t dst, uint32_t src, int slot = 1) { voice::CallInfo c; c.dst = dst; c.src = src; c.slot = slot; c.group = true; c.access = "CC 1"; return c; }

int main() {
    sesstest::TempDir td;
    sess::SessionStore st;
    std::string err;
    CHECK(st.openAt(td.file("s.db"), sess::StoreConfig(), &err), "store opens");
    voice::Monitor& m = voice::monitor("dmr");
    m.setClock([] { return gT; }, [] { return gWall; });
    m.setSettings(voice::Settings());
    m.clearHistory();
    m.setSessionStore(&st);
    m.setSessionReceiver(0);
    const int dmr = sess::voicebridge::modeNumber("dmr");
    CHECK(dmr > 0, "dmr has a session mode number");
    CHECK(sess::voicebridge::modeNumber("nonexistent-test-mode") < 0, "unknown monitor names have none");

    gT = 100;
    m.offer(tg(91, 1001));
    gT += 0.5; m.offer(tg(91, 1001));
    st.flush();
    auto w = st.window((uint16_t)dmr);
    CHECK(w.size() == 1 && w[0].endWall == 0, "one active session while the call runs");
    CHECK(w.size() == 1 && w[0].title.find("TG 91") != std::string::npos, "title names the talkgroup");

    gT += 10; m.housekeep();   // hang time over
    st.flush();
    w = st.window((uint16_t)dmr);
    CHECK(w.size() == 1 && w[0].endWall != 0, "the session ended with the call");

    // an encrypted call: metadata only, verdict encrypted
    voice::CallInfo e = tg(7, 2002, 2); e.encrypted = true;
    gT += 1; m.offer(e);
    gT += 10; m.housekeep();
    st.flush();
    w = st.window((uint16_t)dmr);
    bool enc = false;
    for (auto& s : w) if (s.verdict == (uint8_t)sess::Verdict::Encrypted) enc = true;
    CHECK(w.size() == 2 && enc, "encrypted call is a session with the encrypted verdict");

    // rows reach the database
    sess::Query q; q.mode = (int)dmr;
    CHECK(st.query(q).total >= 2, "both sessions are in the database");

    m.setSessionStore(nullptr);
    st.close();
    return testSummary("session_voice_bridge");
}
