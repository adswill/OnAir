// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The TV recorder: a schedule of recordings (from the programme guide or by hand), conflict detection, file naming, the recordings index (a JSON
// sidecar next to every .ts file) and the engine that records several services of one multiplex at the same time (each service is a PID filter
// over the same packet stream, one file per service).
//
// The recorder only runs while the application is running and the receiver is tuned to the multiplex of the job: a computer that is asleep records
// nothing. Encrypted (scrambled) services are recorded as they arrive, never decoded.
#pragma once
#include "dect2/tv/epg.h"
#include "dect2/tv/subtitle_export.h"
#include "dect2/tv/ts.h"
#include "dect2/tv/ts_record.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct RecJob {
    enum State { Scheduled = 0, Recording = 1, Done = 2, Failed = 3, Missed = 4, Cancelled = 5 };
    int id = 0;
    std::string title, serviceName;
    ServiceKey svc;
    int muxKey = 0;                   // what the receiver must be tuned to (frequency in kHz); jobs with the same key share one tuner
    int64_t start = 0, end = 0;       // the programme (UTC seconds); a manual recording gives its own start / end
    int padBefore = 0, padAfter = 0;  // seconds recorded before / after
    int eventId = 0;
    std::string crid, seriesCrid;
    bool series = false;              // a standing request: every future event with seriesCrid gets a job of its own
    bool reminder = false;            // no recording: the application only tells the user the programme starts
    State state = Scheduled;
    std::string file, error;
    uint64_t bytes = 0;
    EpgItem epg;                      // the guide entry at the time it was scheduled (goes into the sidecar)
    int64_t effStart() const { return start - padBefore; }
    int64_t effEnd() const { return end + padAfter; }
};

// A job that cannot be recorded because more multiplexes are needed at once than there are tuners. `with`: the jobs it collides with.
struct RecConflict { int id = 0; std::vector<int> with; };
// Jobs are served in id order (the older request wins). Services of the same muxKey never collide with each other.
std::vector<RecConflict> findConflicts(const std::vector<RecJob>& jobs, int tuners);

class RecSchedule {
public:
    int add(RecJob j);                           // returns the new id
    bool remove(int id);                         // also cancels
    bool get(int id, RecJob& out) const;
    std::vector<RecJob> list() const;            // by start time
    void update(int id, const RecJob& j);
    bool setState(int id, RecJob::State s, const std::string& error = "");
    // Standing series requests: adds a job for every guide event whose series CRID matches and that is not scheduled yet. Returns how many.
    // Only events of the service the request was made on are used; an episode (programme CRID) that has a job already is skipped (repeats).
    int expandSeries(const EpgDb& db, int64_t now, const std::function<int(const ServiceKey&)>& muxOf);
    // Jobs whose window ended without a start become Missed; old finished jobs beyond `keep` are dropped.
    void expire(int64_t now, size_t keepFinished = 200);
    std::string toJson() const;
    bool fromJson(const std::string& json);
    bool save(const std::string& path) const;
    bool load(const std::string& path);
private:
    mutable std::mutex mu_;
    std::vector<RecJob> jobs_;
    int nextId_ = 1;
};

// ---- file names ------------------------------------------------------------------------------------------------------------------------------
struct NameVars { std::string title, service; int64_t start = 0; int id = 0; int utcOffsetSec = 0; };
// Template fields: {title} {service} {date} (YYYY-MM-DD) {time} (HHMM) {year} {month} {day} {id}; anything else is kept as text. The result is a
// file name without extension that is safe on Windows, macOS and Linux (no path separators or reserved characters, at most 120 bytes, valid UTF-8).
std::string expandNameTemplate(const std::string& tmpl, const NameVars& v);
std::string sanitizeFileName(const std::string& s);
// dir/base.ts, or dir/base_2.ts ... when the name is taken.
std::string uniqueRecordingPath(const std::string& dir, const std::string& base);

