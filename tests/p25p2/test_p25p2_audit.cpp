// Phase 2 audit fixes, one check each: A2 the receiver joins mid-superframe (the first bursts after the lock are not read on a guessed slot), B8 a lost ESS
// does not make an encrypted call unknown, B9 a call that never gets an ESS is labelled and skipped (not silently held), keyed calls carry key ID and MI from
// the PTT, C14/C15 the MAC message length rules (null, avoid-zero-bias, variable lists, extended grant), and the explicit grant takes channel (T).
// NOT covered: a real transmitter; the generator's PTT/ESS layout is this program's reading of OP25's field offsets.
#include "dect2/p25p2/p25p2_gen.h"
#include "dect2/p25p2/p25p2_proto.h"
#include "dect2/p25p2/p25p2_rx.h"
#include "dect2/p25p2/p25p2_tel.h"
#include <cstdio>
#include <cstring>
using namespace dect2;
#include "../common/test_util.h"

static P25p2Telemetry run(const P25p2GenConfig& c, bool sys = true) {
    P25p2Receiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    if (sys) rx.setSystem(c.wacn, c.sysId, c.colorCode);
    const std::vector<cf32> s = p25p2Signal(c);
    for (size_t i = 0; i < s.size(); i += 20000) rx.feed(s.data() + i, std::min<size_t>(20000, s.size() - i));
    P25p2Telemetry t;
    rx.telemetry(t, 0);
    return t;
}
static P25p2GenConfig base() {
    P25p2GenConfig c;
    c.rate = 240000; c.snrDb = 60; c.superframes = 14; c.scramble = true;
    return c;
}
static const P25p2Call* findCall(const P25p2Telemetry& t, int slot) {
    for (const auto& c : t.log) if (c.kind == 0 && c.slot == slot) return &c;
    return nullptr;
}
static bool hasEvent(const P25p2Telemetry& t, const char* sub) {
    for (const auto& e : t.events) if (e.find(sub) != std::string::npos) return true;
    return false;
}

