// P25 Phase 2 receiver against the generated signal (p25p2_gen.h): clean, with noise, with carrier offsets, at several input rates, with the slot choice,
// scrambled with and without the right seed. Covered: sync and burst framing, S-ISCH / I-ISCH and the slot of the superframe, the DUID, the
// descrambling, 4V / 2V / SACCH / FACCH bursts, the ESS, MAC PTT / END_PTT / ACTIVE / grants / status / identifier updates, the call table, that an
// encrypted call is reported and none of its voice reaches the tap as clear. NOT covered here: a real transmitter (the burst layer was checked on a
// recording of the NSW PSN with tools/p25p2tool), H-CPM, fading, timing offsets of the radio's clock.
#include "dect2/p25p2/p25p2_gen.h"
#include "dect2/p25p2/p25p2_proto.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/p25p2/p25p2_tel.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

struct Frames { std::vector<std::vector<uint8_t>> bits[3]; std::vector<bool> clear[3]; };

static P25p2GenConfig baseConfig() {
    P25p2GenConfig c;
    c.rate = 240000; c.snrDb = 60; c.superframes = 14;
    P25p2GenCall a; a.channel = 1; a.startSf = 1; a.nSf = 6; a.src = 1234567; a.group = 1001;
    P25p2GenCall b; b.channel = 2; b.startSf = 2; b.nSf = 5; b.src = 777; b.group = 2002; b.algId = 0x84;
    c.calls = {a, b};
    c.sysMsgs = {p25p2::msgIdenUp(1, 100, 0x100 | 6, 100, 170202500u),   // base 851.0125 MHz, 12.5 kHz steps, transmit offset +1.5 MHz
                 p25p2::msgNetSts(0x11, 0xBEE00, 0x3A5, 0x1005, 0x70),
                 p25p2::msgRfssSts(0x11, 0x3A5, 1, 7, 0x1005, 0x70),
                 p25p2::msgAdjSts(0x11, 0x3A5, 1, 8, 0x1010, 0x70),
                 p25p2::msgGrpVGrant(0x00, 0x1005, 3003, 55555),
                 p25p2::msgUuVGrant(0x1006, 4004, 66666)};
    return c;
}

static P25p2Telemetry run(const P25p2GenConfig& c, Frames* fr, int listen = 0, size_t* audio = nullptr, const int* seed = nullptr) {
    P25p2Receiver rx;
    rx.setSilent(true);
    rx.setListenSlot(listen);
    rx.configure(c.rate);
    if (seed) rx.setSystem(seed[0], seed[1], seed[2]);
    if (fr) rx.setVoiceTap([fr](int slot, const uint8_t* bits, bool clear) { fr->bits[slot].emplace_back(bits, bits + 72); fr->clear[slot].push_back(clear); });
    if (audio) rx.setAudioTap([audio](const float*, size_t n) { *audio += n; });
    const std::vector<cf32> s = p25p2Signal(c);
    for (size_t i = 0; i < s.size(); i += 20000) rx.feed(s.data() + i, std::min<size_t>(20000, s.size() - i));
    P25p2Telemetry t;
    rx.telemetry(t, 0);
    return t;
}

static const P25p2Call* findCall(const P25p2Telemetry& t, int slot, int src) {
    for (const auto& c : t.log) if (c.kind == 0 && c.slot == slot && c.src == src) return &c;
    return nullptr;
}

