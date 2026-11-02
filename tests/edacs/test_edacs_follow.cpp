// EDACS system capture (edacs_gen.h): the control channel and analog voice channels in one wide capture. Checks the call table, talkgroup table and site information
// built from the control channel (published Standard / Networked layout and Extended Addressing), the power scan of the voice channels from a frequency table, and the following of one voice channel
// (the tone comes out of the sound; the group filter; the carrier-only follow; no table; a channel outside the capture), with noise and a carrier offset.
// Generated signals only: no real EDACS / ProVoice system was heard.
#include "dect2/edacs/edacs_gen.h"
#include "dect2/edacs/edacs_rx.h"
#include "dect2/edacs/edacs_tel.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
using namespace dect2;
#include "../common/test_util.h"

static double toneShare(const std::vector<float>& a, double f, double fs) {   // the share of the signal power that is at f (Goertzel)
    if (a.size() < 4800) return 0;
    double c = 2 * std::cos(2 * 3.14159265358979323846 * f / fs), s1 = 0, s2 = 0, tot = 0;
    for (float v : a) { const double s0 = v + c * s1 - s2; s2 = s1; s1 = s0; tot += (double)v * v; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return tot > 0 ? 2 * p / ((double)a.size() * tot) : 0;
}

struct Result { EdacsTelemetry t; std::map<int, std::vector<float>> audio; std::vector<int> followed; };

static std::vector<double> table() {
    std::vector<double> f;
    for (int i = 0; i < 10; i++) f.push_back(856.000e6 + 25e3 * i);   // LCN 1 = the control channel, then every 25 kHz
    return f;
}

static EdacsGenConfig baseConfig() {
    EdacsGenConfig c;
    c.lcnFreqHz = table();
    EdacsGenCall a; a.lcn = 3; a.gid = 777; a.lid = 4321; a.startSec = 1.0; a.endSec = 3.0; a.toneHz = 1000;
    EdacsGenCall b; b.lcn = 6; b.gid = 888; b.lid = 5555; b.startSec = 3.6; b.endSec = 5.2; b.toneHz = 1600; b.sendEnd = false; b.emergency = true;
    EdacsGenCall d; d.lcn = 8; d.startSec = 6.0; d.endSec = 7.0; d.toneHz = 700; d.announce = false;   // a carrier no message announced
    c.calls = {a, b, d};
    return c;
}

static Result run(const EdacsGenConfig& c, bool table_, bool follow, bool followAct, const std::vector<int>& groups = {}, const std::vector<double>* tableOverride = nullptr) {
    Result r;
    EdacsReceiver rx;
    rx.setSilent(true);
    rx.configure(c.rate);
    rx.setSignalOffset(c.controlHz - c.centreHz + c.cfoHz);
    rx.setCenterHz(c.centreHz);
    rx.setLayout(c.layout);
    if (table_) rx.setLcnTable(tableOverride ? *tableOverride : c.lcnFreqHz);
    rx.setFollow(follow);
    rx.setFollowActivity(followAct);
    rx.setFollowGroups(groups);
    rx.setAudioTap([&](int lcn, const float* x, size_t n) { r.audio[lcn].insert(r.audio[lcn].end(), x, x + n); });
    const std::vector<cf32> s = edacsSystemSignal(c);
    uint64_t seq = 0;
    for (size_t i = 0; i < s.size(); i += 2400) {
        const size_t n = std::min<size_t>(2400, s.size() - i);
        // the tap is called inside feed(): remember which channel is followed at that moment
        EdacsTelemetry t;
        if (rx.telemetry(t, seq)) { seq = t.seq; const int cur = t.followLcn; if (std::find(r.followed.begin(), r.followed.end(), cur) == r.followed.end() && cur) r.followed.push_back(cur); }
        rx.feed(s.data() + i, n);
    }
    rx.telemetry(r.t, 0);
    return r;
}

static const EdacsCall* find(const EdacsTelemetry& t, int lcn, int gid) {
    for (const auto& c : t.calls) if (c.lcn == lcn && c.gid == gid) return &c;
    return nullptr;
}

int main() {
    {   // Extended Addressing: the same system, read with the EA layout (area instead of the LCN of the control channel)
        EdacsGenConfig c = baseConfig();
        c.layout.extended = true; c.systemId = 99;
        const Result r = run(c, true, false, false);
        const EdacsCall* a = find(r.t, 3, 777);
        CHECK(r.t.siteValid && r.t.siteId == 5 && r.t.systemId == 99, "EA site: %d %d", r.t.siteId, r.t.systemId);
        CHECK(a && a->group && a->lid == 4321, "EA call 1 missing or wrong");
        CHECK(find(r.t, 6, 888) != nullptr, "EA call 2 missing");
    }
    // ---- the message layer, the scan and following, clean
    {
        EdacsGenConfig c = baseConfig();
        const Result r = run(c, true, true, true);
        const EdacsTelemetry& t = r.t;
        CHECK(t.siteValid && t.siteId == 5 && t.systemId == -1 && t.controlLcn == 1 && t.nChannels == 6, "site: %d %d %d %d", t.siteId, t.systemId, t.controlLcn, t.nChannels);
        CHECK(t.messagesKnown > 20 && t.messagesKnown < t.messages, "messages %llu known of %llu", (unsigned long long)t.messagesKnown, (unsigned long long)t.messages);
        const EdacsCall* a = find(t, 3, 777);
        const EdacsCall* b = find(t, 6, 888);
        CHECK(a && a->group && a->lid == 4321 && !a->active && !a->fromActivity && a->startSec > 0.8 && a->startSec < 1.2, "call 1 missing or wrong");
        CHECK(a && std::fabs(a->freqHz - 856.050e6) < 1, "call 1 frequency");
        CHECK(a && a->durSec > 1.7 && a->durSec < 2.4, "call 1 lasted %.2f s", a ? a->durSec : -1.0);
        CHECK(b && b->emergency && b->lid == 5555 && b->startSec > 3.4 && b->startSec < 3.9, "call 2 missing or wrong");
        CHECK(b && !b->active && b->durSec > 1.3 && b->durSec < 3.5, "call 2 (no call end message) lasted %.2f s", b ? b->durSec : -1.0);
        const EdacsCall* d = find(t, 8, -1);
        CHECK(d && d->fromActivity && d->startSec > 5.8 && d->startSec < 6.4, "carrier-only call on LCN 8 missing");
        bool tg1 = false, tg2 = false;
        for (const auto& g : t.talkgroups) { tg1 |= g.gid == 777 && g.lastLid == 4321 && g.lastLcn == 3; tg2 |= g.gid == 888; }
        CHECK(tg1 && tg2, "talkgroup table (777 %d, 888 %d)", tg1, tg2);
        CHECK(t.channels.size() == 10 && t.lcnTableSize == 10, "channel table %zu", t.channels.size());
        CHECK(std::find(r.followed.begin(), r.followed.end(), 3) != r.followed.end() && std::find(r.followed.begin(), r.followed.end(), 6) != r.followed.end() &&
              std::find(r.followed.begin(), r.followed.end(), 8) != r.followed.end(), "followed channels: %zu", r.followed.size());
        auto at = [&](int lcn) -> const std::vector<float>& { static std::vector<float> none; auto it = r.audio.find(lcn); return it == r.audio.end() ? none : it->second; };
        const double s1 = toneShare(at(3), 1000, 48000), s2 = toneShare(at(6), 1600, 48000), s3 = toneShare(at(8), 700, 48000);
        std::printf("tone share: LCN 3 %.2f (1000 Hz, wrong tone %.2f), LCN 6 %.2f (1600 Hz), LCN 8 %.2f (700 Hz); %zu / %zu / %zu samples\n", s1, toneShare(at(3), 1600, 48000), s2, s3, at(3).size(), at(6).size(), at(8).size());
        CHECK(s1 > 0.5 && toneShare(at(3), 1600, 48000) < 0.05, "LCN 3 sound is not the 1000 Hz tone (%.2f)", s1);
        CHECK(s2 > 0.5 && toneShare(at(6), 1000, 48000) < 0.05, "LCN 6 sound is not the 1600 Hz tone (%.2f)", s2);
        CHECK(s3 > 0.5, "LCN 8 sound is not the 700 Hz tone (%.2f)", s3);
        CHECK(at(0).empty(), "sound with nothing followed (%zu samples)", at(0).size());
    }
    // ---- noise and a carrier offset: the control channel's offset is set from the carrier (the engine does it), the voice channels are placed by the table
    for (double cfo : {-1500.0, 0.0, 1200.0}) {
        EdacsGenConfig c = baseConfig();
        c.noise = 0.03; c.cfoHz = cfo; c.seed = 9;
        const Result r = run(c, true, true, true);
        const EdacsTelemetry& t = r.t;
        const EdacsCall* a = find(t, 3, 777);
        const EdacsCall* b = find(t, 6, 888);
        CHECK(t.siteValid && t.siteId == 5, "cfo %+.0f: site", cfo);
        CHECK(a && a->lid == 4321 && b && b->lid == 5555, "cfo %+.0f: calls %d %d", cfo, a != nullptr, b != nullptr);
        auto it = r.audio.find(3);
        const double s1 = it == r.audio.end() ? 0 : toneShare(it->second, 1000, 48000);
        CHECK(s1 > 0.4, "cfo %+.0f: LCN 3 tone share %.2f", cfo, s1);
        std::printf("noise 0.03, cfo %+.0f Hz: site %d, call 1 %s, tone share %.2f\n", cfo, t.siteId, a ? "found" : "MISSING", s1);
    }
    // ---- the group filter: only group 888 is followed
    {
        const Result r = run(baseConfig(), true, true, false, {888});
        CHECK(std::find(r.followed.begin(), r.followed.end(), 3) == r.followed.end(), "group 777 followed with the filter on 888");
        CHECK(std::find(r.followed.begin(), r.followed.end(), 6) != r.followed.end(), "group 888 not followed");
        CHECK(std::find(r.followed.begin(), r.followed.end(), 8) == r.followed.end(), "a carrier-only call followed with the activity follow off");
    }
    // ---- follow off: the tables fill, nothing is played
    {
        const Result r = run(baseConfig(), true, false, false);
        CHECK(r.followed.empty() && r.audio.empty(), "follow off: sound from %zu channels", r.audio.size());
        CHECK(find(r.t, 3, 777) != nullptr, "follow off: call 1 missing");
    }
    // ---- no frequency table: the control channel still decodes, the follow says why it cannot
    {
        const Result r = run(baseConfig(), false, true, true);
        CHECK(r.t.siteValid && r.t.talkgroups.size() >= 2, "no table: control decode (%zu talkgroups)", r.t.talkgroups.size());
        CHECK(r.followed.empty() && r.t.note.find("frequency table") != std::string::npos, "no table: note \"%s\"", r.t.note.c_str());
        CHECK(r.t.channels.empty(), "no table: channel list");
    }
    // ---- a channel outside the capture is reported, not followed
    {
        std::vector<double> t2 = table();
        t2[2] = 858.5e6;   // LCN 3 far outside the 480 kHz capture
        EdacsGenConfig c = baseConfig();
        c.calls.resize(1);
        const Result r = run(c, true, true, false, {}, &t2);
        CHECK(r.followed.empty(), "followed a channel outside the capture");
        CHECK(!r.t.channels.empty() && !r.t.channels[2].inCapture, "LCN 3 should be flagged as outside the capture");
    }
    // ---- no signal on the voice channels: nothing is called
    {
        EdacsGenConfig c = baseConfig();
        c.calls.clear();
        const Result r = run(c, true, true, true);
        CHECK(r.t.calls.empty() && r.followed.empty(), "idle system: %zu calls", r.t.calls.size());
        CHECK(r.t.siteValid, "idle system: site");
    }
    return testSummary("test_edacs_follow");
}
