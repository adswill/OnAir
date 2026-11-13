// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The scheduler core: next-run times (cron, daily, weekly, DST in the EU and US rules), overlap resolution, missed jobs, the disk guard,
// the file-name template, the JSON round trip, and the scheduler driven against a fake radio.
#include "dect2/sched/sched.h"
#include <cstdio>
#include <cstdlib>

using namespace dect2::sched;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int64_t U(int y, int m, int d, int h = 0, int mi = 0, int s = 0) { return daysFromCivil(y, m, d) * 86400 + h * 3600 + mi * 60 + s; }

struct FakeHost : Host {
    RadioState state{100e6, "FM", "rtl", 20, false};
    std::vector<std::string> log;
    bool recording = false;
    RecordSpec lastSpec;
    int64_t free = int64_t(100) << 30;
    uint64_t bytes = 12345;
    std::vector<PassWindow> passList;
    bool failApply = false;
    RadioState snapshot() override { return state; }
    bool apply(const Action& a, std::string& err) override {
        if (failApply) { err = "no radio"; return false; }
        if (a.freqHz > 0) state.freqHz = a.freqHz;
        if (!a.mode.empty()) state.mode = a.mode;
        state.running = true;
        log.push_back("apply");
        return true;
    }
    bool startRecording(const RecordSpec& r, std::string&) override { recording = true; lastSpec = r; log.push_back("rec " + r.baseName); return true; }
    std::vector<FileResult> stopRecording() override { recording = false; log.push_back("stoprec"); return {{lastSpec.baseName + ".wav", "audio", bytes}}; }
    void restore(const RadioState& b) override { state = b; log.push_back("restore"); }
    int64_t freeBytes(const std::string&) override { return free; }
    std::vector<PassWindow> passes(const std::string&, double, int64_t from, int64_t to) override {
        std::vector<PassWindow> r;
        for (auto& p : passList) if (p.los > from && p.aos < to) r.push_back(p);
        return r;
    }
    double iqBytesPerSec() override { return 0; }
};

static Job mkJob(const char* id, Schedule s, int dur, int prio = 0) {
    Job j; j.id = id; j.name = id; j.when = s; j.durationSec = dur; j.priority = prio; j.action.recordAudio = true; j.action.mode = "AM"; j.action.freqHz = 118e6;
    return j;
}

static void testCivil() {
    int y, m, d;
    civilFromDays(daysFromCivil(2026, 10, 22), y, m, d);
    CHECK(y == 2026 && m == 10 && d == 22);
    CHECK(weekdayFromDays(daysFromCivil(1970, 1, 1)) == 4);
    CHECK(weekdayFromDays(daysFromCivil(2026, 10, 22)) == 4);   // a Thursday
    CHECK(weekdayFromDays(daysFromCivil(2024, 2, 29)) == 4);
    CHECK(U(1970, 1, 1) == 0);
}

static void testCron() {
    CronFields c; std::string err;
    CHECK(parseCron("*/15 6-8 * * 1-5", c, err));
    CHECK(c.min[0] && c.min[45] && !c.min[10] && c.hour[7] && !c.hour[9] && c.dow[1] && !c.dow[0]);
    CHECK(!parseCron("61 * * * *", c, err));
    CHECK(!parseCron("* * * *", c, err));
    CHECK(!parseCron("*/0 * * * *", c, err));
    CHECK(parseCron("0 0 * * 7", c, err) && c.dow[0]);
    Schedule s; s.kind = Schedule::Cron; s.cron = "30 6 * * *";
    const Zone utc = zoneUtc();
    CHECK(nextRun(s, utc, U(2026, 10, 22, 6, 0)) == U(2026, 10, 22, 6, 30));
    CHECK(nextRun(s, utc, U(2026, 10, 22, 6, 30)) == U(2026, 10, 23, 6, 30));   // strictly after
    s.cron = "0 */6 * * *";
    CHECK(nextRun(s, utc, U(2026, 10, 22, 7, 0)) == U(2026, 10, 22, 12, 0));
    s.cron = "0 12 29 2 *";   // leap day: found years later
    CHECK(nextRun(s, utc, U(2026, 3, 1)) == U(2028, 2, 29, 12, 0));
    s.cron = "0 9 1 * 1";     // day of month OR weekday when both are given
    CHECK(nextRun(s, utc, U(2026, 10, 22)) == U(2026, 10, 26, 9, 0));   // Monday the 26th comes before Nov 1
    s.cron = "bogus";
    CHECK(nextRun(s, utc, 0) == -1);
}

