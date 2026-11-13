// The TV recorder in the interface: the programme guide grid (Guide tab), scheduled recordings and the recordings list (Recordings tab), and the
// timeshift / playback bar of the Player tab. The work is in core/src/tv (epg, recorder, timeshift, playback); this file keeps the state, feeds it from
// the engine's packet tap and draws it.
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "i18n/i18n.h"
#include "dect2/net/remote_api.h"   // jsonEscape
#include "dect2/tv/epg.h"
#include "dect2/tv/playback.h"
#include "dect2/tv/recorder.h"
#include "dect2/tv/timeshift.h"
#include "dect2/common/u8file.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <thread>

using namespace dect2;

namespace {

enum PlayMode { PmNone = 0, PmShift = 1, PmFile = 2 };

struct TvRec {
    EpgDb epg;
    EpgCollector col{&epg};
    Recorder rec;
    DiskTimeshift shift;
    TsFileSource file;
    PlaybackFeeder feeder;
    App* app = nullptr;
    bool init = false, schedDirty = false;
    std::atomic<int> muxKey{0};
    std::string dir;                       // <data>/tv
    // settings (prefs)
    RecorderConfig cfg;
    bool autoTune = true, shiftOn = false;
    int shiftMin = 30, minFreeGb = 2;
    // timeshift tap
    ServiceFilter shiftFilter;
    int shiftSid = -2;
    // playback
    std::atomic<int> mode{PmNone};
    std::mutex pmu;
    TsDemux pdemux;
    TsSnapshot psnap;
    double psnapT = 0;
    bool pSel = false;
    int modeSid = -1, prevSid = -1;
    std::string playName;
    std::vector<SubCue> playCues;          // the sidecar subtitles of the recording being played
    std::atomic<bool> loading{false};
    std::thread loader;
    // known multiplexes: (onid, tsid) -> frequency in kHz
    std::map<std::pair<int, int>, int> muxOf;
    // timing
    double lastSec = 0, lastSave = 0, lastSeries = 0, lastTune = -1000, lastRecs = -1000;
    // guide UI
    ServiceKey selKey;
    int selEvent = -1;
    char search[96] = "";
    int64_t viewStart = 0;
    float pph = 200;                       // pixels per hour
    bool scrollNow = true, onlyThisMux = false;
    int padPre = 60, padPost = 300;
    std::vector<ServiceKey> order;
    std::map<ServiceKey, std::vector<EpgItem>> rows;
    double rowsT = -10;
    // recordings UI
    std::vector<RecordingEntry> recs;
    std::string msg;
    double msgT = 0;
    std::string confirmDelete;
    char manTitle[96] = "";
    int manSvc = 0, manMin = 60, manH = 20, manM = 0;
    char dirBuf[512] = "", tplBuf[128] = "";
};

TvRec& S() { static TvRec* s = new TvRec; return *s; }   // lives to the end of the process; tvRecShutdown() stops its threads

int64_t nowUtc() { return S().col.utcNow(); }

// time in the broadcaster's zone when the TOT told it, else this computer's
std::string tvTime(int64_t utc, const char* what /* "time" "date" "both" */) {
    const auto z = S().col.zones();
    std::string s;
    if (!z.empty()) s = formatInZone(utc, &z[0]); else s = fmtLocal(utc, "%Y-%m-%d %H:%M");
    if (!strcmp(what, "time")) return s.substr(11, 5);
    if (!strcmp(what, "date")) return s.substr(0, 10);
    return s;
}

const char* genreName(int g) {
    static const char* n[] = {"", "Movie / drama", "News / current affairs", "Show / game show", "Sports", "Children's / youth", "Music / ballet / dance", "Arts / culture", "Social / political", "Education / science", "Leisure / hobbies", "Special characteristics"};
    return g >= 1 && g <= 11 ? n[g] : "";
}
ImU32 genreColor(int g, bool bright) {
    static const ImU32 c[] = {IM_COL32(70, 76, 86, 255), IM_COL32(150, 62, 62, 255), IM_COL32(50, 96, 150, 255), IM_COL32(120, 80, 150, 255), IM_COL32(52, 128, 76, 255), IM_COL32(160, 140, 50, 255),
                              IM_COL32(150, 80, 120, 255), IM_COL32(170, 108, 50, 255), IM_COL32(60, 130, 130, 255), IM_COL32(60, 110, 160, 255), IM_COL32(120, 100, 70, 255), IM_COL32(100, 100, 110, 255)};
    ImU32 v = c[g >= 1 && g <= 11 ? g : 0];
    if (bright) { ImVec4 f = ImGui::ColorConvertU32ToFloat4(v); f.x = std::min(1.f, f.x * 1.25f + .06f); f.y = std::min(1.f, f.y * 1.25f + .06f); f.z = std::min(1.f, f.z * 1.25f + .06f); v = ImGui::ColorConvertFloat4ToU32(f); }
    return v;
}

std::string serviceLabel(App& a, const ServiceKey& k) {
    for (const auto& sv : a.ts.services) if (sv.id == k.sid && (a.ts.tsid == 0 || a.ts.tsid == k.tsid) && !sv.name.empty()) return sv.name;
    const std::string n = S().col.serviceName(k);
    if (!n.empty()) return n;
    return "Service " + std::to_string(k.sid);
}

bool sameMux(App& a, const ServiceKey& k) { return a.ts.tsid != 0 && a.ts.tsid == k.tsid && a.ts.onid == k.onid; }

int muxFor(App& a, const ServiceKey& k) {
    if (sameMux(a, k)) return S().muxKey;
    auto it = S().muxOf.find({k.onid, k.tsid});
    return it == S().muxOf.end() ? 0 : it->second;
}

// ---- persistence ------------------------------------------------------------------------------------------------------------------------------
void saveMuxMap() {
    tvjson::Value v = tvjson::Value::array();
    for (auto& kv : S().muxOf) { tvjson::Value x = tvjson::Value::object(); x.set("onid", kv.first.first).set("tsid", kv.first.second).set("khz", kv.second); v.push(x); }
    writeTextFile(S().dir + "/muxes.json", tvjson::dump(v));
}
void loadMuxMap() {
    FILE* f = u8fopen(S().dir + "/muxes.json", "rb");
    if (!f) return;
    std::string j; char b[4096]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0 && j.size() < (1u << 20)) j.append(b, n);
    fclose(f);
    tvjson::Value v;
    if (!tvjson::parse(j, v) || v.type != tvjson::Value::Arr) return;
    for (auto& x : v.a) if (x.type == tvjson::Value::Obj) S().muxOf[{(int)x.getInt("onid"), (int)x.getInt("tsid")}] = (int)x.getInt("khz");
}

