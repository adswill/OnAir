// The automation family masks are 64 bits wide (Wave 0a, first task of docs/MODE-EXPERIENCE.md): a family index above 31 must not wrap or
// truncate. Covers familyBit / kAllFamilies / maskOfTrigger, the "*" and "<family>.*" triggers, the tap, observer, watcher and streamer
// masks reaching the engine, and wants() for the new families voice and session.
#include "dect2/automation/engine.h"
#include "../common/test_util.h"
#include <cstdint>
#include <string>

namespace au = dect2::automation;

// the 64 bit rules at compile time
static_assert(au::kFamilies <= 64, "families fit the mask");
static_assert(sizeof(au::FamilyMask) == 8, "64 bit mask");
static_assert(au::familyBit(0) == 1ull, "bit 0");
static_assert(au::familyBit(31) == 0x80000000ull, "bit 31");
static_assert(au::familyBit(32) == 0x100000000ull, "bit 32 does not wrap to bit 0");
static_assert(au::familyBit(40) == (1ull << 40), "bit 40");
static_assert(au::familyBit(63) == (1ull << 63), "bit 63");
static_assert(au::familyBit(64) == 0 && au::familyBit(-1) == 0, "out of range gives no bit");
static_assert(au::kAllFamilies == ((1ull << au::kFamilies) - 1), "all families");

int main() {
    // every real family has its own bit
    for (int f = 0; f < au::kFamilies; f++) {
        CHECK(au::familyBit(f) != 0, "family %d has a bit", f);
        for (int g = f + 1; g < au::kFamilies; g++) CHECK(au::familyBit(f) != au::familyBit(g), "bits %d and %d differ", f, g);
    }
    CHECK(au::familyOf("voice.call") == au::FVoice, "voice family");
    CHECK(au::familyOf("session.call") == au::FSession && au::familyOf("session.thread") == au::FSession && au::familyOf("session.track") == au::FSession, "session family");
    CHECK(au::familyOf("adsb.aircraft") == au::FAdsb && au::familyOf("plugin.x") == au::FPlugin, "old families keep their numbers");
    CHECK(au::familyOf("nonsense.x") == -1 && au::familyOf("") == -1, "unknown type");

    // triggers
    CHECK(au::maskOfTrigger("*") == au::kAllFamilies, "star is every family");
    CHECK(au::maskOfTrigger("session.*") == au::familyBit(au::FSession), "family star");
    CHECK(au::maskOfTrigger("session.call") == au::familyBit(au::FSession), "exact type");
    CHECK(au::maskOfTrigger("zzz.*") == 0 && au::maskOfTrigger("") == 0, "unknown trigger");

    // the engine: the four hooks and the rules decide wants()
    au::Engine eng;
    CHECK(!eng.wants(au::FSession) && !eng.wants(au::FVoice) && !eng.wants(-1) && !eng.wants(au::kFamilies), "nothing wanted at first");
    int seen = 0;
    eng.setTap([&](const au::Event&) { seen++; }, au::familyBit(au::FSession));
    CHECK(eng.wants(au::FSession) && !eng.wants(au::FVoice) && !eng.wants(au::FAdsb), "tap on session only");
    eng.submit(au::Event("session.call"));
    CHECK(seen == 1, "tap saw the session event");
    eng.submit(au::Event("voice.call"));
    CHECK(seen == 1, "tap does not see voice");
    eng.setTap(nullptr, 0);
    CHECK(!eng.wants(au::FSession), "tap removed");
    eng.setObserver([](const au::Event&) {}, au::kAllFamilies);
    eng.setWatcher([](const au::Event&) {}, au::familyBit(au::FVoice));
    eng.setStreamer([](const au::Event&) {}, au::familyBit(au::FPlugin));
    CHECK(eng.wants(au::FSession) && eng.wants(au::FVoice) && eng.wants(au::kFamilies - 1) && !eng.wants(au::kFamilies), "observer sees all families, nothing beyond the last");
    eng.setObserver(nullptr, 0);
    CHECK(eng.wants(au::FVoice) && eng.wants(au::FPlugin) && !eng.wants(au::FSession), "watcher and streamer keep theirs");
    eng.setWatcher(nullptr, 0);
    eng.setStreamer(nullptr, 0);

    // rules: "*" and a family star
    au::Rule r;
    r.id = "r1"; r.name = "all"; r.trigger = "*"; r.enabled = true;
    eng.setRules({r});
    CHECK(eng.wants(au::FSession) && eng.wants(au::FVoice) && eng.wants(au::FAdsb), "star rule wants everything");
    r.trigger = "session.*";
    eng.setRules({r});
    CHECK(eng.wants(au::FSession) && !eng.wants(au::FVoice) && !eng.wants(au::FAdsb), "session rule");
    r.enabled = false;
    eng.setRules({r});
    CHECK(!eng.wants(au::FSession), "disabled rule wants nothing");
    return testSummary("family_mask");
}
