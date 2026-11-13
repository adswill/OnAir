// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The scheduler core: when a job is due (once, every N, daily, weekly, cron-like, satellite pass), how overlapping jobs are resolved, what
// happens to jobs missed while the machine slept, the disk-space guard, the recording file-name template, and the JSON the jobs and the
// run history are stored in. No clocks and no radio: time comes in as UTC seconds and the radio sits behind the Host interface, so all of it
// is tested without a receiver (tests/sched/test_sched.cpp). The window is app/panels/scheduler_ui.cpp.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2::sched {

// ---- time zones and civil dates ----------------------------------------------------------------------------------------------------
// The offset east of UTC, in seconds, that wall clocks show at the UTC instant t. The app passes the system's local zone; the tests use
// fixed zones and the EU / US daylight-saving rules below.
using Zone = std::function<int(int64_t utc)>;
Zone zoneUtc();
Zone zoneFixed(int offsetSec);
Zone zoneEu(int stdOffsetSec);   // summer time from the last Sunday of March 01:00 UTC to the last Sunday of October 01:00 UTC
Zone zoneUs(int stdOffsetSec);   // second Sunday of March 02:00 standard time to first Sunday of November 02:00 daylight time

int64_t daysFromCivil(int y, int m, int d);                    // days since 1970-01-01
void civilFromDays(int64_t z, int& y, int& m, int& d);
int weekdayFromDays(int64_t z);                                // 0 = Sunday
// A wall-clock time to the UTC instant. A time that does not exist (the hour skipped in spring) runs at the instant after the gap; a time
// that happens twice (autumn) is the first of the two.
int64_t localToUtc(const Zone& z, int y, int m, int d, int hh, int mm, int ss = 0);

// ---- when a job is due -------------------------------------------------------------------------------------------------------------
struct Schedule {
    enum Kind { Once, Every, Daily, Weekly, Cron, Pass } kind = Once;
    int64_t at = 0;            // Once: the UTC instant. Every: the anchor (the first run)
    int64_t everySec = 3600;   // Every
    int hour = 0, minute = 0;  // Daily, Weekly: local wall clock
    int weekdays = 0x7f;       // Weekly: bit 0 = Sunday ... bit 6 = Saturday
    std::string cron;          // Cron: "min hour day-of-month month day-of-week", lists, ranges, steps; local wall clock
    std::string satellite;     // Pass: the satellite's name
    double minElevationDeg = 10;   // Pass: the pass has to rise above this
    int leadSec = 0;           // Pass: start this long before the rise
};

struct CronFields {
    std::vector<bool> min, hour, dom, mon, dow;   // 60, 24, 32 (1..31), 13 (1..12), 7 (0 = Sunday)
    bool domAny = true, dowAny = true;
};
bool parseCron(const std::string& text, CronFields& out, std::string& err);

// The first run strictly after `after` (UTC), or -1 when there is none. Not for Pass (those come from the host's pass list).
int64_t nextRun(const Schedule& s, const Zone& z, int64_t after);
// Every run in (from, to], at most `max`.
std::vector<int64_t> runsBetween(const Schedule& s, const Zone& z, int64_t from, int64_t to, size_t max = 1000);

// ---- the recording file name -------------------------------------------------------------------------------------------------------
struct NameFields {
    int64_t utc = 0;
    int zoneOffsetSec = 0;     // local time = utc + this; {date} and {time} are local, {utcdate} {utctime} UTC
    std::string mode, job, kind, satellite;
    double freqHz = 0;
    int counter = 1;
};
// {date} 2026-10-22  {time} 14-05-09  {datetime}  {yyyy} {mm} {dd} {HH} {MM} {SS}  {utcdate} {utctime}  {mode} {job} {kind} {sat}
// {freq} 93.900MHz  {freq_mhz} 93.900  {freq_khz} 93900  {freq_hz}  {n} the counter; {n:3} zero-padded. "/" in the pattern makes sub-folders.
// The result has no characters a file name may not have, no "..", and is at most 200 characters.
std::string expandName(const std::string& pattern, const NameFields& f);
// "" when the pattern is fine, otherwise what is wrong (an unknown or unclosed {token})
std::string validateName(const std::string& pattern);
std::string sanitizeName(const std::string& s);
extern const char* const kDefaultNamePattern;   // "{date}_{time}_{mode}_{freq}"