void applyConfig() {
    TvRec& s = S();
    s.cfg.minFreeBytes = (uint64_t)std::max(0, s.minFreeGb) << 30;
    s.cfg.padBefore = s.padPre; s.cfg.padAfter = s.padPost;
    s.rec.setConfig(s.cfg);
    TimeshiftConfig tc;
    tc.dir = plat::dataDir() + "/timeshift";
    tc.maxSeconds = s.shiftMin * 60.0;
    if (s.shift.isOpen()) s.shift.setLimits(tc.maxSeconds, 0);
}

void savePrefs() {
    TvRec& s = S();
    auto& p = plat::prefs();
    p.setS("tvrec_dir", s.cfg.dir); p.setS("tvrec_tpl", s.cfg.nameTemplate);
    p.setI("tvrec_pre", s.padPre); p.setI("tvrec_post", s.padPost); p.setI("tvrec_tuners", s.cfg.tuners); p.setI("tvrec_minfree", s.minFreeGb);
    p.setD("tvrec_loss", s.cfg.lossStopSec); p.setB("tvrec_subs", s.cfg.saveSubtitles); p.setB("tvrec_autotune", s.autoTune);
    p.setB("tvrec_shift", s.shiftOn); p.setI("tvrec_shiftmin", s.shiftMin);
}

void tap(const uint8_t* p, size_t n, const TsSnapshot* snap, double secs) {
    TvRec& s = *&S();
    if (!snap) return;
    s.col.advance(secs);
    for (size_t i = 0; i < n; i++) s.col.feed(p + i * 188);
    s.rec.feed(p, n, *snap, s.muxKey);
    if (s.shiftOn && s.mode != PmFile && s.shift.isOpen() && s.app) {
        const int sid = s.app->engine.player().selected();
        if (sid != s.shiftSid) {
            if (s.mode == PmNone || sid >= 0) { s.shift.clear(); s.shiftFilter.setStrict(true); s.shiftFilter.select(sid); s.shiftSid = sid; }
        }
        if (sid >= 0) {
            uint8_t out[188];
            std::vector<uint8_t> buf;
            for (size_t i = 0; i < n; i++) if (s.shiftFilter.process(p + i * 188, snap, out)) buf.insert(buf.end(), out, out + 188);
            if (!buf.empty()) s.shift.push(buf.data(), buf.size() / 188);
        }
    }
}

void init(App& a) {
    TvRec& s = S();
    if (s.init) return;
    s.init = true;
    s.app = &a;
    s.dir = plat::dataDir() + "/tv";
    std::error_code ec;
    std::filesystem::create_directories(u8file(s.dir), ec);
    auto& p = plat::prefs();
    s.cfg.dir = p.getS("tvrec_dir", recordingsDir());
    s.cfg.nameTemplate = p.getS("tvrec_tpl", "{title}_{date}_{time}");
    s.padPre = (int)p.getI("tvrec_pre", 60); s.padPost = (int)p.getI("tvrec_post", 300);
    s.cfg.tuners = (int)std::max(1L, p.getI("tvrec_tuners", 1)); s.minFreeGb = (int)p.getI("tvrec_minfree", 2);
    s.cfg.lossStopSec = p.getD("tvrec_loss", 60); s.cfg.saveSubtitles = p.getB("tvrec_subs", true);
    s.autoTune = p.getB("tvrec_autotune", true); s.shiftOn = p.getB("tvrec_shift", false); s.shiftMin = (int)std::max(1L, std::min(240L, p.getI("tvrec_shiftmin", 30)));
    snprintf(s.dirBuf, sizeof s.dirBuf, "%s", s.cfg.dir.c_str());
    snprintf(s.tplBuf, sizeof s.tplBuf, "%s", s.cfg.nameTemplate.c_str());
    s.epg.load(s.dir + "/epg.json");
    s.epg.prune(nowUtc());
    s.rec.schedule().load(s.dir + "/schedule.json");
    s.rec.setEpg(&s.epg);
    loadMuxMap();
    applyConfig();
    a.engine.setRecorderTap(tap);
}

void stopPlayback(App& a, bool backToLive);

void startShiftBuffer() {
    TvRec& s = S();
    if (s.shiftOn && !s.shift.isOpen()) {
        TimeshiftConfig tc; tc.dir = plat::dataDir() + "/timeshift"; tc.maxSeconds = s.shiftMin * 60.0;
        s.shift.open(tc);
        s.shiftSid = -2;
    } else if (!s.shiftOn && s.shift.isOpen()) s.shift.close();
}

// the packets of a playing recording / the timeshift buffer go to the player like live ones
void playSink(const uint8_t* p, size_t n) {
    TvRec& s = S();
    if (!s.app) return;
    std::lock_guard<std::mutex> lk(s.pmu);
    for (size_t i = 0; i < n; i++) s.pdemux.feed(p + i * 188);
    s.app->engine.feedPlaybackSubtitles(p, n);
    const double t = glfwGetTime();
    bool haveSvc = false;
    for (auto& sv : s.psnap.services) if (sv.havePmt) haveSvc = true;
    if (!haveSvc || t - s.psnapT > 1.0) { s.psnap = s.pdemux.snapshot(); s.psnapT = t; }
    Player& pl = s.app->engine.player();
    if (!s.pSel) {
        for (auto& sv : s.psnap.services) if (sv.havePmt) { pl.select(sv.id); pl.setVolume(s.app->volume); s.pSel = true; break; }
    }
    if (pl.selected() >= 0) pl.push(p, n, s.psnap);
}

// forget what the decoders hold (after a jump) so the picture restarts cleanly
void resetPlayerLocked() {
    TvRec& s = S();
    std::lock_guard<std::mutex> lk(s.pmu);
    s.app->engine.player().select(-1);
    s.app->engine.teletext().reset();
    s.pSel = false;
}

void beginExternal(App& a, int mode, const std::string& name) {
    TvRec& s = S();
    s.prevSid = a.engine.player().selected();
    s.mode = mode; s.playName = name;
    { std::lock_guard<std::mutex> lk(s.pmu); s.pdemux.reset(); s.psnap = TsSnapshot(); s.psnapT = 0; }
    a.engine.setPlayerExternal(true);
}

