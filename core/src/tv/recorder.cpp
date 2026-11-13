// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/recorder.h"
#include "dect2/tv/tvjson.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <set>

namespace dect2 {

namespace fs = std::filesystem;

// ---- conflicts ---------------------------------------------------------------------------------------------------------------------------------
std::vector<RecConflict> findConflicts(const std::vector<RecJob>& jobs, int tuners) {
    if (tuners < 1) tuners = 1;
    std::vector<const RecJob*> cand;
    for (const auto& j : jobs) if ((j.state == RecJob::Scheduled || j.state == RecJob::Recording) && !j.reminder && !j.series) cand.push_back(&j);
    // a recording that already runs keeps its tuner; then the older request wins
    std::sort(cand.begin(), cand.end(), [](const RecJob* a, const RecJob* b) {
        const int ra = a->state == RecJob::Recording ? 0 : 1, rb = b->state == RecJob::Recording ? 0 : 1;
        return ra != rb ? ra < rb : a->id < b->id;
    });
    std::vector<const RecJob*> accepted;
    std::vector<RecConflict> out;
    for (const RecJob* c : cand) {
        std::set<int64_t> times{c->effStart()};
        for (const RecJob* a : accepted) if (a->effStart() > c->effStart() && a->effStart() < c->effEnd()) times.insert(a->effStart());
        RecConflict bad;
        for (int64_t t : times) {
            std::set<int> keys{c->muxKey};
            std::vector<int> others;
            for (const RecJob* a : accepted) if (a->effStart() <= t && t < a->effEnd()) { keys.insert(a->muxKey); if (a->muxKey != c->muxKey) others.push_back(a->id); }
            if ((int)keys.size() > tuners) { for (int o : others) if (std::find(bad.with.begin(), bad.with.end(), o) == bad.with.end()) bad.with.push_back(o); }
        }
        if (!bad.with.empty()) { bad.id = c->id; out.push_back(bad); }
        else accepted.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const RecConflict& a, const RecConflict& b) { return a.id < b.id; });
    return out;
}

// ---- schedule ----------------------------------------------------------------------------------------------------------------------------------
int RecSchedule::add(RecJob j) {
    std::lock_guard<std::mutex> lk(mu_);
    j.id = nextId_++;
    jobs_.push_back(std::move(j));
    return jobs_.back().id;
}
bool RecSchedule::remove(int id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < jobs_.size(); i++) if (jobs_[i].id == id) { jobs_.erase(jobs_.begin() + i); return true; }
    return false;
}
bool RecSchedule::get(int id, RecJob& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& j : jobs_) if (j.id == id) { out = j; return true; }
    return false;
}
std::vector<RecJob> RecSchedule::list() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<RecJob> r = jobs_;
    std::stable_sort(r.begin(), r.end(), [](const RecJob& a, const RecJob& b) { return a.effStart() < b.effStart(); });
    return r;
}
void RecSchedule::update(int id, const RecJob& j) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& x : jobs_) if (x.id == id) { x = j; x.id = id; return; }
}
bool RecSchedule::setState(int id, RecJob::State s, const std::string& error) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& x : jobs_) if (x.id == id) { x.state = s; if (!error.empty()) x.error = error; return true; }
    return false;
}

int RecSchedule::expandSeries(const EpgDb& db, int64_t now, const std::function<int(const ServiceKey&)>& muxOf) {
    std::vector<RecJob> templates;
    for (auto& j : list()) if (j.series && !j.seriesCrid.empty() && j.state != RecJob::Cancelled) templates.push_back(j);
    int added = 0;
    for (auto& t : templates) {
        for (auto& hit : db.seriesEvents(t.seriesCrid, now)) {
            if (!(hit.key == t.svc)) continue;
            const EpgItem& e = hit.ev;
            bool have = false;
            for (auto& j : list()) {
                if (j.series || j.state == RecJob::Cancelled) continue;
                if (j.svc == hit.key && j.eventId == e.eventId && j.start == e.start) have = true;
                if (!e.crid.empty() && j.crid == e.crid && (j.state == RecJob::Done || j.state == RecJob::Scheduled || j.state == RecJob::Recording)) have = true;   // a repeat of an episode
            }
            if (have) continue;
            RecJob n;
            n.title = e.title; n.serviceName = t.serviceName; n.svc = hit.key;
            n.muxKey = muxOf ? muxOf(hit.key) : t.muxKey;
            n.start = e.start; n.end = e.end(); n.padBefore = t.padBefore; n.padAfter = t.padAfter;
            n.eventId = e.eventId; n.crid = e.crid; n.seriesCrid = e.seriesCrid; n.epg = e;
            add(n);
            added++;
        }
    }
    return added;
}

