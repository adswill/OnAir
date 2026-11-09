// DRM alternative frequencies (ES 201 980 clause 6.4.3.4, .5, .12): entities built from the clause's layouts, through the parser and through the real transmitter and receiver.
// Simulated: no recording with AFS on hand. And the station book.
#include "../data/drm/testkit.h"
#include "dect2/drm/drm_gen.h"
#include "dect2/drm/drm_msg.h"
#include "dect2/drm/drm_afs.h"
#include <cstdio>
#include <filesystem>
using namespace drmtest;
using namespace dect2;
#include "../common/test_util.h"

static void put(std::vector<uint8_t>& f, int type, int ver, const std::vector<uint8_t>& body) { drm::sdcPutEntity(f, type, ver, body); }

int main() {
    CHECK(drmDabChannelName(64) == "5A" && drmDabChannelName(95) == "12D" && drmDabChannelName(96) == "13A" && drmDabChannelName(101) == "13F" && drmDabChannelName(63).empty(), "DAB names");
    {   // type 3: synchronous, mode E, region + schedule; the service restriction field
        DrmAfs a; std::vector<uint8_t> f;
        put(f, 3, 0, drmAfsEntityBody3(true, {5975, 0x8000 | 8730}, 2, 3));
        put(f, 3, 0, drmAfsEntityBody3(false, {7300}, 0, 0, 0b0101));
        a.feed(f.data(), (int)f.size());
        CHECK(a.altFreqs().size() == 3, "%zu freqs", a.altFreqs().size());
        CHECK(a.altFreqs()[0].khz == 5975 && a.altFreqs()[0].synchronous && a.altFreqs()[0].regionId == 2 && a.altFreqs()[0].scheduleId == 3, "first");
        CHECK(a.altFreqs()[1].modeE && a.altFreqs()[1].khz == 87300, "mode E 10 kHz multiplier");
        CHECK(a.altFreqs()[2].khz == 7300 && !a.altFreqs()[2].synchronous && a.altFreqs()[2].shortIdMask == 5, "restricted");
        // the list is merged over blocks, a new version flag starts it again
        std::vector<uint8_t> g; put(g, 3, 1, drmAfsEntityBody3(true, {9000}));
        a.feed(g.data(), (int)g.size());
        CHECK(a.altFreqs().size() == 1 && a.altFreqs()[0].khz == 9000, "new version replaces");
    }
    {   // type 11: every system id with an FM, AM, DRM, DAB case
        DrmAfs a; std::vector<uint8_t> f;
        put(f, 11, 0, drmAfsEntityBody11(1, 4, true, 0x1234, 16, {105}, 8));                 // FM-RDS, PI only: 98.0 MHz
        put(f, 11, 0, drmAfsEntityBody11(0, 0, false, 0xE12345, 24, {6095}, 16));            // DRM other service
        put(f, 11, 0, drmAfsEntityBody11(0, 2, true, 0, 0, {1296}, 16));                     // AM, no id
        put(f, 11, 0, drmAfsEntityBody11(2, 10, true, 0xC1A2, 16, {64 + 7 * 4 + 2}, 8));     // DAB 12C
        put(f, 11, 0, drmAfsEntityBody11(2, 3, true, 0xE01234, 24, {}, 8));                  // ID only
        a.feed(f.data(), (int)f.size());
        const auto& v = a.altFreqs();
        CHECK(v.size() == 5, "%zu", v.size());
        CHECK(v[0].kind == 2 && v[0].khz == 98000 && v[0].otherId == 0x1234 && v[0].shortId == 1 && v[0].sameService, "FM");
        CHECK(v[1].kind == 0 && v[1].khz == 6095 && v[1].otherId == 0xE12345 && !v[1].sameService, "DRM other");
        CHECK(v[2].kind == 1 && v[2].khz == 1296 && !v[2].otherIdValid, "AM");
        CHECK(v[3].kind == 3 && v[3].dabChannel == "12C", "DAB %s", v[3].dabChannel.c_str());
        CHECK(v[4].khz == 0 && v[4].otherIdValid, "id only");
        // truncated entities do not read out of bounds
        for (size_t c = 0; c < f.size(); c++) { DrmAfs b; b.feed(f.data(), (int)c); }
    }
    {   // type 4 schedule, all days plus durations over a day and a week
        DrmAfsSchedule s; s.id = 1; s.dayCode = 0x40; s.startMin = 23 * 60; s.durationMin = 120;    // Monday 23:00 for 2 h
        CHECK(drmScheduleActive(s, 0, 23 * 60 + 30), "Mon 23:30");
        CHECK(drmScheduleActive(s, 1, 30), "over midnight, Tue 00:30");
        CHECK(!drmScheduleActive(s, 1, 90), "Tue 01:30 not");
        CHECK(!drmScheduleActive(s, 0, 22 * 60), "before");
        s.dayCode = 0x7F; s.startMin = 0; s.durationMin = 60;
        for (int d = 0; d < 7; d++) { CHECK(drmScheduleActive(s, d, 30), "every day %d", d); CHECK(!drmScheduleActive(s, d, 90), "only an hour"); }
        s.dayCode = 0x01; s.durationMin = 16383;   // Sunday 00:00, more than a week
        for (int d = 0; d < 7; d++) CHECK(drmScheduleActive(s, d, 700), "over a week %d", d);
        DrmAfs a; std::vector<uint8_t> f;
        put(f, 3, 0, drmAfsEntityBody3(false, {6000}, 0, 5));
        put(f, 4, 0, drmAfsEntityBody4(5, 0x40, 600, 60));                                    // Monday 10:00 - 11:00
        a.feed(f.data(), (int)f.size());
        a.applySchedule(2026, 11, 9, 10, 30);                                                 // a Monday
        CHECK(a.altFreqs()[0].hasSchedule && a.altFreqs()[0].activeNow, "on the air");
        a.applySchedule(2026, 11, 10, 10, 30);
        CHECK(!a.altFreqs()[0].activeNow, "the next day not");
        a.applySchedule(0, 0, 0, 0, 0);
        CHECK(a.altFreqs()[0].activeNow, "time unknown: shown as active");
    }
    {   // through the air interface: the generator's SDC rotation reaches the receiver at 12 dB fading-free
        SynthConfig sc; sc.mode = 12; sc.snrDb = 25;
        DrmTxConfig cfg; cfg.extraEntities.push_back({3, drmAfsEntityBody3(true, {5975, 7300})}); cfg.extraEntities.push_back({11, drmAfsEntityBody11(0, 4, true, 0xC123, 16, {105}, 8)});
        std::shared_ptr<ModeSynth> syn = makeDrmSynth(cfg, sc, 192000);
        CHECK(syn != nullptr, "synth");
        DrmReceiver rx; rx.setSilent(true); rx.configure(192000);
        std::vector<cf32> b(65536); DrmTelemetry t; uint64_t seq = 0;
        for (int i = 0; i < 90; i++) { syn->generate(b.data(), b.size()); rx.feed(b.data(), b.size()); rx.telemetry(t, seq); seq = t.seq; }
        CHECK(t.state == 2, "locked");
        bool a = false, fm = false; for (const auto& f : t.altFreqs) { if (f.khz == 5975 && f.kind == 0) a = true; if (f.khz == 98000 && f.kind == 2) fm = true; }
        printf("alt freqs through the receiver: %zu\n", t.altFreqs.size());
        CHECK(a && fm, "both arrived");
    }
    {   // the book: merge, save, load, damaged line
        DrmStationBook b; DrmBookEntry e; e.id = 0xE12345; e.label = "One"; e.heardKhz = {6095}; e.lastHeard = 100; b.note(e);
        DrmBookEntry e2; e2.id = 0xE12345; e2.language = "eng"; e2.heardKhz = {7300}; e2.altKhz = {5975}; e2.bestSnr = 20; e2.lastHeard = 200; b.note(e2);
        DrmBookEntry o; o.id = 7; o.label = "tab\there"; o.lastHeard = 50; b.note(o);
        const auto n = b.newestFirst();
        CHECK(n.size() == 2 && n[0].label == "One" && n[0].language == "eng" && n[0].heardKhz.size() == 2 && n[0].altKhz.size() == 1, "merged");
        const std::string p = (std::filesystem::temp_directory_path() / "onair_drm_book_test.txt").string();
        CHECK(b.save(p), "save");
        DrmStationBook c; CHECK(c.load(p), "load");
        CHECK(c.entries().size() == 2 && c.entries().at(0xE12345).bestSnr == 20 && c.entries().at(7).label == "tab here", "round trip");
        std::remove(p.c_str());
    }
    return testSummary("drm_afs");
}