void stopPlayback(App& a, bool backToLive) {
    TvRec& s = S();
    if (s.mode == PmNone) return;
    s.feeder.stop();
    a.engine.setPlayerExternal(false);
    const int prev = s.prevSid;
    const int was = s.mode;
    s.mode = PmNone;
    s.file.close();
    s.playCues.clear();
    resetPlayerLocked();
    a.engine.teletext().setPid(-1);
    if (backToLive && prev >= 0 && was != PmNone) a.playReq = prev;
    s.shiftSid = -2;
}

bool shiftPause(App& a, bool paused) {
    TvRec& s = S();
    Player& pl = a.engine.player();
    if (s.mode == PmFile) { s.feeder.pause(paused); return true; }
    if (s.mode == PmShift) { s.feeder.pause(paused); return true; }
    if (!paused || !s.shiftOn || !s.shift.isOpen() || pl.selected() < 0) return false;
    beginExternal(a, PmShift, "");
    s.modeSid = s.prevSid;
    s.pSel = true;   // the player keeps decoding the live service it already has; the demux learns the PIDs from the buffer
    s.feeder.start(&s.shift, s.shift.end(), playSink);
    s.feeder.pause(true);
    return true;
}

void startFile(App& a, const RecordingEntry& e) {
    TvRec& s = S();
    if (s.loading) return;
    stopPlayback(a, false);
    s.loading = true;
    if (s.loader.joinable()) s.loader.join();
    const std::string path = e.tsPath;
    const std::string title = e.title.empty() ? path : e.title;
    s.loader = dect2::namedThread("onair-tvload", [path, title, &a] {
        TvRec& t = S();
        if (t.file.open(path)) {
            t.playCues.clear();
            beginExternal(a, PmFile, title);
            t.modeSid = -1;
            a.engine.player().select(-1);
            t.pSel = false;
            a.engine.teletext().reset();
            if (t.file.t2mi()) { char b[96]; snprintf(b, sizeof b, "playback: T2-MI on PID 0x%X, playing PLP %d of %zu", t.file.t2miPid(), t.file.t2miPlp(), t.file.t2miPlps().size()); a.engine.log(b); }
            t.feeder.start(&t.file, 0, playSink);
        } else t.msg = "cannot open " + path;
        t.loading = false;
    });
}

// a T2-MI recording: play another PLP (the file source rebuilds its transport stream, one pass over the recording)
void switchT2miPlp(int plp) {
    TvRec& s = S();
    if (s.loading) return;
    s.loading = true;
    if (s.loader.joinable()) s.loader.join();
    s.loader = dect2::namedThread("onair-tvload", [plp] {
        TvRec& t = S();
        t.feeder.stop();
        if (!t.file.selectT2miPlp(plp)) t.msg = "cannot rebuild PLP " + std::to_string(plp);
        { std::lock_guard<std::mutex> lk(t.pmu); t.pdemux.reset(); t.psnap = TsSnapshot(); t.psnapT = 0; }
        resetPlayerLocked();
        t.feeder.start(&t.file, 0, playSink);
        t.loading = false;
    });
}

void tick(App& a) {
    TvRec& s = S();
    init(a);
    s.muxKey = a.engine.running() ? (int)std::llround(a.freqMhz * 1000.0) : 0;
    startShiftBuffer();
    const double t = glfwGetTime();
    if (a.ts.tsid != 0 && s.muxKey && !a.engine.playerExternal()) {
        auto& slot = s.muxOf[{a.ts.onid, a.ts.tsid}];
        if (slot != s.muxKey) { slot = s.muxKey; saveMuxMap(); }
    }
    if (t - s.lastSec >= 1.0) {
        s.lastSec = t;
        const int64_t now = nowUtc();
        if (s.rec.tick(now)) { s.schedDirty = true; s.lastRecs = -1000; }
        for (auto& r : s.rec.dueReminders(now)) { s.msg = "Starting: " + r.title + " (" + r.serviceName + ")"; s.msgT = t; a.engine.log("reminder: " + r.title + " starts on " + r.serviceName); s.schedDirty = true; }
        const int wanted = s.rec.wantedMux(now);
        if (wanted && wanted != s.muxKey && s.autoTune && t - s.lastTune > 45 && s.rec.active().empty() && s.mode == PmNone) {
            for (auto& c : a.channels) if ((int)std::llround(c.freqMhz * 1000.0) == wanted) { a.engine.log("recorder: tuning to " + std::to_string(c.freqMhz) + " MHz for a scheduled recording"); tuneToChannel(a, c); s.lastTune = t; break; }
        }
        if (t - s.lastSeries > 60) {
            s.lastSeries = t;
            if (s.rec.schedule().expandSeries(s.epg, now, [&](const ServiceKey& k) { return muxFor(a, k); })) s.schedDirty = true;
            s.epg.prune(now);
        }
        if (s.mode == PmShift) {   // the user picked another service: leave the buffer
            const int sel = a.engine.player().selected();
            if (sel >= 0 && s.pSel && s.modeSid >= 0 && sel != s.modeSid) stopPlayback(a, false);
        }
        if (s.mode == PmFile && !s.loading && s.feeder.atEnd()) { /* the bar shows "end"; Stop returns to live */ }
    }
    if (s.schedDirty && t - s.lastSave > 5) { s.rec.schedule().save(s.dir + "/schedule.json"); s.schedDirty = false; s.lastSave = t; }
    static double epgSave = 0;
    if (t - epgSave > 300 && s.epg.count() > 0) { epgSave = t; s.epg.save(s.dir + "/epg.json"); }
}

// ---- scheduling actions -------------------------------------------------------------------------------------------------------------------------
const RecJob* findJob(const std::vector<RecJob>& jobs, const ServiceKey& k, const EpgItem& e, bool series) {
    for (auto& j : jobs) {
        if (j.state == RecJob::Cancelled) continue;
        if (series ? (j.series && !e.seriesCrid.empty() && j.seriesCrid == e.seriesCrid && j.svc == k) : (!j.series && j.svc == k && j.eventId == e.eventId && j.start == e.start)) return &j;
    }
    return nullptr;
}

