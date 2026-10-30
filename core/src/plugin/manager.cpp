#include "dect2/plugin/host.h"
#include "dect2/automation/engine.h"
#include "dect2/automation/json_mini.h"
#include "dect2/plugin/zip.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace dect2::plugin {
namespace fs = std::filesystem;
namespace aj = automation::json;

// ---------------- settings ----------------
bool SettingValues::set(const std::string& key, const std::string& value) {
    const SettingDef* d = nullptr;
    for (const auto& s : m_.settings) if (s.key == key) d = &s;
    if (!d || value.size() > 500) return false;
    std::string v = value;
    if (d->type == "slider" || d->type == "combo") {
        char* e = nullptr;
        double x = strtod(value.c_str(), &e);
        if (!e || e == value.c_str() || !std::isfinite(x)) return false;
        if (d->type == "slider") x = std::clamp(x, d->min, d->max);
        else x = std::clamp(std::floor(x), 0.0, (double)d->options.size() - 1);
        char buf[64]; snprintf(buf, sizeof buf, "%.10g", x); v = buf;
    } else if (d->type == "checkbox") {
        if (value != "0" && value != "1" && value != "true" && value != "false") return false;
        v = (value == "1" || value == "true") ? "1" : "0";
    }
    std::lock_guard<std::mutex> lk(mu_);
    v_[key] = v;
    return true;
}
std::string SettingValues::text(const std::string& key) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = v_.find(key);
    if (it != v_.end()) return it->second;
    for (const auto& s : m_.settings) if (s.key == key) {
        if (s.type == "text") return s.defText;
        if (s.type == "checkbox") return s.defFlag || s.def != 0 ? "1" : "0";
        char buf[64]; snprintf(buf, sizeof buf, "%.10g", s.def); return buf;
    }
    return {};
}
double SettingValues::number(const std::string& key, double def) const { const std::string t = text(key); char* e = nullptr; const double d = strtod(t.c_str(), &e); return e && e != t.c_str() ? d : def; }
bool SettingValues::flag(const std::string& key, bool def) const { const std::string t = text(key); return t.empty() ? def : t == "1"; }
std::map<std::string, std::string> SettingValues::all() const { std::lock_guard<std::mutex> lk(mu_); return v_; }
void SettingValues::load(const std::map<std::string, std::string>& v) { for (const auto& kv : v) set(kv.first, kv.second); }

// ---------------- manager ----------------
namespace {
struct Want { std::atomic<double> hz{0}, rate{0}; };
struct Slot { std::unique_ptr<Runner> run; std::shared_ptr<Want> want = std::make_shared<Want>(); std::string pluginId, modeId; };
struct PState { bool enabled = false, answered = false, oop = false; uint32_t granted = 0; std::map<std::string, std::string> values; };
std::string readFile(const fs::path& p, size_t maxBytes) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return s.size() > maxBytes ? std::string() : s;
}
bool writeFileAtomic(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path t = p.string() + ".tmp";
    { std::ofstream f(t, std::ios::binary | std::ios::trunc); if (!f) return false; f << data; if (!f) return false; }
    fs::rename(t, p, ec);
    return !ec;
}
std::string sanitizeType(const std::string& t) {
    std::string o;
    for (char c : t) { if (c >= 'A' && c <= 'Z') c = (char)(c + 32); if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') o += c; else if (c == '.' || c == '-' || c == ' ') o += '_'; }
    return o.substr(0, 40);
}
}

struct Manager::Impl {
    mutable std::mutex mu;
    std::vector<Entry> entries;
    std::map<std::string, PState> state;
    std::map<std::string, int> numbers;     // "plugin/mode" -> number
    std::map<std::string, std::shared_ptr<SettingValues>> settings;
    std::map<std::string, std::shared_ptr<Results>> results;
    std::map<std::string, std::deque<std::string>> logs;
    std::map<std::string, std::unique_ptr<Slot>> slots;   // "plugin/mode"
    std::map<std::string, std::deque<double>> evTimes;     // emit rate limit
    bool safe = false, forced = false;
    std::string safeReason;
    bool markerWritten = false;
    bool automationAttached = false;
    std::map<std::string, std::string> inUse;   // ids of plugins that have run since the marker (for the reason)
};

Manager::Manager(ManagerConfig c) : p_(std::make_shared<Impl>()), cfg_(std::move(c)) {}
Manager::~Manager() { stopAll(); if (p_->automationAttached) automation::global().setObserver(nullptr, 0); endSession(); }