// ---- recordings index ------------------------------------------------------------------------------------------------------------------------
struct RecordingEntry {
    std::string tsPath, sidecarPath;
    std::string title, description, serviceName, status;     // status: recording done failed
    int64_t start = 0, end = 0;                              // the programme
    int64_t recStart = 0, recEnd = 0;                        // what was actually recorded
    uint64_t bytes = 0;
    double seconds = 0;
    uint64_t droppedPackets = 0;
    std::string error;
    std::vector<std::string> subtitleFiles;                  // sidecar .srt / .vtt
    bool hasSidecar = false;
    EpgItem epg;
    ServiceKey svc;
};
bool writeSidecar(const RecordingEntry& e);                          // <tsPath>.json
bool readSidecar(const std::string& sidecarPath, RecordingEntry& e);
// Every recording of a folder (newest first): by sidecar, plus .ts files that have none (listed with the file's size and time).
std::vector<RecordingEntry> listRecordings(const std::string& dir);
bool deleteRecording(const RecordingEntry& e);                       // the .ts, the sidecar and the subtitle files
bool exportRecording(const RecordingEntry& e, const std::string& destDir, std::string* outPath = nullptr);   // copies the .ts and its subtitle files

// ---- the recorder ----------------------------------------------------------------------------------------------------------------------------
struct RecorderConfig {
    std::string dir;
    std::string nameTemplate = "{title}_{date}_{time}";
    int tuners = 1;
    int padBefore = 60, padAfter = 300;          // defaults for new jobs
    uint64_t minFreeBytes = 2ull << 30;          // do not start, and stop, when less than this is free on the disk
    double lossStopSec = 60;                     // stop (job fails) after this long without packets of the multiplex; 0 = never
    bool saveSubtitles = true;                   // teletext subtitles to .srt next to the recording
};

struct RecSessionInfo { int jobId = 0; std::string file; uint64_t bytes = 0, dropped = 0; double seconds = 0; bool failed = false; };

class Recorder {
public:
    Recorder();
    ~Recorder();
    void setConfig(const RecorderConfig& c);
    RecorderConfig config() const;
    RecSchedule& schedule() { return sched_; }
    EpgDb* epg() const { return epg_; }
    void setEpg(EpgDb* e) { epg_ = e; }

    // Packets of the multiplex the receiver is tuned to (whole 188 byte packets). `snap` carries the PIDs of the services.
    void feed(const uint8_t* pkts, size_t nPackets, const TsSnapshot& snap, int muxKey);
    // About once a second: starts jobs whose window is open on the tuned multiplex, finishes jobs whose window closed, checks disk and signal.
    // Returns true when something started, stopped or failed (the UI refreshes its lists).
    bool tick(int64_t nowUtc);
    // Reminders (jobs with reminder set) whose programme starts within `leadSec`: returned once, then marked Done.
    std::vector<RecJob> dueReminders(int64_t nowUtc, int leadSec = 60);
    // The multiplex the receiver should be tuned to now (a job's window is open there and nothing is recording from it), 0 = none needed.
    int wantedMux(int64_t nowUtc) const;
    std::vector<RecSessionInfo> active() const;
    void stopJob(int id, bool cancel);           // finish a running job now
    void stopAll();
    uint64_t freeBytes() const;                  // on the recordings disk, UINT64_MAX when unknown
    std::vector<RecConflict> conflicts() const { return findConflicts(sched_.list(), cfg_.tuners); }

private:
    struct Session;
    void startLocked(RecJob& j, int64_t now);
    void finishLocked(Session& s, RecJob::State st, const std::string& err);
    mutable std::mutex mu_;
    RecorderConfig cfg_;
    RecSchedule sched_;
    EpgDb* epg_ = nullptr;
    std::vector<std::unique_ptr<Session>> sessions_;
    int curMux_ = 0;
    double lastFeedWall_ = 0;
    TsSnapshot lastSnap_;
};

} // namespace dect2