void scheduleEvent(App& a, const ServiceKey& k, const EpgItem& e, bool series, bool reminder) {
    TvRec& s = S();
    RecJob j;
    j.title = e.title; j.serviceName = serviceLabel(a, k); j.svc = k; j.muxKey = muxFor(a, k);
    j.start = e.start; j.end = e.end(); j.padBefore = reminder ? 0 : s.padPre; j.padAfter = reminder ? 0 : s.padPost;
    j.eventId = e.eventId; j.crid = e.crid; j.seriesCrid = e.seriesCrid; j.epg = e; j.reminder = reminder; j.series = series;
    if (!reminder && j.muxKey == 0) { s.msg = "This service is on a multiplex the receiver has not been tuned to yet: tune to it once so it can be found again."; s.msgT = glfwGetTime(); return; }
    s.rec.schedule().add(j);
    if (series) {   // the standing request also records the programme itself
        RecJob one = j; one.series = false;
        s.rec.schedule().add(one);
    }
    s.schedDirty = true;
}

// ---- guide --------------------------------------------------------------------------------------------------------------------------------------
void refreshRows(App& a, int64_t from, int64_t to) {
    TvRec& s = S();
    const double t = glfwGetTime();
    if (t - s.rowsT < 0.5 && !s.rows.empty()) return;
    s.rowsT = t;
    s.rows.clear();
    s.order = s.epg.services();
    std::vector<ServiceKey> keep;
    for (auto& k : s.order) {
        if (s.onlyThisMux && !sameMux(a, k)) continue;
        auto ev = s.epg.eventsBetween(k, from, to);
        if (ev.empty()) continue;
        s.rows[k] = std::move(ev);
        keep.push_back(k);
    }
    auto lcn = [&](const ServiceKey& k) { if (sameMux(a, k)) for (auto& sv : a.ts.services) if (sv.id == k.sid && sv.lcn > 0) return sv.lcn; return 100000 + k.sid; };
    std::stable_sort(keep.begin(), keep.end(), [&](const ServiceKey& x, const ServiceKey& y) {
        const bool mx = sameMux(a, x), my = sameMux(a, y);
        if (mx != my) return mx;
        if (mx) return lcn(x) < lcn(y);
        return x < y;
    });
    s.order = keep;
}

void detailsPane(App& a) {
    TvRec& s = S();
    EpgItem e;
    if (s.selEvent < 0 || !s.epg.find(s.selKey, s.selEvent, e)) { ImGui::TextDisabled("%s", tr("Pick a programme to see its details.")); return; }
    ImGui::TextColored(pal::heading(), "%s", e.title.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s   %s - %s   %d min", serviceLabel(a, s.selKey).c_str(), tvTime(e.start, "both").c_str(), tvTime(e.end(), "time").c_str(), (e.duration + 30) / 60);
    std::string meta;
    if (e.genre() > 0) meta += std::string(genreName(e.genre()));
    if (e.rating) meta += (meta.empty() ? "" : "   ") + std::string("age ") + std::to_string(e.rating) + "+";
    if (!e.lang.empty()) meta += (meta.empty() ? "" : "   ") + e.lang;
    if (e.freeCa) meta += (meta.empty() ? "" : "   ") + std::string("CA flag set");   // free_CA_mode: a hint only, free services carry it too
    if (!meta.empty()) ImGui::TextDisabled("%s", meta.c_str());
    // buttons
    const std::vector<RecJob> jobs = s.rec.schedule().list();
    const RecJob* one = findJob(jobs, s.selKey, e, false);
    const RecJob* ser = findJob(jobs, s.selKey, e, true);
    const bool ended = e.end() < nowUtc();
    ImGui::BeginDisabled(ended);
    if (one && !one->reminder) {
        if (ImGui::Button(tr("Cancel recording"))) { s.rec.stopJob(one->id, true); s.rec.schedule().remove(one->id); s.schedDirty = true; }
    } else if (ImGui::Button(tr("Record"))) scheduleEvent(a, s.selKey, e, false, false);
    ImGui::SameLine();
    if (ser) { if (ImGui::Button(tr("Stop series"))) { s.rec.schedule().remove(ser->id); s.schedDirty = true; } }
    else {
        ImGui::BeginDisabled(e.seriesCrid.empty());
        if (ImGui::Button(tr("Record series"))) scheduleEvent(a, s.selKey, e, true, false);
        ImGui::EndDisabled();
        if (e.seriesCrid.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("The broadcaster sends no series identifier for this programme."));
    }
    ImGui::SameLine();
    const RecJob* rem = nullptr;
    for (auto& j : jobs) if (j.reminder && j.svc == s.selKey && j.eventId == e.eventId && j.state == RecJob::Scheduled) rem = &j;
    if (rem) { if (ImGui::Button(tr("No reminder"))) { s.rec.schedule().remove(rem->id); s.schedDirty = true; } }
    else if (ImGui::Button(tr("Remind me"))) scheduleEvent(a, s.selKey, e, false, true);
    ImGui::EndDisabled();
    if (sameMux(a, s.selKey)) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Watch"))) { a.engine.player().select(s.selKey.sid); a.engine.player().setVolume(a.volume); }
    }
    if (one) {
        for (auto& c : s.rec.conflicts()) if (c.id == one->id) { ImGui::TextColored(pal::warnAmber(), "%s", tr("Not enough tuners: this overlaps a recording on another multiplex and will not be recorded.")); break; }
        if (one->state == RecJob::Failed || one->state == RecJob::Missed) ImGui::TextColored(pal::warnAmber(), "%s", one->error.c_str());
    }
    ImGui::Separator();
    ImGui::BeginChild("evtxt", ImVec2(0, 0));
    ImGui::PushTextWrapPos(0);
    if (!e.text.empty()) ImGui::TextUnformatted(e.text.c_str());
    if (!e.extended.empty()) { ImGui::Spacing(); ImGui::TextUnformatted(e.extended.c_str()); }
    if (e.text.empty() && e.extended.empty()) ImGui::TextDisabled("%s", tr("No description broadcast."));
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
}