static fs::path statePath(const ManagerConfig& c) { return fs::path(c.pluginsDir) / "state.json"; }
static fs::path markerPath(const ManagerConfig& c) { return fs::path(c.pluginsDir) / ".running"; }
static fs::path safeFlagPath(const ManagerConfig& c) { return fs::path(c.pluginsDir) / "safe-mode.flag"; }

static void saveState(Manager::Impl& p, const ManagerConfig& c) {   // call with p.mu held
    aj::Value root = aj::Value::object(), pl = aj::Value::object(), nums = aj::Value::object();
    for (const auto& kv : p.state) {
        aj::Value o = aj::Value::object();
        o.set("enabled", aj::Value::boolean(kv.second.enabled));
        o.set("answered", aj::Value::boolean(kv.second.answered));
        o.set("out_of_process", aj::Value::boolean(kv.second.oop));
        o.set("granted", aj::Value::number(kv.second.granted));
        aj::Value vals = aj::Value::object();
        for (const auto& s : kv.second.values) vals.set(s.first, aj::Value::string(s.second));
        o.set("settings", vals);
        pl.set(kv.first, o);
    }
    for (const auto& kv : p.numbers) nums.set(kv.first, aj::Value::number(kv.second));
    root.set("plugins", pl); root.set("numbers", nums);
    writeFileAtomic(statePath(c), aj::dump(root, true));
}

static void logTo(Manager::Impl& p, const std::string& id, int level, const std::string& m) {
    static const char* const lv[] = {"debug", "info", "warn", "error"};
    char t[32]; const time_t now = time(nullptr); struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &now);
#else
    localtime_r(&now, &tmv);
#endif
    strftime(t, sizeof t, "%H:%M:%S", &tmv);
    std::lock_guard<std::mutex> lk(p.mu);
    auto& q = p.logs[id];
    std::string line = std::string(t) + " " + lv[std::clamp(level, 0, 3)] + " " + m.substr(0, 1000);
    q.push_back(line);
    while (q.size() > 500) q.pop_front();
}

void Manager::scan() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->entries.clear();
    // state
    p_->state.clear(); p_->numbers.clear();
    aj::Value root; std::string perr;
    const std::string st = readFile(statePath(cfg_), 1 << 20);
    if (!st.empty() && aj::parse(st, root, perr) && root.type == aj::Value::Obj) {
        if (const aj::Value* pl = root.get("plugins")) if (pl->type == aj::Value::Obj) for (const auto& kv : pl->o) {
            PState s;
            s.enabled = kv.second.flag("enabled"); s.answered = kv.second.flag("answered"); s.oop = kv.second.flag("out_of_process");
            s.granted = (uint32_t)kv.second.num("granted") & 31u;
            if (const aj::Value* v = kv.second.get("settings")) if (v->type == aj::Value::Obj) for (const auto& x : v->o) if (x.second.type == aj::Value::Str) s.values[x.first] = x.second.s;
            p_->state[kv.first] = s;
        }
        if (const aj::Value* n = root.get("numbers")) if (n->type == aj::Value::Obj) for (const auto& kv : n->o) if (kv.second.type == aj::Value::Num && kv.second.n >= kFirstModeNumber && kv.second.n < 1e6) p_->numbers[kv.first] = (int)kv.second.n;
    }
    auto scanDir = [&](const std::string& dir, bool bundled) {
        if (dir.empty()) return;
        std::error_code ec;
        std::vector<fs::path> dirs;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) if (it->is_directory(ec)) dirs.push_back(it->path());
        std::sort(dirs.begin(), dirs.end());
        for (const auto& d : dirs) {
            Entry e;
            e.dir = d.string(); e.bundled = bundled;
            const std::string folder = d.filename().string();
            const std::string text = readFile(d / "manifest.json", kMaxManifestBytes + 1);
            std::string err;
            if (text.empty()) continue;   // not a plugin folder
            if (!parseManifest(text, e.m, err, cfg_.hostVersion)) { e.m.id = folder; e.m.name = folder; e.error = err; }
            else if (e.m.id != folder) { e.error = "the folder name must be the plugin id (" + e.m.id + ")"; }
            else {
                const std::string lib = e.m.libraryFile();
                if (e.m.kind != "panel-lite" && lib.empty()) e.error = "no library for this platform in the manifest";
                else if (!lib.empty()) {
                    e.libraryPath = (d / lib).string();
                    if (!fs::exists(e.libraryPath, ec)) e.error = "the library file " + lib + " is missing";
                }
            }
            e.valid = e.error.empty();
            bool dup = false;
            for (const auto& o : p_->entries) if (o.m.id == e.m.id) dup = true;
            if (dup) continue;   // a user-installed plugin hides a bundled one with the same id
            auto it = p_->state.find(e.m.id);
            if (it != p_->state.end()) { e.enabled = it->second.enabled; e.permsAnswered = it->second.answered; e.granted = it->second.granted & e.m.permissions; }
            if (e.valid && !p_->settings.count(e.m.id)) {
                auto sv = std::make_shared<SettingValues>(e.m);
                if (it != p_->state.end()) sv->load(it->second.values);
                p_->settings[e.m.id] = sv;
            }
            p_->entries.push_back(std::move(e));
        }
    };
    scanDir(cfg_.pluginsDir, false);
    scanDir(cfg_.bundledDir, true);
}

