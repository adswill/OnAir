// Tools > Scheduler: jobs that tune the radio, start the receiver, record audio / IQ / the log, then stop and put everything back. When a job is due
// (time, daily, weekly, cron-like, or a satellite pass above N degrees), how overlapping jobs are handled and what happens to jobs missed while the
// computer slept are all in core/src/sched/sched.cpp (tested without a radio); this file is the window, the App as the scheduler's radio, and the
// storage (scheduler.json and scheduler_history.json in the data folder). The tick runs every frame from floatingWindows(), with the window shut,
// like the pass tracking does; it is a once-a-second clock check, nothing more.
#include "../app.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "dect2/audio/audio_rec.h"
#include "dect2/geo/passes.h"
#include "dect2/sched/sched.h"
#include "dect2/source/iq_record.h"
#include "panels/gallery.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace {
namespace sd = dect2::sched;
namespace fs = std::filesystem;

int64_t nowUtc() { return (int64_t)std::time(nullptr); }

int64_t civilSecs(const std::tm& t) { return sd::daysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday) * 86400 + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec; }
int localOffset(int64_t t) {   // the system's zone, with its daylight saving, at the instant t
    const std::time_t tt = (std::time_t)t;
    std::tm l{}, g{};
#ifdef _WIN32
    localtime_s(&l, &tt); gmtime_s(&g, &tt);
#else
    localtime_r(&tt, &l); gmtime_r(&tt, &g);
#endif
    return (int)(civilSecs(l) - civilSecs(g));
}

std::string localText(int64_t t, bool secs = false) {
    if (t <= 0) return "";
    const std::time_t tt = (std::time_t)t;
    std::tm l{};
#ifdef _WIN32
    localtime_s(&l, &tt);
#else
    localtime_r(&tt, &l);
#endif
    char b[48];
    strftime(b, sizeof b, secs ? "%Y-%m-%d %H:%M:%S" : "%Y-%m-%d %H:%M", &l);
    return b;
}