int main() {
    // ---- A2: joined mid-superframe. A call on channel 2 only; the signal starts at slot 3 (the first burst has the sync word, a guessed slot 0 would put it on channel 1)
    for (int drop : {3, 4, 7}) {
        P25p2GenConfig c = base();
        P25p2GenCall a; a.channel = 2; a.startSf = 0; a.nSf = 12; a.src = 777; a.group = 2002;
        c.calls = {a};
        c.dropFirstSlots = drop;
        const P25p2Telemetry t = run(c);
        CHECK(!findCall(t, 1), "drop %d: no call on slot 1 (the other channel)", drop);
        CHECK(t.locked && t.slotsSkipped >= (drop % 4 >= 2 || drop == 3 ? 1u : 0u), "drop %d: locked, %llu bursts skipped", drop, (unsigned long long)t.slotsSkipped);
        const P25p2Call* b = findCall(t, 2);
        CHECK(b && b->voiceFrames > 40, "drop %d: the call on slot 2 is heard (%u frames)", drop, b ? b->voiceFrames : 0);
        CHECK(t.slotsBad == 0 && t.duidBad == 0, "drop %d: no bad bursts (%llu)", drop, (unsigned long long)t.slotsBad);
    }
    // ---- keyed call: the PTT carries algorithm, key ID and MI; the call is labelled encrypted with them and no voice is decoded
    {
        P25p2GenConfig c = base();
        P25p2GenCall a; a.channel = 1; a.startSf = 1; a.nSf = 8; a.src = 100; a.group = 3003; a.algId = 0x84; a.keyId = 0x0ABC; a.pttInfo = true;
        c.calls = {a};
        const P25p2Telemetry t = run(c);
        const P25p2Call* k = findCall(t, 1);
        CHECK(k && k->encrypted && k->algId == 0x84 && k->keyId == 0x0ABC, "keyed: encrypted, alg 0x%02X key 0x%X", k ? k->algId : 0, k ? k->keyId : 0);
        CHECK(k && k->miValid && k->mi[0] == 0x11 && k->mi[8] == 0x99, "keyed: message indicator %02X..%02X", k ? k->mi[0] : 0, k ? k->mi[8] : 0);
        CHECK(hasEvent(t, "encrypted, algorithm 0x84 key 2748 MI 112233445566778899"), "keyed: one event with algorithm, key and MI");
        CHECK(t.voiceDecoded == 0 && t.encryptedSkipped > 20, "keyed: voice skipped, not decoded (%llu skipped)", (unsigned long long)t.encryptedSkipped);
    }
    // ---- B8: a late entry (no PTT) learns the encryption from the ESS; one lost ESS later does not make it unknown again
    {
        P25p2GenConfig c = base();
        P25p2GenCall a; a.channel = 1; a.startSf = 1; a.nSf = 10; a.src = 100; a.group = 3003; a.algId = 0x84; a.noPtt = true; a.essBadSf = 5;
        c.calls = {a};
        const P25p2Telemetry t = run(c);
        const P25p2Call* k = findCall(t, 1);
        CHECK(k && k->cryptoKnown && k->encrypted && k->keyId == 0x1234 && !k->essMissing, "late entry: encryption known from the ESS (key 0x%X)", k ? k->keyId : 0);
        CHECK(t.essOk >= 3 && t.essBad >= 1 && t.essReused >= 1, "ESS: %llu good, %llu bad, %llu covered by the last good one", (unsigned long long)t.essOk, (unsigned long long)t.essBad, (unsigned long long)t.essReused);
        CHECK(t.voiceDecoded == 0, "late entry: encrypted voice never decoded");
    }
    // ---- B9: an ESS that never decodes: after three superframes the call is labelled and its voice skipped
    {
        P25p2GenConfig c = base();
        P25p2GenCall a; a.channel = 1; a.startSf = 1; a.nSf = 10; a.src = 100; a.group = 3003; a.noPtt = true; a.noEss = true;
        c.calls = {a};
        const P25p2Telemetry t = run(c);
        const P25p2Call* k = findCall(t, 1);
        CHECK(k && !k->cryptoKnown && k->essMissing, "no ESS: labelled unknown");
        CHECK(hasEvent(t, "encryption unknown"), "no ESS: event");
        CHECK(t.unknownSkipped > 20 && t.voiceDecoded == 0, "no ESS: %llu frames skipped, none decoded", (unsigned long long)t.unknownSkipped);
    }
    // ---- C14 / C15: message lengths
    {
        uint8_t nullInfo[6] = {0x00, 1, 2, 3, 4, 5};
        CHECK(p25p2::macMsgLenAt(nullInfo, 6) == 6, "null information fills the PDU");
        uint8_t avoid[4] = {0x08, 0x03, 0, 0};
        CHECK(p25p2::macMsgLenAt(avoid, 4) == 3, "null avoid zero bias: length in byte 1");
        uint8_t v11[8] = {0x11, 0x01};
        CHECK(p25p2::macMsgLenAt(v11, 8) == 6, "0x11 with 1 -> (1+1)*2+2 = 6");
        uint8_t v12[12] = {0x12, 0x02};
        CHECK(p25p2::macMsgLenAt(v12, 12) == 11, "0x12 with 2 -> (2+1)*3+2 = 11");
        uint8_t mf[12] = {0x90, 0x90, 0x05};
        CHECK(p25p2::macMsgLenAt(mf, 12) == 5, "manufacturer message: its own length");
        uint8_t mf0[12] = {0x80, 0x90, 0x00};
        CHECK(p25p2::macMsgLenAt(mf0, 12) == p25p2::macMsgLen(0x80), "manufacturer message with length 0: the table");
        uint8_t ext[12] = {0xC0};
        CHECK(p25p2::macMsgLenAt(ext, 12) == 11 && p25p2::macMsgLen(0xC3) == 8, "extended grants: table lengths 11 and 8");
        CHECK(p25p2::macMsgLenAt(ext, 0) == 0, "no bytes left");
    }
    // ---- a signalling PDU with a 0x08 and a 0x11 in front of the messages that matter, and an extended grant
    {
        P25p2GenConfig c = base();
        c.calls.clear();
        c.sysMsgs = {};
        std::vector<uint8_t> grant(11, 0);
        grant[0] = 0xC0; grant[1] = 0x04;
        p25p2::put16(&grant[2], 0x1010); p25p2::put16(&grant[4], 0x1020); p25p2::put16(&grant[6], 4004); p25p2::put24(&grant[8], 555);
        const std::vector<uint8_t> lead1 = {0x08, 0x02}, lead2 = {0x11, 0x00, 0, 0};
        auto cat = [&](const std::vector<std::vector<uint8_t>>& m) { std::vector<uint8_t> all; for (const auto& x : m) all.insert(all.end(), x.begin(), x.end()); return all; };
        // each sysMsgs entry is wrapped in one MAC_SIGNAL (18 bytes in a FACCH): the receiver has to step over the 0x08 and the 0x11 to reach the rest
        c.sysMsgs = {cat({lead1, lead2, p25p2::msgNetSts(0x11, 0xBEE00, 0x3A5, 0x1005, 0x70)}), cat({lead1, grant})};
        const P25p2Telemetry t = run(c);
        CHECK(t.site.valid && t.site.wacn == 0xBEE00 && t.site.sysId == 0x3A5, "NET_STS after a 0x08 and a 0x11 is parsed (wacn %X)", t.site.wacn);
        bool g = false;
        for (const auto& e : t.log) if (e.kind == 1 && e.dst == 4004 && e.src == 555 && e.channel == 0x1010) g = true;
        CHECK(g, "extended grant: logged on channel (T) 0x1010");
    }
    return fails ? 1 : 0;
}