std::vector<Entry> Manager::entries() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->entries; }
bool Manager::find(const std::string& id, Entry& out) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const auto& e : p_->entries) if (e.m.id == id) { out = e; return true; }
    return false;
}

// ---- safe mode ----
void Manager::beginSession() {
    std::error_code ec;
    fs::create_directories(cfg_.pluginsDir, ec);
    std::lock_guard<std::mutex> lk(p_->mu);
    const std::string m = readFile(markerPath(cfg_), 4096);
    const bool crashed = fs::exists(markerPath(cfg_), ec);
    if (crashed) {
        std::string who = m.substr(0, m.find('\n'));
        p_->safe = true;
        p_->safeReason = "OnAir did not shut down cleanly while plugin code was running" + (who.empty() ? std::string() : " (" + who + ")");
        writeFileAtomic(safeFlagPath(cfg_), p_->safeReason);
        fs::remove(markerPath(cfg_), ec);
        // the plugin that was running stays off until the user turns it on again
        for (const std::string& id : [&] { std::vector<std::string> v; std::stringstream ss(m); std::string x; while (std::getline(ss, x, ',')) { while (!x.empty() && (x.back() == '\n' || x.back() == ' ')) x.pop_back(); if (!x.empty()) v.push_back(x); } return v; }())
            if (p_->state.count(id)) p_->state[id].enabled = false;
        saveState(*p_, cfg_);
    } else if (fs::exists(safeFlagPath(cfg_), ec)) {
        p_->safe = true;
        p_->safeReason = readFile(safeFlagPath(cfg_), 4096);
        if (p_->safeReason.empty()) p_->safeReason = "safe mode is on";
    }
    for (auto& e : p_->entries) if (p_->state.count(e.m.id)) e.enabled = p_->state[e.m.id].enabled;
}
void Manager::endSession() {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->markerWritten) { std::error_code ec; fs::remove(markerPath(cfg_), ec); p_->markerWritten = false; }
}
bool Manager::safeMode() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->safe || p_->forced; }
std::string Manager::safeModeReason() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->forced && p_->safeReason.empty() ? "safe mode was switched on" : p_->safeReason; }
void Manager::leaveSafeMode() {
    stopAll();
    std::lock_guard<std::mutex> lk(p_->mu);
    std::error_code ec;
    fs::remove(safeFlagPath(cfg_), ec);
    p_->safe = false; p_->forced = false; p_->safeReason.clear();
}
void Manager::forceSafeMode(bool on) { if (on) stopAll(); std::lock_guard<std::mutex> lk(p_->mu); p_->forced = on; }