bool parseLocal(const std::string& s, int64_t& out) {   // "2026-10-22 14:30"
    int y, m, d, hh = 0, mm = 0;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d", &y, &m, &d, &hh, &mm) < 3) return false;
    if (m < 1 || m > 12 || d < 1 || d > 31 || hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    out = sd::localToUtc([](int64_t t) { return localOffset(t); }, y, m, d, hh, mm);
    return true;
}

std::string sizeText(uint64_t b) {
    char t[32];
    if (b >= 1000000000ull) snprintf(t, sizeof t, "%.2f GB", b / 1e9);
    else if (b >= 1000000ull) snprintf(t, sizeof t, "%.1f MB", b / 1e6);
    else snprintf(t, sizeof t, "%.0f kB", b / 1e3);
    return t;
}
std::string durText(double s) {
    const int t = (int)(s + 0.5);
    char b[32];
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

std::string currentModeKey(App& a) {
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) return scanModeKey(kModes[i]);
    return "";
}
std::string modeNameOf(const std::string& key) {
    const int i = key.empty() ? -1 : scanModeIndex(key);
    return i >= 0 ? kModes[i].name : key;
}
std::string recordingsDir() { return plat::dataDir() + "/recordings"; }

// The App as the scheduler's radio.
struct AppHost : sd::Host {
    App* a = nullptr;
    std::vector<std::pair<std::string, std::string>> started;   // path, kind
    size_t logStart = 0;
    std::string logPath;
    struct PassCache { int64_t at = 0, from = 0, to = 0; std::vector<sd::PassWindow> v; };
    std::map<std::string, PassCache> passCache;

    sd::RadioState snapshot() override {
        sd::RadioState s;
        s.freqHz = a->freqMhz * 1e6; s.mode = currentModeKey(*a); s.gainDb = a->tune.gainDb; s.running = a->engine.running();
        if (a->devIdx >= 0 && a->devIdx < (int)a->devices.size()) s.radio = a->devices[(size_t)a->devIdx].name;
        return s;
    }
    bool apply(const sd::Action& act, std::string& err) override {
        if (a->scanner.progress().running) { err = "a channel scan is using the radio"; return false; }
        if (a->engine.recordingStats().active) { err = "a recording is already running"; return false; }
        if (!act.radio.empty()) {
            int idx = -1;
            for (int i = 0; i < (int)a->devices.size(); i++) if (a->devices[(size_t)i].name == act.radio) idx = i;
            if (idx < 0) { err = "radio not found: " + act.radio; return false; }
            if (idx != a->devIdx) { if (a->engine.running()) a->engine.stop(); a->devIdx = idx; }
        }
        if (!act.mode.empty()) {
            const int mi = scanModeIndex(act.mode);
            if (mi < 0) { err = "this build has no mode " + act.mode; return false; }
            selectMode(*a, kModes[mi].family, kModes[mi].subId);   // restarts a running receiver in the new mode
        }
        if (act.gainDb >= 0) { a->tune.gainDb = act.gainDb; if (a->engine.running()) a->engine.retune(a->tune); }
        if (act.freqHz > 0) tuneFreq(*a, act.freqHz / 1e6);
        if (!a->engine.running()) startReceiver(*a);
        if (!a->engine.running()) { err = "the receiver did not start"; return false; }
        return true;
    }
    bool startRecording(const sd::RecordSpec& r, std::string& err) override {
        started.clear();
        const std::string dir = r.dir.empty() ? recordingsDir() : r.dir;
        std::error_code ec;
        const std::string base = dir + "/" + r.baseName;
        fs::create_directories(fs::path(base).parent_path(), ec);
        if (r.audio) {
            dect2::AudioRecOptions o;
            o.format = (dect2::AudioRecFormat)a->audioRecFmt;
            o.tag = "OnAir scheduler";
            const std::string path = base + dect2::AudioRecorder::extension(o.format);
            if (!dect2::AudioRecorder::instance().start(path, o, err)) return false;
            started.push_back({path, "audio"});
        }
        if (r.iq) {
            const FileFormat f = (FileFormat)recFileFormatOf(a->recFormat);
            const std::string path = base + (f == FileFormat::CF32 ? ".cf32" : ".cs8");
            if (!a->engine.startRecording(path, f, err)) {
                if (r.audio) dect2::AudioRecorder::instance().stop();
                return false;
            }
            started.push_back({path, "iq"});
        }
        if (r.log) {
            size_t total = 0;
            a->engine.logSnapshot(total);
            logStart = total;
            logPath = base + ".log";
            started.push_back({logPath, "log"});
        }
        return true;
    }
    std::vector<sd::FileResult> stopRecording() override {
        std::vector<sd::FileResult> out;
        const dect2::RecordingStats rs = a->engine.recordingStats();
        for (const auto& [path, kind] : started) {
            if (kind == "audio") { dect2::AudioRecorder::instance().stop(); const auto st = dect2::AudioRecorder::instance().stats(); out.push_back({st.path.empty() ? path : st.path, kind, (uint64_t)st.bytes}); }
            else if (kind == "iq") { if (rs.active) a->engine.stopRecording(); out.push_back({path, kind, 0}); }
            else if (kind == "log") {
                size_t total = 0;
                const std::vector<std::string> lines = a->engine.logSnapshot(total);
                std::ofstream f(fs::path(path), std::ios::binary);
                const size_t n = total > logStart ? std::min(total - logStart, lines.size()) : 0;
                for (size_t i = lines.size() - n; i < lines.size(); i++) f << lines[i] << "\n";
                f.close();
                out.push_back({path, kind, 0});
            }
        }
        for (auto& f : out) { std::error_code ec; const auto s = fs::file_size(fs::path(f.path), ec); if (!ec) f.bytes = (uint64_t)s; }
        started.clear();
        return out;
    }
    void restore(const sd::RadioState& b) override {
        if (!b.running && a->engine.running()) a->engine.stop();
        if (!b.mode.empty()) { const int mi = scanModeIndex(b.mode); if (mi >= 0) selectMode(*a, kModes[mi].family, kModes[mi].subId); }
        if (b.gainDb >= 0) { a->tune.gainDb = b.gainDb; if (a->engine.running()) a->engine.retune(a->tune); }
        if (b.freqHz > 0) tuneFreq(*a, b.freqHz / 1e6);
        if (!b.radio.empty()) {
            for (int i = 0; i < (int)a->devices.size(); i++) if (a->devices[(size_t)i].name == b.radio && i != a->devIdx) {
                const bool was = a->engine.running();
                if (was) a->engine.stop();
                a->devIdx = i;
                if (was) startReceiver(*a);
            }
        }
        if (b.running && !a->engine.running()) startReceiver(*a);
    }
    int64_t freeBytes(const std::string& dir) override {
        std::error_code ec;
        std::string d = dir.empty() ? recordingsDir() : dir;
        fs::create_directories(d, ec);
        const auto sp = fs::space(fs::path(d), ec);
        return ec ? INT64_MAX : (int64_t)sp.available;
    }
    std::vector<sd::PassWindow> passes(const std::string& sat, double minEl, int64_t from, int64_t to) override {
        char key[200];
        snprintf(key, sizeof key, "%s|%.1f", sat.c_str(), minEl);
        PassCache& c = passCache[key];
        const int64_t now = nowUtc();
        if (c.at == 0 || now - c.at > 600 || from < c.from - 3600 || to > c.to + 600 || (c.v.empty() && now - c.at > 60)) {
            c.at = now; c.from = from - 1800; c.to = std::max(to, from + 6 * 3600);
            c.v.clear();
            if (const dect2::Sgp4* s = passesFindSat(sat, (double)now)) {
                double lat, lon;
                receiverLocation(lat, lon);
                dect2::Observer o; o.latDeg = lat; o.lonDeg = lon; o.altKm = receiverAltitudeM() / 1000;
                for (const dect2::Pass& p : dect2::findPasses(*s, o, (double)c.from, (double)c.to, minEl)) c.v.push_back({(int64_t)p.aos, (int64_t)p.los, p.maxElDeg});
            }
        }
        std::vector<sd::PassWindow> r;
        for (const auto& p : c.v) if (p.los > from && p.aos < to) r.push_back(p);
        return r;
    }
    double iqBytesPerSec() override { return a->tune.sampleRate * (a->recFormat == 1 ? 8 : 2); }
    void warn(const std::string& m) override { a->engine.log("scheduler: " + m); }
};

struct State {
    sd::Scheduler sc{[](int64_t t) { return localOffset(t); }};
    AppHost host;
    bool loaded = false;
    double lastSave = 0;
    size_t histSeen = 0;
    int64_t lastRun = 0;
    // editing
    bool editing = false, isNew = false;
    sd::Job E;
    char name[96] = "", cron[96] = "0 6 * * *", sat[96] = "ISS", pattern[160] = "", folder[260] = "", onceText[40] = "";
    char modeFilter[48] = "";
    double freqMhz = 0;
    int durMin = 10, graceMin = 15;
    std::string msg;
    std::map<std::string, int64_t> nextCache;
    int64_t nextAt = 0;
    std::vector<sd::PlannedRun> upcoming;
    int64_t upcomingAt = 0;
};
State S;

std::string jobsPath() { return plat::dataDir() + "/scheduler.json"; }
std::string histPath() { return plat::dataDir() + "/scheduler_history.json"; }

std::string readAll(const std::string& p) { std::ifstream f(fs::path(p), std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }
void writeAll(const std::string& p, const std::string& t) {
    const std::string tmp = p + ".tmp";
    { std::ofstream f(fs::path(tmp), std::ios::binary); f << t; }
    std::error_code ec;
    fs::rename(fs::path(tmp), fs::path(p), ec);
}

void load(App& a) {
    if (S.loaded) return;
    S.loaded = true;
    S.host.a = &a;
    std::string err;
    const std::string jt = readAll(jobsPath());
    if (!jt.empty() && !sd::fromJson(jt, S.sc.jobs, S.sc.settings, err)) a.engine.log("scheduler: " + jobsPath() + ": " + err);
    const std::string ht = readAll(histPath());
    if (!ht.empty()) sd::historyFromJson(ht, S.sc.history, err);
    S.histSeen = S.sc.history.size();
}
void save() {
    writeAll(jobsPath(), sd::toJson(S.sc.jobs, S.sc.settings));
    writeAll(histPath(), sd::historyToJson(S.sc.history));
    S.lastSave = std::time(nullptr);
}

void newId(sd::Job& j) {
    char b[48];
    snprintf(b, sizeof b, "job%lld_%d", (long long)nowUtc(), (int)S.sc.jobs.size());
    j.id = b;
}

void beginEdit(const sd::Job* j) {
    S.editing = true; S.isNew = !j;
    if (j) S.E = *j;
    else {
        S.E = sd::Job();
        newId(S.E);
        S.E.name = "New job";
        S.E.when.kind = sd::Schedule::Daily; S.E.when.hour = 6; S.E.when.minute = 0;
    }
    snprintf(S.name, sizeof S.name, "%s", S.E.name.c_str());
    snprintf(S.cron, sizeof S.cron, "%s", S.E.when.cron.empty() ? "0 6 * * *" : S.E.when.cron.c_str());
    snprintf(S.sat, sizeof S.sat, "%s", S.E.when.satellite.empty() ? "ISS" : S.E.when.satellite.c_str());
    snprintf(S.pattern, sizeof S.pattern, "%s", S.E.action.namePattern.c_str());
    snprintf(S.folder, sizeof S.folder, "%s", S.E.action.folder.c_str());
    snprintf(S.onceText, sizeof S.onceText, "%s", localText(S.E.when.at > 0 ? S.E.when.at : nowUtc() + 3600).c_str());
    S.freqMhz = S.E.action.freqHz / 1e6;
    S.durMin = std::max(0, S.E.durationSec / 60);
    S.graceMin = std::max(1, S.E.graceSec / 60);
    S.modeFilter[0] = 0;
    S.msg.clear();
}

bool commitEdit(App& a) {
    sd::Job& j = S.E;
    j.name = S.name;
    if (j.name.empty()) { S.msg = "give the job a name"; return false; }
    j.when.cron = S.cron; j.when.satellite = S.sat;
    j.action.namePattern = S.pattern; j.action.folder = S.folder;
    j.action.freqHz = S.freqMhz * 1e6;
    j.durationSec = S.durMin * 60; j.graceSec = S.graceMin * 60;
    if (j.when.kind == sd::Schedule::Once) {
        if (!parseLocal(S.onceText, j.when.at)) { S.msg = "the date is year-month-day hour:minute, like 2026-10-22 14:30"; return false; }
    } else if (j.when.kind == sd::Schedule::Every) {
        if (j.when.everySec < 60) { S.msg = "an interval needs at least a minute"; return false; }
        if (j.when.at == 0) j.when.at = nowUtc();
    } else if (j.when.kind == sd::Schedule::Cron) {
        sd::CronFields c; std::string err;
        if (!sd::parseCron(j.when.cron, c, err)) { S.msg = "cron: " + err; return false; }
    } else if (j.when.kind == sd::Schedule::Weekly && !(j.when.weekdays & 0x7f)) { S.msg = "pick at least one weekday"; return false; }
    if (j.when.kind == sd::Schedule::Pass) {
        if (j.when.satellite.empty()) { S.msg = "name the satellite"; return false; }
        j.fixedEnd = true; j.durationSec = 0;
    } else if (j.durationSec <= 0) { S.msg = "set how many minutes it runs"; return false; }
    if (!j.action.recordAudio && !j.action.recordIq && !j.action.recordLog && j.action.freqHz <= 0 && j.action.mode.empty()) { S.msg = "the job does nothing: tune something or record something"; return false; }
    const std::string bad = sd::validateName(j.action.namePattern);
    if (!bad.empty()) { S.msg = "file name: " + bad; return false; }
    bool found = false;
    for (sd::Job& x : S.sc.jobs) if (x.id == j.id) { x = j; found = true; }
    if (!found) S.sc.jobs.push_back(j);
    S.sc.reschedule();
    S.nextCache.clear();
    S.upcomingAt = 0;
    S.editing = false;
    save();
    (void)a;
    return true;
}

ImVec4 resultColour(sd::Result r) {
    switch (r) {
    case sd::Result::Done: return ImVec4(0.45f, 0.85f, 0.5f, 1);
    case sd::Result::Stopped: return ImVec4(0.7f, 0.8f, 0.9f, 1);
    case sd::Result::Skipped: case sd::Result::Missed: return ImVec4(0.95f, 0.8f, 0.4f, 1);
    default: return ImVec4(0.95f, 0.45f, 0.4f, 1);
    }
}

void drawEditor(App& a) {
    sd::Job& j = S.E;
    ImGui::SeparatorText(S.isNew ? "New job" : "Edit job");
    ImGui::SetNextItemWidth(260 * gUi);
    ImGui::InputText("Name", S.name, sizeof S.name);
    ImGui::SameLine();
    ImGui::Checkbox("Enabled", &j.enabled);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    ImGui::InputInt("Priority", &j.priority);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "When two jobs overlap, the higher number wins; a higher priority job cuts a running one short");

    static const char* const kinds[] = {"Once", "Every", "Daily", "Weekly", "Cron-like", "Satellite pass"};
    int k = (int)j.when.kind;
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::Combo("When", &k, kinds, 6)) j.when.kind = (sd::Schedule::Kind)k;
    ImGui::SameLine();
    switch (j.when.kind) {
    case sd::Schedule::Once:
        ImGui::SetNextItemWidth(170 * gUi);
        ImGui::InputText("local time", S.onceText, sizeof S.onceText);
        break;
    case sd::Schedule::Every: {
        int mins = (int)(j.when.everySec / 60);
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt("minutes", &mins)) j.when.everySec = std::max(1, mins) * 60;
        break;
    }
    case sd::Schedule::Daily: case sd::Schedule::Weekly:
        ImGui::SetNextItemWidth(70 * gUi); ImGui::InputInt("##h", &j.when.hour, 0, 0); ImGui::SameLine(); ImGui::TextUnformatted(":"); ImGui::SameLine();
        ImGui::SetNextItemWidth(70 * gUi); ImGui::InputInt("##m", &j.when.minute, 0, 0);
        j.when.hour = std::max(0, std::min(23, j.when.hour)); j.when.minute = std::max(0, std::min(59, j.when.minute));
        ImGui::SameLine(); ImGui::TextDisabled("(your local time)");
        if (j.when.kind == sd::Schedule::Weekly) {
            static const char* const dn[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            for (int d = 1; d <= 7; d++) {
                const int i = d % 7;
                bool on = (j.when.weekdays >> i) & 1;
                if (d > 1) ImGui::SameLine();
                if (ImGui::Checkbox(dn[i], &on)) j.when.weekdays = on ? (j.when.weekdays | (1 << i)) : (j.when.weekdays & ~(1 << i));
            }
        }
        break;
    case sd::Schedule::Cron:
        ImGui::SetNextItemWidth(200 * gUi);
        ImGui::InputText("min hour day month weekday", S.cron, sizeof S.cron);
        {
            sd::CronFields c; std::string err;
            if (!sd::parseCron(S.cron, c, err)) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", err.c_str()); }
        }
        break;
    case sd::Schedule::Pass: {
        ImGui::SetNextItemWidth(160 * gUi);
        ImGui::InputText("satellite", S.sat, sizeof S.sat);
        std::string resolved;
        const bool ok = passesFindSat(S.sat, (double)nowUtc(), &resolved) != nullptr;
        ImGui::SameLine();
        if (ok) ImGui::TextDisabled("= %s", resolved.c_str()); else ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "%s", "not in the satellite data (open Passes and update the elements)");
        float el = (float)j.when.minElevationDeg;
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::SliderFloat("above (deg)", &el, 0, 80, "%.0f")) j.when.minElevationDeg = el;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100 * gUi);
        ImGui::InputInt("start early (s)", &j.when.leadSec);
        j.when.leadSec = std::max(0, std::min(600, j.when.leadSec));
        break;
    }
    }
    if (j.when.kind != sd::Schedule::Pass) {
        ImGui::SetNextItemWidth(110 * gUi);
        ImGui::InputInt("for (minutes)", &S.durMin);
        S.durMin = std::max(0, std::min(24 * 60, S.durMin));
    } else ImGui::TextDisabled("%s", "A pass is recorded from the rise to the set.");

    ImGui::SeparatorText("What it does");
    ImGui::SetNextItemWidth(150 * gUi);
    ImGui::InputDouble("Frequency (MHz, 0 = as it is)", &S.freqMhz, 0, 0, "%.6f");
    const std::string mlabel = j.action.mode.empty() ? "(mode as it is)" : modeNameOf(j.action.mode);
    ImGui::SetNextItemWidth(240 * gUi);
    if (ImGui::BeginCombo("Mode", mlabel.c_str())) {
        if (ImGui::Selectable("(mode as it is)", j.action.mode.empty())) j.action.mode.clear();
        const char* q = modeSearchBox();
        for (int i = 0; i < kNumModes; i++) {
            if (q && *q && !modeMatches(kModes[i], q)) continue;
            const std::string key = scanModeKey(kModes[i]);
            if (ImGui::Selectable(kModes[i].name, key == j.action.mode)) j.action.mode = key;
        }
        ImGui::EndCombo();
    }
    {
        const std::string rl = j.action.radio.empty() ? "(radio in use)" : j.action.radio;
        ImGui::SetNextItemWidth(240 * gUi);
        if (ImGui::BeginCombo("Radio", rl.c_str())) {
            if (ImGui::Selectable("(radio in use)", j.action.radio.empty())) j.action.radio.clear();
            for (const DeviceInfo& d : a.devices) if (ImGui::Selectable(d.name.c_str(), d.name == j.action.radio)) j.action.radio = d.name;
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    float g = (float)j.action.gainDb;
    ImGui::SetNextItemWidth(150 * gUi);
    if (ImGui::InputFloat("Gain (dB, -1 = as it is)", &g, 0, 0, "%.0f")) j.action.gainDb = g;
    ImGui::Checkbox("Record audio", &j.action.recordAudio);
    ImGui::SameLine(); ImGui::Checkbox("Record IQ", &j.action.recordIq);
    ImGui::SameLine(); ImGui::Checkbox("Save the receiver log", &j.action.recordLog);
    ImGui::SetNextItemWidth(380 * gUi);
    ImGui::InputText("File name", S.pattern, sizeof S.pattern);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Leave empty for {date}_{time}_{mode}_{freq}. Tokens: {date} {time} {datetime} {yyyy} {mm} {dd} {HH} {MM} {SS} {utcdate} {utctime} {mode} {freq} {freq_khz} {job} {sat} {n:3}. A / makes a sub-folder.");
    {
        const std::string bad = sd::validateName(S.pattern);
        sd::NameFields nf; nf.utc = nowUtc(); nf.zoneOffsetSec = localOffset(nf.utc); nf.mode = modeNameOf(j.action.mode); nf.job = S.name; nf.freqHz = S.freqMhz * 1e6; nf.counter = 1;
        ImGui::SameLine();
        if (bad.empty()) ImGui::TextDisabled("e.g. %s", sd::expandName(S.pattern, nf).c_str());
        else ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", bad.c_str());
    }
    ImGui::SetNextItemWidth(380 * gUi);
    ImGui::InputText("Folder (empty = recordings)", S.folder, sizeof S.folder);

    ImGui::SeparatorText("If it was missed (the program was closed or the computer asleep)");
    int mp = j.missed == sd::Missed::RunLate ? 1 : 0;
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::Combo("##missed", &mp, "Skip it, note it in the history\0Run it late\0")) j.missed = mp ? sd::Missed::RunLate : sd::Missed::Skip;
    if (j.missed == sd::Missed::RunLate) { ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi); ImGui::InputInt("only within (min)", &S.graceMin); }

    if (!S.msg.empty()) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "%s", S.msg.c_str());
    if (ImGui::Button("Save job")) commitEdit(a);
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) S.editing = false;
}