void RecSchedule::expire(int64_t now, size_t keepFinished) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& j : jobs_) if (j.state == RecJob::Scheduled && !j.series && !j.reminder && now >= j.effEnd()) { j.state = RecJob::Missed; if (j.error.empty()) j.error = "the receiver was not tuned to it"; }
    // finished jobs beyond the newest `keepFinished`
    std::vector<size_t> fin;
    for (size_t i = 0; i < jobs_.size(); i++) if (jobs_[i].state >= RecJob::Done && !jobs_[i].series) fin.push_back(i);
    if (fin.size() > keepFinished) {
        std::sort(fin.begin(), fin.end(), [&](size_t a, size_t b) { return jobs_[a].start < jobs_[b].start; });
        std::set<size_t> drop(fin.begin(), fin.begin() + (fin.size() - keepFinished));
        std::vector<RecJob> keep;
        for (size_t i = 0; i < jobs_.size(); i++) if (!drop.count(i)) keep.push_back(std::move(jobs_[i]));
        jobs_ = std::move(keep);
    }
}

std::string RecSchedule::toJson() const {
    using namespace tvjson;
    Value root = Value::object();
    Value arr = Value::array();
    std::lock_guard<std::mutex> lk(mu_);
    root.set("version", 1).set("next", nextId_);
    for (auto& j : jobs_) {
        Value x = Value::object();
        x.set("id", j.id).set("title", j.title).set("service", j.serviceName);
        x.set("onid", j.svc.onid).set("tsid", j.svc.tsid).set("sid", j.svc.sid).set("mux", j.muxKey);
        x.set("start", j.start).set("end", j.end).set("pre", j.padBefore).set("post", j.padAfter).set("event", j.eventId);
        if (!j.crid.empty()) x.set("crid", j.crid);
        if (!j.seriesCrid.empty()) x.set("scrid", j.seriesCrid);
        if (j.series) x.set("series", true);
        if (j.reminder) x.set("reminder", true);
        x.set("state", (int)j.state);
        if (!j.file.empty()) x.set("file", j.file);
        if (!j.error.empty()) x.set("error", j.error);
        if (j.bytes) x.set("bytes", (int64_t)j.bytes);
        x.set("epg", epgItemToValue(j.epg));
        arr.push(x);
    }
    root.set("jobs", arr);
    return dump(root);
}

bool RecSchedule::fromJson(const std::string& json) {
    using namespace tvjson;
    Value root;
    if (!parse(json, root) || root.type != Value::Obj) return false;
    const Value* arr = root.get("jobs");
    if (!arr || arr->type != Value::Arr) return false;
    std::vector<RecJob> v;
    int maxId = 0;
    for (auto& x : arr->a) {
        if (x.type != Value::Obj) continue;
        RecJob j;
        j.id = (int)x.getInt("id"); j.title = x.getStr("title"); j.serviceName = x.getStr("service");
        j.svc = {(int)x.getInt("onid"), (int)x.getInt("tsid"), (int)x.getInt("sid")};
        j.muxKey = (int)x.getInt("mux"); j.start = x.getInt("start"); j.end = x.getInt("end");
        j.padBefore = (int)std::max<int64_t>(0, std::min<int64_t>(x.getInt("pre"), 6 * 3600));
        j.padAfter = (int)std::max<int64_t>(0, std::min<int64_t>(x.getInt("post"), 6 * 3600));
        j.eventId = (int)x.getInt("event"); j.crid = x.getStr("crid"); j.seriesCrid = x.getStr("scrid");
        j.series = x.getBool("series"); j.reminder = x.getBool("reminder");
        const int st = (int)x.getInt("state");
        j.state = st >= 0 && st <= RecJob::Cancelled ? (RecJob::State)st : RecJob::Scheduled;
        if (j.state == RecJob::Recording) { j.state = RecJob::Failed; j.error = "the application stopped while recording"; }   // it cannot have continued
        j.file = x.getStr("file"); j.error = j.error.empty() ? x.getStr("error") : j.error; j.bytes = (uint64_t)x.getInt("bytes");
        if (const Value* e = x.get("epg")) j.epg = epgItemFromValue(*e);
        if (j.id <= 0 || j.end <= j.start) continue;
        maxId = std::max(maxId, j.id);
        v.push_back(std::move(j));
    }
    std::lock_guard<std::mutex> lk(mu_);
    jobs_ = std::move(v);
    nextId_ = std::max<int>(maxId + 1, (int)root.getInt("next", 1));
    return true;
}