// ---- state changes ----
bool Manager::setEnabled(const std::string& id, bool on, std::string& err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& e : p_->entries) if (e.m.id == id) {
        if (on && !e.valid) { err = e.error; return false; }
        e.enabled = on;
        p_->state[id].enabled = on;
        saveState(*p_, cfg_);
        return true;
    }
    err = "no such plugin";
    return false;
}
bool Manager::setGranted(const std::string& id, uint32_t mask, std::string& err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& e : p_->entries) if (e.m.id == id) {
        e.granted = mask & e.m.permissions; e.permsAnswered = true;
        p_->state[id].granted = e.granted; p_->state[id].answered = true;
        saveState(*p_, cfg_);
        return true;
    }
    err = "no such plugin";
    return false;
}
bool Manager::setSetting(const std::string& id, const std::string& key, const std::string& value) {
    std::shared_ptr<SettingValues> sv;
    std::vector<Runner*> rs;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        auto it = p_->settings.find(id);
        if (it == p_->settings.end()) return false;
        sv = it->second;
        if (!sv->set(key, value)) return false;
        p_->state[id].values = sv->all();
        saveState(*p_, cfg_);
        for (auto& s : p_->slots) if (s.second->pluginId == id && s.second->run) rs.push_back(s.second->run.get());
    }
    for (Runner* r : rs) r->settingChanged(key);
    return true;
}
std::shared_ptr<SettingValues> Manager::settings(const std::string& id) { std::lock_guard<std::mutex> lk(p_->mu); auto it = p_->settings.find(id); return it == p_->settings.end() ? nullptr : it->second; }
std::shared_ptr<Results> Manager::results(const std::string& id, const std::string& modeId) {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto& r = p_->results[id + "/" + modeId];
    if (!r) r = std::make_shared<Results>();
    return r;
}
void Manager::setOutOfProcess(const std::string& id, bool on) { std::lock_guard<std::mutex> lk(p_->mu); p_->state[id].oop = on; saveState(*p_, cfg_); }
bool Manager::outOfProcess(const std::string& id) const { std::lock_guard<std::mutex> lk(p_->mu); auto it = p_->state.find(id); return it != p_->state.end() && it->second.oop; }

// ---- numbering ----
int Manager::modeNumber(const std::string& pid, const std::string& mid) {
    std::lock_guard<std::mutex> lk(p_->mu);
    const std::string key = pid + "/" + mid;
    auto it = p_->numbers.find(key);
    if (it != p_->numbers.end()) return it->second;
    int next = kFirstModeNumber;
    for (const auto& kv : p_->numbers) next = std::max(next, kv.second + 1);
    p_->numbers[key] = next;
    saveState(*p_, cfg_);
    return next;
}
bool Manager::modeFromNumber(int n, std::string& pid, std::string& mid) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (const auto& kv : p_->numbers) if (kv.second == n) { const size_t s = kv.first.find('/'); pid = kv.first.substr(0, s); mid = kv.first.substr(s + 1); return true; }
    return false;
}
std::vector<Manager::ModeRow> Manager::modeRows() const {
    std::vector<Entry> es = entries();
    const bool safe = safeMode();
    std::vector<ModeRow> rows;
    for (const auto& e : es) {
        if (!e.valid) continue;
        for (const auto& m : e.m.modes) {
            ModeRow r;
            r.number = const_cast<Manager*>(this)->modeNumber(e.m.id, m.id);
            r.pluginId = e.m.id; r.modeId = m.id; r.name = m.name; r.defaultHz = m.defaultHz; r.bandwidthHz = m.bandwidthHz; r.sampleRates = m.sampleRates;
            r.available = e.enabled && !safe && !e.needsPrompt();
            rows.push_back(std::move(r));
        }
    }
    std::sort(rows.begin(), rows.end(), [](const ModeRow& a, const ModeRow& b) { return a.number < b.number; });
    return rows;
}