// the checks of a good decode; ok: how many of the voice frames must be there (fraction)
static void checkDecode(const P25p2GenConfig& c, const P25p2Telemetry& t, const Frames& fr, double frac, const char* what) {
    CHECK(t.syncs + 3 >= (uint64_t)c.superframes, "%s: %llu syncs for %d superframes", what, (unsigned long long)t.syncs, c.superframes);
    const P25p2Call* a = findCall(t, 1, 1234567);
    const P25p2Call* b = findCall(t, 2, 777);
    CHECK(a && a->dst == 1001 && a->group && !a->encrypted && a->endSeen, "%s: clear call on slot 1 missing or wrong", what);
    CHECK(b && b->dst == 2002 && b->group && b->encrypted && b->algId == 0x84, "%s: encrypted call on slot 2 missing or wrong", what);
    const int want1 = p25p2VoiceFrames(c, 1), want2 = p25p2VoiceFrames(c, 2);
    // slot 1: the frames that arrived are in order and match the pattern, enough of them
    size_t good = 0, clearN = 0;
    for (size_t i = 0; i < fr.bits[1].size(); i++) {
        if (fr.clear[1][i]) clearN++;
        uint8_t ex[72];
        // find the pattern index by position: frames can be missing in a noisy run, so match each frame against its neighbours' indices
        for (size_t k = i; k < std::min(fr.bits[1].size() + 40, i + 40); k++) {
            p25p2VoiceBits(1, (int)k, ex);
            if (std::memcmp(ex, fr.bits[1][i].data(), 72) == 0) { good++; break; }
        }
    }
    CHECK((double)clearN >= frac * want1, "%s: %zu clear voice frames of %d", what, clearN, want1);
    CHECK((double)good >= frac * want1, "%s: %zu voice frames match the pattern of %d", what, good, want1);
    size_t clear2 = 0;
    for (bool v : fr.clear[2]) clear2 += v;
    CHECK(clear2 == 0, "%s: %zu frames of the encrypted call were handed on as clear", what, clear2);
    CHECK(t.encryptedSkipped + 2 >= (uint64_t)(frac * want2) && t.encryptedSkipped <= (uint64_t)want2, "%s: %llu encrypted frames counted of %d", what, (unsigned long long)t.encryptedSkipped, want2);
}