static void testSchedules() {
    const Zone utc = zoneUtc();
    Schedule d; d.kind = Schedule::Daily; d.hour = 6; d.minute = 30;
    CHECK(nextRun(d, utc, U(2026, 10, 22, 5)) == U(2026, 10, 22, 6, 30));
    CHECK(nextRun(d, utc, U(2026, 10, 22, 7)) == U(2026, 10, 23, 6, 30));
    // the zone moves the local clock: 06:30 at UTC+3 is 03:30 UTC
    CHECK(nextRun(d, zoneFixed(3 * 3600), U(2026, 10, 22, 0)) == U(2026, 10, 22, 3, 30));
    Schedule w; w.kind = Schedule::Weekly; w.hour = 8; w.minute = 0; w.weekdays = (1 << 1) | (1 << 5);   // Mon, Fri
    CHECK(nextRun(w, utc, U(2026, 10, 22, 9)) == U(2026, 10, 23, 8, 0));   // Thursday: next is Friday
    CHECK(nextRun(w, utc, U(2026, 10, 23, 9)) == U(2026, 10, 26, 8, 0));
    w.weekdays = 0;
    CHECK(nextRun(w, utc, 0) == -1);
    Schedule e; e.kind = Schedule::Every; e.at = 1000; e.everySec = 600;
    CHECK(nextRun(e, utc, 0) == 1000);
    CHECK(nextRun(e, utc, 1000) == 1600);
    CHECK(nextRun(e, utc, 1599) == 1600);
    CHECK(nextRun(e, utc, 5000) == 5200);
    Schedule o; o.kind = Schedule::Once; o.at = 500;
    CHECK(nextRun(o, utc, 100) == 500 && nextRun(o, utc, 500) == -1);
    CHECK(runsBetween(e, utc, 1000, 3000).size() == 3);
    CHECK(describe(d) == "daily 06:30");
}

static void testDst() {
    // EU, CET/CEST (UTC+1/+2). Summer time starts 2026-03-29 01:00 UTC and ends 2026-10-25 01:00 UTC.
    const Zone eu = zoneEu(3600);
    CHECK(eu(U(2026, 3, 29, 0, 59)) == 3600 && eu(U(2026, 3, 29, 1, 0)) == 7200);
    CHECK(eu(U(2026, 10, 25, 0, 59)) == 7200 && eu(U(2026, 10, 25, 1, 0)) == 3600);
    // 02:30 local on the spring day does not exist: it runs just after the gap, at 03:30 local = 01:30 UTC
    CHECK(localToUtc(eu, 2026, 3, 29, 2, 30) == U(2026, 3, 29, 1, 30));
    // 02:30 on the autumn day happens twice: the first (still summer time, 00:30 UTC) is the one
    CHECK(localToUtc(eu, 2026, 10, 25, 2, 30) == U(2026, 10, 25, 0, 30));
    // a daily 03:00 job keeps its wall-clock time over the change: 02:00 UTC in winter, 01:00 UTC in summer
    Schedule d; d.kind = Schedule::Daily; d.hour = 3; d.minute = 0;
    CHECK(nextRun(d, eu, U(2026, 3, 28, 12)) == U(2026, 3, 29, 1, 0));   // 03:00 CEST
    CHECK(nextRun(d, eu, U(2026, 3, 27, 12)) == U(2026, 3, 28, 2, 0));   // 03:00 CET
    // the nonexistent 02:30 daily job runs exactly once on the spring day, and once on the autumn day
    Schedule g; g.kind = Schedule::Daily; g.hour = 2; g.minute = 30;
    auto r = runsBetween(g, eu, U(2026, 3, 27, 12), U(2026, 3, 30, 23));
    CHECK(r.size() == 3 && r[1] == U(2026, 3, 29, 1, 30));
    r = runsBetween(g, eu, U(2026, 10, 23, 12), U(2026, 10, 26, 23));
    CHECK(r.size() == 3 && r[1] == U(2026, 10, 25, 0, 30));
    // a 24 hour interval is 24 hours in UTC however the clock changes
    Schedule e; e.kind = Schedule::Every; e.at = U(2026, 3, 28, 12); e.everySec = 86400;
    CHECK(nextRun(e, eu, U(2026, 3, 28, 13)) - U(2026, 3, 28, 12) == 86400);
    // US: second Sunday of March 2026 is the 8th, first Sunday of November the 1st; Central (UTC-6)
    const Zone us = zoneUs(-6 * 3600);
    CHECK(us(U(2026, 3, 8, 7, 59)) == -6 * 3600 && us(U(2026, 3, 8, 8, 0)) == -5 * 3600);   // 02:00 CST = 08:00 UTC
    CHECK(us(U(2026, 11, 1, 6, 59)) == -5 * 3600 && us(U(2026, 11, 1, 7, 0)) == -6 * 3600);   // 02:00 CDT = 07:00 UTC
    CHECK(localToUtc(us, 2026, 3, 8, 2, 30) == U(2026, 3, 8, 8, 30));    // gap: after it, 03:30 CDT = 08:30 UTC
    CHECK(localToUtc(us, 2026, 11, 1, 1, 30) == U(2026, 11, 1, 6, 30));   // twice: the first, 01:30 CDT
    // southern hemisphere style fixed zone and negative offsets
    CHECK(nextRun(d, zoneFixed(-5 * 3600), U(2026, 10, 22, 0)) == U(2026, 10, 22, 8, 0));
}

