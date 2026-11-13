// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The setup and the lessons (app/ui/learn_logic.h): the tables are complete, every lesson points at a part the interface reports, the
// step checks, the saved progress and resuming, the checkup rules and the glossary search.
#include "../../app/ui/learn_logic.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static std::string readAll(const std::filesystem::path& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }
static int countOf(const char* s, const char* pat) { int c = 0; for (const char* p = std::strstr(s, pat); p; p = std::strstr(p + 1, pat)) c++; return c; }

int main() {
    using namespace learn;
    // the tables
    for (int i = 0; i < GoalCount; i++) CHECK(goals()[i].label && *goals()[i].blurb && *goals()[i].modeId, "goal %d is complete", i);
    for (int i = 0; i < AntCount; i++) CHECK(antennas()[i].label && *antennas()[i].blurb, "antenna %d is complete", i);
    int nr = 0;
    const Region* rg = regions(&nr);
    CHECK(nr >= 8, "regions: %d", nr);
    for (int i = 0; i < nr; i++) {
        CHECK(rg[i].fmMhz >= rg[i].fmLoMhz && rg[i].fmMhz <= rg[i].fmHiMhz, "region %d: the FM frequency is in its band", i);
        CHECK(rg[i].fmDeemphUs == 50 || rg[i].fmDeemphUs == 75, "region %d: de-emphasis", i);
        CHECK(rg[i].bandPlan != nullptr, "region %d: band plan", i);
    }
    CHECK(clampRegion(-3) == 0 && clampRegion(999) == nr - 1, "region clamp");
    CHECK(std::strstr(radioBlurb("", "Generic RTL2832U OEM"), "RTL-SDR") && std::strstr(radioBlurb("HackRF One", "HackRF One 0000"), "HackRF"), "radio notes");
    CHECK(std::strstr(radioBlurb("", "rtl_tcp 192.168.1.5 (network)"), "RTL-SDR") != nullptr, "an rtl_tcp radio is explained");
    CHECK(*radioBlurb("", "Something new"), "an unknown radio still gets a line");

    // the lessons: unique ids, texts in house style, every anchor reported by the interface
    int n = 0;
    const Lesson* L = lessons(&n);
    CHECK(n >= 15, "lessons: %d", n);
    std::set<std::string> ids, anchors;
    for (int i = 0; i < n; i++) {
        CHECK(ids.insert(L[i].id).second, "lesson %s: the id is used once", L[i].id);
        CHECK(*L[i].title && *L[i].text && L[i].done && *L[i].done, "lesson %s is complete", L[i].id);
        CHECK(!std::strchr(L[i].text, '!') && !std::strchr(L[i].title, '!') && !std::strchr(L[i].done, '!'), "lesson %s: no exclamation marks", L[i].id);
        CHECK(!std::strchr(L[i].text, '%'), "lesson %s: no format codes in the text", L[i].id);
        CHECK(countOf(L[i].done, "%") == 0 || (countOf(L[i].done, "%") == 1 && std::strstr(L[i].done, "%.3f")), "lesson %s: at most one %%.3f in the done text", L[i].id);
        CHECK((L[i].help == Help::None) == (L[i].helpLabel == nullptr), "lesson %s: a help button has a label", L[i].id);
        CHECK(L[i].help != Help::SetMode || L[i].modeId, "lesson %s: Set mode needs a mode", L[i].id);
        CHECK(L[i].check != Check::TuneInBand || L[i].lo < L[i].hi, "lesson %s: band", L[i].id);
        if (*L[i].anchor) anchors.insert(L[i].anchor);
    }
    std::string all;
    for (auto& e : std::filesystem::recursive_directory_iterator(std::string(DECT2_SOURCE_DIR) + "/app"))
        if (e.is_regular_file() && e.path().extension() == ".cpp") all += readAll(e.path());
    for (const std::string& a : anchors) {
        const bool legacy = a == "modes" || a == "spectrum" || a == "panel";
        CHECK(legacy || all.find("tourMark(\"" + a + "\"") != std::string::npos, "anchor %s is reported by the interface", a.c_str());
    }
    CHECK(all.find("legacy(\"spectrum\", TgMain)") != std::string::npos, "the lessons report the older rectangles too");
    CHECK(all.find("autogainRun(a, a.devIdx") != std::string::npos, "the gain lesson goes through autogainRun");

    // the plan: the basics in table order, then only the chosen goals' lessons
    const std::vector<int> basics = lessonPlan(0);
    CHECK(!basics.empty() && L[basics[0]].goalMask == 0 && std::string(L[basics[0]].id) == "start", "the plan starts with the receiver");
    for (int i : basics) CHECK(L[i].goalMask == 0, "no goal lessons without a goal");
    const std::vector<int> fm = lessonPlan(1u << GoalListen);
    CHECK(fm.size() == basics.size() + 1 && std::string(L[fm.back()].id) == "g_fm", "FM adds the FM lesson");
    const std::vector<int> track = lessonPlan(1u << GoalTrack);
    CHECK(track.size() == basics.size() + 3, "tracking adds ADS-B, the map and AIS (%zu)", track.size() - basics.size());
    for (int g = 0; g < GoalCount; g++) CHECK(lessonPlan(1u << g).size() > basics.size(), "goal %d has a lesson of its own", g);

    // the checks
    CHECK(freqNear(100.0005, 100.0, 1) && !freqNear(100.002, 100.0, 1) && freqNear(99.999, 100.0, 1), "within 1 kHz");
    Observed s, o;
    s.freqMhz = 522.0; s.modeId = "tv"; s.volume = 0.5f;
    o = s;
    const Lesson& type = L[lessonIndex("type")];
    CHECK(!verify(type, s, o, 100.0), "not tuned yet");
    o.freqMhz = 100.0004;
    CHECK(verify(type, s, o, 100.0), "tuned to the region's FM frequency");
    CHECK(!verify(type, s, o, 97.0), "another region's frequency");
    o = s; o.freqMhz = 522.1;
    CHECK(verify(L[lessonIndex("click")], s, o, 100.0) && !verify(L[lessonIndex("click")], s, s, 100.0), "any retune passes the click lesson");
    const Lesson& ft8 = L[lessonIndex("g_ft8")];
    o = s; o.freqMhz = 14.0745; o.modeId = "hfdig";
    CHECK(!verify(ft8, s, o, 100.0), "FT8: the right frequency in the wrong mode");
    o.modeId = "ft8";
    CHECK(verify(ft8, s, o, 100.0), "FT8: 14.074 MHz in FT8");
    o.freqMhz = 14.080;
    CHECK(!verify(ft8, s, o, 100.0), "FT8: 6 kHz off");
    const Lesson& air = L[lessonIndex("g_air")];
    o = s; o.modeId = "airband"; o.freqMhz = 118.1;
    CHECK(verify(air, s, o, 100.0), "airband in band");
    o.freqMhz = 108.0;
    CHECK(!verify(air, s, o, 100.0), "airband: below the band");
    o = s; o.agcOn = true;
    CHECK(verify(L[lessonIndex("gain")], s, o, 100.0), "gain: AGC on");
    o = s; o.autogainRan = true;
    CHECK(verify(L[lessonIndex("gain")], s, o, 100.0), "gain: auto-gain ran");
    CHECK(!verify(L[lessonIndex("gain")], s, s, 100.0), "gain: nothing done");
    o = s; o.modeId = "fm";
    CHECK(verify(L[lessonIndex("mode")], s, o, 100.0) && !verify(L[lessonIndex("mode")], s, s, 100.0), "mode change");
    o = s; o.muted = true;
    CHECK(verify(L[lessonIndex("volume")], s, o, 100.0), "mute counts as a volume change");
    o = s; o.volume = 0.8f;
    CHECK(verify(L[lessonIndex("volume")], s, o, 100.0) && !verify(L[lessonIndex("volume")], s, s, 100.0), "volume");
    o = s; o.recording = true;
    CHECK(verify(L[lessonIndex("record")], s, o, 100.0), "recording");
    o = s; o.squelch = 1.5;
    CHECK(verify(L[lessonIndex("squelch")], s, o, 100.0) && !verify(L[lessonIndex("squelch")], s, s, 100.0), "squelch");
    o = s; o.bandwidthHz = 12500;
    CHECK(verify(L[lessonIndex("bandwidth")], s, o, 100.0), "bandwidth");
    o = s; o.running = true;
    CHECK(verify(L[lessonIndex("start")], s, o, 100.0), "running");
    o = s; o.windows = WinMap;
    CHECK(verify(L[lessonIndex("g_map")], s, o, 100.0) && !verify(L[lessonIndex("g_pass")], s, o, 100.0), "windows");
    CHECK(!verify(L[lessonIndex("waterfall")], s, o, 100.0), "a manual lesson waits for Got it");

    // progress: encode, decode, resume
    Progress p;
    CHECK(decode("").empty() && encode(p).empty(), "empty");
    p["start"] = Done; p["click"] = Skipped; p["junk"] = Todo;
    const std::string enc = encode(p);
    CHECK(enc == "click=2;start=1", "encoded: %s", enc.c_str());
    const Progress q = decode(enc + ";bad;=1;x=7;y=1x");
    CHECK(q.size() == 2 && stateOf(q, "start") == Done && stateOf(q, "click") == Skipped && stateOf(q, "type") == Todo, "decoded, bad items ignored");
    std::vector<int> plan = lessonPlan(1u << GoalListen);
    Progress r;
    CHECK(resumeAt(plan, r) == 0 && doneCount(plan, r) == 0, "a fresh start");
    r[L[plan[0]].id] = Done; r[L[plan[1]].id] = Skipped;
    CHECK(resumeAt(plan, r) == 2 && doneCount(plan, r) == 1, "resumes after the done and the skipped");
    for (int i : plan) r[L[i].id] = Done;
    CHECK(resumeAt(plan, r) == -1 && doneCount(plan, r) == (int)plan.size(), "all done");
    CHECK(setupResumeStep(3, 6) == 3 && setupResumeStep(9, 6) == 5 && setupResumeStep(-1, 6) == 0, "the setup resumes within its steps");

    // the checkup
    auto has = [](const std::vector<CheckItem>& v, const char* word, Level lv) { for (const CheckItem& c : v) if (c.level == lv && std::strstr(c.text, word)) return true; return false; };
    CheckupIn c;
    c.radio = true; c.radioOpened = true; c.antenna = AntWhip; c.goals = 1u << GoalListen; c.agcOn = true;
    std::vector<CheckItem> v = checkup(c);
    CHECK(has(v, "radio works", Ok) && has(v, "fine for FM", Ok) && !has(v, "airband", Tip), "whip for FM: fine");
    c.goals |= 1u << GoalAirband;
    v = checkup(c);
    CHECK(has(v, "airband", Tip), "whip for airband: a tip");
    c.goals = 1u << GoalHam;
    v = checkup(c);
    CHECK(has(v, "short wave", Warn), "whip for FT8: a warning");
    c.antenna = AntNone;
    v = checkup(c);
    CHECK(has(v, "Without an antenna", Warn), "no antenna");
    c = CheckupIn();
    v = checkup(c);
    CHECK(has(v, "No radio", Tip), "no radio: the demo");
    c.radio = true; c.startError = "HackRF busy";
    v = checkup(c);
    CHECK(has(v, "did not start", Warn) && !has(v, "radio works", Ok), "a failed start");
    c = CheckupIn(); c.radio = true; c.goals = 1u << GoalSat; c.location = false;
    v = checkup(c);
    CHECK(has(v, "location", Tip) && has(v, "Automatic gain is off", Tip), "satellites need the location; AGC off");
    c.location = true; c.agcOn = true; c.lessonsTotal = 5; c.lessonsDone = 5;
    v = checkup(c);
    CHECK(!has(v, "location", Tip) && has(v, "All your lessons", Ok), "location given, lessons done");
    for (const CheckItem& it : v) CHECK(!std::strchr(it.text, '!'), "checkup: no exclamation marks");

    // the glossary
    int ng = 0;
    const Term* t = glossary(&ng);
    std::set<Pic> pics;
    for (int i = 0; i < ng; i++) { CHECK(*t[i].word && *t[i].text, "term %d", i); pics.insert(t[i].pic); }
    for (const char* w : {"SNR", "Gain", "Sample rate", "Bandwidth", "Offset", "ppm", "Squelch", "Modulation", "Decoder"}) {
        bool found = false;
        for (int i = 0; i < ng; i++) found |= std::strstr(t[i].word, w) != nullptr;
        CHECK(found, "the glossary explains %s", w);
    }
    CHECK(pics.size() >= 9, "the glossary has pictures for the main terms");
    CHECK(termMatches("Gain", "How much", "gAI") && termMatches("Squelch", "keeps the sound muted", "muted") && !termMatches("Gain", "x", "zzz") && termMatches("a", "b", ""), "search");

    std::printf(failures ? "FAILED: %d\n" : "learn: ok\n", failures);
    return failures ? 1 : 0;
}