void drawJobs(App& a) {
    const int64_t now = nowUtc();
    if (now != S.nextAt && now - S.nextAt >= 15) {   // the next-run column is recomputed every 15 s (the pass search is not cheap)
        S.nextAt = now;
        for (const sd::Job& j : S.sc.jobs) S.nextCache[j.id] = j.enabled ? S.sc.nextRunOf(j, now, S.host) : -1;
    }
    if (ImGui::Button("New job")) beginEdit(nullptr);
    ImGui::SameLine();
    if (S.sc.active()) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.4f, 1), "Running: %s until %s", S.sc.activeJobId().c_str(), localText(S.sc.activeEnd(), true).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Stop now")) { S.sc.stopActive(now, S.host); save(); }
    } else ImGui::TextDisabled("%s", "Idle");
    if (S.sc.queued()) { ImGui::SameLine(); ImGui::TextDisabled("(%d waiting)", (int)S.sc.queued()); }
    if (!S.sc.lastWarning().empty()) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "%s", S.sc.lastWarning().c_str());
    if (S.sc.jobs.empty()) { ImGui::TextDisabled("%s", "No jobs yet. A job tunes the radio, records for a while, then puts everything back."); return; }
    if (ImGui::BeginTable("##jobs", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX)) {
        ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("When");
        ImGui::TableSetupColumn("Next run");
        ImGui::TableSetupColumn("Does");
        ImGui::TableSetupColumn("Pri", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        int del = -1;
        for (size_t i = 0; i < S.sc.jobs.size(); i++) {
            sd::Job& j = S.sc.jobs[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Checkbox("##on", &j.enabled)) { S.sc.reschedule(); S.nextAt = 0; save(); }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(j.name.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(sd::describe(j.when).c_str());
            ImGui::TableSetColumnIndex(3);
            const auto nc = S.nextCache.find(j.id);
            if (!j.enabled) ImGui::TextDisabled("off");
            else if (nc == S.nextCache.end()) ImGui::TextDisabled("...");
            else if (nc->second < 0) ImGui::TextDisabled("%s", j.when.kind == sd::Schedule::Pass ? "no pass soon" : "never");
            else ImGui::TextUnformatted(localText(nc->second).c_str());
            ImGui::TableSetColumnIndex(4);
            {
                std::string d;
                if (j.action.freqHz > 0) { char b[32]; snprintf(b, sizeof b, "%.3f MHz ", j.action.freqHz / 1e6); d += b; }
                if (!j.action.mode.empty()) d += modeNameOf(j.action.mode) + " ";
                if (j.action.recordAudio) d += "audio ";
                if (j.action.recordIq) d += "IQ ";
                if (j.action.recordLog) d += "log ";
                if (j.when.kind != sd::Schedule::Pass) d += "for " + std::to_string(j.durationSec / 60) + " min";
                ImGui::TextUnformatted(d.c_str());
            }
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%d", j.priority);
            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton("Run now")) { if (!S.sc.runNow(j.id, now, S.host)) S.msg = "another job with the same or higher priority is running"; else S.msg.clear(); save(); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit")) beginEdit(&j);
            ImGui::SameLine();
            if (ImGui::SmallButton("Delete")) del = (int)i;
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (del >= 0) { S.sc.jobs.erase(S.sc.jobs.begin() + del); S.sc.reschedule(); save(); }
    }
    if (!S.msg.empty() && !S.editing) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "%s", S.msg.c_str());
}

void drawUpcoming() {
    const int64_t now = nowUtc();
    if (now - S.upcomingAt >= 30) { S.upcomingAt = now; S.upcoming = S.sc.upcoming(now, 24 * 3600, S.host); }
    if (S.upcoming.empty()) { ImGui::TextDisabled("%s", "Nothing is planned for the next 24 hours."); return; }
    if (ImGui::BeginTable("##up", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 140 * gUi))) {
        ImGui::TableSetupColumn("Starts"); ImGui::TableSetupColumn("Ends"); ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Note");
        ImGui::TableHeadersRow();
        for (const sd::PlannedRun& r : S.upcoming) {
            std::string nm = r.jobId;
            for (const sd::Job& j : S.sc.jobs) if (j.id == r.jobId) nm = j.name;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(localText(r.start).c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(localText(r.end).c_str());
            ImGui::TableSetColumnIndex(2);
            if (r.state == sd::PlannedRun::Dropped) ImGui::TextDisabled("%s", nm.c_str()); else ImGui::TextUnformatted(nm.c_str());
            ImGui::TableSetColumnIndex(3);
            const char* st = r.state == sd::PlannedRun::Delayed ? "starts later: another job first" : r.state == sd::PlannedRun::Shortened ? "cut short by a higher priority job" : r.state == sd::PlannedRun::Dropped ? "will not run: overlap" : "";
            ImGui::TextUnformatted(st);
        }
        ImGui::EndTable();
    }
}

void drawHistory() {
    if (S.sc.history.empty()) { ImGui::TextDisabled("%s", "No runs yet."); return; }
    if (ImGui::SmallButton("Clear history")) { S.sc.history.clear(); S.histSeen = 0; save(); return; }
    if (ImGui::BeginTable("##hist", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(0, 200 * gUi))) {
        ImGui::TableSetupColumn("When"); ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Result");
        ImGui::TableSetupColumn("Length"); ImGui::TableSetupColumn("Size"); ImGui::TableSetupColumn("Files / error", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (size_t n = S.sc.history.size(); n-- > 0;) {
            const sd::RunRecord& r = S.sc.history[n];
            ImGui::PushID((int)n);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(localText(r.start ? r.start : r.planned, true).c_str());
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(r.jobName.c_str());
            ImGui::TableSetColumnIndex(2); ImGui::TextColored(resultColour(r.result), "%s", sd::resultName(r.result));
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted(r.seconds() > 0 ? durText(r.seconds()).c_str() : "");
            ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(r.bytes() ? sizeText(r.bytes()).c_str() : "");
            ImGui::TableSetColumnIndex(5);
            if (!r.error.empty()) ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.4f, 1), "%s", r.error.c_str());
            for (const sd::FileResult& f : r.files) {
                ImGui::TextUnformatted(f.path.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, %s (click to open the folder)", f.kind.c_str(), sizeText(f.bytes).c_str());
                if (ImGui::IsItemClicked()) dect2::openUrl(f.path.substr(0, f.path.find_last_of("/\\")));
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void drawSettings() {
    sd::Settings& st = S.sc.settings;
    bool ch = false;
    ch |= ImGui::Checkbox("A job that overlaps another waits its turn (otherwise it is skipped)", &st.conflict.queue);
    ImGui::SetNextItemWidth(100 * gUi);
    int wait = st.conflict.maxDelaySec / 60;
    if (ImGui::InputInt("longest wait (min)", &wait)) { st.conflict.maxDelaySec = std::max(0, wait) * 60; ch = true; }
    ImGui::SetNextItemWidth(100 * gUi);
    float gb = (float)(st.minFreeBytes / 1e9);
    if (ImGui::InputFloat("stop recording when less than this much disk is free (GB)", &gb, 0, 0, "%.1f")) { st.minFreeBytes = (int64_t)(std::max(0.f, gb) * 1e9); ch = true; }
    if (ch) save();
}

}  // namespace

// Every frame: the clock check. Cheap: it returns at once unless a second has passed.
void schedulerTick(App& a) {
    load(a);
    static int64_t last = 0;
    const int64_t now = nowUtc();
    if (now == last) return;
    last = now;
    S.sc.tick(now, S.host);
    if (S.sc.history.size() != S.histSeen) { S.histSeen = S.sc.history.size(); save(); }
    else if (std::time(nullptr) - S.lastSave > 60) save();   // lastSeen, for the missed-job rule after a restart
}

void schedulerWindow(App& a) {
    schedulerTick(a);
    if (!a.schedulerOpen) return;
    firstWindowSize(980 * gUi, 640 * gUi);
    if (!ImGui::Begin("Scheduler###scheduler", &a.schedulerOpen)) { ImGui::End(); return; }
    ImGui::TextDisabled("%s", "Jobs run while this window is shut and while the program window is hidden. They need OnAir to be running.");
    drawJobs(a);
    if (S.editing) drawEditor(a);
    if (ImGui::CollapsingHeader("Next 24 hours", ImGuiTreeNodeFlags_DefaultOpen)) drawUpcoming();
    if (ImGui::CollapsingHeader("History", ImGuiTreeNodeFlags_DefaultOpen)) drawHistory();
    if (ImGui::CollapsingHeader("Conflicts and disk space")) drawSettings();
    ImGui::End();
}