bool RecSchedule::save(const std::string& path) const {
    const std::string tmp = path + ".tmp";
    if (!writeTextFile(tmp, toJson())) return false;
    std::error_code ec;
    fs::rename(u8file(tmp), u8file(path), ec);
    return !ec;
}
bool RecSchedule::load(const std::string& path) {
    FILE* f = u8fopen(path, "rb");
    if (!f) return false;
    std::string j; char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && j.size() < (32u << 20)) j.append(buf, n);
    fclose(f);
    return fromJson(j);
}

// ---- file names --------------------------------------------------------------------------------------------------------------------------------
std::string sanitizeFileName(const std::string& in) {
    const std::string s = u8valid(in);
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || (c < 0x80 && strchr("/\\:*?\"<>|", c))) { if (o.empty() || o.back() != '_') o += '_'; }
        else if (c == ' ' && !o.empty() && o.back() == ' ') continue;
        else o += (char)c;
    }
    while (!o.empty() && (o.back() == ' ' || o.back() == '.' || o.back() == '_')) o.pop_back();
    size_t b = 0;
    while (b < o.size() && (o[b] == ' ' || o[b] == '.')) b++;
    o.erase(0, b);
    if (o.size() > 120) {
        size_t n = 120;
        while (n > 0 && ((unsigned char)o[n] & 0xC0) == 0x80) n--;
        o.resize(n);
        while (!o.empty() && (o.back() == ' ' || o.back() == '.' || o.back() == '_')) o.pop_back();
    }
    static const char* const reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    std::string up = o;
    for (char& c : up) c = (char)toupper((unsigned char)c);
    for (const char* r : reserved) if (up == r) o += "_";
    if (o.empty()) o = "recording";
    return o;
}

std::string expandNameTemplate(const std::string& tmpl, const NameVars& v) {
    const std::string when = formatInZone(v.start + v.utcOffsetSec, nullptr);   // "YYYY-MM-DD hh:mm"
    std::string o;
    for (size_t i = 0; i < tmpl.size();) {
        if (tmpl[i] == '{') {
            const size_t e = tmpl.find('}', i);
            if (e != std::string::npos) {
                const std::string k = tmpl.substr(i + 1, e - i - 1);
                std::string val;
                bool known = true;
                if (k == "title") val = v.title;
                else if (k == "service") val = v.service;
                else if (k == "date") val = when.substr(0, 10);
                else if (k == "time") val = when.substr(11, 2) + when.substr(14, 2);
                else if (k == "year") val = when.substr(0, 4);
                else if (k == "month") val = when.substr(5, 2);
                else if (k == "day") val = when.substr(8, 2);
                else if (k == "id") val = std::to_string(v.id);
                else known = false;
                if (known) { o += val; i = e + 1; continue; }
            }
        }
        o += tmpl[i++];
    }
    return sanitizeFileName(o);
}

std::string uniqueRecordingPath(const std::string& dir, const std::string& base) {
    std::error_code ec;
    std::string p = dir + "/" + base + ".ts";
    for (int n = 2; fs::exists(u8file(p), ec) && n < 10000; n++) p = dir + "/" + base + "_" + std::to_string(n) + ".ts";
    return p;
}

