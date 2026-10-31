#include "../../app/dmr/call_filter.h"
#include <cstdio>
static int fails = 0;
static void check(bool ok, const char* w) { if (!ok) { printf("FAIL: %s\n", w); fails++; } }
int main() {
    using namespace dmrfilter;
    for (int k = 0; k < 5; k++) check(shown(All, k), "all shows every kind");
    check(shown(GroupVoice, 0) && !shown(GroupVoice, 1) && !shown(GroupVoice, 4), "group voice only");
    check(shown(VoiceOnly, 0) && shown(VoiceOnly, 1) && shown(VoiceOnly, 2) && !shown(VoiceOnly, 3) && !shown(VoiceOnly, 4), "voice only");
    check(!shown(NoControl, 4) && shown(NoControl, 3) && shown(NoControl, 0), "hide control");
    check(shown(DataOnly, 3) && !shown(DataOnly, 0), "data only");
    printf(fails ? "FAILED\n" : "OK\n");
    return fails;
}
