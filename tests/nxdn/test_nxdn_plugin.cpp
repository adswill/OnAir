// NXDN voice through a user-installed "ambe2" codec plug-in (stub library, no codec): generator -> receiver -> plug-in -> PCM tap. A clear voice
// call: every received voice frame (72 bits) reaches the plug-in as 12 bytes and gives 160 samples. A call with the cipher flag set is reported
// as encrypted and none of its frames reaches the plug-in. NOT covered: a real codec or a real NXDN recording.
#include "dect2/nxdn/nxdn_gen.h"
#include "dect2/nxdn/nxdn_rx.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "../codec/plugin_test_util.h"
#include <cstdio>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct R { int frames = 0, plug = 0, bad = 0; size_t samples = 0; uint64_t decoded = 0; NxdnTelemetry t; };

static R run(int cipher, int glitch = 0) {
    plugtest::gFrames = 0; plugtest::gBadSize = 0;
    NxdnGenConfig c;
    c.rate = 500000; c.snrDb = 40; c.loop = false; c.seed = 3;
    NxdnGenTx tx; tx.kind = NxdnGenTx::Voice; tx.baud = 2400; tx.cipher = cipher; tx.sacchGlitchCipher = glitch; tx.durSec = 2;
    c.txs.push_back(tx);
    NxdnGenerator g(c);
    NxdnReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    R r;
    rx.setVoiceTap([&](const uint8_t*) { r.frames++; });
    rx.setAudioTap([&](const float*, size_t n) { r.samples += n; });
    std::vector<cf32> buf(25000);
    for (double s = 0; s < 0.5 + tx.durSec + 0.4 + 1.5; s += buf.size() / c.rate) { g.generate(buf.data(), buf.size()); rx.feed(buf.data(), buf.size()); }
    rx.telemetry(r.t, 0);
    r.plug = plugtest::gFrames; r.bad = plugtest::gBadSize; r.decoded = r.t.voiceDecoded;
    return r;
}

int main() {
    CHECK(plugtest::install("ambe2", STUB_PLUGIN_PATH), "stub install");
    const R a = run(0);
    printf("clear: %d voice frames, %d to the plug-in, %zu samples, decoded %llu\n", a.frames, a.plug, a.samples, (unsigned long long)a.decoded);
    CHECK(a.frames > 20, "too few voice frames (%d)", a.frames);
    CHECK(a.plug > 0 && a.plug >= a.frames * 9 / 10 && a.plug <= a.frames, "plug-in got %d of %d frames", a.plug, a.frames);
    CHECK(a.bad == 0, "%d frames of the wrong size", a.bad);
    CHECK(a.samples == (size_t)a.plug * 160, "audio %zu samples for %d frames", a.samples, a.plug);
    const R e = run(1);
    printf("encrypted: %d voice frames, %d to the plug-in, %zu samples\n", e.frames, e.plug, e.samples);
    CHECK(e.frames > 20, "encrypted call not received");
    CHECK(e.plug == 0 && e.samples == 0 && e.decoded == 0, "an encrypted call reached the codec (%d frames)", e.plug);
    // one SACCH superframe with a valid CRC but a cipher flag (not repeated) must not turn a clear call into "encrypted"; audio must go on to the end
    const R g = run(0, 1);
    printf("clear + one glitched SACCH: %d voice frames, %d to the plug-in\n", g.frames, g.plug);
    CHECK(g.plug >= g.frames * 9 / 10 && g.t.log.empty() == false && g.t.log[0].cipher == 0, "a single flipped cipher flag silenced a clear call (%d of %d)", g.plug, g.frames);
    printf(fails ? "%d failure(s)\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