// ---- sidecars and the index --------------------------------------------------------------------------------------------------------------------
static std::string baseName(const std::string& p) { const size_t i = p.find_last_of("/\\"); return i == std::string::npos ? p : p.substr(i + 1); }
static std::string dirName(const std::string& p) { const size_t i = p.find_last_of("/\\"); return i == std::string::npos ? std::string(".") : p.substr(0, i); }

bool writeSidecar(const RecordingEntry& e) {
    using namespace tvjson;
    Value x = Value::object();
    x.set("version", 1).set("title", e.title).set("description", e.description).set("service", e.serviceName).set("status", e.status);
    x.set("onid", e.svc.onid).set("tsid", e.svc.tsid).set("sid", e.svc.sid);
    x.set("start", e.start).set("end", e.end).set("recStart", e.recStart).set("recEnd", e.recEnd);
    x.set("bytes", (int64_t)e.bytes).set("seconds", e.seconds).set("dropped", (int64_t)e.droppedPackets);
    if (!e.error.empty()) x.set("error", e.error);
    Value subs = Value::array();
    for (auto& s : e.subtitleFiles) subs.push(Value::str(baseName(s)));
    x.set("subtitles", subs);
    x.set("epg", epgItemToValue(e.epg));
    const std::string path = e.tsPath + ".json";
    const std::string tmp = path + ".tmp";
    if (!writeTextFile(tmp, dump(x))) return false;
    std::error_code ec;
    fs::rename(u8file(tmp), u8file(path), ec);
    return !ec;
}

bool readSidecar(const std::string& path, RecordingEntry& e) {
    using namespace tvjson;
    FILE* f = u8fopen(path, "rb");
    if (!f) return false;
    std::string j; char buf[16384]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0 && j.size() < (8u << 20)) j.append(buf, n);
    fclose(f);
    Value x;
    if (!parse(j, x) || x.type != Value::Obj) return false;
    e = RecordingEntry();
    e.sidecarPath = path;
    e.tsPath = path.size() > 5 && path.compare(path.size() - 5, 5, ".json") == 0 ? path.substr(0, path.size() - 5) : path;
    e.title = x.getStr("title"); e.description = x.getStr("description"); e.serviceName = x.getStr("service"); e.status = x.getStr("status", "done");
    e.svc = {(int)x.getInt("onid"), (int)x.getInt("tsid"), (int)x.getInt("sid")};
    e.start = x.getInt("start"); e.end = x.getInt("end"); e.recStart = x.getInt("recStart"); e.recEnd = x.getInt("recEnd");
    e.bytes = (uint64_t)x.getInt("bytes"); e.seconds = x.getNum("seconds"); e.droppedPackets = (uint64_t)x.getInt("dropped");
    e.error = x.getStr("error");
    if (const Value* s = x.get("subtitles")) if (s->type == Value::Arr)
        for (auto& v : s->a) if (v.type == Value::Str) {
            const std::string b = baseName(v.s);   // names only: a sidecar never points outside its folder
            if (!b.empty() && b != "." && b != "..") e.subtitleFiles.push_back(dirName(e.tsPath) + "/" + b);
        }
    if (const Value* g = x.get("epg")) e.epg = epgItemFromValue(*g);
    e.hasSidecar = true;
    return true;
}

std::vector<RecordingEntry> listRecordings(const std::string& dir) {
    std::vector<RecordingEntry> r;
    std::error_code ec;
    if (!fs::is_directory(u8file(dir), ec)) return r;
    std::set<std::string> withSidecar;
    for (auto& de : fs::directory_iterator(u8file(dir), ec)) {
        const std::string name = de.path().filename().string();
        if (name.size() > 8 && name.compare(name.size() - 8, 8, ".ts.json") == 0) {
            RecordingEntry e;
            const std::string full = dir + "/" + name;
            if (!readSidecar(full, e)) continue;
            withSidecar.insert(e.tsPath);
            std::error_code e2;
            if (!fs::exists(u8file(e.tsPath), e2)) continue;   // the video was deleted by hand
            const auto sz = fs::file_size(u8file(e.tsPath), e2);
            if (!e2) e.bytes = sz;
            r.push_back(std::move(e));
        }
    }
    for (auto& de : fs::directory_iterator(u8file(dir), ec)) {
        const std::string name = de.path().filename().string();
        if (name.size() <= 3 || name.compare(name.size() - 3, 3, ".ts") != 0) continue;
        const std::string full = dir + "/" + name;
        if (withSidecar.count(full)) continue;
        RecordingEntry e;
        e.tsPath = full;
        e.title = name.substr(0, name.size() - 3);
        e.status = "done";
        std::error_code e2;
        e.bytes = fs::file_size(de.path(), e2);
        const auto ft = fs::last_write_time(de.path(), e2);
        if (!e2) {
            const auto sys = std::chrono::system_clock::now() + std::chrono::duration_cast<std::chrono::system_clock::duration>(ft - fs::file_time_type::clock::now());
            e.recEnd = std::chrono::duration_cast<std::chrono::seconds>(sys.time_since_epoch()).count();
            e.recStart = e.recEnd;
        }
        r.push_back(std::move(e));
    }
    std::sort(r.begin(), r.end(), [](const RecordingEntry& a, const RecordingEntry& b) { return (a.recStart ? a.recStart : a.start) > (b.recStart ? b.recStart : b.start); });
    return r;
}