void searchResults(App& a) {
    TvRec& s = S();
    const auto hits = s.epg.search(s.search, 200);
    const int64_t now = nowUtc();
    if (ImGui::BeginTable("srch", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
        ImGui::TableSetupColumn("channel", ImGuiTableColumnFlags_WidthFixed, 160 * gUi);
        ImGui::TableSetupColumn("programme");
        for (auto& h : hits) {
            if (h.ev.end() < now) continue;
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            char l[96]; snprintf(l, sizeof l, "%s##%d_%d", tvTime(h.ev.start, "both").c_str(), h.key.sid, h.ev.eventId);
            if (ImGui::Selectable(l, s.selKey == h.key && s.selEvent == h.ev.eventId, ImGuiSelectableFlags_SpanAllColumns)) { s.selKey = h.key; s.selEvent = h.ev.eventId; }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(serviceLabel(a, h.key).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(h.ev.title.c_str());
        }
        ImGui::EndTable();
    }
}

void guideGrid(App& a) {
    TvRec& s = S();
    const int64_t now = nowUtc();
    if (s.viewStart == 0) s.viewStart = now / 3600 * 3600 - 3600;
    const int64_t span = 26 * 3600;
    refreshRows(a, s.viewStart, s.viewStart + span);
    const float nameW = 150 * gUi, rowH = 34 * gUi, headH = 24 * gUi, pph = s.pph * gUi;
    const float totalW = nameW + pph * (float)span / 3600.f, totalH = headH + rowH * std::max<size_t>(1, s.order.size());
    ImGui::BeginChild("gridv", ImVec2(0, -190 * gUi), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(totalW, totalH));
    if (s.scrollNow) { ImGui::SetScrollX(std::max(0.f, pph * (float)(now - s.viewStart - 1800) / 3600.f)); s.scrollNow = false; }
    const float sx = ImGui::GetScrollX();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float visL = wp.x + sx + nameW, visR = wp.x + sx + ImGui::GetWindowWidth();
    auto xOf = [&](int64_t t) { return o.x + nameW + pph * (float)(t - s.viewStart) / 3600.f; };
    const ImU32 textC = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled), line = IM_COL32(120, 120, 130, 70);
    // events
    const std::vector<RecJob> jobs = s.rec.schedule().list();
    for (size_t r = 0; r < s.order.size(); r++) {
        const ServiceKey& k = s.order[r];
        const float y0 = o.y + headH + rowH * (float)r;
        const auto it = s.rows.find(k);
        if (it == s.rows.end()) continue;
        if (y0 + rowH < wp.y || y0 > wp.y + ImGui::GetWindowHeight()) continue;
        dl->AddLine(ImVec2(visL, y0 + rowH), ImVec2(visR, y0 + rowH), line);
        for (const EpgItem& e : it->second) {
            const float x0 = std::max(xOf(e.start), o.x + nameW), x1 = xOf(e.end()) - 1.f;
            if (x1 < visL || x0 > visR || x1 - x0 < 2) continue;
            const bool cur = e.start <= now && now < e.end(), sel = s.selKey == k && s.selEvent == e.eventId;
            const ImVec2 a0(x0, y0 + 2), a1(x1, y0 + rowH - 2);
            dl->AddRectFilled(a0, a1, genreColor(e.genre(), cur), 3.f);
            if (sel) dl->AddRect(a0, a1, IM_COL32(255, 255, 255, 230), 3.f, 0, 2.f);
            const RecJob* rj = findJob(jobs, k, e, false);
            if (rj && !rj->reminder) dl->AddCircleFilled(ImVec2(a1.x - 7 * gUi, a0.y + 7 * gUi), 4 * gUi, IM_COL32(230, 60, 60, 255));
            else if (rj) dl->AddCircle(ImVec2(a1.x - 7 * gUi, a0.y + 7 * gUi), 4 * gUi, IM_COL32(240, 200, 80, 255));
            dl->PushClipRect(ImVec2(std::max(a0.x, visL), a0.y), a1, true);
            dl->AddText(ImVec2(std::max(a0.x, visL) + 4 * gUi, a0.y + 2), IM_COL32(255, 255, 255, 240), e.title.c_str());
            dl->AddText(ImVec2(std::max(a0.x, visL) + 4 * gUi, a0.y + 2 + ImGui::GetTextLineHeight()), IM_COL32(255, 255, 255, 170), tvTime(e.start, "time").c_str());
            dl->PopClipRect();
            ImGui::SetCursorScreenPos(a0);
            char id[48]; snprintf(id, sizeof id, "##e%d_%d_%d", k.sid, k.tsid, e.eventId);
            if (ImGui::InvisibleButton(id, ImVec2(std::max(1.f, a1.x - a0.x), a1.y - a0.y))) { s.selKey = k; s.selEvent = e.eventId; }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s\n%s - %s", e.title.c_str(), tvTime(e.start, "time").c_str(), tvTime(e.end(), "time").c_str());
                if (ImGui::IsMouseDoubleClicked(0) && sameMux(a, k)) { a.engine.player().select(k.sid); a.engine.player().setVolume(a.volume); }
            }
        }
    }
    // time header (drawn over the events so it stays on top of scrolled rows)
    const float hy = wp.y + ImGui::GetScrollY();
    dl->AddRectFilled(ImVec2(visL, hy), ImVec2(visR, hy + headH), ImGui::GetColorU32(ImGuiCol_ChildBg));
    for (int64_t t = s.viewStart; t <= s.viewStart + span; t += 1800) {
        const float x = xOf(t);
        if (x < visL - 40 || x > visR) continue;
        const bool hour = t % 3600 == 0;
        dl->AddLine(ImVec2(x, o.y + headH), ImVec2(x, o.y + totalH), hour ? IM_COL32(120, 120, 130, 60) : IM_COL32(120, 120, 130, 25));
        if (hour && x >= visL) { const std::string lb = tvTime(t, "time") + (tvTime(t, "time") == "00:00" ? "  " + tvTime(t, "date") : ""); dl->AddText(ImVec2(x + 3, hy + 3), textC, lb.c_str()); }
    }
    // now
    const float nx = xOf(now);
    if (nx >= visL && nx <= visR) dl->AddLine(ImVec2(nx, hy), ImVec2(nx, o.y + totalH), IM_COL32(240, 90, 90, 220), 2.f);
    // channel names, pinned left
    dl->AddRectFilled(ImVec2(wp.x + sx, hy), ImVec2(wp.x + sx + nameW, hy + ImGui::GetWindowHeight()), ImGui::GetColorU32(ImGuiCol_ChildBg));
    for (size_t r = 0; r < s.order.size(); r++) {
        const float y0 = o.y + headH + rowH * (float)r;
        if (y0 + rowH < wp.y || y0 > wp.y + ImGui::GetWindowHeight()) continue;
        dl->PushClipRect(ImVec2(wp.x + sx, y0), ImVec2(wp.x + sx + nameW - 2, y0 + rowH), true);
        dl->AddText(ImVec2(wp.x + sx + 4, y0 + (rowH - ImGui::GetTextLineHeight()) * .5f), sameMux(a, s.order[r]) ? textC : dim, serviceLabel(a, s.order[r]).c_str());
        dl->PopClipRect();
    }
    dl->AddRectFilled(ImVec2(wp.x + sx, hy), ImVec2(wp.x + sx + nameW, hy + headH), ImGui::GetColorU32(ImGuiCol_ChildBg));
    ImGui::EndChild();
}

} // namespace