// ---- jobs, runs, history -----------------------------------------------------------------------------------------------------------
struct Action {
    double freqHz = 0;         // 0 = leave the tuning as it is
    std::string mode;          // "" = leave
    std::string radio;         // "" = the radio in use
    double gainDb = -1;        // < 0 = leave
    bool recordAudio = false, recordIq = false, recordLog = false;
    std::string namePattern;   // "" = kDefaultNamePattern
    std::string folder;        // "" = the recordings folder
};
enum class Missed { Skip, RunLate };

struct Job {
    std::string id, name;
    bool enabled = true;
    int priority = 0;          // higher wins a conflict
    Schedule when;
    int durationSec = 600;     // 0 = until the pass sets (Pass jobs)
    Action action;
    Missed missed = Missed::Skip;
    int graceSec = 900;        // RunLate: only when the run's start is not older than this
    bool fixedEnd = false;     // the run ends at its planned time even when it starts late (passes always do)
};

struct FileResult { std::string path, kind; uint64_t bytes = 0; };
enum class Result { Done, Error, Missed, Skipped, Preempted, DiskFull, Stopped };
const char* resultName(Result r);
struct RunRecord {
    std::string jobId, jobName;
    int64_t planned = 0, start = 0, end = 0;   // UTC; start = end = 0 when it never ran
    Result result = Result::Done;
    std::string error;
    std::vector<FileResult> files;
    double seconds() const { return end > start ? double(end - start) : 0; }
    uint64_t bytes() const { uint64_t b = 0; for (auto& f : files) b += f.bytes; return b; }
};

// A run on a timeline, before and after conflict resolution
struct PlannedRun {
    std::string jobId;
    int64_t start = 0, end = 0;
    int priority = 0;
    bool fixedEnd = false;
    enum State { Runs, Delayed, Shortened, Dropped } state = Runs;
    std::string note;
};
struct ConflictPolicy {
    bool queue = true;         // a run that loses waits for the winner to end (otherwise it is dropped)
    int maxDelaySec = 1800;    // longest wait
    int minRunSec = 30;        // a run shorter than this after shortening or waiting is dropped
};
// Overlap resolution on a timeline: the higher priority wins; a later start of a higher priority cuts the running run short; equal
// priority: the earlier one keeps its slot. The loser waits (policy.queue) or is dropped. Output is in start order, losers included.
std::vector<PlannedRun> resolveOverlaps(std::vector<PlannedRun> runs, const ConflictPolicy& p);

// Runs missed in (lastSeen, now] by one job, for the missed-job policy. Skip: none. RunLate: the latest missed run when it is within
// graceSec of its start and still worth it, starting at `now`.
std::vector<PlannedRun> missedRuns(const Job& j, const std::vector<PlannedRun>& missed, int64_t now, int minRunSec);
// A tick gap this long (or the clock going back) means the machine slept or the clock was set.
bool wokeUp(int64_t lastTick, int64_t now, int tickSec);

// ---- the disk guard ----------------------------------------------------------------------------------------------------------------
enum class DiskState { Ok, Warn, Stop };
// Warn when the free space would fall below the limit within warnSec at the recording's rate, Stop when it already is below.
DiskState diskGuard(int64_t freeBytes, int64_t limitBytes, double bytesPerSec, int warnSec = 120);
// Bytes per second the recording of an action needs: IQ at the rate and sample size, audio at 16-bit mono, the log a trickle.
double estimateBytesPerSec(const Action& a, double iqRateHz, int iqBytesPerSample);

// ---- the radio, as the scheduler sees it --------------------------------------------------------------------------------------------
struct RadioState { double freqHz = 0; std::string mode, radio; double gainDb = -1; bool running = false; };
struct RecordSpec { bool audio = false, iq = false, log = false; std::string dir, baseName; };
struct PassWindow { int64_t aos = 0, los = 0; double maxElDeg = 0; };