int main() {
    // ---- clean signal: everything the generator sends comes out
    {
        P25p2GenConfig c = baseConfig();
        Frames fr;
        const P25p2Telemetry t = run(c, &fr);
        checkDecode(c, t, fr, 1.0, "clean");
        CHECK(std::fabs(t.cfoHz) < 20, "clean: cfo %.1f Hz", t.cfoHz);
        CHECK(t.snrDb > 25, "clean: snr %.1f dB", t.snrDb);
        CHECK(t.blocksBad == 0, "clean: %llu bad MAC PDUs", (unsigned long long)t.blocksBad);
        CHECK(t.nac == 0x293, "clean: colour code %03X", t.nac);
        CHECK(t.site.valid && t.site.sysId == 0x3A5 && t.site.wacn == 0xBEE00 && t.site.rfss == 1 && t.site.site == 7, "clean: site %03X %05X %d-%d", t.site.sysId, t.site.wacn, t.site.rfss, t.site.site);
        CHECK(t.adjacent.size() == 1 && t.adjacent[0].site == 8, "clean: %zu adjacent sites", t.adjacent.size());
        CHECK(t.idens[1].valid && std::fabs(t.idens[1].baseHz - 851012500.0) < 1 && std::fabs(t.idens[1].spacingHz - 12500) < 1, "clean: identifier %.0f %.0f", t.idens[1].baseHz, t.idens[1].spacingHz);
        bool g = false, u = false;
        for (const auto& e : t.log) {
            if (e.kind == 1 && e.group && e.dst == 3003 && e.src == 55555 && e.channel == 0x1005 && std::fabs(e.freqHz - (851012500.0 + 5 * 12500)) < 1) g = true;
            if (e.kind == 1 && !e.group && e.dst == 4004 && e.src == 66666 && e.channel == 0x1006) u = true;
        }
        CHECK(g, "clean: group voice grant not in the log");
        CHECK(u, "clean: unit to unit grant not in the log");
        bool tg1 = false, tg2 = false;
        for (const auto& x : t.talkgroups) { tg1 |= x.tg == 1001; tg2 |= x.tg == 2002 && x.encrypted; }
        CHECK(tg1 && tg2, "clean: talkgroup table (1001 %d, 2002 encrypted %d)", tg1, tg2);
        CHECK(t.v4 > 0 && t.v2 > 0 && t.sacch > 0 && t.facch > 0, "clean: burst counts %llu %llu %llu %llu", (unsigned long long)t.v4, (unsigned long long)t.v2, (unsigned long long)t.sacch, (unsigned long long)t.facch);
        CHECK(!t.codec, "clean: a codec was found in the test environment");
        printf("clean: snr %.1f dB, %llu syncs, %llu MAC ok\n", t.snrDb, (unsigned long long)t.syncs, (unsigned long long)t.blocksOk);
    }
    // ---- noise: the Reed-Solomon codes and the sync carry it
    for (double snr : {20.0, 15.0, 12.0, 9.0, 6.0}) {
        P25p2GenConfig c = baseConfig();
        c.snrDb = snr; c.seed = 11;
        Frames fr;
        const P25p2Telemetry t = run(c, &fr);
        char w[48]; snprintf(w, sizeof w, "snr %.0f dB", snr);
        if (snr >= 12) checkDecode(c, t, fr, snr >= 15 ? 0.95 : 0.7, w);   // below that only reported (where the decode falls apart)
        else { size_t cl = 0; for (bool v : fr.clear[1]) cl += v; printf("%s: %zu of %d clear voice frames of slot 1\n", w, cl, p25p2VoiceFrames(c, 1)); }
        printf("%s: measured %.1f dB, MAC ok %llu bad %llu, corrected hexbits %llu\n", w, t.snrDb, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, (unsigned long long)t.rsCorrected);
    }
    // ---- carrier offset (the receiver measures it on the sync)
    for (double cfo : {-400.0, -250.0, -100.0, 60.0, 180.0, 350.0}) {
        P25p2GenConfig c = baseConfig();
        c.snrDb = 25; c.cfoHz = cfo; c.seed = 3;
        Frames fr;
        const P25p2Telemetry t = run(c, &fr);
        char w[48]; snprintf(w, sizeof w, "cfo %+.0f Hz", cfo);
        checkDecode(c, t, fr, 0.97, w);
        CHECK(std::fabs(t.cfoHz - cfo) < 25, "%s: measured %.1f Hz", w, t.cfoHz);
        printf("%s: measured %.1f Hz\n", w, t.cfoHz);
    }
    // ---- other input rates (the decimation chain)
    for (double rate : {192000.0, 300000.0, 480000.0}) {
        P25p2GenConfig c = baseConfig();
        c.rate = rate; c.snrDb = 30; c.cfoHz = 120; c.seed = 5;
        Frames fr;
        const P25p2Telemetry t = run(c, &fr);
        char w[48]; snprintf(w, sizeof w, "rate %.0f", rate);
        checkDecode(c, t, fr, 0.97, w);
    }
    // ---- unit to unit call and an emergency call; the lead-in alignment must not matter
    for (int lead : {0, 5, 90}) {
        P25p2GenConfig c = baseConfig();
        c.leadDibits = lead; c.seed = 2 + (uint32_t)lead;
        c.calls[0].unitToUnit = true; c.calls[0].group = 424242; c.calls[0].svcOpts = 0x80;
        Frames fr;
        const P25p2Telemetry t = run(c, &fr);
        const P25p2Call* a = findCall(t, 1, 1234567);
        CHECK(a && !a->group && a->dst == 424242 && a->emergency, "unit to unit / emergency call (lead %d): %s", lead, a ? "wrong fields" : "missing");
    }
    // ---- the slot choice: only the chosen logical channel is played (its frames counted as heard clear)
    {
        P25p2GenConfig c = baseConfig();
        c.calls[1].algId = 0x80;   // both calls clear now
        for (int listen = 0; listen <= 2; listen++) {
            Frames fr;
            const P25p2Telemetry t = run(c, &fr, listen);
            CHECK(t.listenSlot == listen, "listen %d reported as %d", listen, t.listenSlot);
            CHECK(fr.bits[1].size() > 20 && fr.bits[2].size() > 20, "both slots are still seen with listen %d", listen);
        }
    }
    // ---- scrambled (as every real channel): decodes with the seed of the control channel, and with a wrong one the voice and the ESS fail
    {
        P25p2GenConfig c = baseConfig();
        c.scramble = true; c.snrDb = 30; c.seed = 9;
        const int good[3] = {c.wacn, c.sysId, c.colorCode}, bad[3] = {c.wacn, c.sysId, c.colorCode ^ 1};
        Frames fr;
        const P25p2Telemetry t = run(c, &fr, 0, nullptr, good);
        checkDecode(c, t, fr, 0.95, "scrambled");
        CHECK(t.wacn == c.wacn && t.sysId == c.sysId && t.colorCode == c.colorCode, "scrambled: seed reported %05X %03X %03X", t.wacn, t.sysId, t.colorCode);
        CHECK(t.essOk >= 8 && t.essBad == 0, "scrambled: ESS ok %llu bad %llu", (unsigned long long)t.essOk, (unsigned long long)t.essBad);
        Frames fw;
        const P25p2Telemetry w = run(c, &fw, 0, nullptr, bad);
        CHECK(w.essOk == 0 && w.blocksOk == 0, "wrong seed: ESS ok %llu, MAC ok %llu", (unsigned long long)w.essOk, (unsigned long long)w.blocksOk);
        size_t match = 0;
        for (size_t i = 0; i < fw.bits[1].size(); i++)
            for (int k = 0; k < 200; k++) { uint8_t ex[72]; p25p2VoiceBits(1, k, ex); if (std::memcmp(ex, fw.bits[1][i].data(), 72) == 0) { match++; break; } }
        CHECK(match == 0, "wrong seed: %zu voice frames came out right", match);
        CHECK(w.ischExact > 10 && w.duidBad == 0, "wrong seed: the ISCH and the DUID are not scrambled (%llu, %llu)", (unsigned long long)w.ischExact, (unsigned long long)w.duidBad);
        printf("scrambled: ESS ok %llu, I-ISCH exact %llu\n", (unsigned long long)t.essOk, (unsigned long long)t.ischExact);
    }
    // ---- a call joined without its PTT: the voice is held until the ESS says clear (or dropped when it says encrypted)
    {
        P25p2GenConfig c = baseConfig();
        c.calls[0].startSf = -1; c.calls[0].nSf = 8;   // the PTT is never sent
        c.calls[1].startSf = -1; c.calls[1].nSf = 8;
        Frames fr;
        size_t audio = 0;
        const P25p2Telemetry t = run(c, &fr, 0, &audio);
        const P25p2Call* a = nullptr;
        const P25p2Call* b = nullptr;
        for (const auto& x : t.log) { if (x.kind == 0 && x.slot == 1) a = &x; if (x.kind == 0 && x.slot == 2) b = &x; }
        CHECK(a && !a->encrypted && a->algId == 0x80 && a->clearFrames + 4 >= a->voiceFrames, "late entry, clear: %s %u of %u", a ? "found" : "missing", a ? a->clearFrames : 0, a ? a->voiceFrames : 0);
        CHECK(b && b->encrypted && b->algId == 0x84 && b->keyId == 0x1234 && b->clearFrames == 0, "late entry, encrypted: %s", b ? "wrong" : "missing");
        size_t clear2 = 0;
        for (bool v : fr.clear[2]) clear2 += v;
        CHECK(clear2 == 0, "late entry: %zu encrypted frames handed on as clear", clear2);
    }
    // ---- pure noise: nothing is found
    {
        P25p2GenConfig c = baseConfig();
        c.calls.clear(); c.superframes = 1;
        std::vector<cf32> s = p25p2Signal(c);
        P25p2GenConfig z = c; z.snrDb = -200;
        P25p2Receiver rx;
        rx.setSilent(true);
        rx.configure(c.rate);
        // random noise only (the generator with the signal scaled away): feed the noise part by generating at a very low SNR
        z.snrDb = -60;
        const std::vector<cf32> nz = p25p2Signal(z);
        for (size_t i = 0; i < nz.size(); i += 20000) rx.feed(nz.data() + i, std::min<size_t>(20000, nz.size() - i));
        P25p2Telemetry t;
        rx.telemetry(t, 0);
        CHECK(!t.locked && t.log.empty() && t.syncs == 0, "noise: locked %d, %zu calls, %llu syncs", t.locked, t.log.size(), (unsigned long long)t.syncs);
    }
    return testSummary("test_p25p2_rx");
}