// ---- public ---------------------------------------------------------------------------------------------------------------------------------------
void tvRecTick(App& a) { tick(a); }

std::string tvRecJobsJson() {
    static const char* const names[] = {"scheduled", "recording", "done", "failed", "missed", "cancelled"};
    std::string o = "{\"jobs\":[";
    bool first = true;
    for (const RecJob& j : S().rec.schedule().list()) {
        if (!first) o += ',';
        first = false;
        char b[160];
        snprintf(b, sizeof b, "\"id\":%d,\"start\":%lld,\"end\":%lld,\"state\":\"%s\",\"reminder\":%s,\"series\":%s", j.id, (long long)j.start, (long long)j.end,
                 names[(size_t)j.state < 6 ? (size_t)j.state : 0], j.reminder ? "true" : "false", j.series ? "true" : "false");
        o += "{" + std::string(b) + ",\"title\":\"" + jsonEscape(j.title) + "\",\"service\":\"" + jsonEscape(j.serviceName) + "\"}";
    }
    return o + "]}";
}

void tvRecOverrideTs(App& a) {
    TvRec& s = S();
    if (s.mode == PmNone) return;
    std::lock_guard<std::mutex> lk(s.pmu);
    if (!s.psnap.services.empty()) a.ts = s.psnap;
}

void tvRecShutdown(App& a) {
    TvRec& s = S();
    if (!s.init) return;
    s.feeder.stop();
    if (s.loader.joinable()) s.loader.join();
    a.engine.setPlayerExternal(false);
    a.engine.setRecorderTap(nullptr);
    s.rec.stopAll();
    s.rec.schedule().save(s.dir + "/schedule.json");
    if (s.epg.count()) s.epg.save(s.dir + "/epg.json");
    s.shift.close();
}

void tvGuideTab(App& a) {
    TvRec& s = S();
    init(a);
    if (a.ts.services.empty() && s.epg.count() == 0) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", tr("Waiting for the service list...")); ImGui::PopTextWrapPos(); return; }
    // toolbar
    ImGui::SetNextItemWidth(220 * gUi);
    ImGui::InputTextWithHint("##gsearch", tr("Search title or description"), s.search, sizeof s.search);
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Now"))) { s.viewStart = 0; s.scrollNow = true; }
    ImGui::SameLine();
    if (ImGui::SmallButton("-6 h")) { s.viewStart -= 6 * 3600; s.rowsT = -10; }
    ImGui::SameLine();
    if (ImGui::SmallButton("+6 h")) { s.viewStart += 6 * 3600; s.rowsT = -10; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::SliderFloat("##gzoom", &s.pph, 80, 400, "%.0f px/h");
    ImGui::SameLine();
    ImGui::Checkbox(tr("This multiplex only"), &s.onlyThisMux);
    ImGui::SameLine();
    ImGui::TextDisabled("%zu %s   %s %s", s.epg.count(), tr("programmes"), tr("time zone:"), s.col.zones().empty() ? tr("this computer") : s.col.zones()[0].country.c_str());
    if (!s.msg.empty() && glfwGetTime() - s.msgT < 12) ImGui::TextColored(pal::warnAmber(), "%s", s.msg.c_str());
    if (s.epg.count() == 0) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextWrapped("%s", tr("No programme information received yet. Broadcasters send it in the EIT tables, which repeat every few seconds (now/next) to a few minutes (schedule). Some multiplexes send none at all."));
        ImGui::PopTextWrapPos();
        return;
    }
    if (s.search[0]) {
        ImGui::BeginChild("srchv", ImVec2(0, -190 * gUi), ImGuiChildFlags_Borders);
        searchResults(a);
        ImGui::EndChild();
    } else guideGrid(a);
    ImGui::BeginChild("gdet", ImVec2(0, 0), ImGuiChildFlags_Borders);
    detailsPane(a);
    ImGui::EndChild();
}