static void testNames() {
    NameFields f; f.utc = U(2026, 10, 22, 14, 5, 9); f.zoneOffsetSec = 3 * 3600; f.mode = "FM"; f.freqHz = 93.9e6; f.job = "Morning"; f.counter = 7;
    CHECK(expandName("", f) == "2026-10-22_17-05-09_FM_93.900MHz");
    CHECK(expandName("{utcdate}_{utctime}", f) == "2026-10-22_14-05-09");
    CHECK(expandName("{job}-{n:3}-{freq_khz}", f) == "Morning-007-93900");
    CHECK(expandName("{yyyy}/{mm}/{dd}/{mode}", f) == "2026/10/22/FM");
    f.mode = "../../etc/passwd";
    CHECK(expandName("{mode}", f) == ".._.._etc_passwd" || expandName("{mode}", f).find('/') == std::string::npos);
    CHECK(expandName("../x/./y", f) == "x/y");
    CHECK(expandName("a:b*c?", f) == "a_b_c_");
    CHECK(expandName("{unknown}", f) == "recording");
    f.mode.clear();
    CHECK(expandName("{mode}", f) == "nomode");
    CHECK(validateName("{date}_{n:2}").empty());
    CHECK(!validateName("{bogus}").empty() && !validateName("{date").empty() && !validateName("date}").empty() && !validateName("{date:3}").empty());
    CHECK(expandName(std::string(400, 'a'), f).size() == 200);
}

static void testOverlap() {
    ConflictPolicy p;
    auto R = [](const char* id, int64_t s, int64_t e, int pr, bool fixed = false) { PlannedRun r; r.jobId = id; r.start = s; r.end = e; r.priority = pr; r.fixedEnd = fixed; return r; };
    // equal priority: the later waits (queue) and keeps its length
    auto o = resolveOverlaps({R("a", 0, 600, 0), R("b", 300, 900, 0)}, p);
    CHECK(o.size() == 2 && o[1].state == PlannedRun::Delayed && o[1].start == 600 && o[1].end == 1200);
    // a fixed end (a pass) keeps its end, so it is shorter
    o = resolveOverlaps({R("a", 0, 600, 0), R("b", 300, 900, 0, true)}, p);
    CHECK(o[1].state == PlannedRun::Delayed && o[1].start == 600 && o[1].end == 900);
    // too little left: dropped
    o = resolveOverlaps({R("a", 0, 600, 0), R("b", 300, 620, 0, true)}, p);
    CHECK(o[1].state == PlannedRun::Dropped);
    // a higher priority later start cuts the running one short
    o = resolveOverlaps({R("a", 0, 600, 0), R("b", 300, 500, 5)}, p);
    CHECK(o[0].state == PlannedRun::Shortened && o[0].end == 300 && o[1].state == PlannedRun::Runs);
    // ... and a run cut to nothing is dropped
    o = resolveOverlaps({R("a", 0, 600, 0), R("b", 10, 500, 5)}, p);
    CHECK(o[0].state == PlannedRun::Dropped);
    // same start: the higher priority is first
    o = resolveOverlaps({R("a", 0, 600, 0), R("b", 0, 600, 3)}, p);
    CHECK(o[0].jobId == "b" && o[1].state == PlannedRun::Delayed);
    // no queue: dropped
    p.queue = false;
    o = resolveOverlaps({R("a", 0, 600, 0), R("b", 300, 900, 0)}, p);
    CHECK(o[1].state == PlannedRun::Dropped);
    // a wait beyond the limit: dropped
    p.queue = true; p.maxDelaySec = 100;
    o = resolveOverlaps({R("a", 0, 600, 0), R("b", 300, 900, 0)}, p);
    CHECK(o[1].state == PlannedRun::Dropped);
    // three in a row chain up
    p.maxDelaySec = 5000;
    o = resolveOverlaps({R("a", 0, 100, 0), R("b", 10, 110, 0), R("c", 20, 120, 0)}, p);
    CHECK(o[1].start == 100 && o[2].start == 200);
    // disjoint runs are untouched
    o = resolveOverlaps({R("a", 0, 100, 0), R("b", 100, 200, 0)}, p);
    CHECK(o[0].state == PlannedRun::Runs && o[1].state == PlannedRun::Runs);
}

