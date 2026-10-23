// Mercury monitor: the demo link (CQ, CALL, ACCEPT, carousel rounds both ways, DISCONNECT, a broadcast message and file) through the
// receiver at +8 dB with the two radios 3.5 Hz apart: the called station is named from its CQ, the seed computed, every frame's sender
// told apart, both directions reassembled (with Reed-Solomon repair pieces and the keyed block check), the file rebuilt by RaptorQ
// from repair symbols. Then the same link without the CQ (the seed learned from the frames) and with an encrypted connect.
#include "dect2/mercury/mercury_monitor.h"
#include "dect2/mercury/mercury_gen.h"
#include "dect2/mercury/mercury_proto.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2;
using namespace dect2::mercury;
#include "../common/test_util.h"

namespace {

MercuryTelemetry monitor(const std::vector<GenBurst>& sc, double snr, Monitor& mon) {
    ChannelConfig ch;
    ch.snrDb = snr; ch.cfoHz = 6; ch.seed = 21;
    const std::vector<cf> x = renderScript(sc, ch);
    OfdmReceiver rx;
    size_t at = 0;
    while (at < x.size()) {
        const size_t n = std::min<size_t>(x.size() - at, 1600);
        rx.pushSideband(x.data() + at, n);
        at += n;
        for (const OfdmFrame& f : rx.take()) mon.onFrame(f, 1.7e9 + f.startSample / 8000.0, f.startSample / 8000.0, (f.endSample - f.startSample) / 8000.0);
    }
    rx.flush();
    for (const OfdmFrame& f : rx.take()) mon.onFrame(f, 1.7e9 + f.startSample / 8000.0, f.startSample / 8000.0, (f.endSample - f.startSample) / 8000.0);
    MercuryTelemetry t;
    mon.fill(t);
    return t;
}

const MercurySession* linkOf(const MercuryTelemetry& t) {
    for (const MercurySession& s : t.sessions) if (s.id == 0x21) return &s;
    return nullptr;
}

} // namespace

int main() {
    const std::vector<GenBurst> sc = demoScript(0);
    {
        Monitor mon;
        const MercuryTelemetry t = monitor(sc, 14, mon);
        printf("  %zu frames logged, %llu plain, %llu seeded, %llu failed\n", t.frames.size(), (unsigned long long)t.plainFrames,
               (unsigned long long)t.seededFrames, (unsigned long long)t.fecFailed);
        for (auto it = t.frames.rbegin(); it != t.frames.rend(); ++it)
            printf("    %-8s %-10s dir %2d %-7s -> %-7s %s\n", it->modeName.c_str(), it->kind.c_str(), it->dir, it->from.c_str(), it->to.c_str(), it->detail.c_str());
        CHECK(t.frames.size() == sc.size(), "%zu frames of %zu", t.frames.size(), sc.size());
        const MercurySession* s = linkOf(t);
        CHECK(s != nullptr, "the session");
        if (s) {
            CHECK(s->caller == "LY2ONA" && s->callee == "LY1HRM", "callsigns %s / %s", s->caller.c_str(), s->callee.c_str());
            CHECK(s->seedKnown && !s->seedFromFrames && s->seed == sessionSeed(0x21, 0xBEEF, 3, "LY2ONA", "LY1HRM"), "seed computed");
            CHECK(s->state == "disconnected", "state %s", s->state.c_str());
            CHECK(s->d[0].text.find(demoCallerText()) == 0, "caller's data: \"%s\"", s->d[0].text.c_str());
            CHECK(s->d[1].text == demoCalleeText(), "callee's data: \"%s\"", s->d[1].text.c_str());
            CHECK(s->d[0].blocksOk == 3 && s->d[1].blocksOk == 1 && s->d[0].blocksFailed == 0 && s->d[1].blocksFailed == 0, "blocks %llu %llu",
                  (unsigned long long)s->d[0].blocksOk, (unsigned long long)s->d[1].blocksOk);
            CHECK(s->d[0].cfoKnown && s->d[1].cfoKnown && std::abs((s->d[1].cfoHz - s->d[0].cfoHz) - 3.5) < 0.5, "offsets %.2f %.2f", s->d[0].cfoHz, s->d[1].cfoHz);
        }
        // every frame of the session has the right sender
        int wrong = 0;
        for (const MercuryFrameLog& f : t.frames) {
            if (f.sessionId != 0x21 && f.kind != "POLL" && f.kind != "DATA") continue;
            const bool callee = f.cfoHz > 6 + 1.75;
            if (f.dir >= 0 && f.dir != (callee ? 1 : 0)) wrong++;
        }
        CHECK(wrong == 0, "%d frames given to the wrong station", wrong);
        CHECK(t.messages.find("LY2ONA: broadcast test from the OnAir generator") != std::string::npos, "broadcast message");
        CHECK(t.files.size() == 1 && t.files[0].done && t.files[0].name == demoFileName() && t.files[0].kind == "text", "file");
        std::vector<uint8_t> d;
        std::string name;
        CHECK(mon.fileBytes(0, d, name) && d == demoFileData() && name == demoFileName(), "file bytes");
        std::vector<uint8_t> sb;
        CHECK(s && mon.sessionBytes(s->key, 1, sb) && std::string(sb.begin(), sb.end()) == demoCalleeText(), "session bytes");
    }
    {   // without the CQ the called station stays a CRC; its frames' CRC gives the seed
        std::vector<GenBurst> v(sc.begin() + 1, sc.end());
        Monitor mon;
        const MercuryTelemetry t = monitor(v, 14, mon);
        const MercurySession* s = linkOf(t);
        CHECK(s && s->callee.rfind("CRC ", 0) == 0 && s->seedKnown && s->seedFromFrames, "seed from frames");
        CHECK(s && s->d[1].text == demoCalleeText() && s->d[0].text.find(demoCallerText()) == 0, "reassembled without the callee's name");
    }
    {   // a connect that marks the session encrypted (a reserved bit of the ACCEPT): payload never shown
        std::vector<GenBurst> v = sc;
        for (GenBurst& b : v) {
            if (b.mode != kDatac16 || packetType(b.frame[0]) != kPtCall2 || !(b.frame[1] & 0x80)) continue;
            std::vector<uint8_t> p(b.frame.begin(), b.frame.end() - 2);
            p[8] |= 0x10;
            b.frame = makeFrame(kDatac16, p.data(), p.size());
        }
        Monitor mon;
        const MercuryTelemetry t = monitor(v, 14, mon);
        const MercurySession* s = linkOf(t);
        CHECK(s && s->encrypted && s->d[0].text.empty() && s->d[1].text.empty(), "encrypted session shown in clear");
        int enc = 0;
        for (const MercuryFrameLog& f : t.frames) enc += f.encrypted && f.detail == "encrypted";
        CHECK(enc >= 8, "%d frames marked encrypted", enc);
        std::vector<uint8_t> sb;
        CHECK(s && !mon.sessionBytes(s->key, 0, sb), "an encrypted session's bytes are not handed out");
    }
    return testSummary("mercury monitor");
}