void tvPlaybackBar(App& a) {
    TvRec& s = S();
    init(a);
    Player& pl = a.engine.player();
    const int mode = s.mode;
    if (s.loading) { ImGui::TextDisabled("%s", tr("Opening the recording...")); return; }
    if (mode == PmNone) {
        if (!s.shiftOn) return;
        ImGui::BeginDisabled(pl.selected() < 0 || !s.shift.isOpen());
        if (ImGui::Button(tr("Pause (timeshift)"))) shiftPause(a, true);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Freezes live TV and keeps recording it to disk, so you can resume or rewind."));
        ImGui::SameLine();
        ImGui::TextDisabled("%s %.0f s  %.0f MB", tr("buffered"), s.shift.bufferedSeconds(), s.shift.bytes() / 1e6);
        return;
    }
    const bool shift = mode == PmShift;
    ImGui::TextColored(pal::heading(), "%s", shift ? tr("Timeshift") : s.playName.c_str());
    ImGui::SameLine();
    const bool paused = s.feeder.paused();
    if (ImGui::Button(paused ? tr("Play") : tr("Pause"))) s.feeder.pause(!paused);
    for (int d : {-30, -10, 10, 30}) {
        ImGui::SameLine();
        char l[16]; snprintf(l, sizeof l, "%+d s", d);
        if (ImGui::Button(l)) { s.feeder.skip(d); resetPlayerLocked(); }
    }
    ImGui::SameLine();
    if (shift) {
        ImGui::TextDisabled("%.0f s %s", s.feeder.behindLiveSeconds(), tr("behind live"));
        ImGui::SameLine();
        if (ImGui::Button(tr("Live"))) stopPlayback(a, true);
    } else {
        const double dur = s.file.duration(), pos = s.feeder.positionSeconds();
        double t0 = 0; s.file.timeAt(0, t0);
        float f = dur > 0 ? (float)((pos - t0) / dur) : 0.f;
        ImGui::SetNextItemWidth(220 * gUi);
        if (ImGui::SliderFloat("##seek", &f, 0.f, 1.f, "")) { s.feeder.seekSeconds(t0 + f * dur); resetPlayerLocked(); }
        ImGui::SameLine();
        ImGui::TextDisabled("%d:%02d / %d:%02d%s", (int)(pos - t0) / 60, (int)(pos - t0) % 60, (int)dur / 60, (int)dur % 60, s.feeder.atEnd() ? "  (end)" : "");
        ImGui::SameLine();
        if (ImGui::Button(tr("Stop"))) stopPlayback(a, true);
        if (s.file.t2mi()) {   // a modulator feed (T2-MI): the PLP whose transport stream plays
            ImGui::SameLine();
            const int cur = s.file.t2miPlp();
            char lb[32]; snprintf(lb, sizeof lb, "PLP %d", cur);
            ImGui::SetNextItemWidth(110 * gUi);
            if (ImGui::BeginCombo("##t2miplp", lb)) {
                for (int p : s.file.t2miPlps()) { char b[24]; snprintf(b, sizeof b, "PLP %d", p); if (ImGui::Selectable(b, p == cur) && p != cur) switchT2miPlp(p); }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("T2-MI on PID 0x%X: the transport stream of this PLP plays", s.file.t2miPid());
        }
        // sidecar subtitles: show the current cue under the controls
    }
    (void)pl;
}

void tvRecordingsTab(App& a) {
    TvRec& s = S();
    init(a);
    const double t = glfwGetTime();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Recordings run only while OnAir is open and the computer is awake; a sleeping computer records nothing. Scrambled services are recorded as they arrive and are not decoded. For personal use only."));
    ImGui::PopTextWrapPos();
    if (!s.msg.empty() && t - s.msgT < 12) ImGui::TextColored(pal::warnAmber(), "%s", s.msg.c_str());
    tvPlaybackBar(a);

    // ---- scheduled
    if (ImGui::CollapsingHeader(trw("Scheduled"), ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto jobs = s.rec.schedule().list();
        const auto conf = s.rec.conflicts();
        const auto act = s.rec.active();
        const int64_t now = nowUtc();
        if (ImGui::BeginTable("sched", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("when"); ImGui::TableSetupColumn("channel"); ImGui::TableSetupColumn("programme", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("state"); ImGui::TableSetupColumn("");
            int shown = 0;
            for (auto& j : jobs) {
                if (j.state == RecJob::Done || j.state == RecJob::Cancelled) continue;
                if (shown++ > 100) break;
                ImGui::PushID(j.id);
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Text("%s - %s", tvTime(j.effStart(), "both").c_str(), tvTime(j.effEnd(), "time").c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(j.serviceName.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%s%s", j.series ? "[series] " : j.reminder ? "[reminder] " : "", j.title.c_str());
                ImGui::TableNextColumn();
                bool bad = false;
                for (auto& c : conf) if (c.id == j.id) bad = true;
                if (j.state == RecJob::Recording) {
                    uint64_t by = 0; for (auto& x : act) if (x.jobId == j.id) by = x.bytes;
                    ImGui::TextColored(pal::badRed(), "%s %.0f MB", tr("recording"), by / 1e6);
                } else if (j.state == RecJob::Failed || j.state == RecJob::Missed) ImGui::TextColored(pal::warnAmber(), "%s", j.error.empty() ? "failed" : j.error.c_str());
                else if (bad) ImGui::TextColored(pal::warnAmber(), "%s", tr("conflict"));
                else if (j.muxKey == 0 && !j.reminder && !j.series) ImGui::TextDisabled("%s", tr("any multiplex"));
                else if (j.series) ImGui::TextDisabled("%s", tr("series"));
                else ImGui::TextDisabled("%s", now >= j.effStart() ? tr("waiting for the tuner") : tr("scheduled"));
                ImGui::TableNextColumn();
                if (ImGui::SmallButton(j.state == RecJob::Recording ? tr("Stop") : tr("Remove"))) {
                    if (j.state == RecJob::Recording) s.rec.stopJob(j.id, false); else s.rec.schedule().remove(j.id);
                    s.schedDirty = true; s.lastRecs = -1000;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (!shown) ImGui::TextDisabled("%s", tr("Nothing scheduled. Pick a programme in the Guide, or add a manual recording below."));
        }
        // manual recording
        ImGui::SeparatorText(tr("Manual recording"));
        const auto& sv = a.ts.services;
        if (sv.empty()) ImGui::TextDisabled("%s", tr("Waiting for the service list..."));
        else {
            s.manSvc = std::min(s.manSvc, (int)sv.size() - 1);
            ImGui::SetNextItemWidth(200 * gUi);
            if (ImGui::BeginCombo("##msvc", sv[s.manSvc].name.empty() ? std::to_string(sv[s.manSvc].id).c_str() : sv[s.manSvc].name.c_str())) {
                for (int i = 0; i < (int)sv.size(); i++) if (ImGui::Selectable((sv[i].name.empty() ? std::to_string(sv[i].id) : sv[i].name).c_str(), i == s.manSvc)) s.manSvc = i;
                ImGui::EndCombo();
            }
            ImGui::SameLine(); ImGui::SetNextItemWidth(160 * gUi); ImGui::InputTextWithHint("##mtitle", tr("Title"), s.manTitle, sizeof s.manTitle);
            ImGui::SameLine(); ImGui::SetNextItemWidth(40 * gUi); ImGui::InputInt("##mh", &s.manH, 0, 0); ImGui::SameLine(0, 2); ImGui::TextUnformatted(":"); ImGui::SameLine(0, 2);
            ImGui::SetNextItemWidth(40 * gUi); ImGui::InputInt("##mm", &s.manM, 0, 0);
            s.manH = std::max(0, std::min(23, s.manH)); s.manM = std::max(0, std::min(59, s.manM));
            ImGui::SameLine(); ImGui::SetNextItemWidth(60 * gUi); ImGui::InputInt("min##mdur", &s.manMin, 0, 0);
            s.manMin = std::max(1, std::min(24 * 60, s.manMin));
            ImGui::SameLine();
            if (ImGui::Button(tr("Add"))) {
                std::time_t nt = (std::time_t)now; std::tm tmv = *std::localtime(&nt);
                tmv.tm_hour = s.manH; tmv.tm_min = s.manM; tmv.tm_sec = 0; tmv.tm_isdst = -1;
                std::time_t st = std::mktime(&tmv);
                if (st < nt - 60) st += 24 * 3600;
                RecJob j; j.svc = {a.ts.onid, a.ts.tsid, sv[s.manSvc].id}; j.serviceName = sv[s.manSvc].name; j.muxKey = s.muxKey;
                j.title = s.manTitle[0] ? s.manTitle : (j.serviceName.empty() ? "Recording" : j.serviceName);
                j.start = (int64_t)st; j.end = j.start + (int64_t)s.manMin * 60; j.padBefore = 0; j.padAfter = 0;
                j.epg.title = j.title; j.epg.start = j.start; j.epg.duration = s.manMin * 60;
                s.rec.schedule().add(j); s.schedDirty = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Starts at this clock time (today, or tomorrow when it has passed), no padding."));
        }
    }

    // ---- recorded
    if (ImGui::CollapsingHeader(trw("Recorded"), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (t - s.lastRecs > 5) { s.recs = listRecordings(s.cfg.dir); s.lastRecs = t; }
        if (s.recs.empty()) ImGui::TextDisabled("%s", tr("No recordings yet."));
        const auto act = s.rec.active();
        if (ImGui::BeginTable("recs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, 260 * gUi))) {
            ImGui::TableSetupColumn("date"); ImGui::TableSetupColumn("channel"); ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("size"); ImGui::TableSetupColumn("");
            for (size_t i = 0; i < s.recs.size(); i++) {
                const RecordingEntry& e = s.recs[i];
                bool running = false;
                for (auto& x : act) if (x.file == e.tsPath) running = true;
                ImGui::PushID((int)i);
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::TextUnformatted(tvTime(e.recStart ? e.recStart : e.start, "both").c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(e.serviceName.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.title.c_str());
                if (e.status == "failed") { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "(%s)", e.error.c_str()); }
                if (running) { ImGui::SameLine(); ImGui::TextColored(pal::badRed(), "%s", tr("recording")); }
                if (!e.subtitleFiles.empty()) { ImGui::SameLine(); ImGui::TextDisabled("[subs]"); }
                if (ImGui::IsItemHovered() && !e.description.empty()) ImGui::SetTooltip("%s", e.description.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.1f MB", e.bytes / 1e6);
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(running);
                if (ImGui::SmallButton(tr("Play"))) startFile(a, e);
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Export"))) {
                    const std::string dst = saveFileDialog((std::filesystem::path(e.tsPath).filename().string()).c_str());
                    if (!dst.empty()) {
                        std::error_code ec;
                        std::filesystem::copy_file(u8file(e.tsPath), u8file(dst), std::filesystem::copy_options::overwrite_existing, ec);
                        s.msg = ec ? "export failed: " + ec.message() : "exported to " + dst; s.msgT = t;
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Delete"))) { s.confirmDelete = e.tsPath; ImGui::OpenPopup("confirmdel"); }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (ImGui::BeginPopupModal("confirmdel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(tr("Delete this recording and its subtitle files?"));
            if (ImGui::Button(tr("Delete"))) {
                for (auto& e : s.recs) if (e.tsPath == s.confirmDelete) deleteRecording(e);
                s.lastRecs = -1000; ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (ImGui::SmallButton(tr("Open folder"))) dect2::openUrl(s.cfg.dir);
        ImGui::SameLine();
        const uint64_t fb = s.rec.freeBytes();
        if (fb != UINT64_MAX) ImGui::TextDisabled("%.1f GB %s", fb / 1e9, tr("free"));
    }

    // ---- settings
    if (ImGui::CollapsingHeader(trw("Recorder settings"))) {
        bool ch = false;
        ImGui::SetNextItemWidth(std::min(360 * gUi, ImGui::GetContentRegionAvail().x * 0.8f));
        if (ImGui::InputText(tr("Folder"), s.dirBuf, sizeof s.dirBuf)) { s.cfg.dir = s.dirBuf; ch = true; }
        ImGui::SetNextItemWidth(std::min(360 * gUi, ImGui::GetContentRegionAvail().x * 0.8f));
        if (ImGui::InputText(tr("File name"), s.tplBuf, sizeof s.tplBuf)) { s.cfg.nameTemplate = s.tplBuf; ch = true; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("{title} {service} {date} {time} {year} {month} {day} {id}");
        ImGui::SetNextItemWidth(80 * gUi); ch |= ImGui::InputInt(tr("Start earlier (s)"), &s.padPre, 0, 0);
        ImGui::SetNextItemWidth(80 * gUi); ch |= ImGui::InputInt(tr("End later (s)"), &s.padPost, 0, 0);
        ImGui::SetNextItemWidth(80 * gUi); ch |= ImGui::InputInt(tr("Tuners"), &s.cfg.tuners, 0, 0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("How many different multiplexes can be received at once. Services of the same multiplex always record together."));
        ImGui::SetNextItemWidth(80 * gUi); ch |= ImGui::InputInt(tr("Keep free (GB)"), &s.minFreeGb, 0, 0);
        double loss = s.cfg.lossStopSec; ImGui::SetNextItemWidth(80 * gUi);
        if (ImGui::InputDouble(tr("Stop after signal loss (s, 0 = never)"), &loss, 0, 0, "%.0f")) { s.cfg.lossStopSec = std::max(0.0, loss); ch = true; }
        ch |= ImGui::Checkbox(tr("Save teletext subtitles next to recordings (.srt, .vtt)"), &s.cfg.saveSubtitles);
        ch |= ImGui::Checkbox(tr("Tune to a scheduled recording's channel when it starts"), &s.autoTune);
        if (ImGui::Checkbox(tr("Timeshift buffer on disk"), &s.shiftOn)) ch = true;
        ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi); ch |= ImGui::InputInt(tr("minutes"), &s.shiftMin, 0, 0);
        s.padPre = std::max(0, std::min(3600 * 3, s.padPre)); s.padPost = std::max(0, std::min(3600 * 3, s.padPost));
        s.cfg.tuners = std::max(1, std::min(16, s.cfg.tuners)); s.minFreeGb = std::max(0, s.minFreeGb); s.shiftMin = std::max(1, std::min(240, s.shiftMin));
        if (ch) { applyConfig(); savePrefs(); }
    }
}