// ---- running ----
Runner* Manager::start(const std::string& id, const std::string& modeId, std::string& err) {
    Entry e;
    if (!find(id, e)) { err = "no such plugin"; return nullptr; }
    if (safeMode()) { err = "safe mode is on: plugins are switched off"; return nullptr; }
    if (!e.valid) { err = e.error; return nullptr; }
    if (!e.enabled) { err = "the plugin is disabled"; return nullptr; }
    if (e.needsPrompt()) { err = "the plugin's permissions have not been answered yet"; return nullptr; }
    if (e.m.kind == "panel-lite") { err = "this kind of plugin has no code to run"; return nullptr; }
    if (!modeId.empty()) { bool ok = false; for (const auto& m : e.m.modes) if (m.id == modeId) ok = true; if (!ok) { err = "no such mode in the plugin"; return nullptr; } }
    const std::string key = id + "/" + modeId;
    stop(id, modeId);
    StartOptions o;
    auto man = std::make_shared<Manifest>(e.m);
    o.libraryPath = e.libraryPath; o.manifest = man.get(); o.modeId = modeId; o.granted = e.granted;
    o.results = results(id, modeId);
    o.settings = settings(id);
    o.hostExe = cfg_.hostExe;
    o.services.dataDir = (e.granted & PermFilesystem) ? (fs::path(cfg_.pluginsDir) / id / "data").string() : std::string();
    if (!o.services.dataDir.empty()) { std::error_code ec; fs::create_directories(o.services.dataDir, ec); }
    std::shared_ptr<Impl> p = p_;
    auto slot = std::make_unique<Slot>();
    slot->pluginId = id; slot->modeId = modeId;
    std::shared_ptr<Want> want = slot->want;
    o.services.log = [p, id](int lv, const std::string& m) { logTo(*p, id, lv, m); };
    o.services.requestIq = [want](double hz, double sr) { want->hz = hz; want->rate = sr; return 0; };
    const std::string name = e.m.name;
    o.services.emitEvent = [p, id, name](const std::string& type, const automation::Event& src) {
        const std::string t = sanitizeType(type);
        if (t.empty()) return -1;
        {
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            std::lock_guard<std::mutex> lk(p->mu);
            auto& q = p->evTimes[id];
            while (!q.empty() && now - q.front() > 1.0) q.pop_front();
            if (q.size() >= 100) return -2;   // 100 events a second per plugin
            q.push_back(now);
        }
        automation::Event ev = src;
        ev.type = "plugin." + t; ev.time = automation::wallNow(); ev.mode = "plugin:" + id;
        ev.str("plugin", name);
        if (automation::wants(automation::FPlugin)) automation::emit(std::move(ev));
        return 0;
    };
    // audio out: the app connects it (Manager::setAudioSink would go here); without a sink the audio is dropped
    o.services.audioOut = [](const float*, size_t, double) { return 0; };
    std::unique_ptr<Runner> r;
    if (outOfProcess(id)) r = startProcess(o, err);
    else r = startInProcess(o, err);
    if (!r) { logTo(*p_, id, 3, "start failed: " + err); return nullptr; }
    // the marker: from now on a crash of this process points at this plugin (in-process plugins only; a helper process cannot take the app down)
    slot->run = std::move(r);
    // keep the manifest alive as long as the runner (it copies, but the pointer in options was ours)
    Runner* raw = slot->run.get();
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        auto it = p_->state.find(id);
        const bool oop = it != p_->state.end() && it->second.oop;
        if (!oop) {
            p_->inUse[id] = id;
            std::string list;
            for (const auto& kv : p_->inUse) list += (list.empty() ? "" : ",") + kv.first;
            writeFileAtomic(markerPath(cfg_), list + "\n");
            p_->markerWritten = true;
        }
        p_->slots[key] = std::move(slot);
    }
    logTo(*p_, id, 1, "started" + (modeId.empty() ? std::string() : " mode " + modeId));
    return raw;
}

void Manager::stop(const std::string& id, const std::string& modeId) {
    std::unique_ptr<Slot> s;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        auto it = p_->slots.find(id + "/" + modeId);
        if (it == p_->slots.end()) return;
        s = std::move(it->second);
        p_->slots.erase(it);
    }
    if (s->run) s->run->stop();
    logTo(*p_, id, 1, "stopped");
    std::lock_guard<std::mutex> lk(p_->mu);
    bool any = false;
    for (auto& kv : p_->slots) if (kv.second->pluginId == id) any = true;
    if (!any) p_->inUse.erase(id);
    if (p_->inUse.empty() && p_->markerWritten) { std::error_code ec; fs::remove(markerPath(cfg_), ec); p_->markerWritten = false; }
}
void Manager::stopAll() {
    std::vector<std::pair<std::string, std::string>> v;
    { std::lock_guard<std::mutex> lk(p_->mu); for (auto& kv : p_->slots) v.push_back({kv.second->pluginId, kv.second->modeId}); }
    for (auto& x : v) stop(x.first, x.second);
}
Runner* Manager::running(const std::string& id, const std::string& modeId) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->slots.find(id + "/" + modeId);
    return it == p_->slots.end() ? nullptr : it->second->run.get();
}
void Manager::startActions() {
    for (const auto& e : entries()) if (e.valid && e.enabled && e.m.kind == "action") { std::string err; start(e.m.id, "", err); }
}
void Manager::dispatchEvent(const automation::Event& ev) {
    std::vector<Runner*> rs;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& kv : p_->slots) if (kv.second->modeId.empty() && kv.second->run && ev.mode != "plugin:" + kv.second->pluginId) rs.push_back(kv.second->run.get());
    }
    for (Runner* r : rs) r->feedEvent(ev);
}
void Manager::attachAutomation() {
    if (p_->automationAttached) return;
    p_->automationAttached = true;
    automation::global().setObserver([this](const automation::Event& e) { dispatchEvent(e); }, automation::kAllFamilies);
}
void Manager::tick(double now) {
    std::vector<std::pair<std::string, std::string>> failed;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        for (auto& kv : p_->slots) {
            if (!kv.second->run) continue;
            const RunState s = kv.second->run->state();
            if (s == RunState::Failed || s == RunState::Hung) failed.push_back({kv.second->pluginId, kv.second->modeId});
            else kv.second->run->tick(now);
        }
    }
    for (auto& f : failed) {
        stop(f.first, f.second);
        std::string err;
        setEnabled(f.first, false, err);
        logTo(*p_, f.first, 3, "the plugin failed and was switched off");
    }
}
bool Manager::wantedIq(const std::string& id, double& hz, double& sr) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    for (auto& kv : p_->slots) if (kv.second->pluginId == id && (kv.second->want->hz > 0 || kv.second->want->rate > 0)) { hz = kv.second->want->hz; sr = kv.second->want->rate; return true; }
    return false;
}