static void testMissed() {
    Job j = mkJob("m", Schedule(), 600);
    PlannedRun m; m.jobId = "m"; m.start = 1000; m.end = 1600;
    CHECK(missedRuns(j, {m}, 2000, 30).empty());   // Skip
    j.missed = Missed::RunLate; j.graceSec = 2000;
    auto r = missedRuns(j, {m}, 2000, 30);
    CHECK(r.size() == 1 && r[0].start == 2000 && r[0].end == 2600);   // over: the full length from now
    r = missedRuns(j, {m}, 1300, 30);
    CHECK(r.size() == 1 && r[0].start == 1300 && r[0].end == 1600);   // inside the window: the rest
    CHECK(missedRuns(j, {m}, 5000, 30).empty());   // beyond the grace time
    j.fixedEnd = true;
    CHECK(missedRuns(j, {m}, 2000, 30).empty());
    CHECK(wokeUp(100, 100 + 3600, 5) && !wokeUp(100, 110, 5) && wokeUp(100, 50, 5) && !wokeUp(0, 5000, 5));
}

static void testDisk() {
    const int64_t G = int64_t(1) << 30;
    CHECK(diskGuard(10 * G, 2 * G, 1e6) == DiskState::Ok);
    CHECK(diskGuard(G, 2 * G, 1e6) == DiskState::Stop);
    CHECK(diskGuard(2 * G + 1000000, 2 * G, 1e6) == DiskState::Warn);
    CHECK(diskGuard(2 * G + 1000000, 2 * G, 0) == DiskState::Ok);
    Action a; a.recordIq = true; a.recordAudio = true;
    CHECK(estimateBytesPerSec(a, 2e6, 2) == 4e6 + 96000);
}

static void testJson() {
    std::vector<Job> jobs;
    Job a = mkJob("j1", Schedule(), 90, 2);
    a.when.kind = Schedule::Weekly; a.when.hour = 7; a.when.minute = 5; a.when.weekdays = 0x41;
    a.action.namePattern = "{date}\"x\\y"; a.missed = Missed::RunLate; a.name = "Morning \"net\"";
    Job b = mkJob("j2", Schedule(), 0); b.when.kind = Schedule::Pass; b.when.satellite = "ISS (ZARYA)"; b.when.minElevationDeg = 20; b.when.leadSec = 30; b.enabled = false;
    jobs = {a, b};
    Settings s; s.minFreeBytes = 5000000000ll; s.conflict.queue = false; s.lastSeen = 1234567890;
    const std::string t = toJson(jobs, s);
    std::vector<Job> j2; Settings s2; std::string err;
    CHECK(fromJson(t, j2, s2, err));
    CHECK(j2.size() == 2 && j2[0].name == a.name && j2[0].action.namePattern == a.action.namePattern && j2[0].when.weekdays == 0x41);
    CHECK(j2[0].missed == Missed::RunLate && j2[0].priority == 2 && j2[0].durationSec == 90);
    CHECK(j2[1].when.kind == Schedule::Pass && j2[1].when.satellite == "ISS (ZARYA)" && !j2[1].enabled && j2[1].when.leadSec == 30);
    CHECK(s2.minFreeBytes == 5000000000ll && !s2.conflict.queue && s2.lastSeen == 1234567890);
    CHECK(!fromJson("not json", j2, s2, err));
    CHECK(!fromJson("{\"jobs\":[{\"name\":\"x\"}]}", j2, s2, err));
    std::vector<Job> none; CHECK(fromJson(toJson(none, s), j2, s2, err) && j2.empty());
    std::vector<RunRecord> h(1); h[0].jobId = "j1"; h[0].start = 10; h[0].end = 70; h[0].result = Result::DiskFull; h[0].error = "full";
    h[0].files.push_back({"/x/a.wav", "audio", 99});
    std::vector<RunRecord> h2;
    CHECK(historyFromJson(historyToJson(h), h2, err) && h2.size() == 1 && h2[0].result == Result::DiskFull && h2[0].files[0].bytes == 99 && h2[0].bytes() == 99 && h2[0].seconds() == 60);
}