struct Host {
    virtual ~Host() = default;
    virtual RadioState snapshot() = 0;
    // tune, set the mode and gain, start the receiver; false with err when it cannot
    virtual bool apply(const Action& a, std::string& err) = 0;
    virtual bool startRecording(const RecordSpec& r, std::string& err) = 0;
    virtual std::vector<FileResult> stopRecording() = 0;   // the files with their final sizes
    virtual void restore(const RadioState& before) = 0;    // tune back, stop the receiver if it was stopped before
    virtual int64_t freeBytes(const std::string& dir) = 0;
    virtual std::vector<PassWindow> passes(const std::string& satellite, double minElDeg, int64_t from, int64_t to) = 0;
    virtual double iqBytesPerSec() { return 0; }
    virtual void warn(const std::string&) {}
};

struct Settings {
    ConflictPolicy conflict;
    int64_t minFreeBytes = int64_t(2) << 30;   // stop recording below this much free space
    int tickSec = 5;
    size_t historyMax = 500;
    int passHorizonSec = 6 * 3600;
    int64_t lastSeen = 0;      // the last tick before the program was closed: runs due since then are 'missed' at the next start
};

class Scheduler {
public:
    explicit Scheduler(Zone z = zoneUtc()) : zone_(std::move(z)) {}
    std::vector<Job> jobs;
    std::vector<RunRecord> history;   // newest last
    Settings settings;
    void setZone(Zone z) { zone_ = std::move(z); }

    // Call about every settings.tickSec. Starts due runs, ends finished ones, applies the conflict and missed-job rules and the disk guard.
    void tick(int64_t now, Host& host);
    // "Run now": starts the job at once for its duration (a pass job: for 10 minutes), over a running lower priority run.
    bool runNow(const std::string& jobId, int64_t now, Host& host);
    void reschedule() { nextDue_.clear(); passEnd_.clear(); nextQuery_.clear(); }   // after a job was edited
    void stopActive(int64_t now, Host& host, Result why = Result::Stopped);
    // the next run of a job (UTC) or -1; for a Pass job the next pass from the host
    int64_t nextRunOf(const Job& j, int64_t now, Host& host) const;
    // the next runs of all enabled jobs inside the horizon, after conflict resolution, for the job list
    std::vector<PlannedRun> upcoming(int64_t now, int64_t horizonSec, Host& host) const;

    bool active() const { return active_; }
    const std::string& activeJobId() const { return cur_.jobId; }
    int64_t activeEnd() const { return curEnd_; }
    const std::string& lastWarning() const { return warning_; }
    size_t queued() const { return pending_.size(); }

private:
    Zone zone_;
    bool active_ = false;
    RunRecord cur_;
    int64_t curEnd_ = 0;
    int curPriority_ = 0;
    RadioState before_;
    int64_t lastTick_ = 0;
    std::map<std::string, int64_t> nextDue_;   // per job: the next start not yet turned into a run
    std::map<std::string, int64_t> passEnd_, nextQuery_;
    int counter_ = 0;
    std::vector<PlannedRun> pending_;          // due, waiting for the active run to end
    std::string warning_;
    double rate_ = 0;

    const Job* find(const std::string& id) const;
    void addRecord(RunRecord r);
    bool begin(const Job& j, int64_t planned, int64_t now, int64_t end, Host& host);
    void finish(int64_t now, Host& host, Result why, const std::string& err);
    void collectDue(int64_t now, Host& host);
};

// ---- storage -----------------------------------------------------------------------------------------------------------------------
std::string toJson(const std::vector<Job>& jobs, const Settings& s);
bool fromJson(const std::string& text, std::vector<Job>& jobs, Settings& s, std::string& err);
std::string historyToJson(const std::vector<RunRecord>& h);
bool historyFromJson(const std::string& text, std::vector<RunRecord>& h, std::string& err);

// a short line for the list: "daily 06:30", "every 2 h", "cron 0 */6 * * *", "ISS above 20 deg"
std::string describe(const Schedule& s);

}  // namespace dect2::sched