bool deleteRecording(const RecordingEntry& e) {
    std::error_code ec;
    bool ok = true;
    if (!e.tsPath.empty()) { fs::remove(u8file(e.tsPath), ec); if (ec) ok = false; }
    const std::string sc = e.sidecarPath.empty() ? e.tsPath + ".json" : e.sidecarPath;
    fs::remove(u8file(sc), ec);
    for (auto& s : e.subtitleFiles) {
        if (dirName(s) != dirName(e.tsPath)) continue;
        fs::remove(u8file(s), ec);
    }
    return ok;
}

bool exportRecording(const RecordingEntry& e, const std::string& destDir, std::string* outPath) {
    std::error_code ec;
    fs::create_directories(u8file(destDir), ec);
    const std::string dst = destDir + "/" + baseName(e.tsPath);
    fs::copy_file(u8file(e.tsPath), u8file(dst), fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    for (auto& s : e.subtitleFiles) fs::copy_file(u8file(s), u8file(destDir + "/" + baseName(s)), fs::copy_options::overwrite_existing, ec);
    if (outPath) *outPath = dst;
    return true;
}

// ---- the recorder ------------------------------------------------------------------------------------------------------------------------------
namespace {
double steadyNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int systemUtcOffset(int64_t t) {
    const std::time_t tt = (std::time_t)t;
    std::tm g{};
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    g.tm_isdst = -1;
    return (int)difftime(tt, std::mktime(&g));
}
}

struct Recorder::Session {
    int jobId = 0, muxKey = 0;
    ServiceFilter filter;
    TsWriter writer;
    std::string path;
    RecordingEntry entry;
    double startWall = 0, lastData = 0;
    uint64_t packets = 0;
    std::vector<std::pair<std::string, std::unique_ptr<TeletextSubtitleCollector>>> subs;   // language -> collector
    std::vector<uint8_t> out;
};

Recorder::Recorder() = default;
Recorder::~Recorder() { stopAll(); }

void Recorder::setConfig(const RecorderConfig& c) { std::lock_guard<std::mutex> lk(mu_); cfg_ = c; if (cfg_.tuners < 1) cfg_.tuners = 1; }
RecorderConfig Recorder::config() const { std::lock_guard<std::mutex> lk(mu_); return cfg_; }

uint64_t Recorder::freeBytes() const {
    std::string d;
    { std::lock_guard<std::mutex> lk(mu_); d = cfg_.dir; }
    std::error_code ec;
    if (d.empty()) return UINT64_MAX;
    fs::create_directories(u8file(d), ec);
    const auto sp = fs::space(u8file(d), ec);
    return ec ? UINT64_MAX : (uint64_t)sp.available;
}

void Recorder::feed(const uint8_t* pkts, size_t n, const TsSnapshot& snap, int muxKey) {
    std::lock_guard<std::mutex> lk(mu_);
    curMux_ = muxKey;
    lastFeedWall_ = steadyNow();
    lastSnap_ = snap;
    const double now = steadyNow();
    for (auto& sp : sessions_) {
        Session& s = *sp;
        if (s.muxKey != muxKey && s.muxKey != 0) continue;
        s.lastData = now;
        s.out.clear();
        uint8_t o[188];
        for (size_t i = 0; i < n; i++) {
            const uint8_t* p = pkts + i * 188;
            if (!s.filter.process(p, &snap, o)) continue;
            s.out.insert(s.out.end(), o, o + 188);
            for (auto& sub : s.subs) sub.second->feed(o);
        }
        if (!s.out.empty()) { s.writer.write(s.out.data(), s.out.size()); s.packets += s.out.size() / 188; }
    }
}

void Recorder::startLocked(RecJob& j, int64_t now) {
    std::error_code ec;
    fs::create_directories(u8file(cfg_.dir), ec);
    NameVars nv;
    nv.title = j.title.empty() ? j.serviceName : j.title; nv.service = j.serviceName; nv.start = j.start; nv.id = j.id; nv.utcOffsetSec = systemUtcOffset(j.start);
    auto s = std::make_unique<Session>();
    s->jobId = j.id; s->muxKey = j.muxKey;
    s->path = uniqueRecordingPath(cfg_.dir, expandNameTemplate(cfg_.nameTemplate, nv));
    s->filter.setStrict(true);
    s->filter.select(j.svc.sid);
    if (!s->writer.open(s->path)) {
        sched_.setState(j.id, RecJob::Failed, "cannot write " + s->path);
        j.state = RecJob::Failed;
        return;
    }
    s->startWall = s->lastData = steadyNow();
    RecordingEntry& e = s->entry;
    e.tsPath = s->path; e.title = j.title; e.description = !j.epg.extended.empty() ? j.epg.extended : j.epg.text; e.serviceName = j.serviceName;
    e.svc = j.svc; e.start = j.start; e.end = j.end; e.recStart = now; e.status = "recording"; e.epg = j.epg;
    // teletext subtitle pages of the service, one collector per language
    if (cfg_.saveSubtitles) for (auto& sv : lastSnap_.services) if (sv.id == j.svc.sid) {
        for (auto& st : sv.streams) for (auto& sub : st.subs) {
            if (!sub.teletext || (sub.type != 2 && sub.type != 5) || sub.page < 100 || sub.page > 899) continue;
            auto c = std::make_unique<TeletextSubtitleCollector>();
            c->configure(st.pid, sub.page, sv.pcrPid);
            std::string lang = sub.lang.empty() ? "und" : sub.lang;
            if (sub.type == 5) lang += ".hi";
            s->subs.emplace_back(lang, std::move(c));
        }
    }
    writeSidecar(e);
    sched_.setState(j.id, RecJob::Recording);
    RecJob upd;
    if (sched_.get(j.id, upd)) { upd.file = s->path; sched_.update(j.id, upd); }
    j.state = RecJob::Recording;
    sessions_.push_back(std::move(s));
}

void Recorder::finishLocked(Session& s, RecJob::State st, const std::string& err) {
    s.writer.close();
    s.entry.recEnd = (int64_t)std::time(nullptr);
    s.entry.seconds = steadyNow() - s.startWall;
    s.entry.bytes = s.writer.bytes();
    s.entry.droppedPackets = s.writer.droppedPackets();
    s.entry.error = err;
    s.entry.status = st == RecJob::Done ? "done" : "failed";
    for (auto& sub : s.subs) {
        sub.second->finish();
        const auto cues = sub.second->cues();
        if (cues.empty()) continue;
        const std::string base = s.path.substr(0, s.path.size() - 3) + "." + sub.first;
        if (writeTextFile(base + ".srt", formatSrt(cues))) s.entry.subtitleFiles.push_back(base + ".srt");
        if (writeTextFile(base + ".vtt", formatVtt(cues))) s.entry.subtitleFiles.push_back(base + ".vtt");
    }
    writeSidecar(s.entry);
    RecJob j;
    if (sched_.get(s.jobId, j)) {
        j.state = st; j.bytes = s.entry.bytes; j.error = err; j.file = s.path;
        sched_.update(s.jobId, j);
    }
}

bool Recorder::tick(int64_t now) {
    std::lock_guard<std::mutex> lk(mu_);
    bool changed = false;
    const double wall = steadyNow();
    // running sessions
    for (size_t i = 0; i < sessions_.size();) {
        Session& s = *sessions_[i];
        RecJob j;
        const bool have = sched_.get(s.jobId, j);
        RecJob::State st = RecJob::Recording;
        std::string err;
        if (!have || j.state == RecJob::Cancelled) { st = RecJob::Cancelled; }
        else if (now >= j.effEnd()) st = RecJob::Done;
        else if (s.writer.failed()) { st = RecJob::Failed; err = "writing to the disk failed"; }
        else if (cfg_.lossStopSec > 0 && wall - s.lastData > cfg_.lossStopSec) { st = RecJob::Failed; err = "signal lost"; }
        else {
            std::error_code ec;
            const auto sp = fs::space(u8file(cfg_.dir), ec);
            if (!ec && sp.available < cfg_.minFreeBytes) { st = RecJob::Failed; err = "the disk is nearly full"; }
        }
        if (st != RecJob::Recording) {
            finishLocked(s, st == RecJob::Cancelled ? RecJob::Done : st, err);
            sessions_.erase(sessions_.begin() + i);
            changed = true;
        } else i++;
    }
    sched_.expire(now);
    // start jobs whose window is open
    const std::vector<RecConflict> conf = findConflicts(sched_.list(), cfg_.tuners);
    for (auto& j : sched_.list()) {
        if (j.state != RecJob::Scheduled || j.reminder || j.series) continue;
        if (now < j.effStart() || now >= j.effEnd()) continue;
        if (std::any_of(conf.begin(), conf.end(), [&](const RecConflict& c) { return c.id == j.id; })) {
            sched_.setState(j.id, RecJob::Failed, "not enough tuners: another multiplex is being recorded at the same time");
            changed = true;
            continue;
        }
        const bool tuned = (j.muxKey == 0 || curMux_ == j.muxKey) && wall - lastFeedWall_ < 5.0;
        if (!tuned) continue;
        bool known = false;
        for (auto& sv : lastSnap_.services) if (sv.id == j.svc.sid && sv.havePmt) known = true;
        if (!known) continue;
        std::error_code ec;
        fs::create_directories(u8file(cfg_.dir), ec);
        const auto sp = fs::space(u8file(cfg_.dir), ec);
        if (!ec && sp.available < cfg_.minFreeBytes) { sched_.setState(j.id, RecJob::Failed, "not enough free disk space"); changed = true; continue; }
        RecJob copy = j;
        startLocked(copy, now);
        changed = true;
    }
    return changed;
}

std::vector<RecJob> Recorder::dueReminders(int64_t now, int lead) {
    std::vector<RecJob> r;
    for (auto& j : sched_.list()) if (j.reminder && j.state == RecJob::Scheduled && now >= j.start - lead) {
        r.push_back(j);
        sched_.setState(j.id, now >= j.end ? RecJob::Missed : RecJob::Done);
    }
    return r;
}

int Recorder::wantedMux(int64_t now) const {
    std::lock_guard<std::mutex> lk(mu_);
    const auto conf = findConflicts(sched_.list(), cfg_.tuners);
    for (auto& j : sched_.list()) {
        if (j.state != RecJob::Scheduled || j.reminder || j.series || j.muxKey == 0) continue;
        if (now < j.effStart() - 30 || now >= j.effEnd()) continue;
        if (std::any_of(conf.begin(), conf.end(), [&](const RecConflict& c) { return c.id == j.id; })) continue;
        return j.muxKey;
    }
    return 0;
}

std::vector<RecSessionInfo> Recorder::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<RecSessionInfo> r;
    for (auto& s : sessions_) r.push_back({s->jobId, s->path, s->writer.bytes(), s->writer.droppedPackets(), steadyNow() - s->startWall, s->writer.failed()});
    return r;
}

void Recorder::stopJob(int id, bool cancel) {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < sessions_.size(); i++) if (sessions_[i]->jobId == id) {
        finishLocked(*sessions_[i], cancel ? RecJob::Cancelled : RecJob::Done, "");
        sessions_.erase(sessions_.begin() + i);
        return;
    }
}

void Recorder::stopAll() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& s : sessions_) finishLocked(*s, RecJob::Done, "");
    sessions_.clear();
}

} // namespace dect2