static void testRun() {
    FakeHost h;
    Scheduler sc(zoneUtc());
    Schedule o; o.kind = Schedule::Once; o.at = 10000;
    sc.jobs.push_back(mkJob("a", o, 600));
    sc.tick(9000, h);
    CHECK(!sc.active());
    sc.tick(9995, h);
    CHECK(!sc.active());
    sc.tick(10000, h);
    CHECK(sc.active() && h.state.freqHz == 118e6 && h.state.mode == "AM" && h.recording);
    CHECK(h.lastSpec.baseName.find("AM") != std::string::npos);
    sc.tick(10300, h);
    CHECK(sc.active());
    sc.tick(10600, h);   // the full 600 s
    CHECK(!sc.active() && !h.recording);
    CHECK(h.state.freqHz == 100e6 && h.state.mode == "FM" && !h.state.running);   // the state from before is back
    CHECK(sc.history.size() == 1 && sc.history[0].result == Result::Done && sc.history[0].start == 10000 && sc.history[0].end == 10600);
    CHECK(sc.history[0].files.size() == 1 && sc.history[0].bytes() == 12345);
    sc.tick(20000, h);   // a Once job does not come back
    CHECK(!sc.active() && sc.history.size() == 1);

    // the disk fills during a run
    FakeHost h2; Scheduler s2(zoneUtc());
    s2.jobs.push_back(mkJob("a", o, 600));
    s2.tick(9999, h2); s2.tick(10000, h2);
    CHECK(s2.active());
    h2.free = 1000;
    s2.tick(10010, h2);
    CHECK(!s2.active() && s2.history.back().result == Result::DiskFull && !s2.lastWarning().empty());
    // and a run does not even start on a full disk
    FakeHost h3; h3.free = 1000; Scheduler s3(zoneUtc());
    s3.jobs.push_back(mkJob("a", o, 600));
    s3.tick(9999, h3); s3.tick(10000, h3);
    CHECK(!s3.active() && s3.history.size() == 1 && s3.history[0].result == Result::DiskFull);

    // a radio that cannot be tuned: an error entry, the state restored
    FakeHost h4; h4.failApply = true; Scheduler s4(zoneUtc());
    s4.jobs.push_back(mkJob("a", o, 600));
    s4.tick(9999, h4); s4.tick(10000, h4);
    CHECK(!s4.active() && s4.history.size() == 1 && s4.history[0].result == Result::Error && s4.history[0].error == "no radio");

    // two overlapping jobs: the lower priority one waits, then runs for its own length
    FakeHost h5; Scheduler s5(zoneUtc());
    Schedule o2; o2.kind = Schedule::Once; o2.at = 10100;
    s5.jobs.push_back(mkJob("a", o, 300, 1));
    s5.jobs.push_back(mkJob("b", o2, 200, 0));
    s5.tick(9999, h5); s5.tick(10000, h5);
    CHECK(s5.activeJobId() == "a");
    s5.tick(10100, h5);
    CHECK(s5.activeJobId() == "a" && s5.queued() == 1);
    s5.tick(10300, h5);   // a ends, b starts at once (the queue)
    CHECK(s5.activeJobId() == "b" && s5.activeEnd() == 10500);
    s5.tick(10500, h5);
    CHECK(!s5.active() && s5.history.size() == 2 && h5.state.freqHz == 100e6);

    // a higher priority job takes over
    FakeHost h6; Scheduler s6(zoneUtc());
    s6.jobs.push_back(mkJob("a", o, 600, 0));
    s6.jobs.push_back(mkJob("hi", o2, 100, 9));
    s6.tick(9999, h6); s6.tick(10000, h6);
    s6.tick(10100, h6);
    CHECK(s6.activeJobId() == "hi" && s6.history.size() == 1 && s6.history[0].result == Result::Preempted && s6.history[0].jobId == "a");

    // missed while asleep: skip policy writes a Missed entry; run-late runs it
    FakeHost h7; Scheduler s7(zoneUtc());
    s7.jobs.push_back(mkJob("a", o, 600));
    s7.tick(9000, h7);
    s7.tick(12000, h7);   // the machine slept through 10000
    CHECK(!s7.active() && s7.history.size() == 1 && s7.history[0].result == Result::Missed);
    FakeHost h8; Scheduler s8(zoneUtc());
    Job late = mkJob("a", o, 600); late.missed = Missed::RunLate; late.graceSec = 4000;
    s8.jobs.push_back(late);
    s8.tick(9000, h8);
    s8.tick(12000, h8);
    CHECK(s8.active() && s8.activeEnd() == 12600);
    // the program was closed and started again: lastSeen says since when
    FakeHost h9; Scheduler s9(zoneUtc());
    s9.settings.lastSeen = 9000;
    s9.jobs.push_back(mkJob("a", o, 600));
    s9.tick(12000, h9);
    CHECK(s9.history.size() == 1 && s9.history[0].result == Result::Missed);

    // a daily job fires every day
    FakeHost h10; Scheduler s10(zoneUtc());
    Schedule d; d.kind = Schedule::Daily; d.hour = 6; d.minute = 0;
    s10.jobs.push_back(mkJob("d", d, 60));
    int64_t t = U(2026, 10, 22, 5, 59, 0);
    for (; t < U(2026, 10, 25, 6, 30); t += 5) s10.tick(t, h10);
    CHECK(s10.history.size() == 4 && s10.history[0].result == Result::Done);
    CHECK(s10.history[1].start - s10.history[0].start == 86400);

    // a pass job: starts at the rise and ends at the set, whatever the duration says
    FakeHost h11; Scheduler s11(zoneUtc());
    h11.passList = {{50000, 50600, 40}, {60000, 60500, 25}};
    Job pj = mkJob("p", Schedule(), 0); pj.when.kind = Schedule::Pass; pj.when.satellite = "ISS"; pj.when.minElevationDeg = 10;
    s11.jobs.push_back(pj);
    for (t = 49000; t < 61000; t += 5) s11.tick(t, h11);
    CHECK(s11.history.size() == 2 && s11.history[0].start >= 50000 && s11.history[0].start <= 50010 && s11.history[0].end == 50600);
    CHECK(s11.history[1].end == 60500);
    CHECK(s11.nextRunOf(pj, 49000, h11) == 50000);

    // run now, and the upcoming list with conflict resolution
    FakeHost h12; Scheduler s12(zoneUtc());
    Job rn = mkJob("r", Schedule(), 120); s12.jobs.push_back(rn);
    CHECK(s12.runNow("r", 5000, h12) && s12.active() && s12.activeEnd() == 5120);
    CHECK(!s12.runNow("nothing", 5000, h12));
    s12.stopActive(5050, h12);
    CHECK(!s12.active() && s12.history[0].result == Result::Stopped && s12.history[0].seconds() == 50);
    Schedule e1; e1.kind = Schedule::Every; e1.at = 100000; e1.everySec = 1000;
    Scheduler s13(zoneUtc());
    s13.jobs.push_back(mkJob("x", e1, 1500));
    auto up = s13.upcoming(99000, 4000, h12);
    CHECK(up.size() >= 3);
    for (size_t i = 1; i < up.size(); i++) if (up[i].state != PlannedRun::Dropped && up[i - 1].state != PlannedRun::Dropped) CHECK(up[i].start >= up[i - 1].end);
}

int main() {
    testCivil(); testCron(); testSchedules(); testDst(); testNames(); testOverlap(); testMissed(); testDisk(); testJson(); testRun();
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("sched: ok\n");
    return 0;
}
