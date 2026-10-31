// Shortwave decoder routing without a GUI or a radio: button -> decoder plan, the plan follows a retune, stop / start, several decoders at once,
// sideband mirroring, the audio shift, options, the identifier hook and the log helper. (The decoders themselves run in test_shortwave_path.)
#include "dect2/shortwave/shortwave_route.h"
#include <cmath>
#include <cstdio>
#include <string>
using namespace dect2;
using namespace dect2::swr;
#include "../common/test_util.h"

int main() {
    // ---- the table
    int n = 0;
    const DecoderInfo* tab = decoderTable(n);
    CHECK(n == kDecCount, "one row per decoder (%d)", n);
    for (int i = 0; i < n; i++) {
        CHECK((int)tab[i].id == i, "row %d is in enum order", i);
        CHECK(tab[i].key && *tab[i].key && tab[i].name && *tab[i].name && tab[i].blurb && *tab[i].blurb, "row %d has its texts", i);
        bool ok = false;
        CHECK(decoderFromKey(tab[i].key, &ok) == tab[i].id && ok, "key %s round trips", tab[i].key);
        for (int j = i + 1; j < n; j++) CHECK(std::string(tab[i].key) != tab[j].key, "keys %s are unique", tab[i].key);
    }
    bool ok = true;
    CHECK(decoderFromKey("nope", &ok) == Dec::Count && !ok, "unknown key");
    for (const char* want : {"rtty", "cw", "psk", "ft8", "ft4", "wspr", "js8", "sstv", "olivia", "contestia", "mfsk", "thor", "dominoex", "hell", "hamdrm",
                             "freedv", "ale", "ardop", "pactor", "mt63", "fsq", "ifkp", "navtex", "wefax", "time", "hfid"})
        CHECK(decoderFromKey(want, &ok) != Dec::Count && ok, "decoder %s exists", want);
    CHECK(decoderInfo(Dec::FreeDv).needs != nullptr, "FreeDV says what it needs");
    CHECK(decoderInfo(Dec::Rtty).needs == nullptr, "RTTY needs nothing");

    // ---- a click starts the decoder on the channel; the plan puts its dial `pitch` below the signal
    Router r;
    r.setChannel(-20000 + 51360);                       // the channel marker: 31.36 kHz above the input's centre
    CHECK(r.count() == 0 && r.plans().empty(), "nothing runs at first");
    const uint64_t rev0 = r.revision();
    CHECK(r.start(Dec::Rtty), "start RTTY");
    CHECK(!r.start(Dec::Rtty), "starting again changes nothing");
    CHECK(r.revision() != rev0, "a start changes the revision");
    Plan p = r.plan(Dec::Rtty);
    CHECK(p.active && !p.mirror, "RTTY active, upper sideband");
    CHECK(std::fabs(p.pitchHz - 1500) < 1e-9, "RTTY pitch %.1f", p.pitchHz);
    CHECK(std::fabs(p.dialOffsetHz - (31360 - 1500)) < 1e-9, "RTTY dial offset %.1f", p.dialOffsetHz);
    // CW is a skimmer: the dial goes on the channel itself
    CHECK(r.start(Dec::Cw), "start CW (a second decoder, same channel)");
    CHECK(std::fabs(r.plan(Dec::Cw).dialOffsetHz - 31360) < 1e-9, "CW dial offset %.1f", r.plan(Dec::Cw).dialOffsetHz);
    CHECK(r.count() == 2 && r.running().size() == 2 && r.plans().size() == 2, "two decoders run in parallel");
    CHECK(r.running()[0] == Dec::Rtty && r.running()[1] == Dec::Cw, "in table order");

    // ---- a retune moves every decoder and restarts none
    const uint64_t rev1 = r.revision(), mv1 = r.moves();
    r.setChannel(-20000 + 51360 + 1000);
    CHECK(r.revision() == rev1, "a retune does not change the revision (nothing is rebuilt)");
    CHECK(r.moves() != mv1, "a retune is a move");
    CHECK(r.active(Dec::Rtty) && r.active(Dec::Cw), "both still on after the retune");
    CHECK(std::fabs(r.plan(Dec::Rtty).dialOffsetHz - (32360 - 1500)) < 1e-9, "RTTY followed: %.1f", r.plan(Dec::Rtty).dialOffsetHz);
    CHECK(std::fabs(r.plan(Dec::Cw).dialOffsetHz - 32360) < 1e-9, "CW followed: %.1f", r.plan(Dec::Cw).dialOffsetHz);
    r.setChannel(32360);
    CHECK(r.moves() == r.moves(), "same channel again is no move");
    const uint64_t mv2 = r.moves();
    r.setChannel(32360);
    CHECK(r.moves() == mv2, "the same channel is no move");
    // a retune across the whole capture (10 MHz to 11 MHz is another capture, but the channel offset can jump by hundreds of kHz)
    r.setChannel(-480000);
    CHECK(std::fabs(r.plan(Dec::Rtty).dialOffsetHz - (-480000 - 1500)) < 1e-9, "a big jump is followed");

    // ---- stop / start
    CHECK(r.stop(Dec::Rtty), "stop RTTY");
    CHECK(!r.stop(Dec::Rtty), "stopping twice changes nothing");
    CHECK(!r.active(Dec::Rtty) && r.active(Dec::Cw), "only CW is left");
    CHECK(!r.plan(Dec::Rtty).active, "a stopped decoder has an inactive plan");
    CHECK(r.toggle(Dec::Rtty) && r.active(Dec::Rtty), "toggle starts it again");
    CHECK(!r.toggle(Dec::Rtty) && !r.active(Dec::Rtty), "toggle stops it");
    r.start(Dec::Rtty);
    r.stopAll();
    CHECK(r.count() == 0, "None stops everything");
    const uint64_t rev2 = r.revision();
    r.stopAll();
    CHECK(r.revision() == rev2, "None with nothing running changes nothing");

    // ---- sideband: a lower sideband decoder gets the mirrored capture and its dial is mirrored too
    r.setChannel(40000);
    Opts o;
    o.sideband = -1;
    r.setOpts(Dec::Psk, o);
    r.start(Dec::Psk);
    p = r.plan(Dec::Psk);
    CHECK(p.mirror && std::fabs(p.dialOffsetHz - (-40000 - 1500)) < 1e-9, "LSB: mirror %d dial %.1f", p.mirror, p.dialOffsetHz);
    CHECK(r.effectiveSideband(Dec::Psk) == -1 && r.effectiveSideband(Dec::Rtty) == +1, "sideband per decoder");
    o.sideband = 0;
    o.pitchHz = 1000;
    r.setOpts(Dec::Psk, o);
    p = r.plan(Dec::Psk);
    CHECK(!p.mirror && std::fabs(p.dialOffsetHz - (40000 - 1000)) < 1e-9, "pitch override: dial %.1f", p.dialOffsetHz);
    // the mirror is exact: a signal at +f in the capture is at -f after conj, so dial' + pitch must equal the mirrored channel
    o.sideband = -1; o.pitchHz = -1;
    r.setOpts(Dec::Psk, o);
    for (double ch : {-300000.0, -1234.5, 0.0, 777.0, 250000.0}) {
        r.setChannel(ch);
        p = r.plan(Dec::Psk);
        CHECK(std::fabs((p.dialOffsetHz + p.pitchHz) - (-ch)) < 1e-9, "LSB: signal sits at pitch in the mirrored audio (ch %.1f)", ch);
        const Plan u = r.plan(Dec::Rtty);
        CHECK(std::fabs((u.dialOffsetHz + u.pitchHz) - ch) < 1e-9, "USB: signal sits at pitch (ch %.1f)", ch);
    }

    // ---- options are kept per decoder and bump the revision
    Router q;
    Opts ro;
    ro.rttyBaud = 1; ro.rttyShift = 2; ro.rttyReverse = true;
    const uint64_t qr = q.revision();
    q.setOpts(Dec::Rtty, ro);
    CHECK(q.revision() != qr && q.opts(Dec::Rtty) == ro && !(q.opts(Dec::Cw) == ro), "RTTY options kept for RTTY only");
    const uint64_t qr2 = q.revision();
    q.setOpts(Dec::Rtty, ro);
    CHECK(q.revision() == qr2, "the same options change nothing");
    q.start(Dec::Rtty);
    CHECK(q.plan(Dec::Rtty).opts == ro, "the plan carries the options");

    // ---- the listening audio follows the decoder
    Router a;
    CHECK(a.audioShiftHz(true, false) == 0 && a.wantedSideband() == 0, "no decoder: no shift");
    a.start(Dec::Rtty);
    CHECK(std::fabs(a.audioShiftHz(true, false) - (-1500)) < 1e-9, "USB: the listening channel goes 1500 Hz below the signal");
    CHECK(std::fabs(a.audioShiftHz(false, true) - 1500) < 1e-9, "LSB: above");
    CHECK(a.audioShiftHz(false, false) == 0, "AM / CW: left alone");
    a.start(Dec::Cw);   // the newest decoder decides (CW: pitch 0)
    CHECK(a.audioShiftHz(true, false) == 0, "CW is the newest: no shift");
    a.stop(Dec::Cw);
    CHECK(std::fabs(a.audioShiftHz(true, false) - (-1500)) < 1e-9, "after stopping CW the RTTY decides again");
    CHECK(a.wantedSideband() == +1, "wanted sideband");

    // ---- the General / Auto hook
    CHECK(decoderForSignalId("hfdig", "rtty") == Dec::Rtty, "hfdig/rtty");
    CHECK(decoderForSignalId("psk", "") == Dec::Psk && decoderForSignalId("cw", "") == Dec::Cw && decoderForSignalId("ale", "") == Dec::Ale, "mode ids");
    CHECK(decoderForSignalId("hfdig", "olivia") == Dec::Olivia && decoderForSignalId("hfdig", "ft8") == Dec::Ft8, "sub-mode ids");
    CHECK(decoderForSignalId("milhf", "") == Dec::HfId, "waveform id");
    CHECK(decoderForSignalId("dmr", "") == Dec::Count && decoderForSignalId("", "") == Dec::Count, "other modes are not shortwave decoders");
    a.setDetected(Dec::Psk, 0.8f);
    CHECK(a.detected() == Dec::Psk && a.detectedConfidence() > 0.79f && !a.active(Dec::Psk), "a detection lights a button, it does not start it");
    a.clearDetected();
    CHECK(a.detected() == Dec::Count, "cleared");

    // ---- the log helper: only the new characters
    CHECK(textDelta("", "HELLO") == "HELLO", "first text");
    CHECK(textDelta("HELLO", "HELLO WORLD") == " WORLD", "text grew");
    CHECK(textDelta("HELLO WORLD", "HELLO WORLD") == "", "nothing new");
    const std::string win1 = "0123456789ABCDEFGHIJ", win2 = "56789ABCDEFGHIJKLM";   // the window slid by five, three new characters
    CHECK(textDelta(win1, win2) == "KLM", "window slid: '%s'", textDelta(win1, win2).c_str());
    CHECK(textDelta("abc", "xyz123") == "xyz123", "a different text is all new");

    // ---- every decoder can be started and gives a finite plan
    Router all;
    all.setChannel(12345.6);
    for (int i = 0; i < n; i++) CHECK(all.start((Dec)i), "start %s", tab[i].key);
    CHECK(all.count() == kDecCount, "all %d run at once", kDecCount);
    for (const Plan& pl : all.plans()) CHECK(std::isfinite(pl.dialOffsetHz) && pl.active, "plan of %s", decoderInfo(pl.id).key);
    return testSummary("shortwave_route");
}