// ---- install / remove ----
bool Manager::installFile(const std::string& path, std::string& id, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open the file"; return false; }
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() > (300u << 20)) { err = "the file is too large"; return false; }
    return installBytes(d, id, err);
}
bool Manager::installBytes(const std::vector<uint8_t>& zip, std::string& id, std::string& err) {
    ZipReader z;
    if (!z.open(zip, err)) return false;
    // the manifest sits at the top, or inside a single top folder
    std::string prefix;
    const ZipEntry* mf = nullptr;
    for (const auto& e : z.entries()) if (e.name == "manifest.json") mf = &e;
    if (!mf) {
        for (const auto& e : z.entries()) { const size_t s = e.name.find('/'); if (s != std::string::npos && e.name.substr(s) == "/manifest.json") { mf = &e; prefix = e.name.substr(0, s + 1); } }
    }
    if (!mf) { err = "the file has no manifest.json"; return false; }
    std::vector<uint8_t> mb;
    if (!z.read(*mf, mb, err)) return false;
    Manifest m;
    if (!parseManifest(std::string(mb.begin(), mb.end()), m, err, cfg_.hostVersion)) return false;
    const std::string lib = m.libraryFile();
    bool haveLib = lib.empty();
    for (const auto& e : z.entries()) if (!e.dir && e.name == prefix + lib) haveLib = true;
    if (!haveLib) { err = "this plugin has no library for this platform (" + (lib.empty() ? std::string("none named in the manifest") : lib) + ")"; return false; }
    // unpack to a temporary folder, then swap it in
    std::error_code ec;
    const fs::path base = cfg_.pluginsDir, tmp = base / (".install-" + m.id), dest = base / m.id;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    for (const auto& e : z.entries()) {
        if (prefix.size() && e.name.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rel = e.name.substr(prefix.size());
        if (rel.empty() || e.dir) continue;
        if (!safeZipName(rel)) { err = "unsafe file name in the zip"; fs::remove_all(tmp, ec); return false; }
        std::vector<uint8_t> data;
        if (!z.read(e, data, err)) { fs::remove_all(tmp, ec); return false; }
        const fs::path target = tmp / rel;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream o(target, std::ios::binary | std::ios::trunc);
        o.write((const char*)data.data(), (std::streamsize)data.size());
        if (!o) { err = "cannot write " + rel; fs::remove_all(tmp, ec); return false; }
    }
    stop(m.id, "");
    { std::string mid; for (const auto& md : m.modes) stop(m.id, md.id); (void)mid; }
    fs::remove_all(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) { err = "cannot move the plugin into place: " + ec.message(); fs::remove_all(tmp, ec); return false; }
    id = m.id;
    scan();
    return true;
}
bool Manager::remove(const std::string& id, std::string& err) {
    Entry e;
    if (!find(id, e)) { err = "no such plugin"; return false; }
    if (e.bundled) { err = "bundled examples cannot be removed (disable them instead)"; return false; }
    if (!validId(id)) { err = "bad id"; return false; }
    for (const auto& m : e.m.modes) stop(id, m.id);
    stop(id, "");
    std::error_code ec;
    fs::remove_all(fs::path(cfg_.pluginsDir) / id, ec);
    if (ec) { err = ec.message(); return false; }
    { std::lock_guard<std::mutex> lk(p_->mu); p_->state.erase(id); p_->settings.erase(id); saveState(*p_, cfg_); }
    scan();
    return true;
}
std::vector<std::string> Manager::log(const std::string& id, size_t n) const {
    std::lock_guard<std::mutex> lk(p_->mu);
    auto it = p_->logs.find(id);
    if (it == p_->logs.end()) return {};
    const size_t k = std::min(n, it->second.size());
    return std::vector<std::string>(it->second.end() - k, it->second.end());
}

} // namespace dect2::plugin
